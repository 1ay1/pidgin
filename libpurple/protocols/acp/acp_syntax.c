/*
 * ACP protocol plugin -- syntax highlighter for fenced code blocks.
 *
 * A faithful C port of the token classes + line-number gutter from maya's
 * markdown widget (1ay1/maya src/widget/markdown/render/syntax_highlight.cpp
 * + syntax_lang.cpp). It converts a code block's body into an HTML fragment
 * with:
 *   - a right-aligned line-number gutter ("NNN | ") in a dim colour, exactly
 *     one output line per input line, with a stable 3-digit floor so the
 *     gutter width never shifts as a streamed block grows;
 *   - per-token colouring: comments, strings, numbers, keywords, types,
 *     constants, function calls, operators, punctuation -- driven by a
 *     small per-language keyword table (Python, C/C++, JS/TS, Rust, Go,
 *     shell, plus a generic C-style fallback).
 *
 * The colour palette mirrors maya's syntax:: styles mapped from ANSI to hex:
 *   keyword  magenta      type    cyan        function blue
 *   string   green        number  bright_yel  comment  bright_black italic
 *   operator red          punct   bright_blk  plain    body-grey
 *   gutter   bright_black dim     shellvar bright_cyan
 *
 * Copyright (C) 2024 Ayush Bhat <tfeayush@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 (or, at your
 * option, any later version).
 */
#include "acp.h"

#include <ctype.h>
#include <string.h>

/* ── token colours (maya syntax::) ──────────────────────────────────── */
#define SC_KW      "#ad7fa8"   /* magenta      */
#define SC_TYPE    "#34e2e2"   /* cyan         */
#define SC_FN      "#729fcf"   /* blue         */
#define SC_STR     "#8ae234"   /* green        */
#define SC_NUM     "#fce94f"   /* bright_yellow */
#define SC_COMMENT "#888a85"   /* bright_black (italic) */
#define SC_CONST   "#fce94f"   /* bright_yellow */
#define SC_PREPROC "#c4a000"   /* yellow       */
#define SC_ATTR    "#c4a000"   /* yellow       */
#define SC_OP      "#ef2929"   /* red          */
#define SC_PUNCT   "#888a85"   /* bright_black */
#define SC_PLAIN   "#d4d4d4"   /* body text    */
#define SC_SHVAR   "#34e2e2"   /* bright_cyan  */
#define SC_GUTTER  "#666666"   /* dim line-number gutter */
#define SC_CODEBG  "#181818"   /* code panel background fill */
#define SC_BORDER  "#3a3a3a"   /* code panel frame           */

/* ── language ids ───────────────────────────────────────────────────── */
typedef enum {
	L_UNKNOWN, L_C, L_CPP, L_PYTHON, L_RUST, L_JS, L_TS,
	L_GO, L_SHELL, L_JSON, L_SQL
} AcpLang;

typedef struct {
	const char *line;        /* line comment lead, or NULL   */
	const char *block_open;  /* block comment open, or NULL  */
	const char *block_close; /* block comment close, or NULL */
	gboolean    hash;        /* '#' line comment            */
	gboolean    triple;      /* python/js triple/backtick strings */
} AcpComment;

static AcpLang
detect_lang(const char *tag)
{
	char b[24];
	gsize i, len;
	if (!tag) return L_UNKNOWN;
	for (len = 0; tag[len] && len < sizeof(b) - 1; len++)
		b[len] = (char)tolower((unsigned char)tag[len]);
	b[len] = '\0';
	for (i = 0; i < len; i++) if (b[i] == ' ') { b[i] = '\0'; break; }

	if (!strcmp(b, "c") || !strcmp(b, "h"))                      return L_C;
	if (!strcmp(b, "cpp") || !strcmp(b, "c++") || !strcmp(b, "cc") ||
	    !strcmp(b, "cxx") || !strcmp(b, "hpp"))                  return L_CPP;
	if (!strcmp(b, "python") || !strcmp(b, "py"))               return L_PYTHON;
	if (!strcmp(b, "rust") || !strcmp(b, "rs"))                 return L_RUST;
	if (!strcmp(b, "js") || !strcmp(b, "javascript") ||
	    !strcmp(b, "jsx") || !strcmp(b, "mjs"))                 return L_JS;
	if (!strcmp(b, "ts") || !strcmp(b, "typescript") ||
	    !strcmp(b, "tsx"))                                      return L_TS;
	if (!strcmp(b, "go") || !strcmp(b, "golang"))               return L_GO;
	if (!strcmp(b, "sh") || !strcmp(b, "bash") ||
	    !strcmp(b, "shell") || !strcmp(b, "zsh"))               return L_SHELL;
	if (!strcmp(b, "json") || !strcmp(b, "jsonc"))              return L_JSON;
	if (!strcmp(b, "sql"))                                      return L_SQL;
	return L_UNKNOWN;
}

static AcpComment
comment_for(AcpLang l)
{
	AcpComment c = { "//", "/*", "*/", FALSE, FALSE };
	switch (l) {
	case L_PYTHON:  c.line = NULL; c.block_open = NULL; c.block_close = NULL;
	                c.hash = TRUE; c.triple = TRUE; break;
	case L_SHELL:   c.line = NULL; c.block_open = NULL; c.block_close = NULL;
	                c.hash = TRUE; break;
	case L_JS: case L_TS: c.triple = TRUE; break;   /* backtick strings */
	case L_SQL:     c.line = "--"; break;
	case L_JSON:    c.line = NULL; c.block_open = NULL; c.block_close = NULL; break;
	default: break;
	}
	return c;
}

/* ── keyword / type / constant classification ───────────────────────── */
static gboolean
in_words(const char *w, const char *const *list)
{
	int i;
	for (i = 0; list[i]; i++)
		if (!strcmp(w, list[i]))
			return TRUE;
	return FALSE;
}

/* class: 1 = keyword, 2 = type, 3 = constant, 0 = none */
static int
classify_word(const char *w, AcpLang lang)
{
	static const char *const universal_const[] = {
	    "true", "false", "null", "nullptr", "None", "nil", "True", "False",
	    "NULL", "NaN", "Infinity", "undefined", "YES", "NO", NULL };
	static const char *const py_kw[] = {
	    "if","elif","else","for","while","break","continue","return","yield",
	    "pass","raise","try","except","finally","with","as","assert","del",
	    "def","class","lambda","async","await","import","from","global",
	    "nonlocal","and","or","not","in","is","self","cls","super", NULL };
	static const char *const py_ty[] = {
	    "int","float","str","bool","list","dict","tuple","set","bytes",
	    "complex","frozenset","type","object","range","enumerate","zip",
	    "map","filter","Exception","ValueError","TypeError","KeyError",
	    "IndexError","RuntimeError","AttributeError","ImportError", NULL };
	static const char *const c_kw[] = {
	    "if","else","for","while","do","switch","case","break","continue",
	    "return","goto","default","struct","enum","union","typedef","class",
	    "namespace","template","typename","using","static","extern","inline",
	    "const","constexpr","volatile","mutable","virtual","override","final",
	    "explicit","noexcept","public","private","protected","friend","new",
	    "delete","operator","sizeof","decltype","throw","try","catch","auto",
	    "void","co_await","co_return","co_yield","requires","concept", NULL };
	static const char *const c_ty[] = {
	    "int","char","float","double","long","short","unsigned","signed",
	    "bool","size_t","uint8_t","uint16_t","uint32_t","uint64_t","int8_t",
	    "int16_t","int32_t","int64_t","string","vector","map","set","array",
	    "optional","variant","pair","tuple","unique_ptr","shared_ptr", NULL };
	static const char *const rust_kw[] = {
	    "if","else","for","while","loop","break","continue","return","match",
	    "as","fn","struct","enum","impl","trait","type","where","let","mut",
	    "const","static","ref","move","pub","mod","use","crate","super",
	    "self","Self","async","await","unsafe","extern","dyn", NULL };
	static const char *const rust_ty[] = {
	    "i8","i16","i32","i64","isize","u8","u16","u32","u64","usize","f32",
	    "f64","bool","char","str","String","Vec","Box","Rc","Arc","Option",
	    "Result","Ok","Err","Some","HashMap","HashSet","Clone","Copy", NULL };
	static const char *const js_kw[] = {
	    "if","else","for","while","do","switch","case","break","continue",
	    "return","throw","try","catch","finally","default","in","of","typeof",
	    "instanceof","void","delete","function","class","extends","new","this",
	    "super","const","let","var","async","await","yield","import","export",
	    "from","as","interface","type","enum","namespace","declare","readonly",
	    "implements","abstract", NULL };
	static const char *const js_ty[] = {
	    "string","number","boolean","object","symbol","bigint","any","unknown",
	    "never","void","Array","Map","Set","Promise","Date","RegExp","Error",
	    "Object","Function","Symbol","Record","Partial","Readonly", NULL };
	static const char *const go_kw[] = {
	    "if","else","for","switch","case","break","continue","return","goto",
	    "default","fallthrough","select","func","type","struct","interface",
	    "map","chan","var","const","package","import","go","defer","range", NULL };
	static const char *const go_ty[] = {
	    "int","int8","int16","int32","int64","uint","uint8","uint32","uint64",
	    "uintptr","float32","float64","bool","byte","rune","string","error",
	    "any","comparable", NULL };
	static const char *const go_const[] = {
	    "make","append","len","cap","copy","close","new","delete","panic",
	    "recover","iota","nil", NULL };
	static const char *const sh_kw[] = {
	    "if","then","else","elif","fi","for","while","until","do","done",
	    "case","esac","in","function","return","local","export","unset",
	    "readonly","declare","source","eval","exec","set","shift","trap",
	    "echo","printf","read","test","exit","cd","source", NULL };
	static const char *const sql_kw[] = {
	    "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","DELETE",
	    "CREATE","TABLE","DROP","ALTER","JOIN","LEFT","RIGHT","INNER","OUTER",
	    "ON","GROUP","BY","ORDER","HAVING","LIMIT","AS","AND","OR","NOT",
	    "NULL","PRIMARY","KEY","FOREIGN","INDEX","DISTINCT","COUNT", NULL };

	if (in_words(w, universal_const)) return 3;

	switch (lang) {
	case L_PYTHON:
		if (in_words(w, py_kw)) return 1;
		if (in_words(w, py_ty)) return 2;
		break;
	case L_C: case L_CPP:
		if (in_words(w, c_kw)) return 1;
		if (in_words(w, c_ty)) return 2;
		break;
	case L_RUST:
		if (in_words(w, rust_kw)) return 1;
		if (in_words(w, rust_ty)) return 2;
		break;
	case L_JS: case L_TS:
		if (in_words(w, js_kw)) return 1;
		if (in_words(w, js_ty)) return 2;
		break;
	case L_GO:
		if (in_words(w, go_kw))    return 1;
		if (in_words(w, go_ty))    return 2;
		if (in_words(w, go_const)) return 3;
		break;
	case L_SHELL:
		if (in_words(w, sh_kw)) return 1;
		break;
	case L_SQL:
		if (in_words(w, sql_kw)) return 1;
		break;
	default:
		if (in_words(w, c_kw)) return 1;
		if (in_words(w, c_ty)) return 2;
		break;
	}
	return 0;
}

static gboolean is_op_char(char c)
{
	return c=='+'||c=='-'||c=='*'||c=='/'||c=='='||c=='!'||c=='&'||c=='|'||c=='^';
}
static gboolean is_punct_char(char c)
{
	return c=='{'||c=='}'||c=='['||c==']'||c=='('||c==')'||c=='.'||c==','||
	       c==';'||c==':'||c=='<'||c=='>'||c=='?'||c=='~'||c=='%'||c=='@'||c=='\\';
}

/* Append a coloured span of [p,p+len) (HTML-escaped) to out, over a code
 * panel background. Returns the number of display columns emitted (tabs = 4,
 * every other byte-run of one glyph = 1) so the caller can pad the line. */
static int
span(GString *out, const char *color, gboolean italic, const char *p, gsize len)
{
	char *tmp = g_strndup(p, len);
	char *esc = g_markup_escape_text(tmp, -1);
	const char *q;
	int cols = 0;
	g_string_append_printf(out,
	    "<font face=\"monospace\" back=\"" SC_CODEBG "\" color=\"%s\">%s",
	    color, italic ? "<i>" : "");
	/* preserve spaces/tabs as nbsp so indentation survives IMHtml */
	for (q = esc; *q; q++) {
		if (*q == ' ')       { g_string_append(out, "&#160;"); cols++; }
		else if (*q == '\t') { g_string_append(out, "&#160;&#160;&#160;&#160;"); cols += 4; }
		else if (*q == '&') { /* an entity like &lt; &#160; -- one glyph */
			const char *semi = strchr(q, ';');
			if (semi) { g_string_append_len(out, q, semi - q + 1); q = semi; cols++; }
			else { g_string_append_c(out, *q); cols++; }
		}
		else if (((unsigned char)*q & 0xC0) == 0x80) { g_string_append_c(out, *q); }
		else                 { g_string_append_c(out, *q); cols++; }
	}
	g_string_append_printf(out, "%s</font>", italic ? "</i>" : "");
	g_free(esc); g_free(tmp);
	return cols;
}

/* Tokenise one physical line [p,p+len) into coloured HTML spans over the code
 * panel background. Returns the display width (columns) emitted. */
static int
highlight_line(GString *out, const char *p, gsize len, AcpLang lang,
               AcpComment cm, gboolean *in_block)
{
	gsize i = 0;
	int cols = 0;

	/* continuing a multi-line block comment */
	if (*in_block) {
		gsize j = 0;
		if (cm.block_close) {
			const char *hit = g_strstr_len(p, len, cm.block_close);
			if (hit) {
				j = (gsize)(hit - p) + strlen(cm.block_close);
				*in_block = FALSE;
			} else {
				j = len;
			}
		} else {
			j = len;
		}
		cols += span(out, SC_COMMENT, TRUE, p, j);
		i = j;
	}

	while (i < len) {
		char ch = p[i];

		/* whitespace run */
		if (ch == ' ' || ch == '\t') {
			gsize j = i;
			while (j < len && (p[j] == ' ' || p[j] == '\t')) j++;
			cols += span(out, SC_PLAIN, FALSE, p + i, j - i);
			i = j;
			continue;
		}
		/* preprocessor (#... at line start, C/C++) */
		if ((lang == L_C || lang == L_CPP) && ch == '#' && i == 0) {
			cols += span(out, SC_PREPROC, FALSE, p + i, len - i);
			i = len;
			continue;
		}
		/* line comment */
		if (cm.line && !strncmp(p + i, cm.line, strlen(cm.line))) {
			cols += span(out, SC_COMMENT, TRUE, p + i, len - i);
			i = len;
			continue;
		}
		if (cm.hash && ch == '#') {
			cols += span(out, SC_COMMENT, TRUE, p + i, len - i);
			i = len;
			continue;
		}
		/* block comment open */
		if (cm.block_open && !strncmp(p + i, cm.block_open, strlen(cm.block_open))) {
			const char *rest = p + i + strlen(cm.block_open);
			gsize restlen = len - i - strlen(cm.block_open);
			const char *hit = cm.block_close
			    ? g_strstr_len(rest, restlen, cm.block_close) : NULL;
			gsize j;
			if (hit) {
				j = (gsize)(hit - p) + strlen(cm.block_close);
			} else {
				j = len;
				*in_block = TRUE;
			}
			cols += span(out, SC_COMMENT, TRUE, p + i, j - i);
			i = j;
			continue;
		}
		/* strings: " ' ` */
		if (ch == '"' || ch == '\'' || (cm.triple && ch == '`')) {
			char q = ch;
			gsize j = i + 1;
			while (j < len && p[j] != q) {
				if (p[j] == '\\' && j + 1 < len) j++;
				j++;
			}
			if (j < len) j++;   /* include closing quote */
			cols += span(out, SC_STR, FALSE, p + i, j - i);
			i = j;
			continue;
		}
		/* shell variable $VAR / ${VAR} */
		if (lang == L_SHELL && ch == '$' && i + 1 < len) {
			gsize j = i + 1;
			if (p[j] == '{') { while (j < len && p[j] != '}') j++; if (j < len) j++; }
			else while (j < len && (isalnum((unsigned char)p[j]) || p[j]=='_')) j++;
			cols += span(out, SC_SHVAR, FALSE, p + i, j - i);
			i = j;
			continue;
		}
		/* decorator / attribute @name */
		if (ch == '@' && i + 1 < len && isalpha((unsigned char)p[i+1])) {
			gsize j = i + 1;
			while (j < len && (isalnum((unsigned char)p[j]) || p[j]=='_' || p[j]=='.')) j++;
			cols += span(out, SC_ATTR, FALSE, p + i, j - i);
			i = j;
			continue;
		}
		/* number */
		if (isdigit((unsigned char)ch) ||
		    (ch == '.' && i + 1 < len && isdigit((unsigned char)p[i+1]))) {
			gsize j = i;
			if (ch == '0' && i + 1 < len && (p[i+1]=='x'||p[i+1]=='X'||
			    p[i+1]=='b'||p[i+1]=='o')) {
				j += 2;
				while (j < len && (isalnum((unsigned char)p[j]) || p[j]=='_')) j++;
			} else {
				while (j < len && (isdigit((unsigned char)p[j]) || p[j]=='.' ||
				       p[j]=='_' || p[j]=='e' || p[j]=='E' ||
				       ((p[j]=='+'||p[j]=='-') && j>i && (p[j-1]=='e'||p[j-1]=='E'))))
					j++;
				while (j < len && (isalpha((unsigned char)p[j]) || p[j]=='_')) j++; /* suffix */
			}
			cols += span(out, SC_NUM, FALSE, p + i, j - i);
			i = j;
			continue;
		}
		/* identifier / keyword / type / function */
		if (isalpha((unsigned char)ch) || ch == '_') {
			gsize j = i;
			char *word;
			int cls;
			gboolean is_call, is_type_case;
			while (j < len && (isalnum((unsigned char)p[j]) || p[j]=='_')) j++;
			if (lang == L_RUST && j < len && p[j] == '!') j++;   /* macro! */
			word = g_strndup(p + i, j - i);
			is_call = (j < len && p[j] == '(');
			is_type_case = (isupper((unsigned char)word[0]) && (j - i) > 1);
			cls = classify_word(word, lang);
			if      (cls == 3)      cols += span(out, SC_CONST, FALSE, p + i, j - i);
			else if (cls == 1)      cols += span(out, SC_KW,    FALSE, p + i, j - i);
			else if (cls == 2)      cols += span(out, SC_TYPE,  FALSE, p + i, j - i);
			else if (is_call)       cols += span(out, SC_FN,    FALSE, p + i, j - i);
			else if (is_type_case)  cols += span(out, SC_TYPE,  FALSE, p + i, j - i);
			else                    cols += span(out, SC_PLAIN, FALSE, p + i, j - i);
			g_free(word);
			i = j;
			continue;
		}
		/* multi-char operator (max 3) */
		if (is_op_char(ch)) {
			gsize j = i;
			while (j < len && is_op_char(p[j]) && (j - i) < 3) j++;
			cols += span(out, SC_OP, FALSE, p + i, j - i);
			i = j;
			continue;
		}
		/* punctuation */
		if (is_punct_char(ch)) {
			cols += span(out, SC_PUNCT, FALSE, p + i, 1);
			i++;
			continue;
		}
		/* anything else -- consume one byte (or a UTF-8 sequence) plain */
		{
			gsize j = i + 1;
			if ((unsigned char)ch >= 0x80)
				while (j < len && ((unsigned char)p[j] & 0xC0) == 0x80) j++;
			cols += span(out, SC_PLAIN, FALSE, p + i, j - i);
			i = j;
		}
	}
	return cols;
}

/* Public: produce an HTML fragment for a code block body as a framed,
 * background-filled panel: a top rule (╭─ lang ───╮), one row per source line
 * (line-number gutter + syntax-highlighted code, padded with the panel
 * background to a uniform width), and a bottom rule (╰───╯). Caller frees.
 * `lang_tag` may be NULL / empty (generic C-style highlighting). */
char *
acp_highlight_code(const char *code, const char *lang_tag)
{
	AcpLang lang = detect_lang(lang_tag);
	AcpComment cm = comment_for(lang);
	gboolean in_block = FALSE;
	GString *out = g_string_new(NULL);
	gchar **lines;
	int i, nlines = 0, wdig = 3, gutter_w, content_w = 0, panel_w;
	GString **rows;
	int *rowcols;

	if (!code) code = "";
	lines = g_strsplit(code, "\n", -1);

	/* count real lines (drop a trailing empty element from a final '\n') */
	for (i = 0; lines[i]; i++) {
		if (lines[i + 1] == NULL && lines[i][0] == '\0') break;
		nlines++;
	}
	if (nlines == 0) nlines = 1;   /* always draw at least one (blank) row */
	{ int v = nlines, need = 1; for (; v >= 10; v /= 10) need++;
	  if (need > wdig) wdig = need; }   /* stable 3-digit floor */
	gutter_w = wdig + 3;   /* digits + " │ " (space + bar + space) */

	/* First pass: render each line's code spans into its own buffer and note
	 * its display width, so the panel can pad every row to one uniform edge. */
	rows    = g_new0(GString *, nlines);
	rowcols = g_new0(int, nlines);
	for (i = 0; i < nlines; i++) {
		const char *ln = lines[i] ? lines[i] : "";
		rows[i] = g_string_new(NULL);
		rowcols[i] = highlight_line(rows[i], ln, strlen(ln), lang, cm, &in_block);
		if (rowcols[i] > content_w) content_w = rowcols[i];
	}
	if (content_w < 8)  content_w = 8;    /* minimum panel body width  */
	if (content_w > 96) content_w = 96;   /* cap very wide lines        */
	panel_w = gutter_w + content_w;

	/* Top rule: ╭─ lang ─────╮ (lang chip inline in the border). */
	{
		int dashes, k;
		char *le = (lang_tag && *lang_tag)
		    ? g_markup_escape_text(lang_tag, -1) : NULL;
		int lang_cols = le ? (int)g_utf8_strlen(lang_tag, -1) + 2 : 0; /* " x " */
		g_string_append(out,
		    "<font face=\"monospace\" back=\"" SC_CODEBG "\" color=\"" SC_BORDER
		    "\">\xE2\x95\xAD\xE2\x94\x80");   /* ╭─ */
		if (le) {
			g_string_append_printf(out,
			    "&#160;<font color=\"" SC_GUTTER "\">%s</font>&#160;", le);
		}
		dashes = panel_w - 2 - lang_cols;
		if (dashes < 1) dashes = 1;
		for (k = 0; k < dashes; k++) g_string_append(out, "\xE2\x94\x80"); /* ─ */
		g_string_append(out, "\xE2\x95\xAE</font><br>");   /* ╮ */
		g_free(le);
	}

	/* Body rows. */
	for (i = 0; i < nlines; i++) {
		int pad = content_w - rowcols[i], k;
		/* left border + gutter */
		g_string_append_printf(out,
		    "<font face=\"monospace\" back=\"" SC_CODEBG "\" color=\"" SC_BORDER
		    "\">\xE2\x94\x82</font>"   /* │ left border */
		    "<font face=\"monospace\" back=\"" SC_CODEBG "\" color=\"" SC_GUTTER
		    "\">%*d&#160;\xE2\x94\x82&#160;</font>",   /* NNN │ */
		    wdig, i + 1);
		/* code content */
		g_string_append(out, rows[i]->str);
		/* pad to uniform width with background fill */
		if (pad > 0) {
			g_string_append(out,
			    "<font face=\"monospace\" back=\"" SC_CODEBG "\" color=\""
			    SC_CODEBG "\">");
			for (k = 0; k < pad; k++) g_string_append(out, "&#160;");
			g_string_append(out, "</font>");
		}
		g_string_append(out, "<br>");
		g_string_free(rows[i], TRUE);
	}

	/* Bottom rule: ╰─────╯ */
	{
		int k;
		g_string_append(out,
		    "<font face=\"monospace\" back=\"" SC_CODEBG "\" color=\"" SC_BORDER
		    "\">\xE2\x95\xB0");   /* ╰ */
		for (k = 0; k < panel_w - 1; k++) g_string_append(out, "\xE2\x94\x80");
		g_string_append(out, "\xE2\x95\xAF</font>");   /* ╯ */
	}

	g_free(rows);
	g_free(rowcols);
	g_strfreev(lines);
	return g_string_free(out, FALSE);
}

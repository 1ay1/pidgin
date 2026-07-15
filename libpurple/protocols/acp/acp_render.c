/*
 * ACP protocol plugin -- inline Markdown + tool-call / plan cards.
 *
 * The live, in-place streaming Markdown renderer lives in acp_stream.c (it
 * drives the GtkIMHtml widget directly so text can be revised as it streams).
 * This file keeps the pieces that do not need in-place revision:
 *   - acp_md_inline(): inline markdown (bold/italic/code/links/strike),
 *     shared by the stream renderer and the cards below;
 *   - tool-call cards and the plan checklist, rendered as cohesive HTML and
 *     handed to acp_stream_write_card() so they interleave with streamed text.
 *
 * Copyright (C) 2024 Ayush Bhat <tfeayush@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 (or, at your
 * option, any later version).
 */
#include "acp.h"

#include "debug.h"
#include "server.h"

#include <ctype.h>
#include <string.h>

/* Colours for the agent chrome + inline markdown (mapped from 1ay1/maya). */
#define COL_TOOL   "#fcaf3e"   /* orange (tool)        */
#define COL_PLAN   "#ad7fa8"   /* plum (plan)          */
#define COL_DIM    "#888a85"   /* bright_black         */
#define COL_CODE   "#34e2e2"   /* bright_cyan (code)   */
#define COL_LINK   "#729fcf"   /* bright_blue (link)   */
#define COL_BOLD   "#eeeeec"   /* bright_white (bold)  */
#define COL_ADD    "#8ae234"   /* bright_green         */
#define COL_DEL    "#ef2929"   /* bright_red           */

/* ------------------------------------------------------------------------- *
 *  Conversation plumbing (message-API fallback, used by headless frontends)
 * ------------------------------------------------------------------------- */

static PurpleConversation *
acp_get_conv(AcpData *d)
{
	PurpleAccount *acct = purple_connection_get_account(d->gc);
	PurpleConversation *conv;

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
	                                              d->buddy, acct);
	if (!conv)
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, acct, d->buddy);
	return conv;
}

void
acp_conv_write_html(AcpData *d, const char *html, PurpleMessageFlags extra)
{
	PurpleConversation *conv = acp_get_conv(d);
	PurpleConvIm *im = purple_conversation_get_im_data(conv);

	purple_conv_im_write(im, d->buddy, html,
	                     PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_RAW | extra,
	                     time(NULL));
}

/* ------------------------------------------------------------------------- *
 *  Inline markdown: bold, italic, inline code, links, strikethrough
 * ------------------------------------------------------------------------- */

static gboolean
scan_delim(const char *s, const char *delim, gsize *span)
{
	gsize dl = strlen(delim);
	if (strncmp(s, delim, dl) != 0)
		return FALSE;
	const char *end = strstr(s + dl, delim);
	if (!end || end == s + dl)   /* no close, or empty */
		return FALSE;
	*span = (end - (s + dl));
	return TRUE;
}

/* Escape a raw substring [p, p+len) into g_string. */
static void
append_escaped_n(GString *out, const char *p, gsize len)
{
	char *tmp = g_strndup(p, len);
	char *esc = g_markup_escape_text(tmp, -1);
	g_string_append(out, esc);
	g_free(esc);
	g_free(tmp);
}

char *
acp_md_inline(const char *text)
{
	GString *out = g_string_new(NULL);
	const char *p = text;

	while (*p) {
		gsize span;

		/* inline code `...` -- monospace, distinct colour (no fragile bg) */
		if (*p == '`') {
			const char *end = strchr(p + 1, '`');
			if (end && end > p + 1) {
				g_string_append(out,
				    "<font face=\"monospace\" color=\"" COL_CODE "\">");
				append_escaped_n(out, p + 1, end - (p + 1));
				g_string_append(out, "</font>");
				p = end + 1;
				continue;
			}
		}
		/* links [text](url) */
		if (*p == '[') {
			const char *rb = strchr(p, ']');
			if (rb && rb[1] == '(') {
				const char *rp = strchr(rb + 2, ')');
				if (rp) {
					char *label = g_strndup(p + 1, rb - (p + 1));
					char *url   = g_strndup(rb + 2, rp - (rb + 2));
					char *lesc  = g_markup_escape_text(label, -1);
					char *uesc  = g_markup_escape_text(url, -1);
					g_string_append_printf(out,
					    "<a href=\"%s\"><font color=\"" COL_LINK
					    "\"><u>%s</u></font></a>",
					                       uesc, lesc);
					g_free(label); g_free(url); g_free(lesc); g_free(uesc);
					p = rp + 1;
					continue;
				}
			}
		}
		/* bold **...** */
		if (scan_delim(p, "**", &span)) {
			char *inner = g_strndup(p + 2, span);
			char *r = acp_md_inline(inner);
			g_string_append_printf(out,
			    "<font color=\"" COL_BOLD "\"><b>%s</b></font>", r);
			g_free(inner); g_free(r);
			p += 2 + span + 2;
			continue;
		}
		/* strikethrough ~~...~~ */
		if (scan_delim(p, "~~", &span)) {
			char *inner = g_strndup(p + 2, span);
			char *r = acp_md_inline(inner);
			g_string_append_printf(out, "<s>%s</s>", r);
			g_free(inner); g_free(r);
			p += 2 + span + 2;
			continue;
		}
		/* italic *...* or _..._ (single char, not part of **) */
		if ((*p == '*' || *p == '_') && p[1] && p[1] != *p) {
			char d0[2] = { *p, 0 };
			if (scan_delim(p, d0, &span)) {
				char *inner = g_strndup(p + 1, span);
				char *r = acp_md_inline(inner);
				g_string_append_printf(out, "<i>%s</i>", r);
				g_free(inner); g_free(r);
				p += 1 + span + 1;
				continue;
			}
		}
		/* plain char */
		append_escaped_n(out, p, 1);
		p++;
	}
	return g_string_free(out, FALSE);
}

/* Display width (in glyph columns) of a cell string AFTER inline markdown is
 * applied -- i.e. with `code` backticks, bold/italic/strike emphasis markers
 * and [label](url) link syntax removed, counting only what will actually be
 * drawn. Table column widths and alignment slack MUST use this, not the raw
 * byte/char length, or cells containing inline markup misalign the borders. */
static int
md_visible_width(const char *text)
{
	const char *p = text;
	int cols = 0;

	while (*p) {
		gsize span;
		/* inline code `...` -> inner text only */
		if (*p == '`') {
			const char *end = strchr(p + 1, '`');
			if (end && end > p + 1) {
				cols += (int)g_utf8_strlen(p + 1, end - (p + 1));
				p = end + 1;
				continue;
			}
		}
		/* link [label](url) -> label only */
		if (*p == '[') {
			const char *rb = strchr(p, ']');
			if (rb && rb[1] == '(') {
				const char *rp = strchr(rb + 2, ')');
				if (rp) {
					cols += (int)g_utf8_strlen(p + 1, rb - (p + 1));
					p = rp + 1;
					continue;
				}
			}
		}
		/* bold ** / strike ~~ -> drop the 2-char delimiters */
		if (scan_delim(p, "**", &span)) { char *in = g_strndup(p + 2, span);
			cols += md_visible_width(in); g_free(in); p += 2 + span + 2; continue; }
		if (scan_delim(p, "~~", &span)) { char *in = g_strndup(p + 2, span);
			cols += md_visible_width(in); g_free(in); p += 2 + span + 2; continue; }
		/* italic * or _ */
		if ((*p == '*' || *p == '_') && p[1] && p[1] != *p) {
			char d0[2] = { *p, 0 };
			if (scan_delim(p, d0, &span)) {
				char *in = g_strndup(p + 1, span);
				cols += md_visible_width(in); g_free(in);
				p += 1 + span + 1; continue;
			}
		}
		/* plain glyph (advance one UTF-8 char) */
		p = g_utf8_next_char(p);
		cols++;
	}
	return cols;
}

/* ------------------------------------------------------------------------- *
 *  GFM tables (maya-style box-drawn, alignment-aware)
 * ------------------------------------------------------------------------- */

#define TB_BORDER  "#3a3a3a"   /* table frame (matches code panel)   */
#define TB_HEAD    "#34e2e2"   /* header text (bright_cyan)          */
#define TB_BG      "#181818"   /* table background fill              */

enum { TB_LEFT = 0, TB_CENTER, TB_RIGHT };

/* Split a "| a | b | c |" row into trimmed cell strings. Leading/trailing
 * pipes are optional (GFM); an escaped \| becomes a literal pipe. Returns a
 * NULL-terminated array (g_strfreev) and writes the count to *ncols. */
static char **
table_split_row(const char *line, int *ncols)
{
	GPtrArray *cells = g_ptr_array_new();
	GString *cur = g_string_new(NULL);
	const char *p = line;

	while (*p == ' ') p++;
	if (*p == '|') p++;                 /* optional leading pipe */

	for (; *p; p++) {
		if (*p == '\\' && p[1] == '|') { g_string_append_c(cur, '|'); p++; continue; }
		if (*p == '|') {
			g_ptr_array_add(cells, g_strstrip(g_strdup(cur->str)));
			g_string_truncate(cur, 0);
			continue;
		}
		g_string_append_c(cur, *p);
	}
	/* trailing cell only if non-empty (a trailing pipe yields an empty tail) */
	{
		char *t = g_strstrip(g_strdup(cur->str));
		if (*t) g_ptr_array_add(cells, t);
		else    g_free(t);
	}
	g_string_free(cur, TRUE);
	g_ptr_array_add(cells, NULL);
	if (ncols) *ncols = (int)cells->len - 1;
	return (char **)g_ptr_array_free(cells, FALSE);
}

/* Is `line` a GFM alignment separator?  |---|:--:|---:| -- every cell of the
 * form (:?)(-+)(:?) with at least one dash. */
gboolean
acp_table_is_separator(const char *line)
{
	int n = 0, i;
	char **cells = table_split_row(line, &n);
	gboolean ok = (n > 0);
	for (i = 0; i < n && ok; i++) {
		const char *c = cells[i];
		int dashes = 0;
		while (*c == ' ') c++;
		if (*c == ':') c++;
		while (*c == '-') { dashes++; c++; }
		if (*c == ':') c++;
		while (*c == ' ') c++;
		if (dashes == 0 || *c != '\0') ok = FALSE;
	}
	g_strfreev(cells);
	return ok;
}

/* Parse the separator row into per-column alignment codes. */
static int *
table_parse_align(const char *sep, int ncols)
{
	int *al = g_new0(int, ncols > 0 ? ncols : 1);
	int n = 0, i;
	char **cells = table_split_row(sep, &n);
	for (i = 0; i < ncols; i++) {
		gboolean l = FALSE, r = FALSE;
		if (i < n) {
			const char *c = cells[i];
			while (*c == ' ') c++;
			if (*c == ':') l = TRUE;
			if (*c) { const char *e = c + strlen(c); while (e > c && e[-1] == ' ') e--;
			          if (e > c && e[-1] == ':') r = TRUE; }
		}
		al[i] = (l && r) ? TB_CENTER : r ? TB_RIGHT : TB_LEFT;
	}
	g_strfreev(cells);
	return al;
}

/* Emit a horizontal rule line. kind: 0=top ┌┬┐, 1=mid ├┼┤, 2=bottom └┴┘. */
static void
table_border(GString *out, const int *w, int ncols, int kind)
{
	static const char *L[] = { "\xE2\x94\x8C", "\xE2\x94\x9C", "\xE2\x94\x94" };
	static const char *M[] = { "\xE2\x94\xAC", "\xE2\x94\xBC", "\xE2\x94\xB4" };
	static const char *R[] = { "\xE2\x94\x90", "\xE2\x94\xA4", "\xE2\x94\x98" };
	int c, k;
	g_string_append(out,
	    "<font face=\"monospace\" back=\"" TB_BG "\" color=\"" TB_BORDER "\">");
	g_string_append(out, L[kind]);
	for (c = 0; c < ncols; c++) {
		if (c > 0) g_string_append(out, M[kind]);
		for (k = 0; k < w[c] + 2; k++) g_string_append(out, "\xE2\x94\x80");
	}
	g_string_append(out, R[kind]);
	g_string_append(out, "</font><br>");
}

/* Emit one data/header row. `is_head` bolds + colours the cell text. */
static void
table_row(GString *out, char **cells, int ncols, const int *w, const int *al,
          gboolean is_head)
{
	int c;
	g_string_append(out,
	    "<font face=\"monospace\" back=\"" TB_BG "\" color=\"" TB_BORDER
	    "\">\xE2\x94\x82</font>");   /* leading | */
	for (c = 0; c < ncols; c++) {
		const char *raw = (c < ncols && cells[c]) ? cells[c] : "";
		int cw = md_visible_width(raw);
		int slack = w[c] - cw; if (slack < 0) slack = 0;
		int lp = 0, rp = slack, k;
		char *inner;
		if (al[c] == TB_RIGHT)  { lp = slack; rp = 0; }
		else if (al[c] == TB_CENTER) { lp = slack / 2; rp = slack - lp; }
		/* fixed 1-space pad + left slack, over the table bg */
		g_string_append(out, "<font face=\"monospace\" back=\"" TB_BG "\">");
		for (k = 0; k < lp + 1; k++) g_string_append(out, "&#160;");
		g_string_append(out, "</font>");
		/* content */
		inner = acp_md_inline(raw);
		if (is_head)
			g_string_append_printf(out,
			    "<font face=\"monospace\" back=\"" TB_BG "\" color=\"" TB_HEAD
			    "\"><b>%s</b></font>", inner);
		else
			g_string_append_printf(out,
			    "<font face=\"monospace\" back=\"" TB_BG "\">%s</font>", inner);
		g_free(inner);
		/* right slack + fixed pad, then trailing border */
		g_string_append(out, "<font face=\"monospace\" back=\"" TB_BG "\">");
		for (k = 0; k < rp + 1; k++) g_string_append(out, "&#160;");
		g_string_append(out, "</font>");
		g_string_append(out,
		    "<font face=\"monospace\" back=\"" TB_BG "\" color=\"" TB_BORDER
		    "\">\xE2\x94\x82</font>");   /* | */
	}
	g_string_append(out, "<br>");
}

/* Render a whole GFM table (raw lines: header, separator, then data rows) as a
 * framed, alignment-aware HTML fragment. Caller frees. `lines` is a NULL-
 * terminated array; `nlines` >= 2 (header + separator). Matches maya. */
char *
acp_render_table(char **lines, int nlines)
{
	GString *out = g_string_new(NULL);
	int ncols = 0, i, c;
	char **head;
	int *w, *al;
	GPtrArray *body = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);

	if (nlines < 2) { return g_string_free(out, FALSE); }

	head = table_split_row(lines[0], &ncols);
	if (ncols == 0) { g_strfreev(head); g_ptr_array_free(body, TRUE);
	                  return g_string_free(out, FALSE); }
	al = table_parse_align(lines[1], ncols);
	w  = g_new0(int, ncols);

	/* header widths */
	for (c = 0; c < ncols; c++)
		if (head[c]) { int hw = md_visible_width(head[c]);
		               if (hw > w[c]) w[c] = hw; }

	/* body rows (from line 2 on) + widen columns */
	for (i = 2; i < nlines && lines[i]; i++) {
		int rn = 0;
		char **row = table_split_row(lines[i], &rn);
		for (c = 0; c < ncols; c++) {
			const char *v = (c < rn && row[c]) ? row[c] : "";
			int vw = md_visible_width(v);
			if (vw > w[c]) w[c] = vw;
		}
		g_ptr_array_add(body, row);
	}
	for (c = 0; c < ncols; c++) if (w[c] < 1) w[c] = 1;

	/* compose: top rule, header, mid rule, body rows (dashed separators). */
	table_border(out, w, ncols, 0);
	table_row(out, head, ncols, w, al, TRUE);
	table_border(out, w, ncols, 1);
	for (i = 0; i < (int)body->len; i++) {
		char **row = g_ptr_array_index(body, i);
		int rn = 0; while (row[rn]) rn++;
		/* pad short rows so table_row always has ncols entries */
		table_row(out, row, ncols, w, al, FALSE);
	}
	table_border(out, w, ncols, 2);

	g_free(w); g_free(al);
	g_strfreev(head);
	g_ptr_array_free(body, TRUE);
	return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------------- *
 *  Tool-call cards
 * ------------------------------------------------------------------------- */

/* A little glyph per tool kind. */
static const char *
kind_glyph(const char *kind)
{
	if (!kind)                              return "\xE2\x9A\x99";       /* gear */
	if (strstr(kind, "read"))               return "\xF0\x9F\x93\x96";  /* book */
	if (strstr(kind, "edit"))               return "\xE2\x9C\x8F";      /* pencil */
	if (strstr(kind, "delete"))             return "\xF0\x9F\x97\x91";  /* trash */
	if (strstr(kind, "move"))               return "\xF0\x9F\x93\xA6";  /* box */
	if (strstr(kind, "search"))             return "\xF0\x9F\x94\x8D";  /* magnifier */
	if (strstr(kind, "execute"))            return "\xE2\x96\xB6";      /* play */
	if (strstr(kind, "fetch"))              return "\xF0\x9F\x8C\x90";  /* globe */
	if (strstr(kind, "think"))              return "\xF0\x9F\x92\xAD";  /* thought */
	return "\xE2\x9A\x99";
}

static const char *
status_color(const char *status)
{
	if (!status)                                return COL_TOOL;
	if (purple_strequal(status, "completed"))   return COL_ADD;
	if (purple_strequal(status, "failed"))      return COL_DEL;
	return COL_TOOL;
}

/* Append rendered tool content items (text, diffs) to an HTML buffer. */
static void
append_tool_content(GString *html, JsonArray *content)
{
	guint i, n = content ? json_array_get_length(content) : 0;

	for (i = 0; i < n; i++) {
		JsonObject *item = json_array_get_object_element(content, i);
		const char *type = item && json_object_has_member(item, "type")
		                 ? json_object_get_string_member(item, "type") : "";

		if (purple_strequal(type, "diff")) {
			const char *path = json_object_has_member(item, "path")
			                 ? json_object_get_string_member(item, "path") : "";
			const char *oldt = json_object_has_member(item, "oldText") &&
			    !json_object_get_null_member(item, "oldText")
			  ? json_object_get_string_member(item, "oldText") : NULL;
			const char *newt = json_object_has_member(item, "newText")
			                 ? json_object_get_string_member(item, "newText") : "";
			char *pe = g_markup_escape_text(path, -1);
			g_string_append_printf(html,
			    "<font color=\"" COL_DIM "\" size=\"2\">&#160;&#160;%s</font><br>", pe);
			g_free(pe);

			if (oldt && *oldt) {
				gchar **ol = g_strsplit(oldt, "\n", -1);
				int k;
				for (k = 0; ol[k]; k++) {
					char *e = g_markup_escape_text(ol[k], -1);
					g_string_append_printf(html,
					    "<font color=\"" COL_DEL "\" face=\"monospace\" size=\"2\">"
					    "&#160;- %s</font><br>", e);
					g_free(e);
				}
				g_strfreev(ol);
			}
			if (newt && *newt) {
				gchar **nl = g_strsplit(newt, "\n", -1);
				int k;
				for (k = 0; nl[k]; k++) {
					char *e = g_markup_escape_text(nl[k], -1);
					g_string_append_printf(html,
					    "<font color=\"" COL_ADD "\" face=\"monospace\" size=\"2\">"
					    "&#160;+ %s</font><br>", e);
					g_free(e);
				}
				g_strfreev(nl);
			}

		} else if (purple_strequal(type, "content")) {
			JsonObject *c = json_object_has_member(item, "content")
			              ? json_object_get_object_member(item, "content") : NULL;
			const char *t = c && json_object_has_member(c, "text")
			              ? json_object_get_string_member(c, "text") : NULL;
			if (t && *t) {
				char *e = g_markup_escape_text(t, -1);
				const char *p;
				g_string_append(html,
				    "<font face=\"monospace\" size=\"2\" color=\"" COL_DIM "\">");
				for (p = e; *p; p++) {
					if (*p == '\n')      g_string_append(html, "<br>");
					else if (*p == ' ')  g_string_append(html, "&#160;");
					else                 g_string_append_c(html, *p);
				}
				g_string_append(html, "</font><br>");
				g_free(e);
			}
		}
	}
}

void
acp_render_tool_call(AcpData *d, JsonObject *update, gboolean is_update)
{
	const char *id = json_object_has_member(update, "toolCallId")
	               ? json_object_get_string_member(update, "toolCallId") : NULL;
	const char *title = json_object_has_member(update, "title")
	                  ? json_object_get_string_member(update, "title") : NULL;
	const char *kind = json_object_has_member(update, "kind")
	                 ? json_object_get_string_member(update, "kind") : NULL;
	const char *status = json_object_has_member(update, "status")
	                   ? json_object_get_string_member(update, "status") : NULL;
	JsonArray *content = json_object_has_member(update, "content")
	                   ? json_object_get_array_member(update, "content") : NULL;
	AcpToolCall *tc = NULL;
	GString *html;

	if (id)
		tc = g_hash_table_lookup(d->tools, id);

	if (!tc && id) {
		tc = g_new0(AcpToolCall, 1);
		tc->id = g_strdup(id);
		g_hash_table_insert(d->tools, g_strdup(id), tc);
	}
	if (tc) {
		if (title)  { g_free(tc->title);  tc->title  = g_strdup(title); }
		if (kind)   { g_free(tc->kind);   tc->kind   = g_strdup(kind); }
		if (status) { g_free(tc->status); tc->status = g_strdup(status); }
	}

	/* On a bare tool_call_update that only changes status (no title/content),
	 * only emit a compact status line for terminal states. */
	if (is_update && !title && !content) {
		if (status && (purple_strequal(status, "completed") ||
		               purple_strequal(status, "failed"))) {
			const char *t = tc && tc->title ? tc->title : "tool";
			char *te = g_markup_escape_text(t, -1);
			char *m = g_strdup_printf(
			    "<font color=\"%s\">%s %s &#8212; %s</font><br>",
			    status_color(status),
			    kind_glyph(tc ? tc->kind : NULL), te, status);
			acp_stream_write_card(d, m);
			g_free(m); g_free(te);
		}
		return;
	}

	/* Header line + content for the tool call, as one cohesive card. */
	html = g_string_new(NULL);
	{
		const char *t = title ? title : (tc && tc->title ? tc->title : "tool");
		const char *k = kind ? kind : (tc ? tc->kind : NULL);
		const char *s = status ? status : (tc ? tc->status : NULL);
		char *te = g_markup_escape_text(t, -1);
		g_string_append_printf(html,
		    "<font color=\"%s\"><b>%s %s</b>%s%s%s</font><br>",
		    status_color(s), kind_glyph(k), te,
		    (s && *s) ? " <font color=\"" COL_DIM "\" size=\"2\">(" : "",
		    (s && *s) ? s : "",
		    (s && *s) ? ")</font>" : "");
		g_free(te);
	}
	if (content)
		append_tool_content(html, content);

	acp_stream_write_card(d, html->str);
	g_string_free(html, TRUE);
}

/* ------------------------------------------------------------------------- *
 *  Plan checklist
 * ------------------------------------------------------------------------- */

void
acp_render_plan(AcpData *d, JsonArray *entries)
{
	guint i, n = entries ? json_array_get_length(entries) : 0;
	GString *html;

	if (n == 0)
		return;

	html = g_string_new(NULL);
	g_string_append(html,
	    "<font color=\"" COL_PLAN "\"><b>Plan</b></font><br>");

	for (i = 0; i < n; i++) {
		JsonObject *e = json_array_get_object_element(entries, i);
		const char *content = e && json_object_has_member(e, "content")
		                    ? json_object_get_string_member(e, "content") : "";
		const char *status = e && json_object_has_member(e, "status")
		                   ? json_object_get_string_member(e, "status") : "";
		const char *box;
		char *inner;

		if (purple_strequal(status, "completed"))
			box = "\xE2\x9C\x94";       /* check */
		else if (purple_strequal(status, "in_progress"))
			box = "\xE2\x96\xB6";       /* play */
		else
			box = "\xE2\x97\xAB";       /* white square */

		inner = acp_md_inline(content);
		g_string_append_printf(html,
		    "&#160;&#160;<font color=\"%s\">%s</font> %s<br>",
		    purple_strequal(status, "completed") ? COL_ADD : COL_DIM,
		    box, inner);
		g_free(inner);
	}
	acp_stream_write_card(d, html->str);
	g_string_free(html, TRUE);
}

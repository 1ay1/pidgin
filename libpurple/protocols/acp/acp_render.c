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
#define ACP_FG_C   "#d4d4d4"   /* body text (card header) */

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
		/* <kbd>key</kbd> -> a boxed key cap; also swallow other simple inline
		 * HTML tags we don't support so raw "<tag>" never shows through. */
		if (*p == '<') {
			if (g_ascii_strncasecmp(p, "<kbd>", 5) == 0) {
				const char *end = strstr(p + 5, "</kbd>");
				if (end) {
					char *key = g_strndup(p + 5, end - (p + 5));
					char *ke = g_markup_escape_text(key, -1);
					g_string_append_printf(out,
					    "<font face=\"monospace\" color=\"" COL_BOLD
					    "\">\xE2\x8C\xA8%s</font>", ke);   /* ⌨ key */
					g_free(key); g_free(ke);
					p = end + 6;
					continue;
				}
			}
		}
		/* bold+italic ***...*** (must precede ** so the triple isn't split) */
		if (scan_delim(p, "***", &span)) {
			char *inner = g_strndup(p + 3, span);
			char *r = acp_md_inline(inner);
			g_string_append_printf(out,
			    "<font color=\"" COL_BOLD "\"><b><i>%s</i></b></font>", r);
			g_free(inner); g_free(r);
			p += 3 + span + 3;
			continue;
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
		if (scan_delim(p, "***", &span)) { char *in = g_strndup(p + 3, span);
			cols += md_visible_width(in); g_free(in); p += 3 + span + 3; continue; }
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

/* Is `line` a GFM alignment separator?  |---|:--:|---:| -- per CommonMark/GFM
 * (matching maya's looks_like_table_delim): after trimming it must be non-empty,
 * contain at least one '-', and consist ONLY of '-', '|', ':', space and tab.
 * This is stricter (and safer) than parsing cells, so a stray "---" hrule or
 * ":::" never masquerades as a table separator. */
gboolean
acp_table_is_separator(const char *line)
{
	const char *p = line;
	const char *end;
	gboolean seen_dash = FALSE;

	while (*p == ' ' || *p == '\t') p++;
	end = p + strlen(p);
	while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
	if (end == p)
		return FALSE;
	for (; p < end; p++) {
		if (*p == '-') seen_dash = TRUE;
		else if (*p != '|' && *p != ':' && *p != ' ' && *p != '\t')
			return FALSE;
	}
	return seen_dash;
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
	    "<font face=\"monospace\" color=\"" TB_BORDER "\">");
	g_string_append(out, L[kind]);
	for (c = 0; c < ncols; c++) {
		if (c > 0) g_string_append(out, M[kind]);
		for (k = 0; k < w[c] + 2; k++) g_string_append(out, "\xE2\x94\x80");
	}
	g_string_append(out, R[kind]);
	g_string_append(out, "</font><br>");
}

/* Emit one data/header row. `is_head` bolds + colours the cell text. `ncells`
 * is the ACTUAL number of entries in `cells` (which may be fewer OR more than
 * `ncols` -- a short GFM row is padded with empty cells, a long one is
 * clipped). Never index past `ncells`: `cells` is only NULL-terminated at
 * `ncells`, so reading cells[ncells+1] would run off the allocation. */
static void
table_row(GString *out, char **cells, int ncells, int ncols, const int *w,
          const int *al, gboolean is_head)
{
	int c;
	g_string_append(out,
	    "<font face=\"monospace\" color=\"" TB_BORDER
	    "\">\xE2\x94\x82</font>");   /* leading | */
	for (c = 0; c < ncols; c++) {
		const char *raw = (c < ncells && cells[c]) ? cells[c] : "";
		int cw = md_visible_width(raw);
		int slack = w[c] - cw; if (slack < 0) slack = 0;
		int lp = 0, rp = slack, k;
		char *inner;
		if (al[c] == TB_RIGHT)  { lp = slack; rp = 0; }
		else if (al[c] == TB_CENTER) { lp = slack / 2; rp = slack - lp; }
		/* fixed 1-space pad + left slack */
		g_string_append(out, "<font face=\"monospace\">");
		for (k = 0; k < lp + 1; k++) g_string_append(out, "&#160;");
		g_string_append(out, "</font>");
		/* content */
		inner = acp_md_inline(raw);
		if (is_head)
			g_string_append_printf(out,
			    "<font face=\"monospace\" color=\"" TB_HEAD
			    "\"><b>%s</b></font>", inner);
		else
			g_string_append_printf(out,
			    "<font face=\"monospace\">%s</font>", inner);
		g_free(inner);
		/* right slack + fixed pad, then trailing border */
		g_string_append(out, "<font face=\"monospace\">");
		for (k = 0; k < rp + 1; k++) g_string_append(out, "&#160;");
		g_string_append(out, "</font>");
		g_string_append(out,
		    "<font face=\"monospace\" color=\"" TB_BORDER
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
	int headn = 0;
	int *w, *al;
	GArray *body_n = g_array_new(FALSE, FALSE, sizeof(int));
	GPtrArray *body = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);

	if (nlines < 2) { g_array_free(body_n, TRUE);
	                  g_ptr_array_free(body, TRUE);
	                  return g_string_free(out, FALSE); }

	head = table_split_row(lines[0], &ncols);
	headn = ncols;
	if (ncols == 0) { g_strfreev(head); g_ptr_array_free(body, TRUE);
	                  g_array_free(body_n, TRUE);
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
		for (c = 0; c < ncols && c < rn; c++) {
			const char *v = row[c] ? row[c] : "";
			int vw = md_visible_width(v);
			if (vw > w[c]) w[c] = vw;
		}
		g_ptr_array_add(body, row);
		g_array_append_val(body_n, rn);
	}
	for (c = 0; c < ncols; c++) if (w[c] < 1) w[c] = 1;

	/* compose: top rule, header, mid rule, body rows. */
	table_border(out, w, ncols, 0);
	table_row(out, head, headn, ncols, w, al, TRUE);
	table_border(out, w, ncols, 1);
	for (i = 0; i < (int)body->len; i++) {
		char **row = g_ptr_array_index(body, i);
		int rn = g_array_index(body_n, int, i);
		table_row(out, row, rn, ncols, w, al, FALSE);
	}
	table_border(out, w, ncols, 2);

	g_free(w); g_free(al);
	g_strfreev(head);
	g_ptr_array_free(body, TRUE);
	g_array_free(body_n, TRUE);
	return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------------- *
 *  Tool-call cards -- maya-style framed shell
 *
 *  Every card is drawn as a rounded (or, on failure, dashed) box, matching
 *  maya's tool_call widget (docs/agent_panel/07_tool_cards.md):
 *
 *    ╭─ <icon> <tool name> ──────────╮
 *    │ <header: title / path / cmd>  status │
 *    ├┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┤      (only when there is a body)
 *    │ <body: diff / content>              │
 *    ╰─────────────────────────╯
 *
 *  Status drives the icon + colour: pending ○ dim, running ● amber,
 *  completed ✓ green, failed ✗ red (dashed border).
 * ------------------------------------------------------------------------- */

#define CARD_BORDER "#3a3a3a"   /* rounded border, dim              */
#define CARD_FAIL   "#78383f"   /* failed border, darkened red      */
#define CARD_W      54          /* interior width in glyph columns  */

/* Status -> icon glyph (maya table). */
static const char *
status_icon(const char *status)
{
	if (!status)                                  return "\xE2\x97\x8F"; /* ● running */
	if (purple_strequal(status, "completed"))     return "\xE2\x9C\x93"; /* ✓ */
	if (purple_strequal(status, "failed"))        return "\xE2\x9C\x97"; /* ✗ */
	if (purple_strequal(status, "pending"))       return "\xE2\x97\x8B"; /* ○ */
	if (purple_strequal(status, "in_progress"))   return "\xE2\x97\x8F"; /* ● */
	return "\xE2\x97\x8F";
}

static const char *
status_icon_color(const char *status)
{
	if (!status)                                  return COL_TOOL;
	if (purple_strequal(status, "completed"))     return COL_ADD;
	if (purple_strequal(status, "failed"))        return COL_DEL;
	if (purple_strequal(status, "pending"))       return COL_DIM;
	return COL_TOOL;   /* running/in_progress -> amber */
}

static gboolean
status_failed(const char *status)
{
	return status && purple_strequal(status, "failed");
}

/* Emit the card's top rule with the tool name in the border label:
 *   ╭─ <icon> <name> ──────────╮   (dashed ╭┄ ┄┄┄ ╮ on failure). */
static void
card_top(GString *o, const char *icon, const char *icol, const char *name,
         gboolean fail)
{
	const char *bcol = fail ? CARD_FAIL : CARD_BORDER;
	const char *dash = fail ? "\xE2\x94\x84" : "\xE2\x94\x80"; /* ┄ / ─ */
	char *ne = g_markup_escape_text(name && *name ? name : "tool", -1);
	/* rendered leader columns before the trailing dashes:
	 *   ╭(1) ─(1) space(1) icon(1) space(1) name(N) space(1) = 6 + N
	 * and the closing ╮ is 1. total line = CARD_W, so:              */
	int name_cols = (int)g_utf8_strlen(name && *name ? name : "tool", -1);
	int dashes = CARD_W - 1 /*╭*/ - 5 /*─ sp icon sp + trailing sp*/
	             - name_cols - 1 /*╮*/, k;
	if (dashes < 1) dashes = 1;
	g_string_append_printf(o,
	    "<font face=\"monospace\" color=\"%s\">\xE2\x95\xAD%s </font>",
	    bcol, dash);   /* ╭─ space */
	g_string_append_printf(o, "<font face=\"monospace\" color=\"%s\">%s</font>",
	    icol, icon);
	g_string_append_printf(o,
	    " <font face=\"monospace\" color=\"%s\"><b>%s</b></font> ", bcol, ne);
	g_string_append_printf(o, "<font face=\"monospace\" color=\"%s\">", bcol);
	for (k = 0; k < dashes; k++) g_string_append(o, dash);
	g_string_append(o, "\xE2\x95\xAE</font><br>");   /* ╮ */
	g_free(ne);
}

/* Emit a full-width bottom rule ╰───╯ (dashed ╰┄┄┄╯ on failure). */
static void
card_bottom(GString *o, gboolean fail)
{
	const char *bcol = fail ? CARD_FAIL : CARD_BORDER;
	const char *dash = fail ? "\xE2\x94\x84" : "\xE2\x94\x80";
	int k;
	g_string_append_printf(o, "<font face=\"monospace\" color=\"%s\">\xE2\x95\xB0",
	    bcol);
	for (k = 0; k < CARD_W - 2; k++) g_string_append(o, dash);
	g_string_append(o, "\xE2\x95\xAF</font><br>");   /* ╯ */
}

/* Emit a mid separator ├┈┈┈┤ (dim dashed, always). */
static void
card_sep(GString *o, gboolean fail)
{
	const char *bcol = fail ? CARD_FAIL : CARD_BORDER;
	int k;
	g_string_append_printf(o, "<font face=\"monospace\" color=\"%s\">\xE2\x94\x9C",
	    bcol);
	for (k = 0; k < CARD_W - 2; k++) g_string_append(o, "\xE2\x94\x88"); /* ┈ */
	g_string_append(o, "\xE2\x94\xA4</font><br>");   /* ┤ */
}

/* Emit one framed body row: │ <html content, display width `cols`> <pad> │.
 * `content_html` is already-escaped HTML; `cols` is its visible glyph width. */
static void
card_row(GString *o, const char *content_html, int cols, gboolean fail)
{
	const char *bcol = fail ? CARD_FAIL : CARD_BORDER;
	const char *bar  = fail ? "\xE2\x94\x86" : "\xE2\x94\x82"; /* ┆ / │ */
	int inner = CARD_W - 2;         /* space between the two bars      */
	int pad = inner - 1 - cols, k;  /* -1 for the single leading space */
	if (pad < 0) pad = 0;
	g_string_append_printf(o, "<font face=\"monospace\" color=\"%s\">%s</font>",
	    bcol, bar);
	g_string_append(o, "<font face=\"monospace\">&#160;</font>");
	g_string_append(o, content_html);
	g_string_append(o, "<font face=\"monospace\">");
	for (k = 0; k < pad; k++) g_string_append(o, "&#160;");
	g_string_append(o, "</font>");
	g_string_append_printf(o, "<font face=\"monospace\" color=\"%s\">%s</font><br>",
	    bcol, bar);
}

/* Convenience: a plain (single-colour, monospace) framed text row. `text` is
 * raw (unescaped); it is escaped + truncated to the interior width here. */
static void
card_text_row(GString *o, const char *text, const char *color, gboolean fail)
{
	int budget = CARD_W - 3;   /* interior minus leading space         */
	glong len = g_utf8_strlen(text ? text : "", -1);
	char *clip;
	char *esc;
	GString *frag = g_string_new(NULL);
	int cols;

	if (len > budget) {
		/* truncate with an ellipsis */
		const char *cut = g_utf8_offset_to_pointer(text, budget - 1);
		clip = g_strndup(text, cut - text);
		esc = g_markup_escape_text(clip, -1);
		g_free(clip);
		cols = budget;
		g_string_append_printf(frag,
		    "<font face=\"monospace\" color=\"%s\">%s\xE2\x80\xA6</font>",
		    color, esc);
	} else {
		esc = g_markup_escape_text(text ? text : "", -1);
		cols = (int)len;
		g_string_append_printf(frag,
		    "<font face=\"monospace\" color=\"%s\">%s</font>", color, esc);
	}
	g_free(esc);
	card_row(o, frag->str, cols, fail);
	g_string_free(frag, TRUE);
}

/* Append rendered tool content items (text, diffs) as framed card rows. Each
 * physical line becomes one card_text_row so the box stays square. Returns
 * TRUE if it emitted anything (so the caller knows to draw the separator). */
static gboolean
append_tool_content(GString *html, JsonArray *content, gboolean fail)
{
	guint i, n = content ? json_array_get_length(content) : 0;
	gboolean any = FALSE;
	int budget = CARD_W - 3;

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
			if (path && *path) {
				card_text_row(html, path, COL_DIM, fail);
				any = TRUE;
			}
			if (oldt && *oldt) {
				gchar **ol = g_strsplit(oldt, "\n", -1);
				int k;
				for (k = 0; ol[k]; k++) {
					char *ln = g_strdup_printf("- %s", ol[k]);
					card_text_row(html, ln, COL_DEL, fail);
					g_free(ln); any = TRUE;
				}
				g_strfreev(ol);
			}
			if (newt && *newt) {
				gchar **nl = g_strsplit(newt, "\n", -1);
				int k;
				for (k = 0; nl[k]; k++) {
					char *ln = g_strdup_printf("+ %s", nl[k]);
					card_text_row(html, ln, COL_ADD, fail);
					g_free(ln); any = TRUE;
				}
				g_strfreev(nl);
			}

		} else if (purple_strequal(type, "content")) {
			JsonObject *c = json_object_has_member(item, "content")
			              ? json_object_get_object_member(item, "content") : NULL;
			const char *t = c && json_object_has_member(c, "text")
			              ? json_object_get_string_member(c, "text") : NULL;
			if (t && *t) {
				gchar **ls = g_strsplit(t, "\n", -1);
				int k, shown = 0;
				for (k = 0; ls[k] && shown < 12; k++) {
					/* skip a trailing empty final split element */
					if (ls[k + 1] == NULL && ls[k][0] == '\0') break;
					card_text_row(html, ls[k], COL_DIM, fail);
					shown++; any = TRUE;
				}
				if (ls[k] && shown >= 12) {
					int more = 0; while (ls[k + more]) more++;
					char *m = g_strdup_printf("\xE2\x80\xA6 %d more lines", more);
					card_text_row(html, m, COL_DIM, fail);
					g_free(m);
				}
				g_strfreev(ls);
				(void)budget;
			}
		}
	}
	return any;
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

	/* Draw the card LIVE: acp_stream_write_live_card keeps this card in its own
	 * region and redraws it in place on each update, so the same card animates
	 * pending -> running (●) -> completed (✓) / failed (✗) without stacking
	 * duplicates. It is sealed (frozen) once terminal or when other output
	 * lands. We render on EVERY update -- including the initial tool_call. */
	html = g_string_new(NULL);
	{
		const char *t = title ? title : (tc && tc->title ? tc->title : NULL);
		const char *k = kind ? kind : (tc ? tc->kind : NULL);
		const char *s = status ? status : (tc ? tc->status : NULL);
		gboolean fail = status_failed(s);
		gboolean terminal = s && (purple_strequal(s, "completed") ||
		                          purple_strequal(s, "failed"));
		/* border label = Title-cased tool kind; the status icon carries state. */
		char *label = g_strdup(k && *k ? k : "tool");
		gboolean has_body;
		GString *body = g_string_new(NULL);

		if (label[0]) label[0] = g_ascii_toupper(label[0]);

		/* pre-render the body (diff / output) so we know whether to draw the
		 * separator; output only shows once it has arrived. */
		has_body = content ? append_tool_content(body, content, fail) : FALSE;

		card_top(html, status_icon(s), status_icon_color(s), label, fail);
		/* header line: the title (falls back to the kind if no title) */
		card_text_row(html, t && *t ? t : label, ACP_FG_C, fail);
		if (has_body) {
			card_sep(html, fail);
			g_string_append(html, body->str);
		}
		card_bottom(html, fail);
		g_string_free(body, TRUE);
		g_free(label);

		acp_stream_write_live_card(d, id, html->str, terminal);
	}

	g_string_free(html, TRUE);
	(void)is_update;
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
	card_top(html, "\xE2\x98\xB0", COL_PLAN, "Plan", FALSE);   /* ☰ icon */

	for (i = 0; i < n; i++) {
		JsonObject *e = json_array_get_object_element(entries, i);
		const char *content = e && json_object_has_member(e, "content")
		                    ? json_object_get_string_member(e, "content") : "";
		const char *status = e && json_object_has_member(e, "status")
		                   ? json_object_get_string_member(e, "status") : "";
		const char *box, *bcol;

		if (purple_strequal(status, "completed"))
			{ box = "\xE2\x9C\x94"; bcol = COL_ADD; }       /* check */
		else if (purple_strequal(status, "in_progress"))
			{ box = "\xE2\x96\xB6"; bcol = COL_TOOL; }      /* play */
		else
			{ box = "\xE2\x97\xAB"; bcol = COL_DIM; }       /* white square */

		/* "<glyph> <content>" -- glyph coloured, content truncated to fit */
		{
			int budget = CARD_W - 3 - 2;   /* minus glyph + space */
			glong len = g_utf8_strlen(content, -1);
			char *ce;
			GString *frag = g_string_new(NULL);
			int cols;
			if (len > budget) {
				const char *cut = g_utf8_offset_to_pointer(content, budget - 1);
				char *clip = g_strndup(content, cut - content);
				ce = g_markup_escape_text(clip, -1); g_free(clip);
				cols = 2 + budget;
				g_string_append_printf(frag,
				    "<font face=\"monospace\" color=\"%s\">%s</font>"
				    "<font face=\"monospace\"> %s\xE2\x80\xA6</font>",
				    bcol, box, ce);
			} else {
				ce = g_markup_escape_text(content, -1);
				cols = 2 + (int)len;
				g_string_append_printf(frag,
				    "<font face=\"monospace\" color=\"%s\">%s</font>"
				    "<font face=\"monospace\"> %s</font>",
				    bcol, box, ce);
			}
			card_row(html, frag->str, cols, FALSE);
			g_string_free(frag, TRUE);
			g_free(ce);
		}
	}
	card_bottom(html, FALSE);
	acp_stream_write_card(d, html->str);
	g_string_free(html, TRUE);
}

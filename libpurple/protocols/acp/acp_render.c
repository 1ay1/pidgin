/*
 * ACP protocol plugin -- Markdown rendering + streaming into the conversation.
 *
 * The agent streams Markdown text in small chunks and emits structured
 * tool-call / plan updates. This module turns all of that into a rich, live
 * transcript in the Pidgin conversation window, using the HTML subset that
 * GtkIMHtml understands.
 *
 * Streaming strategy: we buffer the raw markdown of the *current, incomplete
 * block* (a paragraph, list, fenced code block, heading, ...). As soon as a
 * block is finalised (a blank line, or a fence close), we convert that block
 * to HTML and write it to the conversation -- so text appears promptly while
 * still rendering Markdown correctly. The trailing partial block is flushed at
 * end of turn.
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

/* Colours for the agent chrome. */
#define COL_TOOL   "#a05a00"
#define COL_PLAN   "#5c3566"
#define COL_DIM    "#888888"
#define COL_CODE   "#c7254e"
#define COL_ADD    "#4e9a06"
#define COL_DEL    "#cc0000"

/* ------------------------------------------------------------------------- *
 *  Conversation plumbing
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

/* Write an HTML fragment to the agent conversation as an incoming line. We
 * only stamp the "agent:" sender on the first write of a turn; subsequent
 * fragments are written with PURPLE_MESSAGE_RAW so the transcript reads as one
 * continuous streamed reply instead of many "agent:" headers. */
void
acp_conv_write_html(AcpData *d, const char *html, PurpleMessageFlags extra)
{
	PurpleConversation *conv = acp_get_conv(d);
	PurpleConvIm *im = purple_conversation_get_im_data(conv);

	if (!d->turn_opened) {
		/* First fragment of the turn: a normal incoming message so Pidgin
		 * draws the "agent:" nick + timestamp. */
		purple_conv_im_write(im, d->buddy, html,
		                     PURPLE_MESSAGE_RECV | extra, time(NULL));
		d->turn_opened = TRUE;
	} else {
		/* Continuation: raw append, no repeated nick header. */
		purple_conv_im_write(im, d->buddy, html,
		                     PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_RAW | extra,
		                     time(NULL));
	}
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
					g_string_append_printf(out, "<a href=\"%s\">%s</a>",
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
			g_string_append_printf(out, "<b>%s</b>", r);
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

/* ------------------------------------------------------------------------- *
 *  Block markdown: headings, lists, quotes, fenced code, rules, paragraphs
 * ------------------------------------------------------------------------- */

/* Count leading heading hashes (returns 0 if not a heading). */
static int
heading_level(const char *line)
{
	int n = 0;
	while (line[n] == '#') n++;
	if (n >= 1 && n <= 6 && (line[n] == ' ' || line[n] == '\0'))
		return n;
	return 0;
}

static gboolean
is_hr(const char *line)
{
	const char *p = line;
	int dashes = 0;
	while (*p == ' ') p++;
	while (*p == '-' || *p == '*' || *p == '_') { dashes++; p++; }
	while (*p == ' ') p++;
	return (dashes >= 3 && *p == '\0');
}

/* Render a fenced code block body verbatim in a monospace, coloured box. */
static char *
render_code_block(const char *code, const char *lang)
{
	GString *out = g_string_new(NULL);
	char *esc = g_markup_escape_text(code, -1);
	GString *withbr = g_string_new(NULL);
	const char *p;

	for (p = esc; *p; p++) {
		if (*p == '\n')
			g_string_append(withbr, "<br>");
		else if (*p == ' ')
			g_string_append(withbr, "&#160;");
		else
			g_string_append_c(withbr, *p);
	}
	if (lang && *lang) {
		g_string_append_printf(out,
		    "<font color=\"" COL_DIM "\" size=\"2\">%s</font><br>", lang);
	}
	g_string_append_printf(out,
	    "<font face=\"monospace\" color=\"" COL_CODE "\" size=\"2\">%s</font><br>",
	    withbr->str);

	g_string_free(withbr, TRUE);
	g_free(esc);
	return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------------- *
 *  Streaming: render each line the instant it completes
 * ------------------------------------------------------------------------- */

/* Render one finished markdown line (no trailing newline) to HTML and write it
 * to the conversation. Handles headings, list items, quotes, rules; falls back
 * to an inline-formatted paragraph line. */
static void
emit_line(AcpData *d, const char *line)
{
	const char *t = line;
	int hl;
	char *inner, *html;

	while (*t == ' ') t++;

	/* blank line -> a small vertical gap */
	if (*t == '\0') {
		acp_conv_write_html(d, "<br>", 0);
		return;
	}
	/* horizontal rule */
	if (is_hr(t)) {
		acp_conv_write_html(d, "<hr>", 0);
		return;
	}
	/* heading */
	hl = heading_level(t);
	if (hl) {
		int size = (6 - hl) + 2;
		inner = acp_md_inline(t + hl + 1);
		html = g_strdup_printf("<font size=\"%d\"><b>%s</b></font>",
		                       size < 3 ? 3 : size, inner);
		acp_conv_write_html(d, html, 0);
		g_free(html); g_free(inner);
		return;
	}
	/* blockquote */
	if (t[0] == '>') {
		inner = acp_md_inline(t[1] == ' ' ? t + 2 : t + 1);
		html = g_strdup_printf(
		    "<font color=\"" COL_DIM "\">&#8214; %s</font>", inner);
		acp_conv_write_html(d, html, 0);
		g_free(html); g_free(inner);
		return;
	}
	/* unordered list item */
	if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
		int indent = (t - line);   /* nesting by leading spaces */
		inner = acp_md_inline(t + 2);
		html = g_strdup_printf("%s&#8226; %s",
		                       indent >= 2 ? "&#160;&#160;&#160;" : "&#160;&#160;",
		                       inner);
		acp_conv_write_html(d, html, 0);
		g_free(html); g_free(inner);
		return;
	}
	/* ordered list item "N. " */
	if (isdigit((unsigned char)t[0])) {
		const char *q = t;
		while (isdigit((unsigned char)*q)) q++;
		if (q[0] == '.' && q[1] == ' ') {
			int num = atoi(t);
			inner = acp_md_inline(q + 2);
			html = g_strdup_printf("&#160;&#160;%d. %s", num, inner);
			acp_conv_write_html(d, html, 0);
			g_free(html); g_free(inner);
			return;
		}
	}
	/* plain paragraph line */
	inner = acp_md_inline(line);
	acp_conv_write_html(d, inner, 0);
	g_free(inner);
}

/* Feed streamed message text. We render each line as soon as its newline
 * arrives, so the transcript advances line-by-line as the agent types. Fenced
 * code blocks accumulate until the closing ``` and then render as one box.
 * Any text before the first newline is held in md_block until the line ends. */
void
acp_stream_message(AcpData *d, const char *text)
{
	const char *p;

	if (!text || !*text)
		return;

	for (p = text; *p; p++) {
		if (*p != '\n') {
			g_string_append_c(d->md_block, *p);
			continue;
		}

		/* a full line is now in md_block */
		{
			const char *line = d->md_block->str;

			if (strncmp(line, "```", 3) == 0) {
				if (!d->in_code_fence) {
					char *lang = g_strdup(line + 3);
					g_strchomp(lang);
					g_free(d->fence_lang);
					d->fence_lang = lang;
					d->in_code_fence = TRUE;
					d->code_buf_reset = TRUE;
				} else {
					char *html = render_code_block(
					    d->code_buf ? d->code_buf->str : "", d->fence_lang);
					acp_conv_write_html(d, html, 0);
					g_free(html);
					if (d->code_buf) g_string_truncate(d->code_buf, 0);
					d->in_code_fence = FALSE;
					g_free(d->fence_lang);
					d->fence_lang = NULL;
				}
				g_string_truncate(d->md_block, 0);
				continue;
			}

			if (d->in_code_fence) {
				if (!d->code_buf)
					d->code_buf = g_string_new(NULL);
				if (d->code_buf_reset) {
					g_string_truncate(d->code_buf, 0);
					d->code_buf_reset = FALSE;
				}
				if (d->code_buf->len)
					g_string_append_c(d->code_buf, '\n');
				g_string_append(d->code_buf, line);
				g_string_truncate(d->md_block, 0);
				continue;
			}

			emit_line(d, line);
			g_string_truncate(d->md_block, 0);
		}
	}
}

void
acp_stream_thought(AcpData *d, const char *text)
{
	char *esc, *html;
	GString *s;
	const char *p;

	if (!text || !*text)
		return;
	/* thoughts are shown dimmed+italic, verbatim (not markdown-parsed) */
	esc = g_markup_escape_text(text, -1);
	s = g_string_new(NULL);
	for (p = esc; *p; p++) {
		if (*p == '\n') g_string_append(s, "<br>");
		else            g_string_append_c(s, *p);
	}
	html = g_strdup_printf("<font color=\"" COL_DIM "\"><i>%s</i></font>", s->str);
	acp_conv_write_html(d, html, 0);
	g_free(html);
	g_string_free(s, TRUE);
	g_free(esc);
}

void
acp_stream_flush(AcpData *d)
{
	if (d->in_code_fence) {
		/* unterminated fence: render what we have */
		char *html = render_code_block(
		    d->code_buf ? d->code_buf->str : "", d->fence_lang);
		acp_conv_write_html(d, html, 0);
		g_free(html);
		if (d->code_buf) g_string_truncate(d->code_buf, 0);
		d->in_code_fence = FALSE;
		g_free(d->fence_lang);
		d->fence_lang = NULL;
	}
	/* emit any trailing line that never got a newline (the final line of the
	 * reply usually arrives without one). */
	if (d->md_block->len > 0) {
		emit_line(d, d->md_block->str);
		g_string_truncate(d->md_block, 0);
	}
}

void
acp_stream_reset(AcpData *d)
{
	g_string_truncate(d->md_block, 0);
	d->in_code_fence = FALSE;
	g_free(d->fence_lang);
	d->fence_lang = NULL;
	d->turn_opened = FALSE;
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

/* Render tool content items: text, diffs, terminal output. */
static void
render_tool_content(AcpData *d, JsonArray *content)
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
			GString *html = g_string_new(NULL);
			char *pe = g_markup_escape_text(path, -1);
			g_string_append_printf(html,
			    "<font color=\"" COL_DIM "\" size=\"2\">&#160;&#160;%s</font><br>", pe);
			g_free(pe);

			/* show removed then added lines, colour-coded */
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
			acp_conv_write_html(d, html->str, 0);
			g_string_free(html, TRUE);

		} else if (purple_strequal(type, "content")) {
			/* nested content block: usually {type:text,text:...} */
			JsonObject *c = json_object_has_member(item, "content")
			              ? json_object_get_object_member(item, "content") : NULL;
			const char *t = c && json_object_has_member(c, "text")
			              ? json_object_get_string_member(c, "text") : NULL;
			if (t && *t) {
				char *e = g_markup_escape_text(t, -1);
				GString *s = g_string_new(NULL);
				const char *p;
				for (p = e; *p; p++) {
					if (*p == '\n')
						g_string_append(s, "<br>");
					else
						g_string_append_c(s, *p);
				}
				{
					char *html = g_strdup_printf(
					    "<font face=\"monospace\" size=\"2\" color=\"" COL_DIM
					    "\">%s</font><br>", s->str);
					acp_conv_write_html(d, html, 0);
					g_free(html);
				}
				g_string_free(s, TRUE);
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

	/* Any tool output ends the current text block cleanly. */
	acp_stream_flush(d);

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
			acp_conv_write_html(d, m, 0);
			g_free(m); g_free(te);
		}
		return;
	}

	/* Header line for the tool call. */
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
	acp_conv_write_html(d, html->str, 0);
	g_string_free(html, TRUE);

	if (content)
		render_tool_content(d, content);
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

	acp_stream_flush(d);
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
	acp_conv_write_html(d, html->str, 0);
	g_string_free(html, TRUE);
}

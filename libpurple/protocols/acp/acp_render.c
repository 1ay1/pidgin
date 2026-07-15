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

/* Colours for the agent chrome. */
#define COL_TOOL   "#a05a00"
#define COL_PLAN   "#5c3566"
#define COL_DIM    "#888888"
#define COL_CODE   "#c7254e"
#define COL_ADD    "#4e9a06"
#define COL_DEL    "#cc0000"

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

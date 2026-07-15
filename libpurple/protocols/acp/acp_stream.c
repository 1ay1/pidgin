/*
 * ACP protocol plugin -- incremental streaming Markdown renderer.
 *
 * WHY THIS EXISTS
 * ---------------
 * libpurple's conversation API (purple_conv_im_write) is *message oriented*:
 * every call appends one immutable line to the transcript. That model cannot
 * express live streaming Markdown, where a half-received `**bold` must first
 * show as literal text and then "snap" to bold the instant the closing `**`
 * arrives -- i.e. the already-drawn text has to be *revised in place*.
 *
 * So for the streaming text we bypass the message API and render directly into
 * the conversation's GtkIMHtml widget (the rich-text view). Pidgin exports the
 * gtk_imhtml_* / PidginConversation symbols to plugins loaded in its process,
 * so a protocol plugin running inside Pidgin can drive the widget. (When the
 * plugin runs in a non-GTK libpurple frontend there simply is no widget and we
 * degrade to nothing here; acp_render.c keeps the message-API path for tool
 * cards which do not need in-place revision.)
 *
 * DESIGN: committed prefix + live tail
 * ------------------------------------
 * The transcript is split at a GtkTextMark (`tail`) into:
 *   [ committed prefix ..................|.... live tail ]
 *                                      tail-mark
 * Everything before `tail` is finalized Markdown HTML that will never change.
 * `open` holds the *raw markdown of the single block currently being typed*
 * (one paragraph, one list, one heading, or the body of one fenced code block).
 *
 * On each streamed chunk we: append to `open`, DELETE the widget region from
 * `tail` to end, re-render `open` to HTML and re-insert it at `tail`. That
 * O(open) redraw makes the current block update live. When the block is
 * *closed* (a blank line, a fence close, or an interrupting new block starts)
 * we COMMIT it -- the rendered HTML stays, `tail` is moved to the end, `open`
 * is cleared -- so committed text is never re-touched and cost stays O(1)
 * amortized regardless of how long the turn grows.
 *
 * Copyright (C) 2024 Ayush Bhat <tfeayush@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 (or, at your
 * option, any later version).
 */
#include "acp.h"

#include "debug.h"

#include <ctype.h>
#include <string.h>

#include <gtk/gtk.h>
#include "gtkconv.h"
#include "gtkimhtml.h"

/* Palette -- mapped from maya's markdown widget (1ay1/maya) ANSI colours to
 * hex, so the transcript reads like agentty's renderer:
 *   heading1 bright_cyan / heading2 cyan / heading3 bright_blue
 *   code bright_cyan mono / links bright_blue underline / bold bright_white
 *   quote bright_yellow bar + italic / list markers bright_blue
 *   dim chrome (rules, lang tags, strike) bright_black. */
#define COL_H1     "#34e2e2"   /* bright_cyan  */
#define COL_H2     "#06989a"   /* cyan         */
#define COL_H3     "#729fcf"   /* bright_blue  */
#define COL_HDIM   "#3465a4"   /* blue (h4-h6) */
#define COL_RULE   "#888a85"   /* bright_black */
#define COL_CODE   "#34e2e2"   /* bright_cyan (code fg)  */
#define COL_CODEBG "#1a1a1a"   /* near-black code bg      */
#define COL_LANG   "#888a85"   /* bright_black (lang tag)  */
#define COL_LINK   "#729fcf"   /* bright_blue  */
#define COL_BOLD   "#eeeeec"   /* bright_white (emphasis) */
#define COL_QUOTE  "#fce94f"   /* bright_yellow (quote bar) */
#define COL_QTEXT  "#cccccc"   /* quote text                */
#define COL_BULLET "#729fcf"   /* bright_blue (list marker) */
#define COL_DIM    "#888a85"   /* bright_black */
#define COL_CHECK  "#8ae234"   /* bright_green (task done)   */

/* ------------------------------------------------------------------------- *
 *  Per-turn streaming state, attached to AcpData->stream (void* in header).
 * ------------------------------------------------------------------------- */
typedef struct {
	GtkIMHtml    *imhtml;      /* target rich-text widget (may be NULL)       */
	GtkTextMark  *tail;        /* start of the live (uncommitted) region      */

	GString      *open;        /* raw markdown of the block being typed       */
	GString      *open_line;   /* the current, still-incomplete source line   */
	gboolean      opened;      /* have we placed the "agent:" header + tail?  */

	/* fenced code block state */
	gboolean      in_fence;
	gchar        *fence_lang;
	GString      *fence_body;  /* verbatim lines inside the open fence         */

	/* the kind of block currently accumulating in `open`, so we can detect
	 * an interrupter (e.g. a list item ending a paragraph) and commit early. */
	int           open_kind;   /* AcpBlk* below                               */

	/* live "agent is typing" placeholder drawn in the transcript while a turn
	 * is in flight but before any content has landed. Removed the instant the
	 * first chunk/card arrives or the turn ends. */
	gboolean      typing;      /* placeholder currently shown                 */
	GtkTextMark  *type_start;  /* start of the placeholder region             */

	/* live tool card: the card for `card_id` occupies [card_start .. end] and
	 * is redrawn IN PLACE on every status update (pending -> running ->
	 * completed), so tool calls animate live. Sealed (mark dropped) when a
	 * different card / text chunk / the turn end arrives. */
	GtkTextMark  *card_start;  /* start of the live card region, or NULL      */
	gchar        *card_id;     /* toolCallId currently occupying that region  */
} AcpStream;

enum { BLK_NONE = 0, BLK_PARA, BLK_LIST, BLK_QUOTE, BLK_HEAD, BLK_TABLE };

/* Forward decls from acp_render.c (shared inline renderer). */
char *acp_md_inline(const char *text);

/* forward decl: defined below, used by ensure_open to clear the placeholder */
void acp_stream_typing_off(AcpData *d);
static AcpStream *stream_get(AcpData *d);
static void seal_live_card(AcpStream *s);

/* A line that could be a table row: contains a '|' and, after trimming, is not
 * a fence / heading / list. (The real promotion to BLK_TABLE still requires a
 * following separator row -- see feed_line.) */
static gboolean
looks_table_row(const char *l)
{
	while (*l == ' ') l++;
	if (*l == '\0' || *l == '#' || *l == '>')
		return FALSE;
	return strchr(l, '|') != NULL;
}

/* ------------------------------------------------------------------------- *
 *  Widget lookup
 * ------------------------------------------------------------------------- */

/* Give the conversation view an agentty-like dark canvas so the bright_cyan /
 * bright_blue markdown palette reads correctly (Pidgin's default IMHtml is a
 * light background, on which those colours would wash out). Applied once per
 * widget via a GtkCssProvider on the GtkTextView; guarded by a data flag so we
 * do not stack providers on every turn. */
#define ACP_BG    "#1e1e1e"   /* editor-dark canvas */
#define ACP_FG    "#d4d4d4"   /* body text          */

static void
acp_apply_dark_theme(GtkIMHtml *imhtml)
{
	GtkWidget *w = GTK_WIDGET(imhtml);
	GtkStyleContext *ctx;
	GtkCssProvider *prov;

	if (g_object_get_data(G_OBJECT(imhtml), "acp-themed"))
		return;

	prov = gtk_css_provider_new();
	gtk_css_provider_load_from_data(prov,
	    "textview {"
	    "  background-color: " ACP_BG ";"
	    "  color: " ACP_FG ";"
	    "  padding: 10px 16px 10px 16px;"   /* top right bottom left breathing room */
	    "  font-size: 10.5pt;"
	    "}"
	    "textview text {"
	    "  background-color: " ACP_BG ";"
	    "  color: " ACP_FG ";"
	    "}", -1, NULL);
	ctx = gtk_widget_get_style_context(w);
	gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(prov),
	    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(prov);

	g_object_set_data(G_OBJECT(imhtml), "acp-themed", GINT_TO_POINTER(1));
}

static GtkIMHtml *
acp_find_imhtml(AcpData *d)
{
	PurpleAccount *acct = purple_connection_get_account(d->gc);
	PurpleConversation *conv;
	PidginConversation *gtkconv;
	GtkIMHtml *imhtml;

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
	                                              d->buddy, acct);
	if (!conv)
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, acct, d->buddy);
	if (!conv)
		return NULL;

	gtkconv = PIDGIN_CONVERSATION(conv);
	if (!gtkconv || !gtkconv->imhtml)
		return NULL;
	imhtml = GTK_IMHTML(gtkconv->imhtml);
	acp_apply_dark_theme(imhtml);
	return imhtml;
}

/* ------------------------------------------------------------------------- *
 *  Low-level widget helpers
 * ------------------------------------------------------------------------- */

/* Insert HTML at the very end of the buffer; leaves `tail` (if any) put. */
static void
imhtml_append(AcpStream *s, const char *html)
{
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &end);
	gtk_imhtml_insert_html_at_iter(s->imhtml, html,
	    GTK_IMHTML_NO_SCROLL | GTK_IMHTML_NO_COMMENTS, &end);
}

/* Delete everything from the tail mark to the end of the buffer. */
static void
imhtml_clear_tail(AcpStream *s)
{
	GtkTextIter start, end;
	if (!s->tail)
		return;
	gtk_text_buffer_get_iter_at_mark(s->imhtml->text_buffer, &start, s->tail);
	gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &end);
	if (!gtk_text_iter_equal(&start, &end))
		gtk_imhtml_delete(s->imhtml, &start, &end);
}

/* Render HTML for the live tail: clear old tail, insert fresh, keep scrolled. */
static void
imhtml_set_tail(AcpStream *s, const char *html)
{
	GtkTextIter at;
	imhtml_clear_tail(s);
	if (html && *html) {
		gtk_text_buffer_get_iter_at_mark(s->imhtml->text_buffer, &at, s->tail);
		gtk_imhtml_insert_html_at_iter(s->imhtml, html,
		    GTK_IMHTML_NO_SCROLL | GTK_IMHTML_NO_COMMENTS, &at);
	}
	gtk_imhtml_scroll_to_end(s->imhtml, FALSE);
}

/* Move the tail mark to the current end of buffer (commit point advances). */
static void
imhtml_anchor_tail(AcpStream *s)
{
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &end);
	if (!s->tail)
		s->tail = gtk_text_buffer_create_mark(s->imhtml->text_buffer,
		                                       NULL, &end, TRUE /*left grav*/);
	else
		gtk_text_buffer_move_mark(s->imhtml->text_buffer, s->tail, &end);
}

/* ------------------------------------------------------------------------- *
 *  Block markdown -> HTML  (whole-block, cohesive; used for the live tail)
 * ------------------------------------------------------------------------- */

static int
heading_level(const char *l)
{
	int n = 0;
	while (l[n] == '#') n++;
	if (n >= 1 && n <= 6 && (l[n] == ' ' || l[n] == '\0'))
		return n;
	return 0;
}

static gboolean
is_hr(const char *l)
{
	int dashes = 0;
	while (*l == ' ') l++;
	while (*l == '-' || *l == '*' || *l == '_') { dashes++; l++; }
	while (*l == ' ') l++;
	return (dashes >= 3 && *l == '\0');
}

static gboolean
is_ulist(const char *l, const char **body)
{
	if ((l[0] == '-' || l[0] == '*' || l[0] == '+') && l[1] == ' ') {
		if (body) *body = l + 2;
		return TRUE;
	}
	return FALSE;
}

static gboolean
is_olist(const char *l, const char **body, int *num)
{
	const char *q = l;
	if (!isdigit((unsigned char)*q))
		return FALSE;
	while (isdigit((unsigned char)*q)) q++;
	if ((q[0] == '.' || q[0] == ')') && q[1] == ' ') {
		if (num)  *num = atoi(l);
		if (body) *body = q + 2;
		return TRUE;
	}
	return FALSE;
}

/* Classify a *trimmed* line's block kind (for interrupter detection). */
static int
classify(const char *t)
{
	if (*t == '\0')                 return BLK_NONE;
	if (heading_level(t))           return BLK_HEAD;
	if (t[0] == '>')                return BLK_QUOTE;
	if (is_ulist(t, NULL) || is_olist(t, NULL, NULL)) return BLK_LIST;
	return BLK_PARA;
}

/* Render one markdown line to an HTML fragment (no trailing separator).
 * Styling mirrors maya's markdown widget (heading accent bar + rule, ▸/◦
 * bullets, │ quote bar). */
static void
render_line(GString *out, const char *line)
{
	const char *t = line;
	const char *body;
	int hl, num, indent;
	char *inner;

	while (*t == ' ') t++;
	indent = (int)(t - line);

	if (*t == '\0')
		return;
	if (is_hr(t)) {
		g_string_append(out, "<hr>");
		return;
	}
	hl = heading_level(t);
	if (hl) {
		/* maya: h1 bright_cyan+▍, h2 cyan+▍, h3 bright_blue, h4-6 dim+marker */
		const char *col = (hl == 1) ? COL_H1 : (hl == 2) ? COL_H2
		               : (hl == 3) ? COL_H3 : COL_HDIM;
		int size = (hl <= 2) ? 5 : (hl == 3) ? 4 : 3;
		const char *accent = (hl <= 2) ? "\xE2\x96\x8D " : "";   /* ▍ */
		const char *marker = (hl == 4) ? "\xC2\xA7 "          /* § */
		                   : (hl == 5) ? "\xE2\x80\xBA "      /* › */
		                   : (hl == 6) ? "\xE2\x80\xA3 " : "";  /* ‣ */
		inner = acp_md_inline(t + hl + (t[hl] == ' ' ? 1 : 0));
		g_string_append_printf(out,
		    "<font color=\"%s\" size=\"%d\"><b>%s%s%s</b></font>",
		    col, size, accent, marker, inner);
		g_free(inner);
		return;
	}
	if (t[0] == '>') {
		/* maya: │ bright_yellow bar + muted italic text. Nested quotes (> >)
		 * render one bar per level; the content is whatever remains after the
		 * markers. A bare "> " (blank quote line) still draws its bar so the
		 * gutter stays continuous down the whole quote. */
		int level = 0;
		const char *q = t;
		while (*q == '>') {
			level++;
			q++;
			if (*q == ' ') q++;
		}
		for (; level > 0; level--)
			g_string_append(out,
			    "<font color=\"" COL_QUOTE "\">\xE2\x94\x82 </font>");
		if (*q) {
			inner = acp_md_inline(q);
			g_string_append_printf(out,
			    "<font color=\"" COL_QTEXT "\"><i>%s</i></font>", inner);
			g_free(inner);
		}
		return;
	}
	/* GFM task list: - [ ] / - [x] */
	if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ' &&
	    t[2] == '[' && (t[3] == ' ' || t[3] == 'x' || t[3] == 'X') && t[4] == ']') {
		gboolean done = (t[3] == 'x' || t[3] == 'X');
		inner = acp_md_inline(t[5] == ' ' ? t + 6 : t + 5);
		g_string_append_printf(out,
		    "&#160;&#160;<font color=\"%s\">%s</font> %s",
		    done ? COL_CHECK : COL_DIM,
		    done ? "\xE2\x9C\x93" : "\xE2\x97\x8B",   /* ✓ / ○ */
		    inner);
		g_free(inner);
		return;
	}
	if (is_ulist(t, &body)) {
		inner = acp_md_inline(body);
		if (indent >= 2)
			g_string_append_printf(out,
			    "&#160;&#160;&#160;&#160;<font color=\"" COL_BULLET
			    "\">\xE2\x97\xA6</font> %s", inner);   /* ◦ nested */
		else
			g_string_append_printf(out,
			    "&#160;&#160;<font color=\"" COL_BULLET
			    "\"><b>\xE2\x96\xB8</b></font> %s", inner);  /* ▸ top */
		g_free(inner);
		return;
	}
	if (is_olist(t, &body, &num)) {
		inner = acp_md_inline(body);
		g_string_append_printf(out,
		    "&#160;&#160;<font color=\"" COL_BULLET "\"><b>%d.</b></font> %s",
		    num, inner);
		g_free(inner);
		return;
	}
	inner = acp_md_inline(line);
	g_string_append(out, inner);
	g_free(inner);
}

/* Render the accumulated raw markdown of the open (non-fence) block. Lines are
 * joined with <br>. A code fence body is rendered as a monospace block. */
static char *
render_open_block(AcpStream *s)
{
	GString *out = g_string_new(NULL);

	if (s->in_fence) {
		/* Code block, maya-style: acp_highlight_code renders a full framed
		 * panel -- top rule with the language chip, a line-number gutter +
		 * syntax-highlighted body on a filled dark background, and a bottom
		 * rule. Nothing else to add here. */
		const char *src = s->fence_body ? s->fence_body->str : "";
		char *body = acp_highlight_code(src, s->fence_lang);
		g_string_append(out, body ? body : "");
		g_free(body);
		return g_string_free(out, FALSE);
	}

	if (s->open_kind == BLK_TABLE) {
		/* Table: split the accumulated raw lines and hand them to the
		 * alignment-aware box-drawn renderer. While still streaming the
		 * header alone (no separator yet) it is shown as plain text below. */
		gchar **lines = g_strsplit(s->open->str, "\n", -1);
		int n = 0;
		while (lines[n]) n++;
		if (n >= 2 && acp_table_is_separator(lines[1])) {
			char *tbl = acp_render_table(lines, n);
			g_string_append(out, tbl ? tbl : "");
			g_free(tbl);
		} else {
			/* not yet a full table -- echo raw lines so the header shows */
			int i;
			for (i = 0; i < n; i++) {
				char *e = g_markup_escape_text(lines[i], -1);
				if (i) g_string_append(out, "<br>");
				g_string_append_printf(out,
				    "<font face=\"monospace\">%s</font>", e);
				g_free(e);
			}
		}
		g_strfreev(lines);
		return g_string_free(out, FALSE);
	}

	{
		gchar **lines = g_strsplit(s->open->str, "\n", -1);
		int i;
		gboolean first = TRUE;
		for (i = 0; lines[i]; i++) {
			/* trailing empty split element from a final '\n' -> skip */
			if (lines[i + 1] == NULL && lines[i][0] == '\0')
				break;
			if (!first)
				g_string_append(out, "<br>");
			render_line(out, lines[i]);
			first = FALSE;
		}
		g_strfreev(lines);
	}
	return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------------- *
 *  Commit / open lifecycle
 * ------------------------------------------------------------------------- */

/* Ensure the header ("agent:" nick line) exists and the tail mark is set. */
static void
ensure_open(AcpData *d, AcpStream *s)
{
	GtkTextIter end;
	char *stamp, *nick, *header;
	time_t now = time(NULL);
	struct tm *lt = localtime(&now);

	if (s->opened)
		return;

	/* tear down the "thinking" placeholder before drawing the header */
	acp_stream_typing_off(d);

	/* Pop / raise the conversation window so the user actually sees the reply
	 * even if they never opened the chat (the agent buddy is always online and
	 * may reply to a session action). Routed through the GTK conv UI op. */
	{
		PurpleAccount *acct = purple_connection_get_account(d->gc);
		PurpleConversation *conv =
		    purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
		                                          d->buddy, acct);
		if (conv)
			purple_conversation_present(conv);
	}

	/* Draw our own incoming header row directly into the widget: a dim
	 * timestamp + the buddy nick in a colour, matching Pidgin's own
	 * "(HH:MM:SS) nick:" style but fully under our control so the streamed
	 * HTML that follows lands cleanly after it (no message-API interleave). */
	stamp = g_strdup_printf("%02d:%02d:%02d",
	    lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0, lt ? lt->tm_sec : 0);
	nick = g_markup_escape_text(d->buddy ? d->buddy : "agent", -1);
	header = g_strdup_printf(
	    "<font color=\"" COL_DIM "\" size=\"2\">(%s)</font> "
	    "<font color=\"" COL_H3 "\"><b>%s:</b></font><br>",
	    stamp, nick);

	gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &end);
	/* newline before the header unless the buffer is empty */
	if (gtk_text_buffer_get_char_count(s->imhtml->text_buffer) > 0)
		imhtml_append(s, "<br>");
	imhtml_append(s, header);

	g_free(header);
	g_free(nick);
	g_free(stamp);

	imhtml_anchor_tail(s);
	s->opened = TRUE;
}

/* Commit the current open block: freeze its HTML, advance tail, reset state. */
static void
commit_open(AcpData *d, AcpStream *s)
{
	if (!s->imhtml)
		return;
	if (s->open->len == 0 && !s->in_fence)
		return;

	/* trim trailing bare-bar rows we inserted for blank lines inside a quote,
	 * so a quote that ends on a blank line has no dangling empty │ */
	if (s->open_kind == BLK_QUOTE) {
		while (s->open->len >= 2 &&
		       g_str_has_suffix(s->open->str, "\n>"))
			g_string_truncate(s->open, s->open->len - 2);
	}

	{
		char *html = render_open_block(s);
		/* replace the live tail with the final render, then anchor past it */
		imhtml_set_tail(s, html);
		g_free(html);
	}
	/* separate committed blocks with a blank line for breathing room */
	imhtml_append(s, "<br>");
	imhtml_anchor_tail(s);

	g_string_truncate(s->open, 0);
	s->open_kind = BLK_NONE;
	if (s->in_fence) {
		s->in_fence = FALSE;
		g_free(s->fence_lang);
		s->fence_lang = NULL;
		if (s->fence_body)
			g_string_truncate(s->fence_body, 0);
	}
}

/* Re-render the live tail from the current open block (no commit). */
static void
paint_tail(AcpStream *s)
{
	char *html = render_open_block(s);
	imhtml_set_tail(s, html);
	g_free(html);
}

/* ------------------------------------------------------------------------- *
 *  "agent is typing" placeholder (in-transcript, matches agentty's spinner)
 * ------------------------------------------------------------------------- */

/* Show a dim thinking line at the end of the transcript. Idempotent. It sits
 * after the committed prefix and is torn down (typing_off) the instant real
 * content or a card arrives, so it never mixes into the streamed reply. */
void
acp_stream_typing_on(AcpData *d)
{
	AcpStream *s = stream_get(d);
	GtkTextIter end;

	if (!s->imhtml || s->typing)
		return;
	/* only show the placeholder before any reply text has been drawn */
	if (s->opened)
		return;

	/* present the conversation so the user sees the agent start working */
	{
		PurpleAccount *acct = purple_connection_get_account(d->gc);
		PurpleConversation *conv =
		    purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
		                                          d->buddy, acct);
		if (conv)
			purple_conversation_present(conv);
	}

	if (gtk_text_buffer_get_char_count(s->imhtml->text_buffer) > 0)
		imhtml_append(s, "<br>");
	gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &end);
	s->type_start = gtk_text_buffer_create_mark(s->imhtml->text_buffer,
	                                            NULL, &end, TRUE);
	imhtml_append(s,
	    "<font color=\"" COL_DIM "\"><i>\xE2\x97\x8F agent is thinking\xE2\x80\xA6"
	    "</i></font>");
	gtk_imhtml_scroll_to_end(s->imhtml, FALSE);
	s->typing = TRUE;
}

/* Remove the thinking placeholder if present. */
void
acp_stream_typing_off(AcpData *d)
{
	AcpStream *s = d->stream;
	GtkTextIter a, b;

	if (!s || !s->typing || !s->imhtml || !GTK_IS_IMHTML(s->imhtml)) {
		if (s) s->typing = FALSE;
		return;
	}
	if (s->type_start) {
		gtk_text_buffer_get_iter_at_mark(s->imhtml->text_buffer, &a,
		                                 s->type_start);
		gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &b);
		if (!gtk_text_iter_equal(&a, &b))
			gtk_imhtml_delete(s->imhtml, &a, &b);
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->type_start);
		s->type_start = NULL;
	}
	s->typing = FALSE;
}

/* ------------------------------------------------------------------------- *
 *  Public streaming API (called from acp_render.c)
 * ------------------------------------------------------------------------- */

static AcpStream *
stream_get(AcpData *d)
{
	AcpStream *s = d->stream;
	if (!s) {
		s = g_new0(AcpStream, 1);
		s->open = g_string_new(NULL);
		s->fence_body = g_string_new(NULL);
		s->open_kind = BLK_NONE;
		d->stream = s;
	}
	/* (re)bind the widget each turn -- the conversation may have been closed
	 * and reopened, invalidating the old GtkIMHtml pointer + mark. */
	if (!s->imhtml || !GTK_IS_IMHTML(s->imhtml)) {
		s->imhtml = acp_find_imhtml(d);
		s->tail = NULL;
		s->opened = FALSE;
	}
	return s;
}

/* Process one completed source line of streamed markdown (no newline). */
static void
feed_line(AcpData *d, AcpStream *s, const char *line)
{
	/* fence toggling */
	if (strncmp(line, "```", 3) == 0 || strncmp(line, "~~~", 3) == 0) {
		if (!s->in_fence) {
			/* an open non-fence block ends before a code block starts */
			if (s->open->len)
				commit_open(d, s);
			s->in_fence = TRUE;
			g_free(s->fence_lang);
			s->fence_lang = g_strchomp(g_strdup(line + 3));
			if (s->fence_body) g_string_truncate(s->fence_body, 0);
			paint_tail(s);   /* show empty code box immediately */
		} else {
			commit_open(d, s);   /* fence close -> freeze the code block */
		}
		return;
	}

	if (s->in_fence) {
		if (s->fence_body->len)
			g_string_append_c(s->fence_body, '\n');
		g_string_append(s->fence_body, line);
		paint_tail(s);
		return;
	}

	/* blank line: paragraph/list/quote separator -> commit the open block */
	{
		const char *t = line;
		while (*t == ' ') t++;
		if (*t == '\0') {
			/* Inside a blockquote a blank line does NOT end the quote (a
			 * quote can span paragraphs); draw a bare bar row so the │ gutter
			 * stays continuous, matching maya. It closes on the first
			 * non-quote, non-blank line (handled below as an interrupter). */
			if (s->open->len && s->open_kind == BLK_QUOTE) {
				g_string_append(s->open, "\n>");
				paint_tail(s);
				return;
			}
			if (s->open->len)
				commit_open(d, s);
			return;
		}
	}

	/* interrupter: a heading or list item starting mid-paragraph closes the
	 * paragraph first, so blocks stay cohesive without needing a blank line. */
	{
		const char *t = line;
		int kind;
		while (*t == ' ') t++;
		kind = classify(t);

		/* ---- GFM table handling -------------------------------------- *
		 * We cannot know line 1 ("| a | b |") is a table header until the
		 * separator ("|---|---|") arrives on line 2. So: buffer the header
		 * as an ordinary block, and when a separator lands on top of a
		 * single pipe-row block, PROMOTE the block to BLK_TABLE. Once in a
		 * table, further pipe rows keep appending; a non-pipe line ends it. */
		if (s->open_kind != BLK_TABLE && acp_table_is_separator(line) &&
		    s->open->len && strchr(s->open->str, '\n') == NULL &&
		    looks_table_row(s->open->str)) {
			s->open_kind = BLK_TABLE;
			g_string_append_c(s->open, '\n');
			g_string_append(s->open, line);
			paint_tail(s);
			return;
		}
		if (s->open_kind == BLK_TABLE) {
			if (looks_table_row(line)) {
				g_string_append_c(s->open, '\n');
				g_string_append(s->open, line);
				paint_tail(s);
				return;
			}
			/* non-table line ends the table; fall through to normal path */
			commit_open(d, s);
		}

		if (s->open->len && s->open_kind == BLK_PARA &&
		    (kind == BLK_HEAD || kind == BLK_LIST || kind == BLK_QUOTE)) {
			commit_open(d, s);
		}
		/* a blockquote ends when a non-quote line arrives */
		if (s->open->len && s->open_kind == BLK_QUOTE && kind != BLK_QUOTE) {
			commit_open(d, s);
		}
		if (s->open->len)
			g_string_append_c(s->open, '\n');
		g_string_append(s->open, line);
		if (s->open_kind == BLK_NONE)
			s->open_kind = kind;
		/* a heading is a one-line block: commit right away */
		if (kind == BLK_HEAD) {
			paint_tail(s);
			commit_open(d, s);
			return;
		}
	}

	paint_tail(s);
}

/* Build a temporary "open block + partial current line" and paint it as the
 * live tail. The partial line is truncated to the last complete UTF-8
 * character so we never hand GtkTextBuffer an invalid byte sequence (a
 * multi-byte glyph can be split across streamed chunks). */
static void
paint_partial(AcpStream *s)
{
	const char *lp = s->open_line ? s->open_line->str : "";
	gsize llen = s->open_line ? s->open_line->len : 0;
	const char *valid_end = lp;
	GString *save, *tmp;

	/* clamp to the last valid UTF-8 boundary */
	if (llen && !g_utf8_validate(lp, llen, &valid_end))
		llen = (gsize)(valid_end - lp);

	if (!s->in_fence) {
		save = s->open;
		tmp = g_string_new(save->str);
		if (tmp->len && s->open_kind != BLK_NONE)
			g_string_append_c(tmp, '\n');
		g_string_append_len(tmp, lp, llen);
		s->open = tmp;
		paint_tail(s);
		s->open = save;
	} else {
		save = s->fence_body;
		tmp = g_string_new(save->str);
		if (tmp->len) g_string_append_c(tmp, '\n');
		g_string_append_len(tmp, lp, llen);
		s->fence_body = tmp;
		paint_tail(s);
		s->fence_body = save;
	}
	g_string_free(tmp, TRUE);
}

void
acp_stream_message(AcpData *d, const char *text)
{
	AcpStream *s;
	const char *p;
	gboolean touched_line = FALSE;

	if (!text || !*text)
		return;
	s = stream_get(d);
	if (!s->imhtml)          /* no GTK view (headless frontend): give up */
		return;
	/* a live tool card is always the last thing in the buffer; text that
	 * follows it must seal it first so the card freezes above the new text */
	if (s->card_start)
		seal_live_card(s);
	ensure_open(d, s);

	/* Consume the chunk byte-wise for newline splitting, but only repaint the
	 * live tail ONCE at the end of the chunk (or when a line completes). This
	 * keeps redraw cost proportional to chunks, not bytes, and lets the
	 * UTF-8 clamp in paint_partial() hide any character split across chunks. */
	for (p = text; *p; p++) {
		if (*p == '\n') {
			feed_line(d, s, s->open_line ? s->open_line->str : "");
			if (s->open_line)
				g_string_truncate(s->open_line, 0);
			touched_line = FALSE;
		} else {
			if (!s->open_line)
				s->open_line = g_string_new(NULL);
			g_string_append_c(s->open_line, *p);
			touched_line = TRUE;
		}
	}

	/* paint the trailing partial line once, UTF-8-safe */
	if (touched_line)
		paint_partial(s);
}

void
acp_stream_thought(AcpData *d, const char *text)
{
	AcpStream *s;
	char *esc, *html;
	GString *b;
	const char *p;

	if (!text || !*text)
		return;
	s = stream_get(d);
	if (!s->imhtml)
		return;
	ensure_open(d, s);
	/* thoughts are their own committed block, dim + italic, verbatim */
	if (s->open->len || (s->open_line && s->open_line->len))
		acp_stream_flush(d);

	esc = g_markup_escape_text(text, -1);
	b = g_string_new(NULL);
	for (p = esc; *p; p++) {
		if (*p == '\n') g_string_append(b, "<br>");
		else            g_string_append_c(b, *p);
	}
	html = g_strdup_printf(
	    "<font color=\"" COL_DIM "\"><i>%s</i></font><br>", b->str);
	imhtml_append(s, html);
	imhtml_anchor_tail(s);
	g_free(html);
	g_string_free(b, TRUE);
	g_free(esc);
	gtk_imhtml_scroll_to_end(s->imhtml, FALSE);
}

/* Flush the trailing partial line + open block at end of turn / before a card. */
void
acp_stream_flush(AcpData *d)
{
	AcpStream *s = d->stream;
	if (!s || !s->imhtml)
		return;

	/* fold any partial (newline-less) line into the open block */
	if (s->open_line && s->open_line->len) {
		const char *pl = s->open_line->str;
		/* a closing fence can arrive as the last line with no trailing '\n';
		 * treat it as the fence close, not as a code line. */
		if (s->in_fence &&
		    (strncmp(pl, "```", 3) == 0 || strncmp(pl, "~~~", 3) == 0)) {
			g_string_truncate(s->open_line, 0);
			commit_open(d, s);
			return;
		}
		if (s->in_fence) {
			if (s->fence_body->len)
				g_string_append_c(s->fence_body, '\n');
			g_string_append(s->fence_body, pl);
		} else {
			if (s->open->len)
				g_string_append_c(s->open, '\n');
			g_string_append(s->open, pl);
			if (s->open_kind == BLK_NONE)
				s->open_kind = classify(pl);
		}
		g_string_truncate(s->open_line, 0);
	}
	if (s->open->len || s->in_fence)
		commit_open(d, s);
}

/* Start-of-turn reset. Re-anchors against the (possibly new) widget. */
void
acp_stream_reset(AcpData *d)
{
	AcpStream *s = stream_get(d);
	g_string_truncate(s->open, 0);
	if (s->open_line)  g_string_truncate(s->open_line, 0);
	if (s->fence_body) g_string_truncate(s->fence_body, 0);
	s->in_fence = FALSE;
	g_free(s->fence_lang);
	s->fence_lang = NULL;
	s->open_kind = BLK_NONE;
	s->opened = FALSE;
	/* drop stale mark; a fresh one is anchored on next ensure_open() */
	if (s->tail && s->imhtml && GTK_IS_IMHTML(s->imhtml)) {
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->tail);
	}
	s->tail = NULL;
	/* clear any lingering thinking placeholder mark */
	if (s->type_start && s->imhtml && GTK_IS_IMHTML(s->imhtml))
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->type_start);
	s->type_start = NULL;
	s->typing = FALSE;
	/* drop any stale live-card mark from a previous turn */
	if (s->card_start && s->imhtml && GTK_IS_IMHTML(s->imhtml))
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->card_start);
	s->card_start = NULL;
	g_free(s->card_id);
	s->card_id = NULL;
}

/* Seal the current live tool-card region: append a trailing blank line, move
 * the commit tail past it, drop the mark + id. After this the card's text is
 * frozen and subsequent output appends below it. No-op if none is open. */
static void
seal_live_card(AcpStream *s)
{
	if (!s->card_start)
		return;
	if (s->imhtml && GTK_IS_IMHTML(s->imhtml)) {
		imhtml_append(s, "<br>");   /* breathing room after the card */
		imhtml_anchor_tail(s);
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->card_start);
	}
	s->card_start = NULL;
	g_free(s->card_id);
	s->card_id = NULL;
}

/* Append a pre-rendered HTML card (tool call / plan) after committing text.
 * Used by acp_render.c so cards interleave correctly with the streamed text. */
void
acp_stream_write_card(AcpData *d, const char *html)
{
	AcpStream *s;

	acp_stream_flush(d);
	s = stream_get(d);
	if (s->imhtml) {
		seal_live_card(s);         /* finalize any in-flight live card */
		ensure_open(d, s);
		/* blank line before the card so it doesn't crowd the preceding text */
		imhtml_append(s, "<br>");
		imhtml_append(s, html);
		imhtml_append(s, "<br>");   /* and one after */
		imhtml_anchor_tail(s);
		gtk_imhtml_scroll_to_end(s->imhtml, FALSE);
	} else {
		/* headless fallback: use the message API */
		PurpleConversation *conv;
		PurpleConvIm *im;
		PurpleAccount *acct = purple_connection_get_account(d->gc);
		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
		                                             d->buddy, acct);
		if (!conv)
			conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, acct, d->buddy);
		im = purple_conversation_get_im_data(conv);
		purple_conv_im_write(im, d->buddy, html,
		    PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_RAW, time(NULL));
	}
}

/* Draw / update a LIVE tool card for `id`. The card occupies its own region
 * ([card_start .. end of buffer]) which is deleted + redrawn in place on every
 * call for the same id, so the card animates through pending -> running ->
 * completed without stacking duplicates. A card for a DIFFERENT id (or any
 * text chunk / plan / turn end) seals the previous region first.
 *
 * `terminal` marks the final state: once true the region is sealed after the
 * draw, so the next event appends fresh instead of overwriting this card. */
void
acp_stream_write_live_card(AcpData *d, const char *id, const char *html,
                           gboolean terminal)
{
	AcpStream *s = stream_get(d);
	GtkTextIter at, end;

	if (!s->imhtml) {
		/* headless: just append once, at terminal, via the message path */
		if (terminal)
			acp_stream_write_card(d, html);
		return;
	}

	/* text may be mid-stream: fold it in, but keep the tool card BELOW it */
	acp_stream_flush(d);
	ensure_open(d, s);

	if (s->card_start && s->card_id && id && purple_strequal(s->card_id, id)) {
		/* same card -> replace its region in place */
		gtk_text_buffer_get_iter_at_mark(s->imhtml->text_buffer, &at,
		                                 s->card_start);
		gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &end);
		if (!gtk_text_iter_equal(&at, &end))
			gtk_imhtml_delete(s->imhtml, &at, &end);
		gtk_text_buffer_get_iter_at_mark(s->imhtml->text_buffer, &at,
		                                 s->card_start);
		gtk_imhtml_insert_html_at_iter(s->imhtml, html,
		    GTK_IMHTML_NO_SCROLL | GTK_IMHTML_NO_COMMENTS, &at);
	} else {
		/* different / first card -> seal previous, open a fresh region */
		seal_live_card(s);
		imhtml_append(s, "<br>");        /* breathing room before the card */
		gtk_text_buffer_get_end_iter(s->imhtml->text_buffer, &end);
		s->card_start = gtk_text_buffer_create_mark(s->imhtml->text_buffer,
		                                            NULL, &end, TRUE /*left*/);
		s->card_id = g_strdup(id ? id : "");
		imhtml_append(s, html);
	}
	gtk_imhtml_scroll_to_end(s->imhtml, FALSE);

	if (terminal)
		seal_live_card(s);   /* freeze; next event appends fresh */
}

void
acp_stream_free(AcpData *d)
{
	AcpStream *s = d->stream;
	if (!s)
		return;
	if (s->tail && s->imhtml && GTK_IS_IMHTML(s->imhtml))
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->tail);
	if (s->type_start && s->imhtml && GTK_IS_IMHTML(s->imhtml))
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->type_start);
	if (s->card_start && s->imhtml && GTK_IS_IMHTML(s->imhtml))
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->card_start);
	if (s->open)       g_string_free(s->open, TRUE);
	if (s->open_line)  g_string_free(s->open_line, TRUE);
	if (s->fence_body) g_string_free(s->fence_body, TRUE);
	g_free(s->fence_lang);
	g_free(s);
	d->stream = NULL;
}

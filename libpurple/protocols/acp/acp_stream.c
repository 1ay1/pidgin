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

/* Palette (kept in sync with acp_render.c). */
#define COL_CODE   "#c7254e"
#define COL_DIM    "#888888"
#define COL_HEAD   "#2e3436"
#define COL_QUOTE  "#5c3566"
#define COL_CBG    "#f6f6f6"

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
} AcpStream;

enum { BLK_NONE = 0, BLK_PARA, BLK_LIST, BLK_QUOTE, BLK_HEAD };

/* Forward decls from acp_render.c (shared inline renderer). */
char *acp_md_inline(const char *text);

/* ------------------------------------------------------------------------- *
 *  Widget lookup
 * ------------------------------------------------------------------------- */

static GtkIMHtml *
acp_find_imhtml(AcpData *d)
{
	PurpleAccount *acct = purple_connection_get_account(d->gc);
	PurpleConversation *conv;
	PidginConversation *gtkconv;

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
	                                              d->buddy, acct);
	if (!conv)
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, acct, d->buddy);
	if (!conv)
		return NULL;

	gtkconv = PIDGIN_CONVERSATION(conv);
	if (!gtkconv || !gtkconv->imhtml)
		return NULL;
	return GTK_IMHTML(gtkconv->imhtml);
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

/* Render one markdown line to an HTML fragment (no trailing separator). */
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
		int size = (6 - hl) + 2;
		inner = acp_md_inline(t + hl + (t[hl] == ' ' ? 1 : 0));
		g_string_append_printf(out,
		    "<font color=\"" COL_HEAD "\" size=\"%d\"><b>%s</b></font>",
		    size < 3 ? 3 : size, inner);
		g_free(inner);
		return;
	}
	if (t[0] == '>') {
		inner = acp_md_inline(t[1] == ' ' ? t + 2 : t + 1);
		g_string_append_printf(out,
		    "<font color=\"" COL_QUOTE "\">\xE2\x96\x8E %s</font>", inner);
		g_free(inner);
		return;
	}
	if (is_ulist(t, &body)) {
		inner = acp_md_inline(body);
		if (indent >= 2)
			g_string_append_printf(out,
			    "&#160;&#160;&#160;&#160;\xC2\xB7 %s", inner);   /* nested: middle dot */
		else
			g_string_append_printf(out,
			    "&#160;&#160;\xE2\x80\xA2 %s", inner);           /* top: bullet */
		g_free(inner);
		return;
	}
	if (is_olist(t, &body, &num)) {
		inner = acp_md_inline(body);
		g_string_append_printf(out, "&#160;&#160;%d. %s", num, inner);
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
		/* live code block: language chip + verbatim body in a mono box */
		char *esc = g_markup_escape_text(
		    s->fence_body ? s->fence_body->str : "", -1);
		GString *body = g_string_new(NULL);
		const char *p;
		for (p = esc; *p; p++) {
			if (*p == '\n')      g_string_append(body, "<br>");
			else if (*p == ' ')  g_string_append(body, "&#160;");
			else if (*p == '\t') g_string_append(body, "&#160;&#160;&#160;&#160;");
			else                 g_string_append_c(body, *p);
		}
		if (s->fence_lang && *s->fence_lang)
			g_string_append_printf(out,
			    "<font color=\"" COL_DIM "\" size=\"2\">%s</font><br>",
			    s->fence_lang);
		g_string_append_printf(out,
		    "<font back=\"" COL_CBG "\" face=\"monospace\" "
		    "color=\"" COL_CODE "\" size=\"2\">%s</font>",
		    body->len ? body->str : "&#160;");
		g_string_free(body, TRUE);
		g_free(esc);
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

	/* Draw our own incoming header row directly into the widget: a dim
	 * timestamp + the buddy nick in a colour, matching Pidgin's own
	 * "(HH:MM:SS) nick:" style but fully under our control so the streamed
	 * HTML that follows lands cleanly after it (no message-API interleave). */
	stamp = g_strdup_printf("%02d:%02d:%02d",
	    lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0, lt ? lt->tm_sec : 0);
	nick = g_markup_escape_text(d->buddy ? d->buddy : "agent", -1);
	header = g_strdup_printf(
	    "<font color=\"" COL_DIM "\" size=\"2\">(%s)</font> "
	    "<font color=\"#204a87\"><b>%s:</b></font> ",
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
		if (s->open->len && s->open_kind == BLK_PARA &&
		    (kind == BLK_HEAD || kind == BLK_LIST || kind == BLK_QUOTE)) {
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
		if (s->in_fence) {
			if (s->fence_body->len)
				g_string_append_c(s->fence_body, '\n');
			g_string_append(s->fence_body, s->open_line->str);
		} else {
			if (s->open->len)
				g_string_append_c(s->open, '\n');
			g_string_append(s->open, s->open_line->str);
			if (s->open_kind == BLK_NONE)
				s->open_kind = classify(s->open_line->str);
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
		ensure_open(d, s);
		imhtml_append(s, html);
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

void
acp_stream_free(AcpData *d)
{
	AcpStream *s = d->stream;
	if (!s)
		return;
	if (s->tail && s->imhtml && GTK_IS_IMHTML(s->imhtml))
		gtk_text_buffer_delete_mark(s->imhtml->text_buffer, s->tail);
	if (s->open)       g_string_free(s->open, TRUE);
	if (s->open_line)  g_string_free(s->open_line, TRUE);
	if (s->fence_body) g_string_free(s->fence_body, TRUE);
	g_free(s->fence_lang);
	g_free(s);
	d->stream = NULL;
}

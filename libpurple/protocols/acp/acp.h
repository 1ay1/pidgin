/*
 * ACP protocol plugin -- shared declarations.
 *
 * Copyright (C) 2024 Ayush Bhat <tfeayush@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 (or, at your
 * option, any later version).
 */
#ifndef PIDGIN_ACP_H
#define PIDGIN_ACP_H

#include "internal.h"
#include "connection.h"
#include "conversation.h"

#include <json-glib/json-glib.h>

#define ACP_PRPL_ID          "prpl-acp"
#define ACP_PROTOCOL_VERSION 1

/* Account option pref names. */
#define OPT_COMMAND       "command"
#define OPT_ARGS          "args"
#define OPT_CWD           "cwd"
#define OPT_APPROVE       "auto_approve"
#define OPT_BUDDY         "buddy_name"
#define OPT_SHOW_THOUGHTS "show_thoughts"

typedef struct _AcpData AcpData;

/* Reply handler for a request we sent to the agent. */
typedef void (*AcpReplyCb)(AcpData *d, JsonObject *result, JsonObject *error);

typedef struct {
	AcpReplyCb cb;
} AcpPendingReq;

/* A live tool-call card we are tracking so tool_call_update can revise it. */
typedef struct {
	gchar *id;        /* toolCallId */
	gchar *title;
	gchar *kind;      /* read/edit/execute/... */
	gchar *status;    /* pending/in_progress/completed/failed */
} AcpToolCall;

struct _AcpData {
	PurpleConnection *gc;

	/* subprocess + transport */
	GPid        pid;
	int         write_fd;      /* -> agent stdin  */
	GIOChannel *out_chan;      /* <- agent stdout */
	guint       out_watch;
	guint       child_watch;
	GString    *inbuf;         /* partial stdout line accumulator */

	/* JSON-RPC bookkeeping */
	gint        next_id;
	GHashTable *pending;       /* id (int) -> AcpPendingReq* */
	gchar      *session_id;
	gboolean    initialized;
	gboolean    prompting;

	gchar      *buddy;         /* the agent buddy's name */

	/* ---- streaming render state ------------------------------------- *
	 * The agent's message text arrives as many small chunks. The live,
	 * in-place incremental Markdown renderer (acp_stream.c) owns all of
	 * that; its private state hangs off this opaque pointer. */
	void       *stream;        /* AcpStream* (acp_stream.c)                   */
	gboolean    turn_opened;   /* legacy flag (unused by stream renderer)     */

	GHashTable *tools;         /* toolCallId -> AcpToolCall*                  */
};

/* ---- acp_rpc.c ---------------------------------------------------------- */
void  acp_send_node   (AcpData *d, JsonNode *root);
gint  acp_request     (AcpData *d, const char *method, JsonNode *params, AcpReplyCb cb);
void  acp_notify      (AcpData *d, const char *method, JsonNode *params);
void  acp_reply_result(AcpData *d, JsonNode *id_node, JsonNode *result);
void  acp_reply_error (AcpData *d, JsonNode *id_node, gint code, const char *msg);
void  acp_handle_line (AcpData *d, const char *line);

/* ---- acp_render.c ------------------------------------------------------- */
/* Write raw HTML into the agent conversation (creating it if needed). */
void  acp_conv_write_html(AcpData *d, const char *html, PurpleMessageFlags extra);
/* Convert inline markdown (bold/italic/code/links) in one line (caller frees). */
char *acp_md_inline(const char *text);
/* GFM tables: detect the |---|:--:| separator row; render a framed table from
 * raw lines (header, separator, data rows). acp_render_table caller frees. */
gboolean acp_table_is_separator(const char *line);
char    *acp_render_table(char **lines, int nlines);
/* Render / update a tool-call card. */
void  acp_render_tool_call  (AcpData *d, JsonObject *update, gboolean is_update);
/* Render a plan as a checklist. */
void  acp_render_plan(AcpData *d, JsonArray *entries);

/* ---- acp_stream.c  (live incremental Markdown renderer) ----------------- */
/* Feed a chunk of streamed agent message text (markdown). */
void  acp_stream_message(AcpData *d, const char *text);
/* Feed streamed "thought" text (rendered dimmed, if enabled). */
void  acp_stream_thought(AcpData *d, const char *text);
/* Flush any buffered partial block; call at end of turn / before a card. */
void  acp_stream_flush(AcpData *d);
/* Reset streaming state at the start of a new turn. */
void  acp_stream_reset(AcpData *d);
/* Show/remove an in-transcript "agent is thinking…" placeholder for the turn. */
void  acp_stream_typing_on(AcpData *d);
void  acp_stream_typing_off(AcpData *d);
/* Commit pending text, then append a pre-rendered HTML card (tool/plan). */
void  acp_stream_write_card(AcpData *d, const char *html);
/* Free all streaming state (on connection close). */
void  acp_stream_free(AcpData *d);

/* ---- acp_syntax.c  (fenced-code syntax highlighter) --------------------- */
/* Render a code block body to HTML with a line-number gutter + syntax
 * highlighting (maya-style). lang_tag may be NULL/empty. Caller frees. */
char *acp_highlight_code(const char *code, const char *lang_tag);

#endif /* PIDGIN_ACP_H */

/*
 * ACP - an Agent Client Protocol (ACP) protocol plugin for libpurple.
 *
 * Registers an "ACP Agent" account type. When you sign in, the plugin spawns
 * the configured ACP agent binary as a subprocess, runs the ACP handshake
 * (initialize -> session/new), and -- if that succeeds -- brings you online
 * with a single always-available buddy: the agent itself. Open an IM with it
 * and you are chatting with the agent, exactly like any other contact. What
 * you type becomes an ACP `session/prompt`; the agent's streamed reply,
 * thoughts, plan and tool calls come back as an incoming message.
 *
 * The transport is JSON-RPC 2.0, one JSON value per line, on the agent's
 * stdin/stdout, exactly as the Agent Client Protocol specifies
 * (https://agentclientprotocol.com/). We are the *client* (editor) side.
 *
 * Everything runs on the GLib main loop -- the agent's stdout is watched with
 * a GIOChannel, so there are no worker threads.
 *
 * Copyright (C) 2024 Ayush Bhat <tfeayush@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02111-1301, USA.
 */
#include "internal.h"

#include "accountopt.h"
#include "blist.h"
#include "conversation.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "prpl.h"
#include "server.h"
#include "status.h"
#include "version.h"

#include <json-glib/json-glib.h>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define ACP_PRPL_ID          "prpl-acp"
#define ACP_PROTOCOL_VERSION 1

/* Account option pref names. */
#define OPT_COMMAND    "command"
#define OPT_ARGS       "args"
#define OPT_CWD        "cwd"
#define OPT_APPROVE    "auto_approve"
#define OPT_BUDDY      "buddy_name"
#define OPT_SHOW_THOUGHTS "show_thoughts"

/* ------------------------------------------------------------------------- *
 *  Per-connection state (hangs off gc->proto_data)
 * ------------------------------------------------------------------------- */

typedef struct _AcpData AcpData;

struct _AcpData {
	PurpleConnection *gc;

	GPid   pid;
	int    write_fd;        /* -> agent stdin  */
	GIOChannel *out_chan;   /* <- agent stdout */
	guint  out_watch;
	guint  child_watch;
	GString *inbuf;         /* partial stdout line accumulator */

	gint   next_id;
	GHashTable *pending;    /* request id (int) -> PendingReq* */
	gchar *session_id;
	gboolean initialized;
	gboolean prompting;

	gchar *buddy;           /* the agent buddy's name (from account opts) */

	/* One agent turn == one incoming IM. We accumulate streamed chunks in
	 * `turn` and flush them when the turn ends (stopReason). */
	GString *turn;
};

typedef void (*AcpReplyCb)(AcpData *d, JsonObject *result, JsonObject *error);

typedef struct {
	AcpReplyCb cb;
} PendingReq;

static PurplePlugin *_acp_protocol = NULL;

/* ------------------------------------------------------------------------- *
 *  small helpers
 * ------------------------------------------------------------------------- */

static const char *
acct_str(PurpleAccount *a, const char *opt, const char *dflt)
{
	const char *v = purple_account_get_string(a, opt, dflt);
	return (v && *v) ? v : dflt;
}

/* Append to the current agent turn, converting newlines to <br> for the
 * conversation window (Pidgin renders IM text as HTML). */
static void
turn_append_text(AcpData *d, const char *text)
{
	const char *p;
	char *esc;
	if (!text || !*text)
		return;
	esc = g_markup_escape_text(text, -1);
	for (p = esc; *p; p++) {
		if (*p == '\n')
			g_string_append(d->turn, "<br>");
		else
			g_string_append_c(d->turn, *p);
	}
	g_free(esc);
}

static void
turn_append_markup(AcpData *d, const char *markup)
{
	if (markup)
		g_string_append(d->turn, markup);
}

/* Deliver whatever has accumulated for this turn as one incoming IM. */
static void
turn_flush(AcpData *d)
{
	if (!d->turn || d->turn->len == 0)
		return;
	serv_got_im(d->gc, d->buddy, d->turn->str, PURPLE_MESSAGE_RECV, time(NULL));
	g_string_truncate(d->turn, 0);
}

/* Post a standalone system-ish line immediately (e.g. errors, tool notices). */
static void
acp_system_im(AcpData *d, const char *markup)
{
	serv_got_im(d->gc, d->buddy, markup, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_SYSTEM,
	            time(NULL));
}

/* ------------------------------------------------------------------------- *
 *  Wire I/O
 * ------------------------------------------------------------------------- */

static void
acp_send_node(AcpData *d, JsonNode *root)
{
	JsonGenerator *gen;
	char *line, *withnl;
	gsize total;
	gssize off = 0;

	if (d->write_fd < 0) {
		json_node_free(root);
		return;
	}
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	line = json_generator_to_data(gen, NULL);
	g_object_unref(gen);
	json_node_free(root);

	withnl = g_strdup_printf("%s\n", line);
	total = strlen(withnl);
	purple_debug_misc("acp", "-> %s\n", line);
	while ((gsize)off < total) {
		ssize_t n = write(d->write_fd, withnl + off, total - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			purple_debug_error("acp", "write to agent failed: %s\n",
			                   g_strerror(errno));
			break;
		}
		off += n;
	}
	g_free(withnl);
	g_free(line);
}

static gint
acp_request(AcpData *d, const char *method, JsonNode *params, AcpReplyCb cb)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *root;
	gint id = d->next_id++;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, id);
	json_builder_set_member_name(b, "method");
	json_builder_add_string_value(b, method);
	json_builder_set_member_name(b, "params");
	if (params) json_builder_add_value(b, params);
	else        json_builder_add_null_value(b);
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	g_object_unref(b);

	if (cb) {
		PendingReq *pr = g_new0(PendingReq, 1);
		pr->cb = cb;
		g_hash_table_insert(d->pending, GINT_TO_POINTER(id), pr);
	}
	acp_send_node(d, root);
	return id;
}

static void
acp_notify(AcpData *d, const char *method, JsonNode *params)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *root;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "method");
	json_builder_add_string_value(b, method);
	json_builder_set_member_name(b, "params");
	if (params) json_builder_add_value(b, params);
	else        json_builder_add_null_value(b);
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	g_object_unref(b);
	acp_send_node(d, root);
}

static void
acp_reply_result(AcpData *d, JsonNode *id_node, JsonNode *result)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *root;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "id");
	json_builder_add_value(b, json_node_copy(id_node));
	json_builder_set_member_name(b, "result");
	if (result) json_builder_add_value(b, result);
	else { json_builder_begin_object(b); json_builder_end_object(b); }
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	g_object_unref(b);
	acp_send_node(d, root);
}

static void
acp_reply_error(AcpData *d, JsonNode *id_node, gint code, const char *msg)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *root;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "jsonrpc");
	json_builder_add_string_value(b, "2.0");
	json_builder_set_member_name(b, "id");
	json_builder_add_value(b, json_node_copy(id_node));
	json_builder_set_member_name(b, "error");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "code");
	json_builder_add_int_value(b, code);
	json_builder_set_member_name(b, "message");
	json_builder_add_string_value(b, msg ? msg : "error");
	json_builder_end_object(b);
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	g_object_unref(b);
	acp_send_node(d, root);
}

/* ------------------------------------------------------------------------- *
 *  Handshake: initialize -> session/new
 * ------------------------------------------------------------------------- */

static void acp_on_new_session(AcpData *d, JsonObject *result, JsonObject *error);

static void
acp_bring_online(AcpData *d)
{
	PurpleAccount *acct = purple_connection_get_account(d->gc);
	PurpleGroup *group;
	PurpleBuddy *b;

	/* Ensure the agent buddy exists and show it online. */
	b = purple_find_buddy(acct, d->buddy);
	if (!b) {
		group = purple_find_group("ACP Agents");
		if (!group) {
			group = purple_group_new("ACP Agents");
			purple_blist_add_group(group, NULL);
		}
		b = purple_buddy_new(acct, d->buddy, NULL);
		purple_blist_add_buddy(b, NULL, group, NULL);
	}
	purple_prpl_got_user_status(acct, d->buddy, "available", NULL);
}

static void
acp_on_initialize(AcpData *d, JsonObject *result, JsonObject *error)
{
	PurpleAccount *acct = purple_connection_get_account(d->gc);
	JsonBuilder *b;
	JsonNode *params;

	if (error) {
		purple_connection_error_reason(d->gc,
			PURPLE_CONNECTION_ERROR_OTHER_ERROR,
			_("ACP initialize failed"));
		return;
	}
	d->initialized = TRUE;

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "cwd");
	json_builder_add_string_value(b, acct_str(acct, OPT_CWD, g_get_home_dir()));
	json_builder_set_member_name(b, "mcpServers");
	json_builder_begin_array(b);
	json_builder_end_array(b);
	json_builder_end_object(b);
	params = json_builder_get_root(b);
	g_object_unref(b);

	acp_request(d, "session/new", params, acp_on_new_session);
}

static void
acp_on_new_session(AcpData *d, JsonObject *result, JsonObject *error)
{
	if (error || !result || !json_object_has_member(result, "sessionId")) {
		purple_connection_error_reason(d->gc,
			PURPLE_CONNECTION_ERROR_OTHER_ERROR,
			_("ACP session could not be created"));
		return;
	}
	g_free(d->session_id);
	d->session_id = g_strdup(json_object_get_string_member(result, "sessionId"));

	purple_connection_set_state(d->gc, PURPLE_CONNECTED);
	acp_bring_online(d);
}

static void
acp_do_initialize(AcpData *d)
{
	JsonBuilder *b = json_builder_new();
	JsonNode *params;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "protocolVersion");
	json_builder_add_int_value(b, ACP_PROTOCOL_VERSION);
	json_builder_set_member_name(b, "clientCapabilities");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "fs");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "readTextFile");
	json_builder_add_boolean_value(b, TRUE);
	json_builder_set_member_name(b, "writeTextFile");
	json_builder_add_boolean_value(b, TRUE);
	json_builder_end_object(b);
	json_builder_end_object(b);
	json_builder_set_member_name(b, "clientInfo");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, "pidgin-acp");
	json_builder_set_member_name(b, "version");
	json_builder_add_string_value(b, DISPLAY_VERSION);
	json_builder_end_object(b);
	json_builder_end_object(b);
	params = json_builder_get_root(b);
	g_object_unref(b);

	acp_request(d, "initialize", params, acp_on_initialize);
}

/* ------------------------------------------------------------------------- *
 *  session/prompt
 * ------------------------------------------------------------------------- */

static void
acp_on_prompt_done(AcpData *d, JsonObject *result, JsonObject *error)
{
	const char *reason = "end_turn";
	d->prompting = FALSE;
	serv_got_typing_stopped(d->gc, d->buddy);

	if (error) {
		turn_append_markup(d, "<font color='#cc0000'>[prompt failed]</font>");
	} else if (result && json_object_has_member(result, "stopReason")) {
		reason = json_object_get_string_member(result, "stopReason");
	}
	if (reason && strcmp(reason, "end_turn") != 0 &&
	    strcmp(reason, "cancelled") != 0) {
		char *m = g_strdup_printf("<br><font color='#888888'><i>[%s]</i></font>",
		                          reason);
		turn_append_markup(d, m);
		g_free(m);
	}
	turn_flush(d);
}

static int
acp_send_prompt(AcpData *d, const char *text)
{
	JsonBuilder *b;
	JsonNode *params;
	char *plain;

	if (!d->session_id)
		return -EAGAIN;
	if (d->prompting) {
		acp_system_im(d, _("The agent is still working on the previous turn."));
		return 0;
	}

	/* Conversations hand us HTML; strip it back to text for the prompt. */
	plain = purple_markup_strip_html(text);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "sessionId");
	json_builder_add_string_value(b, d->session_id);
	json_builder_set_member_name(b, "prompt");
	json_builder_begin_array(b);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "text");
	json_builder_set_member_name(b, "text");
	json_builder_add_string_value(b, plain ? plain : "");
	json_builder_end_object(b);
	json_builder_end_array(b);
	json_builder_end_object(b);
	params = json_builder_get_root(b);
	g_object_unref(b);
	g_free(plain);

	d->prompting = TRUE;
	g_string_truncate(d->turn, 0);
	serv_got_typing(d->gc, d->buddy, 0, PURPLE_TYPING);
	acp_request(d, "session/prompt", params, acp_on_prompt_done);
	return 1;
}

/* ------------------------------------------------------------------------- *
 *  session/update streaming
 * ------------------------------------------------------------------------- */

static const char *
content_text(JsonObject *content)
{
	if (content && json_object_has_member(content, "text"))
		return json_object_get_string_member(content, "text");
	return NULL;
}

static void
acp_handle_session_update(AcpData *d, JsonObject *params)
{
	PurpleAccount *acct = purple_connection_get_account(d->gc);
	JsonObject *update;
	const char *kind;

	if (!params || !json_object_has_member(params, "update"))
		return;
	update = json_object_get_object_member(params, "update");
	if (!update || !json_object_has_member(update, "sessionUpdate"))
		return;
	kind = json_object_get_string_member(update, "sessionUpdate");

	if (purple_strequal(kind, "agent_message_chunk")) {
		JsonObject *c = json_object_has_member(update, "content")
		              ? json_object_get_object_member(update, "content") : NULL;
		turn_append_text(d, content_text(c));
		/* keep the typing indicator alive during long turns */
		serv_got_typing(d->gc, d->buddy, 6, PURPLE_TYPING);

	} else if (purple_strequal(kind, "agent_thought_chunk")) {
		if (purple_account_get_bool(acct, OPT_SHOW_THOUGHTS, FALSE)) {
			JsonObject *c = json_object_has_member(update, "content")
			              ? json_object_get_object_member(update, "content") : NULL;
			const char *t = content_text(c);
			if (t && *t) {
				char *esc = g_markup_escape_text(t, -1);
				char *m = g_strdup_printf("<font color='#888888'><i>%s</i></font><br>", esc);
				turn_append_markup(d, m);
				g_free(m);
				g_free(esc);
			}
		}

	} else if (purple_strequal(kind, "tool_call")) {
		const char *title = json_object_has_member(update, "title")
		                  ? json_object_get_string_member(update, "title") : "tool";
		const char *stat = json_object_has_member(update, "status")
		                 ? json_object_get_string_member(update, "status") : "";
		char *esc = g_markup_escape_text(title, -1);
		char *m = g_strdup_printf(
		    "<font color='#a05a00'>\xE2\x9A\x99 %s%s%s</font><br>",
		    esc, (stat && *stat) ? " " : "", stat ? stat : "");
		turn_append_markup(d, m);
		g_free(m);
		g_free(esc);

	} else if (purple_strequal(kind, "tool_call_update")) {
		const char *stat = json_object_has_member(update, "status")
		                 ? json_object_get_string_member(update, "status") : NULL;
		if (stat && (purple_strequal(stat, "completed") ||
		             purple_strequal(stat, "failed"))) {
			char *m = g_strdup_printf(
			    "<font color='#a05a00'>\xE2\x9A\x99 [%s]</font><br>", stat);
			turn_append_markup(d, m);
			g_free(m);
		}

	} else if (purple_strequal(kind, "plan")) {
		JsonArray *entries = json_object_has_member(update, "entries")
		                   ? json_object_get_array_member(update, "entries") : NULL;
		guint i, n = entries ? json_array_get_length(entries) : 0;
		if (n)
			turn_append_markup(d, "<font color='#5c3566'><b>plan:</b></font><br>");
		for (i = 0; i < n; i++) {
			JsonObject *e = json_array_get_object_element(entries, i);
			const char *ec = e && json_object_has_member(e, "content")
			               ? json_object_get_string_member(e, "content") : "";
			const char *es = e && json_object_has_member(e, "status")
			               ? json_object_get_string_member(e, "status") : "";
			char *esc = g_markup_escape_text(ec, -1);
			char *m = g_strdup_printf(
			    "&#160;&#160;&#8226; %s <font color='#888888'>(%s)</font><br>",
			    esc, es);
			turn_append_markup(d, m);
			g_free(m);
			g_free(esc);
		}
	}
}

/* ------------------------------------------------------------------------- *
 *  agent -> client requests
 * ------------------------------------------------------------------------- */

static void
acp_handle_request(AcpData *d, const char *method, JsonNode *id_node,
                   JsonObject *params)
{
	PurpleAccount *acct = purple_connection_get_account(d->gc);

	if (purple_strequal(method, "fs/read_text_file")) {
		const char *path = params && json_object_has_member(params, "path")
		                 ? json_object_get_string_member(params, "path") : NULL;
		gchar *contents = NULL;
		if (path && g_file_get_contents(path, &contents, NULL, NULL)) {
			JsonBuilder *b = json_builder_new();
			JsonNode *res;
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "content");
			json_builder_add_string_value(b, contents);
			json_builder_end_object(b);
			res = json_builder_get_root(b);
			g_object_unref(b);
			acp_reply_result(d, id_node, res);
			g_free(contents);
		} else {
			acp_reply_error(d, id_node, -32000, "cannot read file");
		}

	} else if (purple_strequal(method, "fs/write_text_file")) {
		const char *path = params && json_object_has_member(params, "path")
		                 ? json_object_get_string_member(params, "path") : NULL;
		const char *data = params && json_object_has_member(params, "content")
		                 ? json_object_get_string_member(params, "content") : "";
		if (path && g_file_set_contents(path, data, -1, NULL)) {
			acp_reply_result(d, id_node, NULL);
		} else {
			acp_reply_error(d, id_node, -32000, "cannot write file");
		}

	} else if (purple_strequal(method, "session/request_permission")) {
		JsonArray *opts = params && json_object_has_member(params, "options")
		                ? json_object_get_array_member(params, "options") : NULL;
		const char *chosen = NULL;
		gboolean auto_ok = purple_account_get_bool(acct, OPT_APPROVE, TRUE);
		guint i, n = opts ? json_array_get_length(opts) : 0;
		JsonBuilder *b;
		JsonNode *res;

		for (i = 0; i < n; i++) {
			JsonObject *o = json_array_get_object_element(opts, i);
			const char *okind = o && json_object_has_member(o, "kind")
			                  ? json_object_get_string_member(o, "kind") : "";
			const char *oid = o && json_object_has_member(o, "optionId")
			                ? json_object_get_string_member(o, "optionId") : NULL;
			if (!oid)
				continue;
			if (auto_ok && strstr(okind, "allow")) { chosen = oid; break; }
			if (!auto_ok && strstr(okind, "reject")) { chosen = oid; break; }
			if (!chosen) chosen = oid;  /* fallback */
		}

		b = json_builder_new();
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "outcome");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "outcome");
		json_builder_add_string_value(b, "selected");
		json_builder_set_member_name(b, "optionId");
		json_builder_add_string_value(b, chosen ? chosen : "allow");
		json_builder_end_object(b);
		json_builder_end_object(b);
		res = json_builder_get_root(b);
		g_object_unref(b);
		acp_reply_result(d, id_node, res);

	} else {
		acp_reply_error(d, id_node, -32601, "method not found");
	}
}

/* ------------------------------------------------------------------------- *
 *  dispatch
 * ------------------------------------------------------------------------- */

static void
acp_dispatch(AcpData *d, JsonObject *msg)
{
	gboolean has_method = json_object_has_member(msg, "method");
	gboolean has_id     = json_object_has_member(msg, "id");

	if (has_method && has_id) {
		const char *method = json_object_get_string_member(msg, "method");
		JsonNode *id = json_object_get_member(msg, "id");
		JsonObject *params = json_object_has_member(msg, "params") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(msg, "params"))
		  ? json_object_get_object_member(msg, "params") : NULL;
		acp_handle_request(d, method, id, params);

	} else if (has_method) {
		const char *method = json_object_get_string_member(msg, "method");
		JsonObject *params = json_object_has_member(msg, "params") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(msg, "params"))
		  ? json_object_get_object_member(msg, "params") : NULL;
		if (purple_strequal(method, "session/update"))
			acp_handle_session_update(d, params);

	} else if (has_id) {
		JsonNode *idn = json_object_get_member(msg, "id");
		gint id = JSON_NODE_HOLDS_VALUE(idn) ? (gint)json_node_get_int(idn) : -1;
		PendingReq *pr = g_hash_table_lookup(d->pending, GINT_TO_POINTER(id));
		if (pr) {
			JsonObject *result = json_object_has_member(msg, "result") &&
			    JSON_NODE_HOLDS_OBJECT(json_object_get_member(msg, "result"))
			  ? json_object_get_object_member(msg, "result") : NULL;
			JsonObject *error = json_object_has_member(msg, "error")
			  ? json_object_get_object_member(msg, "error") : NULL;
			if (pr->cb)
				pr->cb(d, result, error);
			g_hash_table_remove(d->pending, GINT_TO_POINTER(id));
		}
	}
}

static void
acp_handle_line(AcpData *d, const char *line)
{
	JsonParser *parser;
	GError *err = NULL;
	JsonNode *root;

	if (!*line)
		return;
	purple_debug_misc("acp", "<- %s\n", line);
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, line, -1, &err)) {
		purple_debug_warning("acp", "bad JSON from agent: %s\n",
		                     err ? err->message : "?");
		g_clear_error(&err);
		g_object_unref(parser);
		return;
	}
	root = json_parser_get_root(parser);
	if (root && JSON_NODE_HOLDS_OBJECT(root))
		acp_dispatch(d, json_node_get_object(root));
	g_object_unref(parser);
}

/* ------------------------------------------------------------------------- *
 *  agent stdout watch
 * ------------------------------------------------------------------------- */

static gboolean
acp_out_readable(GIOChannel *src, GIOCondition cond, gpointer data)
{
	AcpData *d = data;
	char buf[4096];
	gsize nread = 0;
	GIOStatus st = G_IO_STATUS_NORMAL;

	do {
		GError *err = NULL;
		st = g_io_channel_read_chars(src, buf, sizeof(buf), &nread, &err);
		if (nread > 0) {
			gsize i;
			for (i = 0; i < nread; i++) {
				if (buf[i] == '\n') {
					acp_handle_line(d, d->inbuf->str);
					g_string_truncate(d->inbuf, 0);
				} else {
					g_string_append_c(d->inbuf, buf[i]);
				}
			}
		}
		if (err) { g_clear_error(&err); break; }
	} while (st == G_IO_STATUS_NORMAL && nread == sizeof(buf));

	if (st == G_IO_STATUS_EOF || (cond & (G_IO_HUP | G_IO_ERR))) {
		d->out_watch = 0;
		if (purple_connection_get_state(d->gc) != PURPLE_DISCONNECTED)
			purple_connection_error_reason(d->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
				_("The ACP agent process exited."));
		return FALSE;
	}
	return TRUE;
}

static void
acp_child_exited(GPid pid, gint status, gpointer data)
{
	AcpData *d = data;
	g_spawn_close_pid(pid);
	d->pid = 0;
	d->child_watch = 0;
}

/* ------------------------------------------------------------------------- *
 *  prpl ops
 * ------------------------------------------------------------------------- */

static const char *
acp_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
	return "acp";
}

static GList *
acp_status_types(PurpleAccount *acct)
{
	GList *types = NULL;
	PurpleStatusType *t;

	t = purple_status_type_new(PURPLE_STATUS_AVAILABLE, "available",
	                           NULL, TRUE);
	types = g_list_append(types, t);
	t = purple_status_type_new(PURPLE_STATUS_OFFLINE, "offline", NULL, TRUE);
	types = g_list_append(types, t);
	return types;
}

static void
acp_login(PurpleAccount *acct)
{
	PurpleConnection *gc = purple_account_get_connection(acct);
	AcpData *d;
	const char *cmd = acct_str(acct, OPT_COMMAND, "");
	const char *extra = acct_str(acct, OPT_ARGS, "");
	gchar *fullcmd, **argv = NULL;
	GError *err = NULL;
	gint in_fd = -1, out_fd = -1;
	GSpawnFlags flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
	                    G_SPAWN_STDERR_TO_DEV_NULL;

	if (!cmd || !*cmd) {
		purple_connection_error_reason(gc,
			PURPLE_CONNECTION_ERROR_INVALID_SETTINGS,
			_("No agent command is set. Edit the account and set the "
			  "\"Agent command\" field to your ACP agent binary."));
		return;
	}

	d = g_new0(AcpData, 1);
	d->gc = gc;
	d->write_fd = -1;
	d->next_id = 1;
	d->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	d->inbuf = g_string_new(NULL);
	d->turn = g_string_new(NULL);
	d->buddy = g_strdup(acct_str(acct, OPT_BUDDY, "agent"));
	gc->proto_data = d;

	purple_connection_update_progress(gc, _("Launching agent"), 0, 2);

	/* Assemble the command line: command + extra args, verbatim. Whatever
	 * flags the chosen agent needs go in the account's "Arguments" field. */
	fullcmd = g_strdup_printf("%s%s%s", cmd,
	                          (extra && *extra) ? " " : "",
	                          (extra && *extra) ? extra : "");

	if (!g_shell_parse_argv(fullcmd, NULL, &argv, &err)) {
		char *m = g_strdup_printf(_("Bad agent command: %s"),
		                          err ? err->message : "?");
		purple_connection_error_reason(gc,
			PURPLE_CONNECTION_ERROR_INVALID_SETTINGS, m);
		g_free(m);
		g_clear_error(&err);
		g_free(fullcmd);
		return;
	}
	g_free(fullcmd);

	if (!g_spawn_async_with_pipes(acct_str(acct, OPT_CWD, g_get_home_dir()),
	        argv, NULL, flags, NULL, NULL, &d->pid,
	        &in_fd, &out_fd, NULL, &err)) {
		char *m = g_strdup_printf(_("Could not launch the agent: %s"),
		                          err ? err->message : "?");
		purple_connection_error_reason(gc,
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR, m);
		g_free(m);
		g_clear_error(&err);
		g_strfreev(argv);
		return;
	}
	g_strfreev(argv);

	d->write_fd = in_fd;
	d->out_chan = g_io_channel_unix_new(out_fd);
	g_io_channel_set_encoding(d->out_chan, NULL, NULL);
	g_io_channel_set_buffered(d->out_chan, FALSE);
	g_io_channel_set_close_on_unref(d->out_chan, TRUE);
	d->out_watch = g_io_add_watch(d->out_chan, G_IO_IN | G_IO_HUP | G_IO_ERR,
	                              acp_out_readable, d);
	d->child_watch = g_child_watch_add(d->pid, acp_child_exited, d);

	purple_connection_update_progress(gc, _("Initializing session"), 1, 2);
	acp_do_initialize(d);
}

static void
acp_close(PurpleConnection *gc)
{
	AcpData *d = gc->proto_data;
	if (!d)
		return;

	if (d->out_watch)   { g_source_remove(d->out_watch);   d->out_watch = 0; }
	if (d->child_watch) { g_source_remove(d->child_watch); d->child_watch = 0; }
	if (d->write_fd >= 0) { close(d->write_fd); d->write_fd = -1; }
	if (d->out_chan) { g_io_channel_unref(d->out_chan); d->out_chan = NULL; }
	if (d->pid > 0) {
		kill(d->pid, SIGTERM);
		g_spawn_close_pid(d->pid);
		d->pid = 0;
	}
	if (d->pending) g_hash_table_destroy(d->pending);
	if (d->inbuf)   g_string_free(d->inbuf, TRUE);
	if (d->turn)    g_string_free(d->turn, TRUE);
	g_free(d->session_id);
	g_free(d->buddy);
	g_free(d);
	gc->proto_data = NULL;
}

static int
acp_send_im(PurpleConnection *gc, const char *who, const char *message,
            PurpleMessageFlags flags)
{
	AcpData *d = gc->proto_data;
	int r;

	if (!d)
		return -EINVAL;
	if (!purple_strequal(who, d->buddy)) {
		/* Only the agent buddy is a valid recipient. */
		return -ENOTSUP;
	}
	r = acp_send_prompt(d, message);
	if (r == -EAGAIN) {
		acp_system_im(d, _("The agent session is not ready yet."));
		return 1;
	}
	return 1; /* the conversation echoes the outgoing message itself */
}

static void
acp_get_info(PurpleConnection *gc, const char *who)
{
	PurpleNotifyUserInfo *info = purple_notify_user_info_new();
	purple_notify_user_info_add_pair(info, _("Kind"),
	                                 _("Agent Client Protocol agent"));
	purple_notify_userinfo(gc, who, info, NULL, NULL);
	purple_notify_user_info_destroy(info);
}

/* Adding/removing the agent buddy is a no-op on the wire; libpurple keeps it
 * in the local blist. We just make sure it shows online. */
static void
acp_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group)
{
	AcpData *d = gc->proto_data;
	if (d && d->initialized)
		purple_prpl_got_user_status(purple_connection_get_account(gc),
		                            buddy->name, "available", NULL);
}

static void acp_remove_buddy(PurpleConnection *gc, PurpleBuddy *b, PurpleGroup *g) {}

static void
acp_set_status(PurpleAccount *acct, PurpleStatus *status)
{
	/* We are always available while the agent runs; nothing to push. */
}

static void
acp_keepalive(PurpleConnection *gc)
{
	/* nothing: the pipe watch tells us if the agent dies */
}

static gboolean
acp_offline_message(const PurpleBuddy *buddy)
{
	return FALSE;
}

/* ------------------------------------------------------------------------- *
 *  Restart-session account action
 * ------------------------------------------------------------------------- */

static void
acp_action_restart(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	AcpData *d = gc ? gc->proto_data : NULL;
	if (!d || !d->initialized)
		return;
	g_free(d->session_id);
	d->session_id = NULL;
	d->prompting = FALSE;
	acp_do_initialize(d);
	acp_system_im(d, _("Started a fresh agent session."));
}

/* Interrupt the in-flight prompt turn (ACP session/cancel notification). */
static void
acp_action_cancel(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	AcpData *d = gc ? gc->proto_data : NULL;
	JsonBuilder *b;
	JsonNode *params;

	if (!d || !d->session_id || !d->prompting)
		return;

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "sessionId");
	json_builder_add_string_value(b, d->session_id);
	json_builder_end_object(b);
	params = json_builder_get_root(b);
	g_object_unref(b);

	acp_notify(d, "session/cancel", params);
	acp_system_im(d, _("Cancel requested."));
}

static GList *
acp_actions(PurplePlugin *plugin, gpointer context)
{
	GList *l = NULL;
	l = g_list_append(l, purple_plugin_action_new(
	    _("New agent session"), acp_action_restart));
	l = g_list_append(l, purple_plugin_action_new(
	    _("Cancel current turn"), acp_action_cancel));
	return l;
}

/* ------------------------------------------------------------------------- *
 *  plugin definition
 * ------------------------------------------------------------------------- */

static PurplePluginProtocolInfo prpl_info =
{
	OPT_PROTO_NO_PASSWORD | OPT_PROTO_IM_IMAGE,  /* options */
	NULL,                                /* user_splits */
	NULL,                                /* protocol_options (set in init) */
	NO_BUDDY_ICONS,                      /* icon_spec */
	acp_list_icon,                       /* list_icon */
	NULL,                                /* list_emblem */
	NULL,                                /* status_text */
	NULL,                                /* tooltip_text */
	acp_status_types,                    /* status_types */
	NULL,                                /* blist_node_menu */
	NULL,                                /* chat_info */
	NULL,                                /* chat_info_defaults */
	acp_login,                           /* login */
	acp_close,                           /* close */
	acp_send_im,                         /* send_im */
	NULL,                                /* set_info */
	NULL,                                /* send_typing */
	acp_get_info,                        /* get_info */
	acp_set_status,                      /* set_status */
	NULL,                                /* set_idle */
	NULL,                                /* change_passwd */
	acp_add_buddy,                       /* add_buddy */
	NULL,                                /* add_buddies */
	acp_remove_buddy,                    /* remove_buddy */
	NULL,                                /* remove_buddies */
	NULL,                                /* add_permit */
	NULL,                                /* add_deny */
	NULL,                                /* rem_permit */
	NULL,                                /* rem_deny */
	NULL,                                /* set_permit_deny */
	NULL,                                /* join_chat */
	NULL,                                /* reject_chat */
	NULL,                                /* get_chat_name */
	NULL,                                /* chat_invite */
	NULL,                                /* chat_leave */
	NULL,                                /* chat_whisper */
	NULL,                                /* chat_send */
	acp_keepalive,                       /* keepalive */
	NULL,                                /* register_user */
	NULL,                                /* get_cb_info */
	NULL,                                /* get_cb_away */
	NULL,                                /* alias_buddy */
	NULL,                                /* group_buddy */
	NULL,                                /* rename_group */
	NULL,                                /* buddy_free */
	NULL,                                /* convo_closed */
	purple_normalize_nocase,             /* normalize */
	NULL,                                /* set_buddy_icon */
	NULL,                                /* remove_group */
	NULL,                                /* get_cb_real_name */
	NULL,                                /* set_chat_topic */
	NULL,                                /* find_blist_chat */
	NULL,                                /* roomlist_get_list */
	NULL,                                /* roomlist_cancel */
	NULL,                                /* roomlist_expand_category */
	NULL,                                /* can_receive_file */
	NULL,                                /* send_file */
	NULL,                                /* new_xfer */
	acp_offline_message,                 /* offline_message */
	NULL,                                /* whiteboard_prpl_ops */
	NULL,                                /* send_raw */
	NULL,                                /* roomlist_room_serialize */
	NULL,                                /* unregister_user */
	NULL,                                /* send_attention */
	NULL,                                /* get_attention_types */
	sizeof(PurplePluginProtocolInfo),    /* struct_size */
	NULL,                                /* get_account_text_table */
	NULL,                                /* initiate_media */
	NULL,                                /* get_media_caps */
	NULL,                                /* get_moods */
	NULL,                                /* set_public_alias */
	NULL,                                /* get_public_alias */
	NULL,                                /* add_buddy_with_invite */
	NULL,                                /* add_buddies_with_invite */
	NULL,                                /* get_cb_alias */
	NULL,                                /* chat_can_receive_file */
	NULL,                                /* chat_send_file */
};

static void
acp_init(PurplePlugin *plugin)
{
	PurpleAccountOption *opt;
	GList *opts = NULL;

	opt = purple_account_option_string_new(
	    _("Agent command (binary path)"), OPT_COMMAND, "");
	opts = g_list_append(opts, opt);

	opt = purple_account_option_string_new(
	    _("Arguments (agent-specific, e.g. \"acp -w /\")"), OPT_ARGS, "");
	opts = g_list_append(opts, opt);

	opt = purple_account_option_string_new(
	    _("Working directory"), OPT_CWD, g_get_home_dir());
	opts = g_list_append(opts, opt);

	opt = purple_account_option_string_new(
	    _("Agent buddy name"), OPT_BUDDY, "agent");
	opts = g_list_append(opts, opt);

	opt = purple_account_option_bool_new(
	    _("Auto-approve tool permission requests"), OPT_APPROVE, TRUE);
	opts = g_list_append(opts, opt);

	opt = purple_account_option_bool_new(
	    _("Show the agent's thinking"), OPT_SHOW_THOUGHTS, FALSE);
	opts = g_list_append(opts, opt);

	prpl_info.protocol_options = opts;
	_acp_protocol = plugin;
}

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_PROTOCOL,                        /**< type           */
	NULL,                                          /**< ui_requirement */
	0,                                             /**< flags          */
	NULL,                                          /**< dependencies   */
	PURPLE_PRIORITY_DEFAULT,                       /**< priority       */

	ACP_PRPL_ID,                                   /**< id             */
	N_("ACP Agent"),                               /**< name           */
	DISPLAY_VERSION,                               /**< version        */
	                                               /**  summary        */
	N_("Agent Client Protocol (ACP) agents as buddies."),
	                                               /**  description    */
	N_("Sign in to run an ACP agent binary as a subprocess and chat with it "
	   "as an always-online buddy. Configure the agent command, arguments, "
	   "working directory and permissions in the account settings."),
	"Ayush Bhat <tfeayush@gmail.com>",             /**< author         */
	PURPLE_WEBSITE,                                /**< homepage       */

	NULL,                                          /**< load           */
	NULL,                                          /**< unload         */
	NULL,                                          /**< destroy        */

	NULL,                                          /**< ui_info        */
	&prpl_info,                                    /**< extra_info     */
	NULL,                                          /**< prefs_info     */
	acp_actions,                                   /**< actions        */

	NULL,
	NULL,
	NULL,
	NULL
};

PURPLE_INIT_PLUGIN(acp, acp_init, info)

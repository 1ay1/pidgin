/*
 * ACP protocol plugin -- JSON-RPC 2.0 transport + dispatch.
 *
 * Copyright (C) 2024 Ayush Bhat <tfeayush@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 (or, at your
 * option, any later version).
 */
#include "acp.h"

#include "account.h"
#include "debug.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- *
 *  Low-level send: one JSON value per line
 * ------------------------------------------------------------------------- */

void
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
			purple_debug_error("acp", "write failed: %s\n", g_strerror(errno));
			break;
		}
		off += n;
	}
	g_free(withnl);
	g_free(line);
}

gint
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
		AcpPendingReq *pr = g_new0(AcpPendingReq, 1);
		pr->cb = cb;
		g_hash_table_insert(d->pending, GINT_TO_POINTER(id), pr);
	}
	acp_send_node(d, root);
	return id;
}

void
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

void
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

void
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
 *  session/update handling  (streaming)
 * ------------------------------------------------------------------------- */

static const char *
content_text(JsonObject *content)
{
	if (content && json_object_has_member(content, "text"))
		return json_object_get_string_member(content, "text");
	return NULL;
}

static void
handle_session_update(AcpData *d, JsonObject *params)
{
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
		acp_stream_message(d, content_text(c));

	} else if (purple_strequal(kind, "agent_thought_chunk")) {
		PurpleAccount *acct = purple_connection_get_account(d->gc);
		if (purple_account_get_bool(acct, OPT_SHOW_THOUGHTS, FALSE)) {
			JsonObject *c = json_object_has_member(update, "content")
			              ? json_object_get_object_member(update, "content") : NULL;
			acp_stream_thought(d, content_text(c));
		}

	} else if (purple_strequal(kind, "tool_call")) {
		acp_render_tool_call(d, update, FALSE);

	} else if (purple_strequal(kind, "tool_call_update")) {
		acp_render_tool_call(d, update, TRUE);

	} else if (purple_strequal(kind, "plan")) {
		JsonArray *entries = json_object_has_member(update, "entries")
		                   ? json_object_get_array_member(update, "entries") : NULL;
		acp_render_plan(d, entries);
	}
}

/* ------------------------------------------------------------------------- *
 *  agent -> client requests
 * ------------------------------------------------------------------------- */

static void
handle_request(AcpData *d, const char *method, JsonNode *id_node, JsonObject *params)
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
		if (path && g_file_set_contents(path, data, -1, NULL))
			acp_reply_result(d, id_node, NULL);
		else
			acp_reply_error(d, id_node, -32000, "cannot write file");

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
			if (auto_ok && strstr(okind, "allow"))  { chosen = oid; break; }
			if (!auto_ok && strstr(okind, "reject")) { chosen = oid; break; }
			if (!chosen) chosen = oid;
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
 *  dispatch one parsed message
 * ------------------------------------------------------------------------- */

static void
dispatch(AcpData *d, JsonObject *msg)
{
	gboolean has_method = json_object_has_member(msg, "method");
	gboolean has_id     = json_object_has_member(msg, "id");

	if (has_method && has_id) {
		const char *method = json_object_get_string_member(msg, "method");
		JsonNode *id = json_object_get_member(msg, "id");
		JsonObject *params = json_object_has_member(msg, "params") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(msg, "params"))
		  ? json_object_get_object_member(msg, "params") : NULL;
		handle_request(d, method, id, params);

	} else if (has_method) {
		const char *method = json_object_get_string_member(msg, "method");
		JsonObject *params = json_object_has_member(msg, "params") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(msg, "params"))
		  ? json_object_get_object_member(msg, "params") : NULL;
		if (purple_strequal(method, "session/update"))
			handle_session_update(d, params);

	} else if (has_id) {
		JsonNode *idn = json_object_get_member(msg, "id");
		gint id = JSON_NODE_HOLDS_VALUE(idn) ? (gint)json_node_get_int(idn) : -1;
		AcpPendingReq *pr = g_hash_table_lookup(d->pending, GINT_TO_POINTER(id));
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

void
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
		purple_debug_warning("acp", "bad JSON: %s\n", err ? err->message : "?");
		g_clear_error(&err);
		g_object_unref(parser);
		return;
	}
	root = json_parser_get_root(parser);
	if (root && JSON_NODE_HOLDS_OBJECT(root))
		dispatch(d, json_node_get_object(root));
	g_object_unref(parser);
}

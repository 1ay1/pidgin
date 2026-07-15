/*
 * ACP - an Agent Client Protocol (ACP) protocol plugin for libpurple.
 *
 * Registers an "ACP Agent" account type. Signing in spawns the configured ACP
 * agent binary as a subprocess, runs the ACP handshake (initialize ->
 * session/new) and -- on success -- brings you online with a single always-
 * available buddy: the agent. Open an IM with it and you are chatting with the
 * agent, with its replies, thoughts, plans and tool calls streamed live and
 * rendered as rich Markdown -- like a real coding-agent UI, inside Pidgin.
 *
 * This file is the prpl glue (login/close/send_im/account options + process
 * management). The wire protocol lives in acp_rpc.c and all conversation
 * rendering lives in acp_render.c.
 *
 * Transport is JSON-RPC 2.0, one JSON value per line, over the agent's
 * stdin/stdout, exactly as ACP specifies (https://agentclientprotocol.com/).
 * We are the *client* (editor) side. Everything runs on the GLib main loop --
 * the agent's stdout is watched with a GIOChannel, so there are no threads.
 *
 * Copyright (C) 2024 Ayush Bhat <tfeayush@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 (or, at your
 * option, any later version).
 */
#include "acp.h"

#include "accountopt.h"
#include "blist.h"
#include "debug.h"
#include "notify.h"
#include "prpl.h"
#include "server.h"
#include "status.h"
#include "version.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static PurplePlugin *_acp_protocol = NULL;

static const char *
acct_str(PurpleAccount *a, const char *opt, const char *dflt)
{
	const char *v = purple_account_get_string(a, opt, dflt);
	return (v && *v) ? v : dflt;
}

/* Post a standalone system line into the agent conversation. */
static void
acp_system_im(AcpData *d, const char *text)
{
	serv_got_im(d->gc, d->buddy, text,
	            PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_SYSTEM, time(NULL));
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
			PURPLE_CONNECTION_ERROR_OTHER_ERROR, _("ACP initialize failed"));
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

	/* Label our own outgoing lines "You" rather than the account name. */
	purple_connection_set_display_name(d->gc, _("You"));

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

	acp_stream_flush(d);
	d->prompting = FALSE;
	serv_got_typing_stopped(d->gc, d->buddy);

	if (error) {
		acp_conv_write_html(d,
		    "<font color=\"#cc0000\">[prompt failed]</font>", 0);
	} else if (result && json_object_has_member(result, "stopReason")) {
		reason = json_object_get_string_member(result, "stopReason");
	}
	if (reason && strcmp(reason, "end_turn") != 0) {
		char *m = g_strdup_printf(
		    "<font color=\"#888888\" size=\"2\"><i>[%s]</i></font>", reason);
		acp_conv_write_html(d, m, 0);
		g_free(m);
	}
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
	acp_stream_reset(d);
	serv_got_typing(d->gc, d->buddy, 0, PURPLE_TYPING);
	acp_request(d, "session/prompt", params, acp_on_prompt_done);
	return 1;
}

/* ------------------------------------------------------------------------- *
 *  agent stdout watch + process lifecycle
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

static void
free_tool_call(gpointer p)
{
	AcpToolCall *tc = p;
	if (!tc) return;
	g_free(tc->id);
	g_free(tc->title);
	g_free(tc->kind);
	g_free(tc->status);
	g_free(tc);
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

	t = purple_status_type_new(PURPLE_STATUS_AVAILABLE, "available", NULL, TRUE);
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
	d->tools = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_tool_call);
	d->buddy = g_strdup(acct_str(acct, OPT_BUDDY, "agent"));
	gc->proto_data = d;

	purple_connection_update_progress(gc, _("Launching agent"), 0, 2);

	/* command + extra args, verbatim; agent-specific flags go in "Arguments". */
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
	if (d->tools)   g_hash_table_destroy(d->tools);
	if (d->inbuf)    g_string_free(d->inbuf, TRUE);
	acp_stream_free(d);
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
	if (!purple_strequal(who, d->buddy))
		return -ENOTSUP;
	r = acp_send_prompt(d, message);
	if (r == -EAGAIN)
		acp_system_im(d, _("The agent session is not ready yet."));
	return 1;
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

static void
acp_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group)
{
	AcpData *d = gc->proto_data;
	if (d && d->initialized)
		purple_prpl_got_user_status(purple_connection_get_account(gc),
		                            buddy->name, "available", NULL);
}

static void acp_remove_buddy(PurpleConnection *gc, PurpleBuddy *b, PurpleGroup *g) {}
static void acp_set_status(PurpleAccount *acct, PurpleStatus *status) {}
static void acp_keepalive(PurpleConnection *gc) {}
static gboolean acp_offline_message(const PurpleBuddy *buddy) { return FALSE; }

/* ------------------------------------------------------------------------- *
 *  account actions
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
	acp_stream_reset(d);
	acp_do_initialize(d);
	acp_system_im(d, _("Started a fresh agent session."));
}

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
	OPT_PROTO_NO_PASSWORD,                /* options */
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
	N_("Agent Client Protocol (ACP) agents as buddies."),
	N_("Sign in to run an ACP agent binary as a subprocess and chat with it "
	   "as an always-online buddy. Its replies, thoughts, plans and tool "
	   "calls stream live and render as Markdown. Configure the agent "
	   "command, arguments, working directory and permissions in the "
	   "account settings."),
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

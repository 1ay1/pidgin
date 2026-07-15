/*
 * Quick Replies - Text-expansion snippets and canned responses.
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

#include "cmds.h"
#include "conversation.h"
#include "debug.h"
#include "prefs.h"
#include "signals.h"
#include "util.h"
#include "version.h"

#include "gtkplugin.h"

#define QR_PLUGIN_ID   "gtk-quickreplies"

#define PREF_PREFIX     "/plugins/gtk/" QR_PLUGIN_ID
#define PREF_SNIPPETS   PREF_PREFIX "/snippets"   /* string list: "trigger\tvalue" pairs */
#define PREF_INLINE     PREF_PREFIX "/inline"     /* expand :trigger: inside messages */

static PurpleCmdId qr_cmd_id = 0;

/* Return the expansion for a trigger, or NULL. Caller frees. */
static char *
lookup_snippet(const char *trigger)
{
	GList *list, *l;
	char *result = NULL;

	list = purple_prefs_get_string_list(PREF_SNIPPETS);
	for (l = list; l != NULL; l = l->next) {
		const char *entry = l->data;
		const char *tab = strchr(entry, '\t');
		size_t tlen;

		if (tab == NULL)
			continue;
		tlen = tab - entry;
		if (strlen(trigger) == tlen && strncmp(entry, trigger, tlen) == 0) {
			result = g_strdup(tab + 1);
			break;
		}
	}
	g_list_free_full(list, g_free);
	return result;
}

static GList *
all_triggers(void)
{
	GList *list, *l, *out = NULL;

	list = purple_prefs_get_string_list(PREF_SNIPPETS);
	for (l = list; l != NULL; l = l->next) {
		const char *entry = l->data;
		const char *tab = strchr(entry, '\t');
		if (tab != NULL)
			out = g_list_append(out, g_strndup(entry, tab - entry));
	}
	g_list_free_full(list, g_free);
	return out;
}

/* Expand every :trigger: token found in the message. Returns NULL if unchanged. */
static char *
expand_inline(const char *msg)
{
	GString *out;
	const char *p;
	gboolean changed = FALSE;

	if (!purple_prefs_get_bool(PREF_INLINE))
		return NULL;
	if (strchr(msg, ':') == NULL)
		return NULL;

	out = g_string_new(NULL);
	p = msg;
	while (*p) {
		if (*p == ':') {
			const char *end = strchr(p + 1, ':');
			if (end != NULL && end > p + 1) {
				char *trigger = g_strndup(p + 1, end - (p + 1));
				char *val = lookup_snippet(trigger);
				g_free(trigger);
				if (val != NULL) {
					g_string_append(out, val);
					g_free(val);
					p = end + 1;
					changed = TRUE;
					continue;
				}
			}
		}
		g_string_append_c(out, *p);
		p++;
	}

	if (!changed) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

static void
sending_im_cb(PurpleAccount *account, const char *receiver, char **message, void *data)
{
	char *expanded;

	if (message == NULL || *message == NULL)
		return;

	expanded = expand_inline(*message);
	if (expanded != NULL) {
		g_free(*message);
		*message = expanded;
	}
}

static void
sending_chat_cb(PurpleAccount *account, char **message, int id, void *data)
{
	char *expanded;

	if (message == NULL || *message == NULL)
		return;

	expanded = expand_inline(*message);
	if (expanded != NULL) {
		g_free(*message);
		*message = expanded;
	}
}

static PurpleCmdRet
qr_cmd_cb(PurpleConversation *conv, const gchar *cmd, gchar **args,
          gchar **error, void *data)
{
	const char *sub = args ? args[0] : NULL;

	if (sub == NULL || *sub == '\0') {
		/* List all snippets. */
		GList *triggers = all_triggers(), *l;
		GString *msg = g_string_new(_("<b>Quick reply snippets:</b><br/>"));
		if (triggers == NULL)
			g_string_append(msg, _("(none defined yet — add some in the plugin preferences)"));
		for (l = triggers; l != NULL; l = l->next)
			g_string_append_printf(msg, ":%s:<br/>", (char *)l->data);
		purple_conversation_write(conv, NULL, msg->str,
		                          PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_string_free(msg, TRUE);
		g_list_free_full(triggers, g_free);
		return PURPLE_CMD_RET_OK;
	}

	{
		char *val = lookup_snippet(sub);
		if (val == NULL) {
			*error = g_strdup_printf(_("No quick reply named '%s'."), sub);
			return PURPLE_CMD_RET_FAILED;
		}
		/* Send the expanded snippet into the conversation. */
		if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
			purple_conv_im_send(PURPLE_CONV_IM(conv), val);
		else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
			purple_conv_chat_send(PURPLE_CONV_CHAT(conv), val);
		g_free(val);
		return PURPLE_CMD_RET_OK;
	}
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *conv_handle = purple_conversations_get_handle();

	purple_signal_connect(conv_handle, "sending-im-msg", plugin,
	                      PURPLE_CALLBACK(sending_im_cb), NULL);
	purple_signal_connect(conv_handle, "sending-chat-msg", plugin,
	                      PURPLE_CALLBACK(sending_chat_cb), NULL);

	qr_cmd_id = purple_cmd_register("qr", "s", PURPLE_CMD_P_PLUGIN,
	        PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS,
	        NULL, PURPLE_CMD_FUNC(qr_cmd_cb),
	        _("qr &lt;name&gt;:  Send a saved quick reply. Without a name, lists them."),
	        NULL);

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	if (qr_cmd_id != 0) {
		purple_cmd_unregister(qr_cmd_id);
		qr_cmd_id = 0;
	}
	return TRUE;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *pref;

	frame = purple_plugin_pref_frame_new();

	pref = purple_plugin_pref_new_with_label(
	        _("Define snippets, then type :trigger: in any message, or use /qr trigger."));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_INLINE,
	        _("Expand :trigger: tokens inside messages automatically"));
	purple_plugin_pref_frame_add(frame, pref);

	return frame;
}

static PurplePluginUiInfo prefs_info = {
	get_plugin_pref_frame,
	0,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,
	PIDGIN_PLUGIN_TYPE,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,

	QR_PLUGIN_ID,
	N_("Quick Replies"),
	DISPLAY_VERSION,
	N_("Canned responses and text-expansion snippets."),
	N_("Define reusable snippets and drop them into any conversation. Type "
	   ":trigger: inside a message to auto-expand it, or use the /qr command "
	   "to send a saved reply. Great for greetings, links, and boilerplate."),
	"Ayush Bhat <tfeayush@gmail.com>",
	PURPLE_WEBSITE,

	plugin_load,
	plugin_unload,
	NULL,

	NULL,
	NULL,
	&prefs_info,
	NULL,

	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	purple_prefs_add_none(PREF_PREFIX);
	purple_prefs_add_bool(PREF_INLINE, TRUE);

	if (!purple_prefs_exists(PREF_SNIPPETS)) {
		GList *defaults = NULL;
		defaults = g_list_append(defaults, g_strdup("brb\tI'll be right back!"));
		defaults = g_list_append(defaults, g_strdup("omw\tOn my way."));
		defaults = g_list_append(defaults, g_strdup("ty\tThank you so much!"));
		purple_prefs_add_string_list(PREF_SNIPPETS, defaults);
		g_list_free_full(defaults, g_free);
	}
}

PURPLE_INIT_PLUGIN(quickreplies, init_plugin, info)

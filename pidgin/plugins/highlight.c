/*
 * Keyword Highlight - Get pinged when your keywords appear in a chat.
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

#include "conversation.h"
#include "debug.h"
#include "prefs.h"
#include "signals.h"
#include "util.h"
#include "version.h"

#include "gtkplugin.h"

#define HL_PLUGIN_ID   "gtk-keywordhighlight"

#define PREF_PREFIX     "/plugins/gtk/" HL_PLUGIN_ID
#define PREF_KEYWORDS   PREF_PREFIX "/keywords"     /* space/comma separated */
#define PREF_CASE       PREF_PREFIX "/case"         /* case sensitive */
#define PREF_WORD       PREF_PREFIX "/whole_word"   /* match whole words only */
#define PREF_IMS        PREF_PREFIX "/ims"          /* also highlight in IMs */

static gchar **
get_keyword_list(void)
{
	const char *raw = purple_prefs_get_string(PREF_KEYWORDS);
	GPtrArray *arr;
	char *norm, *tok, *saveptr = NULL;

	if (raw == NULL || *raw == '\0')
		return NULL;

	/* Split on commas and whitespace. */
	norm = g_strdup(raw);
	for (char *p = norm; *p; p++)
		if (*p == ',')
			*p = ' ';

	arr = g_ptr_array_new();
	for (tok = strtok_r(norm, " \t\n", &saveptr);
	     tok != NULL;
	     tok = strtok_r(NULL, " \t\n", &saveptr)) {
		if (*tok)
			g_ptr_array_add(arr, g_strdup(tok));
	}
	g_free(norm);

	if (arr->len == 0) {
		g_ptr_array_free(arr, TRUE);
		return NULL;
	}
	g_ptr_array_add(arr, NULL);
	return (gchar **)g_ptr_array_free(arr, FALSE);
}

static gboolean
message_matches(const char *message)
{
	gchar **keywords;
	gboolean case_sensitive, whole_word;
	gboolean matched = FALSE;
	char *haystack;
	int i;

	if (message == NULL || *message == '\0')
		return FALSE;

	keywords = get_keyword_list();
	if (keywords == NULL)
		return FALSE;

	case_sensitive = purple_prefs_get_bool(PREF_CASE);
	whole_word     = purple_prefs_get_bool(PREF_WORD);

	haystack = case_sensitive ? g_strdup(message)
	                          : g_utf8_casefold(message, -1);

	for (i = 0; keywords[i] != NULL && !matched; i++) {
		char *needle = case_sensitive ? g_strdup(keywords[i])
		                              : g_utf8_casefold(keywords[i], -1);

		if (whole_word) {
			if (purple_utf8_has_word(haystack, needle))
				matched = TRUE;
		} else if (strstr(haystack, needle) != NULL) {
			matched = TRUE;
		}
		g_free(needle);
	}

	g_free(haystack);
	g_strfreev(keywords);
	return matched;
}

static gboolean
receiving_chat_cb(PurpleAccount *account, char **sender, char **message,
                  PurpleConversation *conv, PurpleMessageFlags *flags, void *data)
{
	if (message == NULL || *message == NULL)
		return FALSE;

	if (message_matches(*message)) {
		/* Flag it as a nick-highlight: Pidgin colors it, plays the nick
		 * sound, and marks the tab/blist with the "mentioned" state. */
		*flags |= PURPLE_MESSAGE_NICK;
		purple_debug_info("keywordhighlight", "keyword hit in chat message\n");
	}

	return FALSE; /* don't swallow the message */
}

static gboolean
receiving_im_cb(PurpleAccount *account, char **sender, char **message,
                PurpleConversation *conv, PurpleMessageFlags *flags, void *data)
{
	if (!purple_prefs_get_bool(PREF_IMS))
		return FALSE;
	if (message == NULL || *message == NULL)
		return FALSE;

	if (message_matches(*message)) {
		*flags |= PURPLE_MESSAGE_NICK;
		purple_debug_info("keywordhighlight", "keyword hit in IM message\n");
	}

	return FALSE;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *conv_handle = purple_conversations_get_handle();

	purple_signal_connect(conv_handle, "receiving-chat-msg", plugin,
	                      PURPLE_CALLBACK(receiving_chat_cb), NULL);
	purple_signal_connect(conv_handle, "receiving-im-msg", plugin,
	                      PURPLE_CALLBACK(receiving_im_cb), NULL);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	return TRUE;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *pref;

	frame = purple_plugin_pref_frame_new();

	pref = purple_plugin_pref_new_with_name_and_label(PREF_KEYWORDS,
	        _("Keywords (comma or space separated)"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_WORD,
	        _("Match whole words only"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_CASE,
	        _("Case sensitive"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_IMS,
	        _("Also highlight in one-to-one IMs"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_label(
	        _("Matching messages are highlighted, play the mention sound, and "
	          "mark the tab — just like being named directly."));
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

	HL_PLUGIN_ID,
	N_("Keyword Highlight"),
	DISPLAY_VERSION,
	N_("Get pinged when your keywords are mentioned in a chat."),
	N_("Watch busy group chats for the words you care about — your name, a "
	   "project, a topic. Matching messages are highlighted, trigger the "
	   "mention sound, and flag the conversation, so you never miss the "
	   "lines meant for you."),
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
	purple_prefs_add_string(PREF_KEYWORDS, "");
	purple_prefs_add_bool(PREF_CASE, FALSE);
	purple_prefs_add_bool(PREF_WORD, TRUE);
	purple_prefs_add_bool(PREF_IMS, FALSE);
}

PURPLE_INIT_PLUGIN(keywordhighlight, init_plugin, info)

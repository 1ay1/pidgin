/*
 * Message Splitter - Break long outgoing messages into multiple parts.
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

#define SPLIT_PLUGIN_ID   "gtk-messagesplitter"

#define PREF_PREFIX     "/plugins/gtk/" SPLIT_PLUGIN_ID
#define PREF_LIMIT      PREF_PREFIX "/limit"     /* max chars per part */
#define PREF_COUNTER    PREF_PREFIX "/counter"   /* append (n/m) markers */
#define PREF_WORDWRAP   PREF_PREFIX "/wordwrap"  /* prefer splitting at spaces */

/* Guard against recursion: the extra parts we send re-enter sending-im-msg. */
static gboolean in_split = FALSE;

/*
 * Split text into a list of newly-allocated chunks each at most `limit`
 * characters (unicode-aware). If wordwrap, try to break at the last space.
 */
static GList *
split_text(const char *text, int limit, gboolean wordwrap)
{
	GList *parts = NULL;
	const char *p = text;

	while (*p) {
		glong remaining = g_utf8_strlen(p, -1);
		const char *end;

		if (remaining <= limit) {
			parts = g_list_append(parts, g_strdup(p));
			break;
		}

		/* Advance `limit` UTF-8 chars. */
		end = g_utf8_offset_to_pointer(p, limit);

		if (wordwrap) {
			/* Walk back to the last whitespace within this chunk. */
			const char *scan = end;
			const char *brk = NULL;
			while (scan > p) {
				gunichar c;
				const char *prev = g_utf8_prev_char(scan);
				c = g_utf8_get_char(prev);
				if (g_unichar_isspace(c)) {
					brk = scan; /* split after this run start */
					break;
				}
				scan = prev;
			}
			if (brk != NULL && brk > p)
				end = brk;
		}

		parts = g_list_append(parts, g_strndup(p, end - p));

		/* Skip leading spaces of the next chunk when wordwrapping. */
		p = end;
		if (wordwrap) {
			while (*p && g_unichar_isspace(g_utf8_get_char(p)))
				p = g_utf8_next_char(p);
		}
	}

	return parts;
}

static void
send_extra_part(PurpleConversation *conv, const char *text)
{
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
		purple_conv_im_send(PURPLE_CONV_IM(conv), text);
	else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
		purple_conv_chat_send(PURPLE_CONV_CHAT(conv), text);
}

static PurpleConversation *
find_conv(PurpleAccount *account, const char *name, PurpleConversationType type)
{
	return purple_find_conversation_with_account(type, name, account);
}

static void
do_split(PurpleConversation *conv, PurpleAccount *account, const char *name,
         char **message, PurpleConversationType type)
{
	int limit;
	gboolean counter, wordwrap;
	GList *parts, *l;
	guint total, idx;

	if (in_split)
		return;
	if (message == NULL || *message == NULL)
		return;

	limit = purple_prefs_get_int(PREF_LIMIT);
	if (limit < 20)
		limit = 20;

	if (g_utf8_strlen(*message, -1) <= limit)
		return;

	counter  = purple_prefs_get_bool(PREF_COUNTER);
	wordwrap = purple_prefs_get_bool(PREF_WORDWRAP);

	/* Reserve room for the "(n/m) " counter prefix. */
	parts = split_text(*message, counter ? limit - 8 : limit, wordwrap);
	total = g_list_length(parts);
	if (total <= 1) {
		g_list_free_full(parts, g_free);
		return;
	}

	if (conv == NULL)
		conv = find_conv(account, name, type);
	if (conv == NULL) {
		g_list_free_full(parts, g_free);
		return;
	}

	purple_debug_info("messagesplitter", "splitting message into %u parts\n", total);

	in_split = TRUE;

	/* The first part replaces the original outgoing message; the rest we
	 * send ourselves. This keeps the "sent" echo natural for part 1. */
	idx = 1;
	for (l = parts; l != NULL; l = l->next, idx++) {
		char *chunk = l->data;
		char *out;

		if (counter)
			out = g_strdup_printf("(%u/%u) %s", idx, total, chunk);
		else
			out = g_strdup(chunk);

		if (l == parts) {
			g_free(*message);
			*message = out;   /* becomes part 1, sent by the core */
		} else {
			send_extra_part(conv, out);
			g_free(out);
		}
	}

	in_split = FALSE;
	g_list_free_full(parts, g_free);
}

static void
sending_im_cb(PurpleAccount *account, const char *receiver, char **message, void *data)
{
	do_split(NULL, account, receiver, message, PURPLE_CONV_TYPE_IM);
}

static void
sending_chat_cb(PurpleAccount *account, char **message, int id, void *data)
{
	/* Chats are looked up by id; find the conv via the account's convs. */
	GList *l;
	PurpleConversation *conv = NULL;

	for (l = purple_get_chats(); l != NULL; l = l->next) {
		PurpleConversation *c = l->data;
		if (purple_conversation_get_account(c) == account &&
		    purple_conv_chat_get_id(PURPLE_CONV_CHAT(c)) == id) {
			conv = c;
			break;
		}
	}
	do_split(conv, account, conv ? purple_conversation_get_name(conv) : NULL,
	         message, PURPLE_CONV_TYPE_CHAT);
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *conv_handle = purple_conversations_get_handle();

	purple_signal_connect(conv_handle, "sending-im-msg", plugin,
	                      PURPLE_CALLBACK(sending_im_cb), NULL);
	purple_signal_connect(conv_handle, "sending-chat-msg", plugin,
	                      PURPLE_CALLBACK(sending_chat_cb), NULL);
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

	pref = purple_plugin_pref_new_with_name_and_label(PREF_LIMIT,
	        _("Maximum characters per message part"));
	purple_plugin_pref_set_bounds(pref, 20, 5000);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_WORDWRAP,
	        _("Split at word boundaries when possible"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_COUNTER,
	        _("Prefix each part with a (n/m) counter"));
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

	SPLIT_PLUGIN_ID,
	N_("Message Splitter"),
	DISPLAY_VERSION,
	N_("Automatically breaks long messages into multiple parts."),
	N_("Protocols like IRC and SMS reject or truncate long messages. This "
	   "plugin transparently splits anything over your chosen length into "
	   "several messages, optionally on word boundaries and with (n/m) "
	   "counters so the recipient knows more is coming."),
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
	purple_prefs_add_int(PREF_LIMIT, 400);
	purple_prefs_add_bool(PREF_COUNTER, TRUE);
	purple_prefs_add_bool(PREF_WORDWRAP, TRUE);
}

PURPLE_INIT_PLUGIN(messagesplitter, init_plugin, info)

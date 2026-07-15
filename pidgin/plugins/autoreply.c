/*
 * Auto Reply - Send a customizable automatic reply when you are away or idle.
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
#include "status.h"
#include "util.h"
#include "version.h"

#include "gtkplugin.h"
#include "gtkutils.h"

#define AUTOREPLY_PLUGIN_ID   "gtk-autoreply"

#define PREF_PREFIX     "/plugins/gtk/" AUTOREPLY_PLUGIN_ID
#define PREF_ENABLED    PREF_PREFIX "/enabled"
#define PREF_MESSAGE    PREF_PREFIX "/message"
#define PREF_WHEN       PREF_PREFIX "/when"       /* 0 = away only, 1 = away+idle, 2 = always */
#define PREF_COOLDOWN   PREF_PREFIX "/cooldown"   /* minutes between replies to same buddy */
#define PREF_MAXPER     PREF_PREFIX "/max_per_conv"

#define WHEN_AWAY   0
#define WHEN_IDLE   1
#define WHEN_ALWAYS 2

/* Per-account+buddy last-reply bookkeeping. Key = "account_id\0buddy", value = ReplyState* */
typedef struct {
	time_t last;
	int    count;
} ReplyState;

static GHashTable *reply_states = NULL;

static gboolean
should_autoreply(PurpleAccount *account)
{
	PurpleStatus *status;
	PurpleStatusType *type;
	PurpleStatusPrimitive prim;
	int when;

	if (!purple_prefs_get_bool(PREF_ENABLED))
		return FALSE;

	when = purple_prefs_get_int(PREF_WHEN);
	if (when == WHEN_ALWAYS)
		return TRUE;

	status = purple_account_get_active_status(account);
	if (status == NULL)
		return FALSE;

	type = purple_status_get_type(status);
	prim = purple_status_type_get_primitive(type);

	if (prim == PURPLE_STATUS_AWAY ||
	    prim == PURPLE_STATUS_EXTENDED_AWAY ||
	    prim == PURPLE_STATUS_UNAVAILABLE)
		return TRUE;

	if (when == WHEN_IDLE) {
		PurplePresence *presence = purple_account_get_presence(account);
		if (presence != NULL && purple_presence_is_idle(presence))
			return TRUE;
	}

	return FALSE;
}

static char *
expand_message(PurpleAccount *account, const char *who)
{
	const char *tmpl = purple_prefs_get_string(PREF_MESSAGE);
	GString *out;
	const char *p;
	const char *alias = who;
	PurpleBuddy *buddy;

	if (tmpl == NULL || *tmpl == '\0')
		tmpl = _("I am currently away and will reply as soon as I can.");

	buddy = purple_find_buddy(account, who);
	if (buddy != NULL)
		alias = purple_buddy_get_contact_alias(buddy);

	out = g_string_new(NULL);
	for (p = tmpl; *p; p++) {
		if (*p == '%' && *(p + 1)) {
			p++;
			switch (*p) {
			case 'n':  /* buddy name/alias */
				g_string_append(out, alias);
				break;
			case 't': { /* current time */
				time_t now = time(NULL);
				g_string_append(out, purple_time_format(localtime(&now)));
				break;
			}
			case 'm': { /* my name on this account */
				const char *me = purple_account_get_alias(account);
				if (me == NULL || *me == '\0')
					me = purple_account_get_username(account);
				g_string_append(out, me);
				break;
			}
			case '%':
				g_string_append_c(out, '%');
				break;
			default:
				g_string_append_c(out, '%');
				g_string_append_c(out, *p);
				break;
			}
		} else {
			g_string_append_c(out, *p);
		}
	}

	return g_string_free(out, FALSE);
}

static void
received_im_cb(PurpleAccount *account, char *sender, char *message,
               PurpleConversation *conv, PurpleMessageFlags flags, void *data)
{
	ReplyState *st;
	char *key;
	time_t now = time(NULL);
	int cooldown, maxper;
	char *reply;
	PurpleConvIm *im;

	/* Ignore our own echoes and system/auto messages. */
	if (flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_AUTO_RESP))
		return;

	if (sender == NULL || *sender == '\0')
		return;

	if (!should_autoreply(account))
		return;

	cooldown = purple_prefs_get_int(PREF_COOLDOWN) * 60;
	maxper   = purple_prefs_get_int(PREF_MAXPER);

	key = g_strdup_printf("%s%c%s",
	                      purple_account_get_username(account), '\1', sender);

	st = g_hash_table_lookup(reply_states, key);
	if (st == NULL) {
		st = g_new0(ReplyState, 1);
		g_hash_table_insert(reply_states, g_strdup(key), st);
	}
	g_free(key);

	/* Throttle: honor cooldown window and per-conversation cap. */
	if (st->last != 0 && cooldown > 0 && (now - st->last) < cooldown)
		return;
	if (maxper > 0 && st->count >= maxper)
		return;

	if (conv == NULL)
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, sender);
	if (conv == NULL || purple_conversation_get_type(conv) != PURPLE_CONV_TYPE_IM)
		return;

	reply = expand_message(account, sender);
	im = PURPLE_CONV_IM(conv);
	purple_conv_im_send_with_flags(im, reply, PURPLE_MESSAGE_AUTO_RESP);
	g_free(reply);

	st->last = now;
	st->count++;

	purple_debug_info("autoreply", "auto-replied to %s on %s\n",
	                  sender, purple_account_get_username(account));
}

static void
sent_im_cb(PurpleAccount *account, const char *receiver, const char *message, void *data)
{
	/* When *I* deliberately message someone, reset their auto-reply cap so
	 * a fresh conversation gets a fresh reply budget. */
	char *key;
	ReplyState *st;

	if (receiver == NULL)
		return;

	key = g_strdup_printf("%s%c%s",
	                      purple_account_get_username(account), '\1', receiver);
	st = g_hash_table_lookup(reply_states, key);
	if (st != NULL)
		st->count = 0;
	g_free(key);
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *conv_handle = purple_conversations_get_handle();

	reply_states = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                     g_free, g_free);

	purple_signal_connect(conv_handle, "received-im-msg", plugin,
	                      PURPLE_CALLBACK(received_im_cb), NULL);
	purple_signal_connect(conv_handle, "sent-im-msg", plugin,
	                      PURPLE_CALLBACK(sent_im_cb), NULL);

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	if (reply_states != NULL) {
		g_hash_table_destroy(reply_states);
		reply_states = NULL;
	}
	return TRUE;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *pref;

	frame = purple_plugin_pref_frame_new();

	pref = purple_plugin_pref_new_with_name_and_label(PREF_ENABLED,
	                _("_Enable auto-reply"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_WHEN,
	                _("Reply when I am"));
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(pref, _("Away"),        GINT_TO_POINTER(WHEN_AWAY));
	purple_plugin_pref_add_choice(pref, _("Away or idle"), GINT_TO_POINTER(WHEN_IDLE));
	purple_plugin_pref_add_choice(pref, _("Always"),      GINT_TO_POINTER(WHEN_ALWAYS));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_MESSAGE,
	                _("Auto-reply message"));
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_STRING_FORMAT);
	purple_plugin_pref_set_format_type(pref, PURPLE_STRING_FORMAT_TYPE_MULTILINE);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_label(
	                _("Substitutions: %n = their name, %m = your name, %t = time, %% = percent."));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_COOLDOWN,
	                _("Minimum minutes between replies to the same person"));
	purple_plugin_pref_set_bounds(pref, 0, 1440);
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_MAXPER,
	                _("Maximum replies per conversation (0 = no limit)"));
	purple_plugin_pref_set_bounds(pref, 0, 100);
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

	AUTOREPLY_PLUGIN_ID,
	N_("Auto Reply"),
	DISPLAY_VERSION,
	N_("Sends a customizable automatic reply when you are away or idle."),
	N_("Automatically responds to incoming messages while you are away or "
	   "idle, with smart throttling so the same person is not spammed. "
	   "Supports %n/%m/%t substitutions and a per-conversation reply cap."),
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
	purple_prefs_add_bool(PREF_ENABLED, FALSE);
	purple_prefs_add_string(PREF_MESSAGE,
	        _("I am currently away and will reply as soon as I can, %n."));
	purple_prefs_add_int(PREF_WHEN, WHEN_AWAY);
	purple_prefs_add_int(PREF_COOLDOWN, 10);
	purple_prefs_add_int(PREF_MAXPER, 3);
}

PURPLE_INIT_PLUGIN(autoreply, init_plugin, info)

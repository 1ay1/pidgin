/*
 * Focus Mode - Silence and queue incoming messages while you concentrate,
 * then get a digest of everything you missed.
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
#include "sound.h"
#include "util.h"
#include "version.h"

#include "gtkplugin.h"
#include "gtkconv.h"

#define FOCUS_PLUGIN_ID   "gtk-focusmode"

#define PREF_PREFIX     "/plugins/gtk/" FOCUS_PLUGIN_ID
#define PREF_ACTIVE     PREF_PREFIX "/active"
#define PREF_KEEP_URGENT PREF_PREFIX "/keep_urgent"  /* let keyword pings through */
#define PREF_URGENT_KW  PREF_PREFIX "/urgent_keywords"

/* Per-sender tally of what arrived while focused. */
typedef struct {
	char *sender;
	int   count;
	char *last;      /* last message text (stripped) */
	time_t when;
} MissEntry;

static GHashTable *missed = NULL;   /* sender -> MissEntry* */
static int total_missed = 0;

static void
miss_free(MissEntry *m)
{
	if (m == NULL)
		return;
	g_free(m->sender);
	g_free(m->last);
	g_free(m);
}

static gboolean
focus_active(void)
{
	return purple_prefs_get_bool(PREF_ACTIVE);
}

static gboolean
message_is_urgent(const char *message)
{
	const char *raw;
	char *hay, *kw, *tok, *save = NULL;
	gboolean hit = FALSE;

	if (!purple_prefs_get_bool(PREF_KEEP_URGENT))
		return FALSE;

	raw = purple_prefs_get_string(PREF_URGENT_KW);
	if (raw == NULL || *raw == '\0')
		return FALSE;

	hay = g_utf8_casefold(message, -1);
	kw  = g_utf8_casefold(raw, -1);

	for (tok = strtok_r(kw, " ,\t", &save); tok && !hit; tok = strtok_r(NULL, " ,\t", &save)) {
		if (*tok && strstr(hay, tok) != NULL)
			hit = TRUE;
	}

	g_free(hay);
	g_free(kw);
	return hit;
}

static void
record_miss(const char *sender, const char *message)
{
	MissEntry *m = g_hash_table_lookup(missed, sender);
	char *stripped = purple_markup_strip_html(message);

	if (m == NULL) {
		m = g_new0(MissEntry, 1);
		m->sender = g_strdup(sender);
		g_hash_table_insert(missed, m->sender, m);
	}
	m->count++;
	g_free(m->last);
	m->last = stripped;   /* takes ownership */
	m->when = time(NULL);
	total_missed++;
}

/*
 * Gate incoming IMs. Returning TRUE from a "receiving-*-msg" handler tells the
 * core to DROP the message entirely (no window, no sound, no notification).
 * We record it for the digest and swallow it -- unless it matches an urgent
 * keyword, in which case we let it through untouched.
 */
static gboolean
receiving_im_cb(PurpleAccount *account, char **sender, char **message,
                PurpleConversation *conv, PurpleMessageFlags *flags, void *data)
{
	if (!focus_active())
		return FALSE;
	if (sender == NULL || *sender == NULL || message == NULL || *message == NULL)
		return FALSE;

	if (message_is_urgent(*message))
		return FALSE;   /* urgent: deliver normally */

	record_miss(*sender, *message);
	purple_debug_info("focusmode", "held a message from %s\n", *sender);
	return TRUE;         /* swallow */
}

static void
emit_digest(void)
{
	GList *convs;
	PurpleConversation *target = NULL;
	GHashTableIter it;
	gpointer key, val;
	GString *digest;

	if (total_missed == 0)
		return;

	/* Prefer an already-open conversation to print the digest into; else the
	 * debug log is our fallback. */
	convs = purple_get_conversations();
	if (convs != NULL)
		target = convs->data;

	digest = g_string_new(NULL);
	g_string_append_printf(digest,
		ngettext("<b>Focus mode ended.</b> You missed %d message:",
		         "<b>Focus mode ended.</b> You missed %d messages:",
		         total_missed), total_missed);
	g_string_append(digest, "<br/>");

	g_hash_table_iter_init(&it, missed);
	while (g_hash_table_iter_next(&it, &key, &val)) {
		MissEntry *m = val;
		char *esc = m->last ? g_markup_escape_text(m->last, -1) : g_strdup("");
		g_string_append_printf(digest,
			"\342\200\242 <b>%s</b> (%d): %s<br/>",
			m->sender, m->count, esc);
		g_free(esc);
	}

	if (target != NULL)
		purple_conversation_write(target, NULL, digest->str,
			PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
	else
		purple_debug_info("focusmode", "%s\n", digest->str);

	g_string_free(digest, TRUE);
}

static void
reset_state(void)
{
	g_hash_table_remove_all(missed);
	total_missed = 0;
}

static void
active_pref_cb(const char *name, PurplePrefType type,
               gconstpointer val, gpointer data)
{
	gboolean now = GPOINTER_TO_INT(val);

	if (now) {
		reset_state();
		purple_debug_info("focusmode", "focus mode ON\n");
	} else {
		emit_digest();
		reset_state();
		purple_debug_info("focusmode", "focus mode OFF\n");
	}
}

static void
toggle_focus_action(PurplePluginAction *action)
{
	purple_prefs_set_bool(PREF_ACTIVE, !focus_active());
}

static GList *
plugin_actions(PurplePlugin *plugin, gpointer context)
{
	GList *list = NULL;
	const char *label = focus_active()
		? _("Turn Focus Mode OFF (show what I missed)")
		: _("Turn Focus Mode ON");
	list = g_list_append(list,
		purple_plugin_action_new(label, toggle_focus_action));
	return list;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	missed = g_hash_table_new_full(g_str_hash, g_str_equal,
	                               NULL, (GDestroyNotify)miss_free);

	purple_signal_connect(purple_conversations_get_handle(), "receiving-im-msg",
	        plugin, PURPLE_CALLBACK(receiving_im_cb), NULL);

	purple_prefs_connect_callback(plugin, PREF_ACTIVE, active_pref_cb, NULL);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	purple_prefs_disconnect_by_handle(plugin);
	if (missed != NULL) {
		g_hash_table_destroy(missed);
		missed = NULL;
	}
	total_missed = 0;
	return TRUE;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *pref;

	frame = purple_plugin_pref_frame_new();

	pref = purple_plugin_pref_new_with_name_and_label(PREF_ACTIVE,
	        _("Focus mode active (hold and queue incoming IMs)"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_KEEP_URGENT,
	        _("Still let urgent messages through"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_URGENT_KW,
	        _("Urgent keywords (comma/space separated)"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_label(
	        _("Toggle quickly from Tools \342\206\222 Focus Mode. When you turn it "
	          "off you'll get a digest of everyone who messaged you."));
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

	FOCUS_PLUGIN_ID,
	N_("Focus Mode"),
	DISPLAY_VERSION,
	N_("Silence and queue incoming messages, then digest what you missed."),
	N_("Get into deep work without leaving Pidgin. Focus mode holds back "
	   "incoming IMs -- no popups, no sounds, no blinking tabs -- and when you "
	   "switch it off it hands you a tidy digest of who tried to reach you and "
	   "what they said. Optionally let messages containing urgent keywords "
	   "break through."),
	"Ayush Bhat <tfeayush@gmail.com>",
	PURPLE_WEBSITE,

	plugin_load,
	plugin_unload,
	NULL,

	NULL,
	NULL,
	&prefs_info,
	plugin_actions,

	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	purple_prefs_add_none(PREF_PREFIX);
	purple_prefs_add_bool(PREF_ACTIVE, FALSE);
	purple_prefs_add_bool(PREF_KEEP_URGENT, TRUE);
	purple_prefs_add_string(PREF_URGENT_KW, "urgent, asap, emergency");
}

PURPLE_INIT_PLUGIN(focusmode, init_plugin, info)

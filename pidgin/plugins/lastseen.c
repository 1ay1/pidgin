/*
 * Last Seen - Remember and display when each buddy was last online.
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

#include "blist.h"
#include "debug.h"
#include "prefs.h"
#include "signals.h"
#include "util.h"
#include "version.h"

#include "gtkblist.h"
#include "gtkplugin.h"

#define LS_PLUGIN_ID   "gtk-lastseen"

/* Per-buddy persisted keys (stored on the blist node, saved in blist.xml). */
#define KEY_LASTSEEN   "lastseen::time"     /* time_t of last sign-off/seen-online */

#define PREF_PREFIX     "/plugins/gtk/" LS_PLUGIN_ID
#define PREF_TOOLTIP    PREF_PREFIX "/tooltip"   /* show in buddy tooltip */

/* Produce a human-friendly relative time string. Caller frees. */
static char *
humanize(time_t then)
{
	time_t now = time(NULL);
	long diff = (long)(now - then);
	long d, h, m;

	if (then <= 0)
		return NULL;
	if (diff < 0)
		diff = 0;

	if (diff < 60)
		return g_strdup(_("just now"));

	m = diff / 60;
	if (m < 60)
		return g_strdup_printf(ngettext("%ld minute ago", "%ld minutes ago", m), m);

	h = diff / 3600;
	if (h < 24)
		return g_strdup_printf(ngettext("%ld hour ago", "%ld hours ago", h), h);

	d = diff / 86400;
	if (d < 30)
		return g_strdup_printf(ngettext("%ld day ago", "%ld days ago", d), d);

	/* Older than a month: give the calendar date. */
	{
		struct tm *tm = localtime(&then);
		char buf[64];
		strftime(buf, sizeof(buf), "%x", tm);
		return g_strdup_printf(_("on %s"), buf);
	}
}

static void
mark_seen(PurpleBuddy *buddy)
{
	if (buddy == NULL)
		return;
	purple_blist_node_set_int(PURPLE_BLIST_NODE(buddy), KEY_LASTSEEN,
	                          (int)time(NULL));
}

static void
buddy_signed_on_cb(PurpleBuddy *buddy, gpointer data)
{
	/* Stamp on sign-on so "last seen" tracks online presence continuously
	 * as long as they are online (updated again at sign-off). */
	mark_seen(buddy);
}

static void
buddy_signed_off_cb(PurpleBuddy *buddy, gpointer data)
{
	mark_seen(buddy);
	purple_debug_info("lastseen", "recorded sign-off for %s\n",
	                  purple_buddy_get_name(buddy));
}

static void
drawing_tooltip_cb(PurpleBlistNode *node, GString *text, gboolean full, gpointer data)
{
	PurpleBuddy *buddy;
	int last;
	char *human;

	if (!purple_prefs_get_bool(PREF_TOOLTIP))
		return;
	if (node == NULL || purple_blist_node_get_type(node) != PURPLE_BLIST_BUDDY_NODE)
		return;

	buddy = (PurpleBuddy *)node;

	/* Online right now? Nothing useful to add. */
	if (PURPLE_BUDDY_IS_ONLINE(buddy))
		return;

	last = purple_blist_node_get_int(node, KEY_LASTSEEN);
	if (last <= 0)
		return;

	human = humanize((time_t)last);
	if (human == NULL)
		return;

	if (text->len && text->str[text->len - 1] != '\n')
		g_string_append_c(text, '\n');
	g_string_append_printf(text, _("<b>Last seen:</b> %s"), human);
	g_free(human);
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *blist_handle = purple_blist_get_handle();

	purple_signal_connect(blist_handle, "buddy-signed-on", plugin,
	                      PURPLE_CALLBACK(buddy_signed_on_cb), NULL);
	purple_signal_connect(blist_handle, "buddy-signed-off", plugin,
	                      PURPLE_CALLBACK(buddy_signed_off_cb), NULL);

	purple_signal_connect(pidgin_blist_get_handle(), "drawing-tooltip", plugin,
	                      PURPLE_CALLBACK(drawing_tooltip_cb), NULL);

	/* Stamp everyone who is already online at load time. */
	{
		GSList *buddies = purple_blist_get_buddies(), *b;
		for (b = buddies; b != NULL; b = b->next) {
			if (PURPLE_BUDDY_IS_ONLINE((PurpleBuddy *)b->data))
				mark_seen((PurpleBuddy *)b->data);
		}
		g_slist_free(buddies);
	}

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

	pref = purple_plugin_pref_new_with_name_and_label(PREF_TOOLTIP,
	        _("Show \"last seen\" in the buddy tooltip"));
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

	LS_PLUGIN_ID,
	N_("Last Seen"),
	DISPLAY_VERSION,
	N_("Remembers and shows when each buddy was last online."),
	N_("Records when your contacts sign on and off, and shows a friendly "
	   "\"last seen 3 hours ago\" line in their tooltip so you know how long "
	   "someone has been away. The data is saved with your buddy list and "
	   "survives restarts."),
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
	purple_prefs_add_bool(PREF_TOOLTIP, TRUE);
}

PURPLE_INIT_PLUGIN(lastseen, init_plugin, info)

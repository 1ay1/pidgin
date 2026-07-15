/*
 * Sed Replace - Fix a typo in your last message with s/old/new/ syntax.
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

#define SED_PLUGIN_ID   "gtk-sedreplace"

#define PREF_PREFIX     "/plugins/gtk/" SED_PLUGIN_ID
#define PREF_PREFIX_STR PREF_PREFIX "/marker"   /* how to annotate the fix */

/* Per-conversation last outgoing message, keyed via conversation data. */
#define KEY_LAST  "sedreplace::last"

/*
 * Detect an s/old/new/[flags] correction. Supports any single-char delimiter
 * right after the 's' (so paths with '/' can use s#..#..#). Escaped delimiters
 * (\<delim>) inside the pattern/replacement are honored. flags: g (global),
 * i (case-insensitive).
 */
static gboolean
parse_sed(const char *msg, char **out_old, char **out_new,
          gboolean *global, gboolean *icase)
{
	char delim;
	const char *p;
	GString *a, *b;

	if (msg == NULL || msg[0] != 's')
		return FALSE;
	delim = msg[1];
	if (delim == '\0' || g_ascii_isalnum(delim) || delim == ' ')
		return FALSE;

	p = msg + 2;
	a = g_string_new(NULL);
	b = g_string_new(NULL);

	/* pattern */
	while (*p && *p != delim) {
		if (*p == '\\' && p[1] == delim) { g_string_append_c(a, delim); p += 2; continue; }
		g_string_append_c(a, *p);
		p++;
	}
	if (*p != delim) { g_string_free(a, TRUE); g_string_free(b, TRUE); return FALSE; }
	p++; /* skip delim */

	/* replacement */
	while (*p && *p != delim) {
		if (*p == '\\' && p[1] == delim) { g_string_append_c(b, delim); p += 2; continue; }
		g_string_append_c(b, *p);
		p++;
	}
	/* trailing delim is optional; flags may follow */
	if (*p == delim)
		p++;

	*global = FALSE;
	*icase  = FALSE;
	for (; *p; p++) {
		if (*p == 'g') *global = TRUE;
		else if (*p == 'i') *icase = TRUE;
	}

	if (a->len == 0) {
		g_string_free(a, TRUE);
		g_string_free(b, TRUE);
		return FALSE;
	}

	*out_old = g_string_free(a, FALSE);
	*out_new = g_string_free(b, FALSE);
	return TRUE;
}

/* Case-(in)sensitive substring replace. Returns NULL if no match. */
static char *
do_replace(const char *hay, const char *old, const char *rep,
           gboolean global, gboolean icase)
{
	GString *out = g_string_new(NULL);
	const char *p = hay;
	size_t oldlen = strlen(old);
	gboolean any = FALSE;
	char *hay_f = NULL, *old_f = NULL;

	if (oldlen == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}

	if (icase) {
		hay_f = g_utf8_casefold(hay, -1);
		old_f = g_utf8_casefold(old, -1);
		/* Fall back to byte offsets only when casefold preserves length; for
		 * ASCII (the common typo case) it does. If lengths diverge we bail to
		 * a simple case-sensitive pass to avoid corrupting multibyte text. */
		if (strlen(hay_f) != strlen(hay) || strlen(old_f) != strlen(old)) {
			g_free(hay_f); g_free(old_f);
			hay_f = NULL; old_f = NULL;
			icase = FALSE;
		}
	}

	while (*p) {
		const char *hit;
		if (icase) {
			size_t off = p - hay;
			const char *m = strstr(hay_f + off, old_f);
			hit = m ? hay + (m - hay_f) : NULL;
		} else {
			hit = strstr(p, old);
		}

		if (hit == NULL || (any && !global)) {
			g_string_append(out, p);
			break;
		}

		g_string_append_len(out, p, hit - p);
		g_string_append(out, rep);
		p = hit + oldlen;
		any = TRUE;

		if (!global) {
			g_string_append(out, p);
			break;
		}
	}

	g_free(hay_f);
	g_free(old_f);

	if (!any) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

static void
sending_im_cb(PurpleAccount *account, const char *receiver, char **message, void *data)
{
	PurpleConversation *conv;
	char *old = NULL, *rep = NULL, *fixed;
	gboolean global, icase;
	const char *last;

	if (message == NULL || *message == NULL)
		return;

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, receiver, account);

	if (!parse_sed(*message, &old, &rep, &global, &icase)) {
		/* Not a correction: just remember it as the last outgoing line. */
		if (conv != NULL)
			purple_conversation_set_data(conv, KEY_LAST, g_strdup(*message));
		return;
	}

	/* It's an s/// command. Grab the remembered last message. */
	last = conv ? purple_conversation_get_data(conv, KEY_LAST) : NULL;
	if (last == NULL) {
		g_free(old); g_free(rep);
		if (conv)
			purple_conversation_write(conv, NULL,
				_("(nothing to correct yet)"),
				PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
		/* Swallow the s/// so it isn't sent literally. */
		g_free(*message);
		*message = g_strdup("");
		return;
	}

	fixed = do_replace(last, old, rep, global, icase);
	g_free(old);
	g_free(rep);

	if (fixed == NULL) {
		if (conv)
			purple_conversation_write(conv, NULL,
				_("(that text isn't in your last message)"),
				PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(*message);
		*message = g_strdup("");
		return;
	}

	/* Replace the outgoing s/// with the corrected message + a marker. */
	{
		const char *marker = purple_prefs_get_string(PREF_PREFIX_STR);
		char *outgoing;
		if (marker && *marker)
			outgoing = g_strdup_printf("%s %s", marker, fixed);
		else
			outgoing = g_strdup(fixed);
		g_free(*message);
		*message = outgoing;
	}

	/* Remember the corrected text as the new "last" so chained fixes work. */
	if (conv != NULL)
		purple_conversation_set_data(conv, KEY_LAST, g_strdup(fixed));
	g_free(fixed);
}

static void
deleting_conv_cb(PurpleConversation *conv, void *data)
{
	char *last = purple_conversation_get_data(conv, KEY_LAST);
	g_free(last);
	purple_conversation_set_data(conv, KEY_LAST, NULL);
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *h = purple_conversations_get_handle();

	/* High priority so we rewrite before other sending-side plugins run. */
	purple_signal_connect_priority(h, "sending-im-msg", plugin,
	        PURPLE_CALLBACK(sending_im_cb), NULL, PURPLE_SIGNAL_PRIORITY_HIGHEST);
	purple_signal_connect(h, "deleting-conversation", plugin,
	        PURPLE_CALLBACK(deleting_conv_cb), NULL);
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

	pref = purple_plugin_pref_new_with_label(
	        _("Type s/typo/fixed/ (or s/typo/fixed/g) to resend your last "
	          "message with the correction applied."));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_PREFIX_STR,
	        _("Prefix added to a corrected message (blank for none)"));
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

	SED_PLUGIN_ID,
	N_("Sed Replace"),
	DISPLAY_VERSION,
	N_("Fix a typo in your last message with s/old/new/ syntax."),
	N_("Made a typo? Just send s/teh/the/ and the plugin resends your last "
	   "message corrected -- the same muscle memory as IRC. Supports the /g "
	   "(all occurrences) and /i (case-insensitive) flags and alternate "
	   "delimiters like s#old#new#."),
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
	purple_prefs_add_string(PREF_PREFIX_STR, "*");
}

PURPLE_INIT_PLUGIN(sedreplace, init_plugin, info)

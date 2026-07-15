/*
 * Inline Translate - Translate messages on the fly with /tr, and optionally
 * auto-translate everything you receive into your language.
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

#define TR_PLUGIN_ID   "gtk-inlinetranslate"

#define PREF_PREFIX     "/plugins/gtk/" TR_PLUGIN_ID
#define PREF_MYLANG     PREF_PREFIX "/my_lang"     /* target for auto/incoming */
#define PREF_AUTO_IN    PREF_PREFIX "/auto_in"     /* auto-translate incoming */

static PurpleCmdId tr_cmd_id = 0;

/* Context carried through the async fetch back to the conversation. */
typedef struct {
	PurpleAccount *account;
	int            conv_type;
	char          *conv_name;
	char          *original;
	char          *target;
	gboolean       incoming;   /* TRUE = auto-translated incoming line */
} TrCtx;

static void
tr_ctx_free(TrCtx *c)
{
	if (c == NULL)
		return;
	g_free(c->conv_name);
	g_free(c->original);
	g_free(c->target);
	g_free(c);
}

/*
 * The googleapis translate_a/single endpoint returns a JSON-ish array whose
 * first element is a list of [translated, source, ...] chunks. We don't want a
 * JSON dependency, so pull out the quoted translated segments by hand: they are
 * the first string in each of the leading sub-arrays, i.e. the pattern is
 *   [[["translated piece","original piece",...],["...","..."]],...]
 * We concatenate the first string of every [ "..","..",.. ] pair.
 */
static char *
extract_translation(const char *body)
{
	GString *out;
	const char *p;

	if (body == NULL)
		return NULL;

	/* Expect it to start with [[[" */
	p = strstr(body, "[[[\"");
	if (p == NULL)
		return NULL;
	p += 2;   /* sit on the first inner '[' */

	out = g_string_new(NULL);

	while (*p) {
		/* Find the next  ["  which starts a chunk pair. */
		if (p[0] == '[' && p[1] == '"') {
			const char *s = p + 2;
			/* Read the (possibly escaped) JSON string. */
			while (*s) {
				if (*s == '\\' && s[1]) {
					switch (s[1]) {
					case 'n': g_string_append_c(out, '\n'); break;
					case 't': g_string_append_c(out, '\t'); break;
					case '"': g_string_append_c(out, '"');  break;
					case '\\': g_string_append_c(out, '\\'); break;
					case 'u': {
						/* \uXXXX -> UTF-8 */
						if (g_ascii_isxdigit(s[2]) && g_ascii_isxdigit(s[3]) &&
						    g_ascii_isxdigit(s[4]) && g_ascii_isxdigit(s[5])) {
							char hex[5] = { s[2], s[3], s[4], s[5], 0 };
							gunichar u = (gunichar)strtol(hex, NULL, 16);
							char buf[8];
							int n = g_unichar_to_utf8(u, buf);
							g_string_append_len(out, buf, n);
							s += 4;
						}
						break;
					}
					default: g_string_append_c(out, s[1]); break;
					}
					s += 2;
					continue;
				}
				if (*s == '"')
					break;   /* end of translated piece */
				g_string_append_c(out, *s);
				s++;
			}
			/* We took the first string of this pair; skip to the closing ] of
			 * the pair so we don't also grab the original text. */
			while (*s && *s != ']')
				s++;
			p = (*s) ? s + 1 : s;
			/* Stop once the outer list of chunks ends (]] ). */
			if (p[0] == ']')
				break;
			continue;
		}
		p++;
	}

	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

static void
tr_fetch_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data,
            const gchar *url_text, gsize len, const gchar *error_message)
{
	TrCtx *c = user_data;
	PurpleConversation *conv;
	char *translated;

	conv = purple_find_conversation_with_account(c->conv_type, c->conv_name, c->account);
	if (conv == NULL) {
		tr_ctx_free(c);
		return;
	}

	if (error_message != NULL || url_text == NULL) {
		purple_conversation_write(conv, NULL,
			_("(translation failed)"),
			PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
		tr_ctx_free(c);
		return;
	}

	translated = extract_translation(url_text);
	if (translated == NULL) {
		purple_conversation_write(conv, NULL,
			_("(could not parse translation)"),
			PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
		tr_ctx_free(c);
		return;
	}

	{
		char *esc = g_markup_escape_text(translated, -1);
		char *line;
		if (c->incoming)
			line = g_strdup_printf(_("<span color=\"#888888\">[%s] %s</span>"),
			                       c->target, esc);
		else
			line = g_strdup_printf(_("<b>\342\206\222 %s:</b> %s"), c->target, esc);
		purple_conversation_write(conv, NULL, line,
			PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_free(line);
		g_free(esc);
	}

	g_free(translated);
	tr_ctx_free(c);
}

static void
request_translation(PurpleConversation *conv, const char *text,
                    const char *target, gboolean incoming)
{
	TrCtx *c;
	char *enc;
	char *url;

	if (text == NULL || *text == '\0')
		return;

	enc = g_uri_escape_string(text, NULL, FALSE);

	/* Free, keyless endpoint; sl=auto detects the source language. */
	url = g_strdup_printf(
		"https://translate.googleapis.com/translate_a/single"
		"?client=gtx&sl=auto&tl=%s&dt=t&q=%s",
		target, enc);

	c = g_new0(TrCtx, 1);
	c->account   = purple_conversation_get_account(conv);
	c->conv_type = purple_conversation_get_type(conv);
	c->conv_name = g_strdup(purple_conversation_get_name(conv));
	c->original  = g_strdup(text);
	c->target    = g_strdup(target);
	c->incoming  = incoming;

	purple_util_fetch_url_request(url, TRUE,
		"Mozilla/5.0 (Pidgin translate plugin)", TRUE,
		NULL, FALSE, tr_fetch_cb, c);

	g_free(url);
	g_free(enc);
}

static PurpleCmdRet
tr_cmd_cb(PurpleConversation *conv, const gchar *cmd, gchar **args,
          gchar **error, void *data)
{
	const char *arg = args ? args[0] : NULL;
	const char *text;
	char target[8];

	if (arg == NULL || *arg == '\0') {
		*error = g_strdup(_("Usage: /tr [lang] <text>   e.g. /tr es hello there\n"
		                    "If you omit the language, your default is used."));
		return PURPLE_CMD_RET_FAILED;
	}

	/* Optional leading 2-3 letter language code. */
	{
		int i = 0;
		while (arg[i] && g_ascii_isalpha(arg[i]) && i < 5)
			i++;
		if ((i == 2 || i == 3) && arg[i] == ' ') {
			memcpy(target, arg, i);
			target[i] = '\0';
			text = arg + i + 1;
		} else {
			g_strlcpy(target, purple_prefs_get_string(PREF_MYLANG), sizeof(target));
			text = arg;
		}
	}

	while (*text == ' ')
		text++;
	if (*text == '\0') {
		*error = g_strdup(_("Nothing to translate."));
		return PURPLE_CMD_RET_FAILED;
	}

	request_translation(conv, text, target, FALSE);
	return PURPLE_CMD_RET_OK;
}

static void
received_im_cb(PurpleAccount *account, char *sender, char *message,
               PurpleConversation *conv, PurpleMessageFlags flags, void *data)
{
	char *stripped;

	if (!purple_prefs_get_bool(PREF_AUTO_IN))
		return;
	if (flags & (PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_AUTO_RESP))
		return;
	if (conv == NULL || message == NULL)
		return;

	/* Translate the plain text (strip any HTML markup first). */
	stripped = purple_markup_strip_html(message);
	if (stripped != NULL && *stripped != '\0')
		request_translation(conv, stripped,
			purple_prefs_get_string(PREF_MYLANG), TRUE);
	g_free(stripped);
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	tr_cmd_id = purple_cmd_register("tr", "s", PURPLE_CMD_P_PLUGIN,
	        PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS,
	        NULL, PURPLE_CMD_FUNC(tr_cmd_cb),
	        _("tr [lang] &lt;text&gt;:  Translate text (auto-detects source). "
	          "Omit lang to use your default."),
	        NULL);

	purple_signal_connect(purple_conversations_get_handle(), "received-im-msg",
	        plugin, PURPLE_CALLBACK(received_im_cb), NULL);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	if (tr_cmd_id != 0) {
		purple_cmd_unregister(tr_cmd_id);
		tr_cmd_id = 0;
	}
	return TRUE;
}

static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *pref;

	frame = purple_plugin_pref_frame_new();

	pref = purple_plugin_pref_new_with_name_and_label(PREF_MYLANG,
	        _("My language code (e.g. en, es, fr, de, ja)"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_AUTO_IN,
	        _("Automatically translate incoming IMs into my language"));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_label(
	        _("Use /tr <text> to translate to your language, or /tr es <text> "
	          "to target a specific one."));
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

	TR_PLUGIN_ID,
	N_("Inline Translate"),
	DISPLAY_VERSION,
	N_("Translate messages inline and auto-translate incoming ones."),
	N_("Break the language barrier without leaving the conversation. Type "
	   "/tr <text> to translate into your language, /tr fr <text> to target "
	   "another, or turn on auto-translate to have every incoming message "
	   "rendered in your language automatically. Uses a free online service; "
	   "no API key required."),
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
	purple_prefs_add_string(PREF_MYLANG, "en");
	purple_prefs_add_bool(PREF_AUTO_IN, FALSE);
}

PURPLE_INIT_PLUGIN(inlinetranslate, init_plugin, info)

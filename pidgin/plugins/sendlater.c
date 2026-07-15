/*
 * Send Later - Queue messages to be delivered at a future time.
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
#include "eventloop.h"
#include "signals.h"
#include "util.h"
#include "version.h"

#include "gtkplugin.h"

#define LATER_PLUGIN_ID   "gtk-sendlater"

typedef struct {
	guint          timer;
	PurpleAccount *account;      /* to re-find the conversation */
	int            conv_type;
	char          *name;         /* conversation name */
	char          *message;
	time_t         due;
	guint          id;
} ScheduledMsg;

static GList *scheduled = NULL;   /* list of ScheduledMsg* */
static guint  next_id = 1;
static PurpleCmdId later_cmd_id = 0;

static void
free_scheduled(ScheduledMsg *s)
{
	if (s == NULL)
		return;
	if (s->timer != 0)
		purple_timeout_remove(s->timer);
	g_free(s->name);
	g_free(s->message);
	g_free(s);
}

/*
 * Parse a leading duration token like "5m", "2h", "90s", "1h30m", "10".
 * Bare numbers are minutes. Returns seconds, or -1 if no duration was found.
 * On success *rest points just past the token (and following spaces).
 */
static long
parse_duration(const char *in, const char **rest)
{
	const char *p = in;
	long total = 0;
	gboolean any = FALSE;

	while (*p == ' ')
		p++;

	while (*p) {
		char *end;
		long val;
		long mult;

		if (!g_ascii_isdigit(*p))
			break;

		val = strtol(p, &end, 10);
		switch (*end) {
		case 's': mult = 1;     end++; break;
		case 'm': mult = 60;    end++; break;
		case 'h': mult = 3600;  end++; break;
		case 'd': mult = 86400; end++; break;
		case ' ':
		case '\0':
		default:  mult = 60;    break;   /* bare number == minutes */
		}
		total += val * mult;
		any = TRUE;
		p = end;
		/* allow "1h30m" chains but stop at the first space */
		if (*p == ' ')
			break;
	}

	if (!any)
		return -1;

	while (*p == ' ')
		p++;
	if (rest != NULL)
		*rest = p;
	return total;
}

static char *
humanize_secs(long secs)
{
	if (secs < 60)
		return g_strdup_printf(ngettext("%ld second", "%ld seconds", secs), secs);
	if (secs < 3600)
		return g_strdup_printf(ngettext("%ld minute", "%ld minutes", secs / 60), secs / 60);
	if (secs < 86400) {
		long h = secs / 3600, m = (secs % 3600) / 60;
		if (m)
			return g_strdup_printf(_("%ldh %ldm"), h, m);
		return g_strdup_printf(ngettext("%ld hour", "%ld hours", h), h);
	}
	return g_strdup_printf(ngettext("%ld day", "%ld days", secs / 86400), secs / 86400);
}

static gboolean
fire_scheduled(gpointer data)
{
	ScheduledMsg *s = data;
	PurpleConversation *conv;

	s->timer = 0;   /* about to be removed by returning FALSE */

	conv = purple_find_conversation_with_account(s->conv_type, s->name, s->account);
	if (conv == NULL)
		conv = purple_conversation_new(s->conv_type, s->account, s->name);

	if (conv != NULL) {
		if (s->conv_type == PURPLE_CONV_TYPE_IM)
			purple_conv_im_send(PURPLE_CONV_IM(conv), s->message);
		else if (s->conv_type == PURPLE_CONV_TYPE_CHAT)
			purple_conv_chat_send(PURPLE_CONV_CHAT(conv), s->message);

		purple_conversation_write(conv, NULL,
			_("(scheduled message sent)"),
			PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
	} else {
		purple_debug_warning("sendlater",
			"could not deliver scheduled message to %s\n", s->name);
	}

	scheduled = g_list_remove(scheduled, s);
	free_scheduled(s);
	return FALSE;   /* one-shot */
}

static PurpleCmdRet
later_cmd_cb(PurpleConversation *conv, const gchar *cmd, gchar **args,
             gchar **error, void *data)
{
	const char *arg = args ? args[0] : NULL;
	const char *rest = NULL;
	long secs;

	if (arg == NULL || *arg == '\0') {
		*error = g_strdup(_("Usage: /later <time> <message>   e.g. /later 5m call you back\n"
		                    "       /later list                (show queued)\n"
		                    "       /later cancel <n>          (drop one)"));
		return PURPLE_CMD_RET_FAILED;
	}

	/* Sub-commands: list / cancel. */
	if (g_str_has_prefix(arg, "list")) {
		GString *out = g_string_new(_("<b>Scheduled messages:</b><br/>"));
		GList *l;
		time_t now = time(NULL);
		int shown = 0;
		for (l = scheduled; l != NULL; l = l->next) {
			ScheduledMsg *s = l->data;
			long remain = (long)(s->due - now);
			char *when = humanize_secs(remain > 0 ? remain : 0);
			char *esc = g_markup_escape_text(s->message, -1);
			g_string_append_printf(out, "#%u \342\206\222 %s (in %s): %s<br/>",
			                       s->id, s->name, when, esc);
			g_free(when);
			g_free(esc);
			shown++;
		}
		if (shown == 0)
			g_string_append(out, _("(nothing queued)"));
		purple_conversation_write(conv, NULL, out->str,
			PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
		g_string_free(out, TRUE);
		return PURPLE_CMD_RET_OK;
	}

	if (g_str_has_prefix(arg, "cancel")) {
		const char *p = arg + 6;
		guint want;
		GList *l;
		while (*p == ' ') p++;
		want = (guint)atoi(p);
		for (l = scheduled; l != NULL; l = l->next) {
			ScheduledMsg *s = l->data;
			if (s->id == want) {
				scheduled = g_list_remove(scheduled, s);
				free_scheduled(s);
				purple_conversation_write(conv, NULL,
					_("Cancelled that scheduled message."),
					PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
				return PURPLE_CMD_RET_OK;
			}
		}
		*error = g_strdup(_("No scheduled message with that number."));
		return PURPLE_CMD_RET_FAILED;
	}

	/* Normal: /later <duration> <message> */
	secs = parse_duration(arg, &rest);
	if (secs < 0) {
		*error = g_strdup(_("Could not read a time. Try: /later 10m your message"));
		return PURPLE_CMD_RET_FAILED;
	}
	if (rest == NULL || *rest == '\0') {
		*error = g_strdup(_("You gave a time but no message."));
		return PURPLE_CMD_RET_FAILED;
	}
	if (secs == 0)
		secs = 1;
	if (secs > 7 * 86400) {
		*error = g_strdup(_("That's more than a week away; pick something sooner."));
		return PURPLE_CMD_RET_FAILED;
	}

	{
		ScheduledMsg *s = g_new0(ScheduledMsg, 1);
		char *when;

		s->account   = purple_conversation_get_account(conv);
		s->conv_type = purple_conversation_get_type(conv);
		s->name      = g_strdup(purple_conversation_get_name(conv));
		s->message   = g_strdup(rest);
		s->due       = time(NULL) + secs;
		s->id        = next_id++;
		s->timer     = purple_timeout_add_seconds((guint)secs, fire_scheduled, s);

		scheduled = g_list_append(scheduled, s);

		when = humanize_secs(secs);
		{
			char *note = g_strdup_printf(
				_("Queued message #%u; it will send in %s."), s->id, when);
			purple_conversation_write(conv, NULL, note,
				PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_SYSTEM, time(NULL));
			g_free(note);
		}
		g_free(when);
	}

	return PURPLE_CMD_RET_OK;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	later_cmd_id = purple_cmd_register("later", "s", PURPLE_CMD_P_PLUGIN,
	        PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS,
	        NULL, PURPLE_CMD_FUNC(later_cmd_cb),
	        _("later &lt;time&gt; &lt;message&gt;:  Send a message after a delay "
	          "(e.g. 30s, 5m, 2h). Also: /later list, /later cancel &lt;n&gt;."),
	        NULL);
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	if (later_cmd_id != 0) {
		purple_cmd_unregister(later_cmd_id);
		later_cmd_id = 0;
	}
	g_list_free_full(scheduled, (GDestroyNotify)free_scheduled);
	scheduled = NULL;
	return TRUE;
}

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

	LATER_PLUGIN_ID,
	N_("Send Later"),
	DISPLAY_VERSION,
	N_("Queue a message now and have it delivered at a future time."),
	N_("Type /later 5m see you soon to send a message after five minutes, or "
	   "/later 2h ... for two hours. Manage the queue with /later list and "
	   "/later cancel <n>. Perfect for reminders, timed nudges, or replying "
	   "when you know you'll be busy."),
	"Ayush Bhat <tfeayush@gmail.com>",
	PURPLE_WEBSITE,

	plugin_load,
	plugin_unload,
	NULL,

	NULL,
	NULL,
	NULL,
	NULL,

	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
}

PURPLE_INIT_PLUGIN(sendlater, init_plugin, info)

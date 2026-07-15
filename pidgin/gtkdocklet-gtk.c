/*
 * System tray icon (aka docklet) plugin for Purple
 *
 * Copyright (C) 2007 Anders Hasselqvist
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "internal.h"
#include "pidgin.h"
#include "debug.h"
#include "prefs.h"
#include "pidginstock.h"
#include "gtkdocklet.h"

#ifdef USE_APPINDICATOR
# ifdef HAVE_AYATANA_APPINDICATOR
#  include <libayatana-appindicator/app-indicator.h>
# else
#  include <libappindicator/app-indicator.h>
# endif
# include <gio/gio.h>
#endif

#define SHORT_EMBED_TIMEOUT 5
#define LONG_EMBED_TIMEOUT 15

/* globals */
static GtkStatusIcon *docklet = NULL;
static guint embed_timeout = 0;

#ifdef USE_APPINDICATOR
/* When an AppIndicator (StatusNotifierItem) host is present we use it instead
 * of the deprecated GtkStatusIcon, which does not work on modern desktops
 * (GNOME, Wayland, KDE without XEmbed, ...). The indicator is created lazily
 * and, once created, is used for the whole session. */
static AppIndicator *app_indicator = NULL;
#endif

/* protos */
static void docklet_gtk_status_create(gboolean);

static gboolean G_GNUC_UNUSED
docklet_gtk_recreate_cb(gpointer data)
{
	docklet_gtk_status_create(TRUE);

	return FALSE;
}

static gboolean
docklet_gtk_embed_timeout_cb(gpointer data)
{
#if !GTK_CHECK_VERSION(2,12,0)
	if (gtk_status_icon_is_embedded(docklet)) {
		/* Older GTK+ (<2.12) don't implement the embedded signal, but the
		   information is still accessable through the above function. */
		purple_debug_info("docklet", "embedded\n");

		pidgin_docklet_embedded();
		purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/docklet/gtk/embedded", TRUE);
	}
	else
#endif
	{
		/* The docklet was not embedded within the timeout.
		 * Remove it as a visibility manager, but leave the plugin
		 * loaded so that it can embed automatically if/when a notification
		 * area becomes available.
		 */
		purple_debug_info("docklet", "failed to embed within timeout\n");
		pidgin_docklet_remove();
		purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/docklet/gtk/embedded", FALSE);
	}

#if GTK_CHECK_VERSION(2,12,0)
	embed_timeout = 0;
	return FALSE;
#else
	return TRUE;
#endif
}

#if GTK_CHECK_VERSION(2,12,0)
static gboolean
docklet_gtk_embedded_cb(GtkWidget *widget, gpointer data)
{
	if (embed_timeout) {
		purple_timeout_remove(embed_timeout);
		embed_timeout = 0;
	}

	if (gtk_status_icon_is_embedded(docklet)) {
		purple_debug_info("docklet", "embedded\n");

		pidgin_docklet_embedded();
		purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/docklet/gtk/embedded", TRUE);
	} else {
		purple_debug_info("docklet", "detached\n");

		pidgin_docklet_remove();
		purple_prefs_set_bool(PIDGIN_PREFS_ROOT "/docklet/gtk/embedded", FALSE);
	}

	return TRUE;
}
#endif

static void G_GNUC_UNUSED
docklet_gtk_destroyed_cb(GtkWidget *widget, gpointer data)
{
	purple_debug_info("docklet", "destroyed\n");

	pidgin_docklet_remove();

	g_object_unref(G_OBJECT(docklet));
	docklet = NULL;

	g_idle_add(docklet_gtk_recreate_cb, NULL);
}

static void
docklet_gtk_status_activated_cb(GtkStatusIcon *status_icon, gpointer user_data)
{
	pidgin_docklet_clicked(1);
}

static void
docklet_gtk_status_clicked_cb(GtkStatusIcon *status_icon, guint button, guint activate_time, gpointer user_data)
{
	purple_debug_info("docklet", "The button is %u\n", button);
#ifdef GDK_WINDOWING_QUARTZ
	/* You can only click left mouse button on MacOSX native GTK. Let that be the menu */
	pidgin_docklet_clicked(3);
#else
	pidgin_docklet_clicked(button);
#endif
}

static void
docklet_gtk_status_update_icon(PurpleStatusPrimitive status, gboolean connecting, gboolean pending)
{
	const gchar *icon_name = NULL;
	const gchar *current_icon_name = NULL;

#ifdef USE_APPINDICATOR
	if (app_indicator != NULL) {
		current_icon_name = app_indicator_get_icon(app_indicator);
	} else
#endif
	{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		current_icon_name = gtk_status_icon_get_icon_name(docklet);
G_GNUC_END_IGNORE_DEPRECATIONS
	}

	switch (status) {
		case PURPLE_STATUS_OFFLINE:
			icon_name = PIDGIN_STOCK_TRAY_OFFLINE;
			break;
		case PURPLE_STATUS_AWAY:
			icon_name = PIDGIN_STOCK_TRAY_AWAY;
			break;
		case PURPLE_STATUS_UNAVAILABLE:
			icon_name = PIDGIN_STOCK_TRAY_BUSY;
			break;
		case PURPLE_STATUS_EXTENDED_AWAY:
			icon_name = PIDGIN_STOCK_TRAY_XA;
			break;
		case PURPLE_STATUS_INVISIBLE:
			icon_name = PIDGIN_STOCK_TRAY_INVISIBLE;
			break;
		default:
			icon_name = PIDGIN_STOCK_TRAY_AVAILABLE;
			break;
	}

	if (connecting && !purple_strequal(current_icon_name, PIDGIN_STOCK_TRAY_CONNECT)) {
		icon_name = PIDGIN_STOCK_TRAY_CONNECT;
	}

	if (pending && !purple_strequal(current_icon_name, PIDGIN_STOCK_TRAY_PENDING)) {
		icon_name = PIDGIN_STOCK_TRAY_PENDING;
	}

	if (icon_name) {
#ifdef USE_APPINDICATOR
		if (app_indicator != NULL) {
			app_indicator_set_icon_full(app_indicator, icon_name, icon_name);
			/* The context menu content is status-dependent (Unread Messages
			 * submenu, New Message/Join Chat sensitivity, status checkmarks),
			 * and the SNI menu is owned by the host rather than rebuilt per
			 * click. Rebuild and re-attach it so it stays current. */
			app_indicator_set_menu(app_indicator,
				GTK_MENU(pidgin_docklet_build_menu()));
		} else
#endif
		{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
			gtk_status_icon_set_from_icon_name(docklet, icon_name);
G_GNUC_END_IGNORE_DEPRECATIONS
		}
	}
}

static void
docklet_gtk_status_set_tooltip(gchar *tooltip)
{
#ifdef USE_APPINDICATOR
	if (app_indicator != NULL) {
		/* AppIndicator/SNI hosts show the title as the tooltip; there is no
		 * separate free-text tooltip API. Setting the title is the closest
		 * GTK3-idiomatic equivalent. */
		app_indicator_set_title(app_indicator, tooltip);
		return;
	}
#endif
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_status_icon_set_tooltip_text(docklet, tooltip);
G_GNUC_END_IGNORE_DEPRECATIONS
}

static void
docklet_gtk_status_position_menu(GtkMenu *menu,
                                 int *x, int *y, gboolean *push_in,
                                 gpointer user_data)
{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_status_icon_position_menu(menu, x, y, push_in, docklet);
G_GNUC_END_IGNORE_DEPRECATIONS
}

static void
docklet_gtk_status_destroy(void)
{
#ifdef USE_APPINDICATOR
	if (app_indicator != NULL) {
		pidgin_docklet_remove();
		app_indicator_set_status(app_indicator, APP_INDICATOR_STATUS_PASSIVE);
		g_object_unref(G_OBJECT(app_indicator));
		app_indicator = NULL;
		purple_debug_info("docklet", "AppIndicator destroyed\n");
		return;
	}
#endif

	g_return_if_fail(docklet != NULL);

	pidgin_docklet_remove();

	if (embed_timeout) {
		purple_timeout_remove(embed_timeout);
		embed_timeout = 0;
	}

	gtk_status_icon_set_visible(docklet, FALSE);
	g_object_unref(G_OBJECT(docklet));
	docklet = NULL;

	purple_debug_info("docklet", "GTK+ destroyed\n");
}

#ifdef USE_APPINDICATOR
/*
 * Return TRUE if a StatusNotifier host/watcher is actually present on the
 * session bus. The AppIndicator library exports our tray item unconditionally
 * and never tells us whether anyone is displaying it, so we ask the bus
 * directly: a real tray exists only if org.kde.StatusNotifierWatcher (the de
 * facto name; org.freedesktop.* is the older spec) has an owner. This keeps
 * the buddy list from being hidden behind a phantom tray icon on compositors
 * with no tray (e.g. Hyprland/sway without a panel), which otherwise looks
 * like a slow/hung startup.
 */
static gboolean
docklet_gtk_sni_host_present(void)
{
	GDBusConnection *bus;
	gboolean present = FALSE;
	const char *const watchers[] = {
		"org.kde.StatusNotifierWatcher",
		"org.freedesktop.StatusNotifierWatcher",
		NULL
	};
	int i;

	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (bus == NULL)
		return FALSE;

	for (i = 0; watchers[i] != NULL && !present; i++) {
		GVariant *reply = g_dbus_connection_call_sync(bus,
			"org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "NameHasOwner",
			g_variant_new("(s)", watchers[i]),
			G_VARIANT_TYPE("(b)"),
			G_DBUS_CALL_FLAGS_NONE, 500 /* ms */, NULL, NULL);
		if (reply != NULL) {
			gboolean owned = FALSE;
			g_variant_get(reply, "(b)", &owned);
			if (owned)
				present = TRUE;
			g_variant_unref(reply);
		}
	}

	g_object_unref(bus);
	return present;
}
#endif /* USE_APPINDICATOR */

static void
docklet_gtk_status_create(gboolean recreate)
{
#ifdef USE_APPINDICATOR
	/* Prefer the AppIndicator (StatusNotifierItem) backend: it works on all
	 * modern desktops (GNOME, KDE, Wayland, Unity, ...) where the deprecated
	 * GtkStatusIcon XEmbed tray is unavailable. It is always "embedded" as far
	 * as we are concerned (the panel/host owns visibility), so there is no
	 * embed-timeout dance. */
	if (app_indicator != NULL) {
		purple_debug_warning("docklet",
			"trying to create indicator but it already exists?\n");
		docklet_gtk_status_destroy();
	}

	app_indicator = app_indicator_new("pidgin",
			PIDGIN_STOCK_TRAY_AVAILABLE,
			APP_INDICATOR_CATEGORY_COMMUNICATIONS);

	if (app_indicator != NULL && APP_IS_INDICATOR(app_indicator)) {
		GtkWidget *menu;

		/* Point the indicator at our bundled tray icon theme so the SNI host
		 * can resolve the pidgin-tray-* icon names. */
		app_indicator_set_icon_theme_path(app_indicator,
			DATADIR G_DIR_SEPARATOR_S "pixmaps" G_DIR_SEPARATOR_S "pidgin"
			G_DIR_SEPARATOR_S "tray" G_DIR_SEPARATOR_S "hicolor");

		app_indicator_set_status(app_indicator, APP_INDICATOR_STATUS_ACTIVE);

		/* The SNI protocol has no separate "activate" (left-click) action that
		 * is honoured everywhere, so the context menu is the primary UI. Build
		 * the same menu the GtkStatusIcon path uses and hand it to the host;
		 * app_indicator_set_secondary_activate_target lets left-click on hosts
		 * that support it toggle the buddy list via the Show/Hide item. */
		menu = pidgin_docklet_build_menu();
		app_indicator_set_menu(app_indicator, GTK_MENU(menu));

		/* Only let the tray icon suppress the buddy list if a real tray host
		 * (StatusNotifierWatcher) is actually present on the session bus.
		 * app_indicator_new() ALWAYS "succeeds" -- it just exports an SNI
		 * object and waits for a host to notice it -- so on bare Wayland
		 * compositors (Hyprland, sway without waybar, ...) there may be no
		 * host at all. If we unconditionally became a visibility manager the
		 * buddy list would start hidden behind a tray icon that never renders,
		 * making Pidgin appear to "hang" at startup. Gate on host presence. */
		if (!recreate) {
			if (docklet_gtk_sni_host_present())
				pidgin_docklet_embedded();
			else
				purple_debug_info("docklet",
					"no StatusNotifier host on the bus; not hiding the "
					"buddy list behind the tray icon\n");
		}

		purple_debug_info("docklet", "AppIndicator created\n");
		return;
	}

	/* app_indicator_new() failed (no SNI host / library problem): fall through
	 * to the legacy GtkStatusIcon path. */
	if (app_indicator != NULL) {
		g_object_unref(G_OBJECT(app_indicator));
		app_indicator = NULL;
	}
	purple_debug_info("docklet",
		"AppIndicator unavailable; falling back to GtkStatusIcon\n");
#endif /* USE_APPINDICATOR */

	if (docklet) {
		/* if this is being called when a tray icon exists, it's because
		   something messed up. try destroying it before we proceed,
		   although docklet_refcount may be all hosed. hopefully won't happen. */
		purple_debug_warning("docklet", "trying to create icon but it already exists?\n");
		docklet_gtk_status_destroy();
	}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	docklet = gtk_status_icon_new();
G_GNUC_END_IGNORE_DEPRECATIONS
	if (docklet == NULL || !G_IS_OBJECT(docklet)) {
		/* GtkStatusIcon is deprecated and does not work on some display
		 * servers (e.g. Wayland with no XEmbed tray); bail out cleanly so we
		 * don't connect signals to a non-object or leave the buddy list
		 * permanently hidden waiting for an embed that never happens. */
		purple_debug_warning("docklet",
			"gtk_status_icon_new() unavailable; disabling tray icon\n");
		docklet = NULL;
		if (!recreate)
			pidgin_docklet_remove();
		return;
	}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	g_signal_connect(G_OBJECT(docklet), "activate", G_CALLBACK(docklet_gtk_status_activated_cb), NULL);
	g_signal_connect(G_OBJECT(docklet), "popup-menu", G_CALLBACK(docklet_gtk_status_clicked_cb), NULL);
#if GTK_CHECK_VERSION(2,12,0)
	g_signal_connect(G_OBJECT(docklet), "notify::embedded", G_CALLBACK(docklet_gtk_embedded_cb), NULL);
#endif
	/* NOTE: GtkStatusIcon is a GObject, not a GtkWidget, and has no "destroy"
	 * signal in GTK3 — connecting one throws a g_signal critical and the
	 * handler (which unrefs the icon) can never fire.  The icon's lifetime is
	 * managed explicitly in docklet_gtk_status_destroy(). */

	gtk_status_icon_set_visible(docklet, TRUE);
G_GNUC_END_IGNORE_DEPRECATIONS

	/* This is a hack to avoid a race condition between the docklet getting
	 * embedded in the notification area and the gtkblist restoring its
	 * previous visibility state.  If the docklet does not get embedded within
	 * the timeout, it will be removed as a visibility manager until it does
	 * get embedded.  Ideally, we would only call docklet_embedded() when the
	 * icon was actually embedded. This only happens when the docklet is first
	 * created, not when being recreated.
	 *
	 * The gtk docklet tracks whether it successfully embedded in a pref and
	 * allows for a longer timeout period if it successfully embedded the last
	 * time it was run. This should hopefully solve problems with the buddy
	 * list not properly starting hidden when Pidgin is started on login.
	 */
	if (!recreate) {
		pidgin_docklet_embedded();
#if GTK_CHECK_VERSION(2,12,0)
		if (purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/docklet/gtk/embedded")) {
			embed_timeout = purple_timeout_add_seconds(LONG_EMBED_TIMEOUT, docklet_gtk_embed_timeout_cb, NULL);
		} else {
			embed_timeout = purple_timeout_add_seconds(SHORT_EMBED_TIMEOUT, docklet_gtk_embed_timeout_cb, NULL);
		}
#else
		embed_timeout = purple_timeout_add_seconds(SHORT_EMBED_TIMEOUT, docklet_gtk_embed_timeout_cb, NULL);
#endif
	}

	purple_debug_info("docklet", "GTK+ created\n");
}

static void
docklet_gtk_status_create_ui_op(void)
{
	docklet_gtk_status_create(FALSE);
}

static struct docklet_ui_ops ui_ops =
{
	docklet_gtk_status_create_ui_op,
	docklet_gtk_status_destroy,
	docklet_gtk_status_update_icon,
	NULL,
	docklet_gtk_status_set_tooltip,
	docklet_gtk_status_position_menu
};

void
docklet_ui_init(void)
{
	pidgin_docklet_set_ui_ops(&ui_ops);

	purple_prefs_add_none(PIDGIN_PREFS_ROOT "/docklet/gtk");
	if (purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/docklet/x11/embedded")) {
		purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/docklet/gtk/embedded", TRUE);
		purple_prefs_remove(PIDGIN_PREFS_ROOT "/docklet/x11/embedded");
	} else {
		purple_prefs_add_bool(PIDGIN_PREFS_ROOT "/docklet/gtk/embedded", FALSE);
	}

	gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(),
		DATADIR G_DIR_SEPARATOR_S "pixmaps" G_DIR_SEPARATOR_S "pidgin" G_DIR_SEPARATOR_S "tray");
}


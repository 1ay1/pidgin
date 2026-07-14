/**
 * @file gtkitemfactory.c Minimal GtkItemFactory compatibility shim for GTK3.
 *
 * See gtkitemfactory.h for rationale. This reimplements the subset of the
 * removed GtkItemFactory API that Pidgin relies on, on top of GtkMenuBar /
 * GtkMenu / GtkMenuItem, preserving the exact menu structure defined by
 * Pidgin's GtkItemFactoryEntry tables.
 */

#include "internal.h"
#include "gtkitemfactory.h"

struct _PidginItemFactory
{
	GtkWidget       *toplevel;      /* the GtkMenuBar or GtkMenu root */
	GtkAccelGroup   *accel_group;
	GtkTranslateFunc translate_func;
	gpointer         translate_data;
	GDestroyNotify   translate_notify;
	GHashTable      *widgets;       /* untranslated path -> GtkWidget* */
};

/* Trampoline: adapt a GtkMenuItem "activate" to the 3-arg Pidgin callback. */
typedef struct
{
	GtkItemFactoryCallback1 callback;
	guint                   action;
	gpointer                data;
} PidginIFClosure;

static void
pidgin_if_activate(GtkMenuItem *item, gpointer user_data)
{
	PidginIFClosure *c = user_data;
	if (c->callback)
		c->callback(c->data, c->action, GTK_WIDGET(item));
}

static void
pidgin_if_closure_free(gpointer data, GClosure *closure)
{
	g_free(data);
}

GtkItemFactory *
gtk_item_factory_new(GType container_type, const gchar *path,
                     GtkAccelGroup *accel_group)
{
	GtkItemFactory *ift = g_new0(GtkItemFactory, 1);

	if (container_type == GTK_TYPE_MENU_BAR)
		ift->toplevel = gtk_menu_bar_new();
	else
		ift->toplevel = gtk_menu_new();

	ift->accel_group = accel_group;
	if (accel_group)
		g_object_ref(accel_group);

	ift->widgets = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                     g_free, NULL);
	/* The root itself is retrievable by its factory path. */
	if (path)
		g_hash_table_insert(ift->widgets, g_strdup(path), ift->toplevel);

	g_object_ref_sink(ift->toplevel);
	return ift;
}

void
gtk_item_factory_set_translate_func(GtkItemFactory *ift, GtkTranslateFunc func,
                                    gpointer data, GDestroyNotify notify)
{
	g_return_if_fail(ift != NULL);
	if (ift->translate_notify)
		ift->translate_notify(ift->translate_data);
	ift->translate_func   = func;
	ift->translate_data   = data;
	ift->translate_notify = notify;
}

static const gchar *
pidgin_if_translate(GtkItemFactory *ift, const gchar *path)
{
	if (ift->translate_func)
		return ift->translate_func(path, ift->translate_data);
	return path;
}

/* Parse "<CTL><SHIFT>M" style accelerators into key+mods. */
static void
pidgin_if_parse_accel(const gchar *accel, guint *key, GdkModifierType *mods)
{
	*key = 0;
	*mods = 0;
	if (accel == NULL || *accel == '\0')
		return;

	while (*accel == '<') {
		const gchar *end = strchr(accel, '>');
		gsize len;
		if (end == NULL)
			break;
		len = end - accel - 1;
		if (g_ascii_strncasecmp(accel + 1, "CTL", len) == 0 ||
		    g_ascii_strncasecmp(accel + 1, "CONTROL", len) == 0)
			*mods |= GDK_CONTROL_MASK;
		else if (g_ascii_strncasecmp(accel + 1, "SHIFT", len) == 0 ||
		         g_ascii_strncasecmp(accel + 1, "SHFT", len) == 0)
			*mods |= GDK_SHIFT_MASK;
		else if (g_ascii_strncasecmp(accel + 1, "ALT", len) == 0 ||
		         g_ascii_strncasecmp(accel + 1, "MOD1", len) == 0)
			*mods |= GDK_MOD1_MASK;
		accel = end + 1;
	}

	if (*accel != '\0')
		*key = gdk_keyval_from_name(accel);
}

/* Find or create the parent GtkMenuShell for a factory path, creating any
 * missing <Branch> submenus along the way. Returns the parent menu shell and
 * sets *leaf to the last path component (the label). */
static GtkWidget *
pidgin_if_ensure_parent(GtkItemFactory *ift, const gchar *path, gchar **leaf)
{
	gchar **parts;
	GtkWidget *shell = ift->toplevel;
	GString *acc;
	guint i, n;

	/* path looks like "/Buddies/Show/Foo"; split, skipping the leading "". */
	parts = g_strsplit(path, "/", -1);
	n = g_strv_length(parts);
	acc = g_string_new("");

	/* parts[0] == "" (before leading slash). Walk parts[1..n-2] as branches. */
	for (i = 1; i + 1 < n; i++) {
		GtkWidget *item, *submenu;
		g_string_append_c(acc, '/');
		g_string_append(acc, parts[i]);

		item = g_hash_table_lookup(ift->widgets, acc->str);
		if (item == NULL) {
			/* Should have been created by a <Branch> entry, but be tolerant. */
			const gchar *lbl = pidgin_if_translate(ift, acc->str);
			const gchar *disp = strrchr(lbl, '/');
			disp = disp ? disp + 1 : lbl;
			item = gtk_menu_item_new_with_mnemonic(disp);
			gtk_menu_shell_append(GTK_MENU_SHELL(shell), item);
			gtk_widget_show(item);
			g_hash_table_insert(ift->widgets, g_strdup(acc->str), item);
		}
		submenu = gtk_menu_item_get_submenu(GTK_MENU_ITEM(item));
		if (submenu == NULL) {
			submenu = gtk_menu_new();
			gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
		}
		shell = submenu;
	}

	*leaf = g_strdup(parts[n - 1]);
	g_string_free(acc, TRUE);
	g_strfreev(parts);
	return shell;
}

static void
pidgin_if_create_one(GtkItemFactory *ift, GtkItemFactoryEntry *e,
                     gpointer callback_data)
{
	const gchar *type = e->item_type ? e->item_type : "<Item>";
	const gchar *tpath = pidgin_if_translate(ift, e->path);
	gchar *leaf = NULL;
	GtkWidget *shell;
	GtkWidget *item = NULL;
	const gchar *label;

	shell = pidgin_if_ensure_parent(ift, tpath, &leaf);

	/* Label is the final path component, translated. */
	label = strrchr(tpath, '/');
	label = label ? label + 1 : tpath;

	if (g_strcmp0(type, "<Separator>") == 0 ||
	    g_strcmp0(type, "<BR>") == 0) {
		item = gtk_separator_menu_item_new();
	} else if (g_strcmp0(type, "<Tearoff>") == 0) {
		/* Tearoff menus are gone in GTK3; skip silently. */
		g_free(leaf);
		return;
	} else if (g_strcmp0(type, "<Branch>") == 0) {
		item = gtk_menu_item_new_with_mnemonic(label);
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), gtk_menu_new());
	} else if (g_strcmp0(type, "<CheckItem>") == 0 ||
	           g_strcmp0(type, "<ToggleItem>") == 0) {
		item = gtk_check_menu_item_new_with_mnemonic(label);
	} else if (g_strcmp0(type, "<StockItem>") == 0 ||
	           g_strcmp0(type, "<ImageItem>") == 0) {
		/* GTK3 has no image menu items in the classic sense; use a plain
		 * mnemonic item so the label/accelerator behaviour is preserved. */
		item = gtk_menu_item_new_with_mnemonic(label);
	} else { /* "<Item>" and anything else */
		item = gtk_menu_item_new_with_mnemonic(label);
	}

	gtk_menu_shell_append(GTK_MENU_SHELL(shell), item);
	gtk_widget_show(item);

	/* Accelerator. */
	if (e->accelerator && ift->accel_group) {
		guint key;
		GdkModifierType mods;
		pidgin_if_parse_accel(e->accelerator, &key, &mods);
		if (key != 0)
			gtk_widget_add_accelerator(item, "activate", ift->accel_group,
			                           key, mods, GTK_ACCEL_VISIBLE);
	}

	/* Callback. */
	if (e->callback) {
		PidginIFClosure *c = g_new0(PidginIFClosure, 1);
		c->callback = (GtkItemFactoryCallback1)e->callback;
		c->action   = e->callback_action;
		c->data     = callback_data;
		g_signal_connect_data(item, "activate",
		                      G_CALLBACK(pidgin_if_activate), c,
		                      pidgin_if_closure_free, 0);
	}

	g_hash_table_insert(ift->widgets, g_strdup(tpath), item);
	/* Also index by the untranslated path so get_widget(N_("...")) works. */
	if (g_strcmp0(tpath, e->path) != 0)
		g_hash_table_insert(ift->widgets, g_strdup(e->path), item);

	g_free(leaf);
}

void
gtk_item_factory_create_items(GtkItemFactory *ift, guint n_entries,
                              GtkItemFactoryEntry *entries, gpointer callback_data)
{
	guint i;
	g_return_if_fail(ift != NULL);
	for (i = 0; i < n_entries; i++)
		pidgin_if_create_one(ift, &entries[i], callback_data);
}

GtkWidget *
gtk_item_factory_get_widget(GtkItemFactory *ift, const gchar *path)
{
	GtkWidget *w;
	g_return_val_if_fail(ift != NULL, NULL);

	/* The root branch is requested by its "<...Main>" path. */
	w = g_hash_table_lookup(ift->widgets, path);
	if (w)
		return w;

	/* Fall back to the translated form. */
	return g_hash_table_lookup(ift->widgets, pidgin_if_translate(ift, path));
}

GtkWidget *
gtk_item_factory_get_item(GtkItemFactory *ift, const gchar *path)
{
	/* Identical semantics for our uses. */
	return gtk_item_factory_get_widget(ift, path);
}

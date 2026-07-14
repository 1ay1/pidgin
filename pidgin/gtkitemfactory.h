/**
 * @file gtkitemfactory.h Minimal GtkItemFactory compatibility shim for GTK3.
 *
 * GtkItemFactory was removed in GTK3. Pidgin's menu definitions
 * (blist_menu[], menu_items[]) are large tables that describe the exact
 * menu layout, so rather than rewrite every menu we reimplement the small
 * subset of the GtkItemFactory API that Pidgin uses, on top of plain
 * GtkMenuBar / GtkMenu / GtkMenuItem. This keeps the identical menu
 * structure -- and therefore the same look and feel.
 *
 * Supported item types: <Branch>, <Item>, <CheckItem>, <StockItem>,
 * <ImageItem>, <Separator>/<BR>, <Tearoff>, and the top-level <...Main>
 * root branch. Accelerators of the form "<CTL>M", "<CTL><SHIFT>X",
 * "<ALT>F4", "F6" are parsed.
 */

#ifndef _PIDGIN_ITEMFACTORY_H_
#define _PIDGIN_ITEMFACTORY_H_

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _PidginItemFactory GtkItemFactory;

typedef void (*GtkItemFactoryCallback)(void);
typedef gchar *(*GtkTranslateFunc)(const gchar *path, gpointer func_data);

typedef struct _GtkItemFactoryEntry
{
	gchar *path;
	gchar *accelerator;
	GtkItemFactoryCallback callback;
	guint  callback_action;
	gchar *item_type;
	gconstpointer extra_data;   /* stock id for <StockItem> */
} GtkItemFactoryEntry;

/* The 3-arg callback signature Pidgin's menu handlers actually use. */
typedef void (*GtkItemFactoryCallback1)(gpointer callback_data,
                                        guint callback_action,
                                        GtkWidget *widget);

GtkItemFactory *gtk_item_factory_new(GType container_type,
                                     const gchar *path,
                                     GtkAccelGroup *accel_group);

const gchar *pidgin_item_factory_get_path(GtkItemFactory *ifactory);

void gtk_item_factory_set_translate_func(GtkItemFactory *ifactory,
                                         GtkTranslateFunc func,
                                         gpointer data,
                                         GDestroyNotify notify);

void gtk_item_factory_create_items(GtkItemFactory *ifactory,
                                   guint n_entries,
                                   GtkItemFactoryEntry *entries,
                                   gpointer callback_data);

GtkWidget *gtk_item_factory_get_widget(GtkItemFactory *ifactory,
                                       const gchar *path);

GtkWidget *gtk_item_factory_get_item(GtkItemFactory *ifactory,
                                     const gchar *path);

G_END_DECLS

#endif /* _PIDGIN_ITEMFACTORY_H_ */

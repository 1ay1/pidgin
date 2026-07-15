/**
 * @file minidialog.c Implementation of the #PidginMiniDialog Gtk widget.
 * @ingroup pidgin
 */

/* pidgin
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include "internal.h"

#include <gtk/gtk.h>

#include "libpurple/prefs.h"

#include "pidgin/minidialog.h"
#include "pidgin/gtkutils.h"
#include "pidgin/pidgin.h"
#include "pidgin/pidginstock.h"

static void     pidgin_mini_dialog_init       (PidginMiniDialog      *self,
                                               GTypeClass *klass);
static void     pidgin_mini_dialog_class_init (PidginMiniDialogClass *klass);

static gpointer pidgin_mini_dialog_parent_class = NULL;

static void
pidgin_mini_dialog_class_intern_init (gpointer klass,
                                      G_GNUC_UNUSED gpointer *class_data)
{
	pidgin_mini_dialog_parent_class = g_type_class_peek_parent (klass);
	pidgin_mini_dialog_class_init ((PidginMiniDialogClass*) klass);
}

GType
pidgin_mini_dialog_get_type (void)
{
	static GType g_define_type_id = 0;
	if (g_define_type_id == 0)
	{
		static const GTypeInfo g_define_type_info = {
			sizeof (PidginMiniDialogClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) pidgin_mini_dialog_class_intern_init,
			(GClassFinalizeFunc) NULL,
			NULL,   /* class_data */
			sizeof (PidginMiniDialog),
			0,      /* n_preallocs */
			(GInstanceInitFunc) pidgin_mini_dialog_init,
			NULL,
		};
		g_define_type_id = g_type_register_static (GTK_TYPE_VBOX,
			"PidginMiniDialog", &g_define_type_info, 0);
	}
	return g_define_type_id;
}

enum
{
	PROP_TITLE = 1,
	PROP_DESCRIPTION,
	PROP_ICON_NAME,
	PROP_CUSTOM_ICON,
	PROP_ENABLE_DESCRIPTION_MARKUP,

	LAST_PROPERTY
} HazeConnectionProperties;

typedef struct _PidginMiniDialogPrivate
{
	GtkImage *icon;
	GtkBox *title_box;
	GtkLabel *title;
	GtkLabel *desc;
	GtkFlowBox *buttons;
	gboolean enable_description_markup;

	guint idle_destroy_cb_id;
} PidginMiniDialogPrivate;

#define PIDGIN_MINI_DIALOG_GET_PRIVATE(dialog) \
	((PidginMiniDialogPrivate *) ((dialog)->priv))

static PidginMiniDialog *
mini_dialog_new(const gchar *title, const gchar *description)
{
	return g_object_new(PIDGIN_TYPE_MINI_DIALOG,
		"title", title,
		"description", description,
		NULL);
}

PidginMiniDialog *
pidgin_mini_dialog_new(const gchar *title,
                       const gchar *description,
                       const gchar *icon_name)
{
	PidginMiniDialog *mini_dialog = mini_dialog_new(title, description);
	pidgin_mini_dialog_set_icon_name(mini_dialog, icon_name);
	return mini_dialog;
}

PidginMiniDialog *
pidgin_mini_dialog_new_with_custom_icon(const gchar *title,
					const gchar *description,
					GdkPixbuf *custom_icon)
{
	PidginMiniDialog *mini_dialog = mini_dialog_new(title, description);
	pidgin_mini_dialog_set_custom_icon(mini_dialog, custom_icon);
	return mini_dialog;
}

void
pidgin_mini_dialog_set_title(PidginMiniDialog *mini_dialog,
                             const char *title)
{
	g_object_set(G_OBJECT(mini_dialog), "title", title, NULL);
}

void
pidgin_mini_dialog_set_description(PidginMiniDialog *mini_dialog,
                                   const char *description)
{
	g_object_set(G_OBJECT(mini_dialog), "description", description, NULL);
}

void
pidgin_mini_dialog_enable_description_markup(PidginMiniDialog *mini_dialog)
{
	g_object_set(G_OBJECT(mini_dialog), "enable-description-markup", TRUE, NULL);
}

gboolean
pidgin_mini_dialog_links_supported()
{
#if GTK_CHECK_VERSION(2,18,0)
	return TRUE;
#else
	return FALSE;
#endif
}

void pidgin_mini_dialog_set_link_callback(PidginMiniDialog *mini_dialog, GCallback cb, gpointer user_data)
{
	g_signal_connect(PIDGIN_MINI_DIALOG_GET_PRIVATE(mini_dialog)->desc, "activate-link", cb, user_data);
}

void
pidgin_mini_dialog_set_icon_name(PidginMiniDialog *mini_dialog,
                                 const char *icon_name)
{
	g_object_set(G_OBJECT(mini_dialog), "icon-name", icon_name, NULL);
}

void
pidgin_mini_dialog_set_custom_icon(PidginMiniDialog *mini_dialog, GdkPixbuf *custom_icon)
{
	g_object_set(G_OBJECT(mini_dialog), "custom-icon", custom_icon, NULL);
}

struct _mini_dialog_button_clicked_cb_data
{
	PidginMiniDialog *mini_dialog;
	PidginMiniDialogCallback callback;
	gpointer user_data;
	gboolean close_dialog_after_click;
};

guint
pidgin_mini_dialog_get_num_children(PidginMiniDialog *mini_dialog)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(mini_dialog->contents));
	guint n = g_list_length(children);
	g_list_free(children);
	return n;
}

static gboolean
idle_destroy_cb(GtkWidget *mini_dialog)
{
	gtk_widget_destroy(mini_dialog);
	return FALSE;
}

static void
mini_dialog_button_clicked_cb(GtkButton *button,
                              gpointer user_data)
{
	struct _mini_dialog_button_clicked_cb_data *data = user_data;
	PidginMiniDialogPrivate *priv =
		PIDGIN_MINI_DIALOG_GET_PRIVATE(data->mini_dialog);

	if (data->close_dialog_after_click) {
		/* Set up the destruction callback before calling the clicked callback,
		 * so that if the mini-dialog gets destroyed during the clicked callback
		 * the idle_destroy_cb is correctly removed by _finalize.
		 */
		priv->idle_destroy_cb_id =
			g_idle_add((GSourceFunc) idle_destroy_cb, data->mini_dialog);
	}

	if (data->callback != NULL)
		data->callback(data->mini_dialog, button, data->user_data);

}

static void
mini_dialog_button_destroy_cb(GtkButton *button,
                              gpointer user_data)
{
	struct _mini_dialog_button_clicked_cb_data *data = user_data;
	g_free(data);
}

static void
mini_dialog_add_button(PidginMiniDialog *self,
                       const char *text,
                       PidginMiniDialogCallback clicked_cb,
                       gpointer user_data,
                       gboolean close_dialog_after_click)
{
	PidginMiniDialogPrivate *priv = PIDGIN_MINI_DIALOG_GET_PRIVATE(self);
	struct _mini_dialog_button_clicked_cb_data *callback_data
		= g_new0(struct _mini_dialog_button_clicked_cb_data, 1);
	GtkWidget *button = gtk_button_new();
	GtkWidget *label = gtk_label_new(NULL);
	char *button_text =
		g_strdup_printf("<span size=\"smaller\">%s</span>", text);

	gtk_label_set_markup_with_mnemonic(GTK_LABEL(label), button_text);
	g_free(button_text);

	callback_data->mini_dialog = self;
	callback_data->callback = clicked_cb;
	callback_data->user_data = user_data;
	callback_data->close_dialog_after_click = close_dialog_after_click;
	g_signal_connect(G_OBJECT(button), "clicked",
		(GCallback) mini_dialog_button_clicked_cb, callback_data);
	g_signal_connect(G_OBJECT(button), "destroy",
		(GCallback) mini_dialog_button_destroy_cb, callback_data);

	pidgin_widget_set_alignment(GTK_WIDGET(label), 0.5, 0.5);
	gtk_container_add(GTK_CONTAINER(button), label);

	/*
	 * A GtkFlowBox wraps its children to the next line when the row is too
	 * narrow (the buddy-list column is only ~200px), so two action buttons
	 * that don't fit side by side stack instead of forcing the whole panel
	 * -- and the buddy list -- wider than the column. Insert at 0 so buttons
	 * keep the previous pack_end ordering (last added appears first).
	 */
	gtk_flow_box_insert(priv->buttons, button, 0);
	gtk_widget_show_all(GTK_WIDGET(button));
}

void
pidgin_mini_dialog_add_button(PidginMiniDialog *self,
                              const char *text,
                              PidginMiniDialogCallback clicked_cb,
                              gpointer user_data)
{
	mini_dialog_add_button(self, text, clicked_cb, user_data, TRUE);
}

void
pidgin_mini_dialog_add_non_closing_button(PidginMiniDialog *self,
                                          const char *text,
                                          PidginMiniDialogCallback clicked_cb,
                                          gpointer user_data)
{
	mini_dialog_add_button(self, text, clicked_cb, user_data, FALSE);
}

static void
pidgin_mini_dialog_get_property(GObject *object,
                                guint property_id,
                                GValue *value,
                                GParamSpec *pspec)
{
	PidginMiniDialog *self = PIDGIN_MINI_DIALOG(object);
	PidginMiniDialogPrivate *priv = PIDGIN_MINI_DIALOG_GET_PRIVATE(self);

	switch (property_id) {
		case PROP_TITLE:
			g_value_set_string(value, gtk_label_get_text(priv->title));
			break;
		case PROP_DESCRIPTION:
			g_value_set_string(value, gtk_label_get_text(priv->desc));
			break;
		case PROP_ICON_NAME:
		{
			gchar *icon_name = NULL;
			GtkIconSize size;
			gtk_image_get_stock(priv->icon, &icon_name, &size);
			g_value_set_string(value, icon_name);
			break;
		}
		case PROP_CUSTOM_ICON:
			g_value_set_object(value, gtk_image_get_pixbuf(priv->icon));
			break;
		case PROP_ENABLE_DESCRIPTION_MARKUP:
			g_value_set_boolean(value, priv->enable_description_markup);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static void
mini_dialog_set_title(PidginMiniDialog *self,
                      const char *title)
{
	PidginMiniDialogPrivate *priv = PIDGIN_MINI_DIALOG_GET_PRIVATE(self);

	char *title_esc = g_markup_escape_text(title, -1);
	char *title_markup = g_strdup_printf(
		"<span weight=\"bold\" size=\"smaller\">%s</span>",
		title_esc ? title_esc : "");

	gtk_label_set_markup(priv->title, title_markup);

	g_free(title_esc);
	g_free(title_markup);
}

static void
mini_dialog_set_description(PidginMiniDialog *self,
                            const char *description)
{
	PidginMiniDialogPrivate *priv = PIDGIN_MINI_DIALOG_GET_PRIVATE(self);
	if(description)
	{
		char *desc_esc = priv->enable_description_markup ? g_strdup(description) : g_markup_escape_text(description, -1);
		char *desc_markup = g_strdup_printf(
			"<span size=\"smaller\">%s</span>", desc_esc);

		gtk_label_set_markup(priv->desc, desc_markup);

		g_free(desc_esc);
		g_free(desc_markup);

		gtk_widget_show(GTK_WIDGET(priv->desc));
		g_object_set(G_OBJECT(priv->desc), "no-show-all", FALSE, NULL);
	}
	else
	{
		gtk_label_set_text(priv->desc, NULL);
		gtk_widget_hide(GTK_WIDGET(priv->desc));
		/* make calling show_all() on the minidialog not affect desc
		 * even though it's packed inside it.
	 	 */
		g_object_set(G_OBJECT(priv->desc), "no-show-all", TRUE, NULL);
	}
}

static void
pidgin_mini_dialog_set_property(GObject *object,
                                guint property_id,
                                const GValue *value,
                                GParamSpec *pspec)
{
	PidginMiniDialog *self = PIDGIN_MINI_DIALOG(object);
	PidginMiniDialogPrivate *priv = PIDGIN_MINI_DIALOG_GET_PRIVATE(self);

	switch (property_id) {
		case PROP_TITLE:
			mini_dialog_set_title(self, g_value_get_string(value));
			break;
		case PROP_DESCRIPTION:
			mini_dialog_set_description(self, g_value_get_string(value));
			break;
		case PROP_ICON_NAME:
			gtk_image_set_from_stock(priv->icon, g_value_get_string(value),
				gtk_icon_size_from_name(PIDGIN_ICON_SIZE_TANGO_EXTRA_SMALL));
			break;
		case PROP_CUSTOM_ICON:
			gtk_image_set_from_pixbuf(priv->icon, g_value_get_object(value));
			break;
		case PROP_ENABLE_DESCRIPTION_MARKUP:
			priv->enable_description_markup = g_value_get_boolean(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static void
pidgin_mini_dialog_finalize(GObject *object)
{
	PidginMiniDialog *self = PIDGIN_MINI_DIALOG(object);
	PidginMiniDialogPrivate *priv = PIDGIN_MINI_DIALOG_GET_PRIVATE(self);

	if (priv->idle_destroy_cb_id)
		g_source_remove(priv->idle_destroy_cb_id);

	g_free(priv);
	self->priv = NULL;

	purple_prefs_disconnect_by_handle(self);

	G_OBJECT_CLASS (pidgin_mini_dialog_parent_class)->finalize (object);
}

static void
pidgin_mini_dialog_class_init(PidginMiniDialogClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GParamSpec *param_spec;

	object_class->get_property = pidgin_mini_dialog_get_property;
	object_class->set_property = pidgin_mini_dialog_set_property;
	object_class->finalize = pidgin_mini_dialog_finalize;

	param_spec = g_param_spec_string("title", "title",
		"String specifying the mini-dialog's title", NULL,
		G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB |
		G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_TITLE, param_spec);

	param_spec = g_param_spec_string("description", "description",
		"Description text for the mini-dialog, if desired", NULL,
		G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB |
		G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_DESCRIPTION, param_spec);

	param_spec = g_param_spec_string("icon-name", "icon-name",
		"String specifying the Gtk stock name of the dialog's icon",
		NULL,
		G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB |
		G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_ICON_NAME, param_spec);

	param_spec = g_param_spec_object("custom-icon", "custom-icon",
		"Pixbuf to use as the dialog's icon",
		GDK_TYPE_PIXBUF,
		G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB |
		G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_CUSTOM_ICON, param_spec);

	param_spec = g_param_spec_boolean("enable-description-markup", "enable-description-markup",
		"Use GMarkup in the description text", FALSE,
		G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB |
		G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_ENABLE_DESCRIPTION_MARKUP, param_spec);
}

/* 16 is the width of the icon, due to PIDGIN_ICON_SIZE_TANGO_EXTRA_SMALL */

static void
pidgin_mini_dialog_init(PidginMiniDialog *self,
                        G_GNUC_UNUSED GTypeClass *klass)
{
	GtkBox *self_box = GTK_BOX(self);

	PidginMiniDialogPrivate *priv = g_new0(PidginMiniDialogPrivate, 1);
	self->priv = priv;

	gtk_container_set_border_width(GTK_CONTAINER(self), PIDGIN_HIG_BOX_SPACE);

	priv->title_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PIDGIN_HIG_BOX_SPACE));

	priv->icon = GTK_IMAGE(gtk_image_new());
	pidgin_widget_set_alignment(GTK_WIDGET(priv->icon), 0, 0);

	priv->title = GTK_LABEL(gtk_label_new(NULL));
	gtk_label_set_line_wrap(priv->title, TRUE);
	gtk_label_set_line_wrap_mode(priv->title, PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(priv->title, TRUE);
	/*
	 * The buddy-list column is narrow (~200px). Do NOT pin a large
	 * width_chars: that sets the label's MINIMUM width, which would make
	 * the whole mini-dialog demand more width than the column and push the
	 * buddy list into horizontal overflow (clipping its left edge). Allow
	 * the label to shrink to the column and wrap; cap the natural width so
	 * it doesn't try to grow the panel wide either.
	 */
	gtk_label_set_width_chars(priv->title, -1);
	gtk_label_set_max_width_chars(priv->title, 24);
	gtk_widget_set_hexpand(GTK_WIDGET(priv->title), TRUE);
	pidgin_widget_set_alignment(GTK_WIDGET(priv->title), 0, 0);

	gtk_box_pack_start(priv->title_box, GTK_WIDGET(priv->icon), FALSE, FALSE, 0);
	gtk_box_pack_start(priv->title_box, GTK_WIDGET(priv->title), TRUE, TRUE, 0);

	priv->desc = GTK_LABEL(gtk_label_new(NULL));
	gtk_label_set_line_wrap(priv->desc, TRUE);
	gtk_label_set_line_wrap_mode(priv->desc, PANGO_WRAP_WORD_CHAR);
	gtk_label_set_width_chars(priv->desc, -1);
	gtk_label_set_max_width_chars(priv->desc, 24);
	gtk_widget_set_hexpand(GTK_WIDGET(priv->desc), TRUE);
	pidgin_widget_set_alignment(GTK_WIDGET(priv->desc), 0, 0);
	gtk_label_set_selectable(priv->desc, TRUE);
	/* make calling show_all() on the minidialog not affect desc even though
	 * it's packed inside it.
	 */
	g_object_set(G_OBJECT(priv->desc), "no-show-all", TRUE, NULL);

	/*
	 * NB: we no longer force a fixed pixel width on the title/desc labels
	 * (that GTK2-era hack broke GTK3 height-for-width and made the dialog
	 * mis-size, letting buttons/close overlap sibling widgets). The labels
	 * now wrap naturally to their allocation, so there is nothing to update
	 * when the buddy-list width changes.
	 */

	self->contents = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

	/*
	 * Action buttons live in a GtkFlowBox so they wrap to a second line
	 * when the narrow buddy-list column can't fit them side by side
	 * (instead of overflowing and clipping the buddy list). Spacing gives
	 * the buttons breathing room from each other and the panel edges.
	 */
	priv->buttons = GTK_FLOW_BOX(gtk_flow_box_new());
	gtk_flow_box_set_selection_mode(priv->buttons, GTK_SELECTION_NONE);
	gtk_flow_box_set_column_spacing(priv->buttons, PIDGIN_HIG_BOX_SPACE);
	gtk_flow_box_set_row_spacing(priv->buttons, PIDGIN_HIG_BOX_SPACE);
	gtk_flow_box_set_homogeneous(priv->buttons, FALSE);
	gtk_widget_set_halign(GTK_WIDGET(priv->buttons), GTK_ALIGN_START);
	gtk_container_set_border_width(GTK_CONTAINER(priv->buttons), PIDGIN_HIG_BOX_SPACE);

	gtk_box_pack_start(self_box, GTK_WIDGET(priv->title_box), FALSE, FALSE, 0);
	gtk_box_pack_start(self_box, GTK_WIDGET(priv->desc), FALSE, FALSE, 0);
	gtk_box_pack_start(self_box, GTK_WIDGET(self->contents), TRUE, TRUE, 0);
	gtk_box_pack_start(self_box, GTK_WIDGET(priv->buttons), FALSE, FALSE, PIDGIN_HIG_BOX_SPACE);

	gtk_widget_show_all(GTK_WIDGET(self));
}

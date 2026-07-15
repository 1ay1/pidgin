/*
 * @file gtkscrollbook.c GTK+ Scrolling notebook widget
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

#include "gtkscrollbook.h"


static void pidgin_scroll_book_init (PidginScrollBook *scroll_book,
                                     GTypeClass *klass);
static void pidgin_scroll_book_class_init (PidginScrollBookClass *klass,
                                           gpointer class_data);

GType
pidgin_scroll_book_get_type (void)
{
	static GType scroll_book_type = 0;

	if (!scroll_book_type)
	{
		static const GTypeInfo scroll_book_info =
		{
			sizeof (PidginScrollBookClass),
			NULL, /* base_init */
			NULL, /* base_finalize */
			(GClassInitFunc) pidgin_scroll_book_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (PidginScrollBook),
			0,
			(GInstanceInitFunc) pidgin_scroll_book_init,
			NULL  /* value_table */
		};

		scroll_book_type = g_type_register_static(GTK_TYPE_VBOX,
							 "PidginScrollBook",
							 &scroll_book_info,
							 0);
	}

	return scroll_book_type;
}

static gboolean
scroll_left_cb(PidginScrollBook *scroll_book, GdkEventButton *event)
{
	int index;

	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;

	index = gtk_notebook_get_current_page(GTK_NOTEBOOK(scroll_book->notebook));

	if (index > 0)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(scroll_book->notebook), index - 1);
	return TRUE;
}

static gboolean
scroll_right_cb(PidginScrollBook *scroll_book, GdkEventButton *event)
{
	int index, count;

	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;

	index = gtk_notebook_get_current_page(GTK_NOTEBOOK(scroll_book->notebook));
	count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(scroll_book->notebook));

	if (index + 1 < count)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(scroll_book->notebook), index + 1);
	return TRUE;
}

static void
refresh_scroll_box(PidginScrollBook *scroll_book, int index, int count)
{
	char *label;

	/*
	 * Only ensure the pages/notebook are shown here. Do NOT show_all the
	 * whole scrollbook: that would reveal the nav header (hbox) even when
	 * there are no pages, leaving an empty "prev/next/x" bar floating above
	 * the status box. The hbox's children are shown once at init; here we
	 * just toggle the hbox itself by page count.
	 */
	gtk_widget_show(scroll_book->notebook);
	if (count < 1)
		gtk_widget_hide(scroll_book->hbox);
	else {
		gtk_widget_show(scroll_book->hbox);
		if (count == 1) {
			gtk_widget_hide(scroll_book->label);
			gtk_widget_hide(scroll_book->left_arrow);
			gtk_widget_hide(scroll_book->right_arrow);
		} else {
			gtk_widget_show(scroll_book->label);
			gtk_widget_show(scroll_book->left_arrow);
			gtk_widget_show(scroll_book->right_arrow);
		}
	}

	label = g_strdup_printf("<span size='smaller' weight='bold'>(%d/%d)</span>", index+1, count);
	gtk_label_set_markup(GTK_LABEL(scroll_book->label), label);
	g_free(label);

	if (index == 0)
		gtk_widget_set_sensitive(scroll_book->left_arrow, FALSE);
	else
		gtk_widget_set_sensitive(scroll_book->left_arrow, TRUE);


	if (index + 1 == count)
		gtk_widget_set_sensitive(scroll_book->right_arrow, FALSE);
	else
		gtk_widget_set_sensitive(scroll_book->right_arrow, TRUE);

	/*
	 * GtkNotebook commits to a height based on the visible page's
	 * height-at-natural-width and does NOT re-request when that page's
	 * height-for-width later changes (e.g. a wrapping label gets a
	 * narrower allocation and needs more rows). That left the mini-dialog
	 * buttons/close overflowing their allocation and overlapping sibling
	 * widgets until some unrelated event finally triggered a resize --
	 * the "it fixes itself after a while" symptom. Force the recompute now.
	 */
	gtk_widget_queue_resize(scroll_book->notebook);
	gtk_widget_queue_resize(GTK_WIDGET(scroll_book));
}


static void
page_count_change_cb(PidginScrollBook *scroll_book)
{
	int count;
	int index = gtk_notebook_get_current_page(GTK_NOTEBOOK(scroll_book->notebook));
	count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(scroll_book->notebook));
	refresh_scroll_box(scroll_book, index, count);
}

static gboolean
scroll_close_cb(PidginScrollBook *scroll_book, GdkEventButton *event)
{
	if (event->type == GDK_BUTTON_PRESS)
		gtk_widget_destroy(gtk_notebook_get_nth_page(GTK_NOTEBOOK(scroll_book->notebook), gtk_notebook_get_current_page(GTK_NOTEBOOK(scroll_book->notebook))));
	return FALSE;
}

static void
switch_page_cb(GtkNotebook *notebook, GtkWidget *page, guint page_num, PidginScrollBook *scroll_book)
{
	int count;
	count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(scroll_book->notebook));
	refresh_scroll_box(scroll_book, page_num, count);
}

static void
pidgin_scroll_book_add(GtkContainer *container, GtkWidget *widget)
{
	PidginScrollBook *scroll_book;

	g_return_if_fail(GTK_IS_WIDGET (widget));
	g_return_if_fail (gtk_widget_get_parent(widget) == NULL);

	scroll_book = PIDGIN_SCROLL_BOOK(container);
	scroll_book->children = g_list_append(scroll_book->children, widget);
	gtk_widget_show(widget);
	gtk_notebook_append_page(GTK_NOTEBOOK(scroll_book->notebook), widget, NULL);
	page_count_change_cb(PIDGIN_SCROLL_BOOK(container));
}

static void
pidgin_scroll_book_remove(GtkContainer *container, GtkWidget *widget)
{
	int page;
	PidginScrollBook *scroll_book;
	g_return_if_fail(GTK_IS_WIDGET(widget));

	scroll_book = PIDGIN_SCROLL_BOOK(container);
	scroll_book->children = g_list_remove(scroll_book->children, widget);
	/* gtk_widget_unparent(widget); */

	page = gtk_notebook_page_num(GTK_NOTEBOOK(PIDGIN_SCROLL_BOOK(container)->notebook), widget);
	if (page >= 0) {
		gtk_notebook_remove_page(GTK_NOTEBOOK(PIDGIN_SCROLL_BOOK(container)->notebook), page);
	}
}

static void
pidgin_scroll_book_class_init (PidginScrollBookClass *klass,
                               G_GNUC_UNUSED gpointer class_data)
{
	GtkContainerClass *container_class = (GtkContainerClass*)klass;

	container_class->add = pidgin_scroll_book_add;
	container_class->remove = pidgin_scroll_book_remove;
	/*
	 * Deliberately NOT overriding container_class->forall.
	 *
	 * hbox and notebook are packed into this widget with
	 * gtk_box_pack_start(), so the inherited GtkBox forall() already
	 * reports them -- which is exactly what GTK3's size negotiation and
	 * allocation need. The old GTK2-era override reported the children
	 * only under include_internals and #if 0'd out everything else; under
	 * GTK3 that hid the children from the size machinery, so the widget
	 * requested ~zero height and its nav header/notebook were mis-placed
	 * (empty scroll bars floating over the status box, real alert content
	 * drawn on top of "Available"). Letting GtkBox handle forall() fixes
	 * the layout.
	 */
}

static gboolean
close_button_left_cb(GtkWidget *widget, GdkEventCrossing *event, GtkLabel *label)
{
	static GdkCursor *ptr = NULL;
	if (ptr == NULL) {
		ptr = gdk_cursor_new_for_display(gdk_window_get_display(event->window), GDK_LEFT_PTR);
	}

	gtk_label_set_markup(label, "×");
	gdk_window_set_cursor(event->window, ptr);
	return FALSE;
}

static gboolean
close_button_entered_cb(GtkWidget *widget, GdkEventCrossing *event, GtkLabel *label)
{
	static GdkCursor *hand = NULL;
	if (hand == NULL) {
		hand = gdk_cursor_new_for_display(gdk_window_get_display(event->window), GDK_HAND2);
	}

	gtk_label_set_markup(label, "<u>×</u>");
	gdk_window_set_cursor(event->window, hand);
	return FALSE;
}

static void
pidgin_scroll_book_init(PidginScrollBook *scroll_book,
                        G_GNUC_UNUSED GTypeClass *klass)
{
	GtkWidget *eb;
	GtkWidget *close_button;

	scroll_book->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	/* Close */
	eb = gtk_event_box_new();
	gtk_box_pack_end(GTK_BOX(scroll_book->hbox), eb, FALSE, FALSE, 0);
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
	gtk_widget_set_events(eb, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
	close_button = gtk_label_new("×");
	g_signal_connect(G_OBJECT(eb), "enter-notify-event", G_CALLBACK(close_button_entered_cb), close_button);
	g_signal_connect(G_OBJECT(eb), "leave-notify-event", G_CALLBACK(close_button_left_cb), close_button);
	gtk_container_add(GTK_CONTAINER(eb), close_button);
	g_signal_connect_swapped(G_OBJECT(eb), "button-press-event", G_CALLBACK(scroll_close_cb), scroll_book);

	/* Right arrow */
	eb = gtk_event_box_new();
	gtk_box_pack_end(GTK_BOX(scroll_book->hbox), eb, FALSE, FALSE, 0);
	/* GTK3: GtkArrow (deprecated 3.14) draws nothing under most GTK3 themes;
	 * use the themed directional icon so the nav arrow stays visible. */
	scroll_book->right_arrow = gtk_image_new_from_icon_name("pan-end-symbolic",
			GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(eb), scroll_book->right_arrow);
	g_signal_connect_swapped(G_OBJECT(eb), "button-press-event", G_CALLBACK(scroll_right_cb), scroll_book);

	/* Count */
	scroll_book->label = gtk_label_new(NULL);
	gtk_box_pack_end(GTK_BOX(scroll_book->hbox), scroll_book->label, FALSE, FALSE, 0);

	/* Left arrow */
	eb = gtk_event_box_new();
	gtk_box_pack_end(GTK_BOX(scroll_book->hbox), eb, FALSE, FALSE, 0);
	scroll_book->left_arrow = gtk_image_new_from_icon_name("pan-start-symbolic",
			GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(eb), scroll_book->left_arrow);
	g_signal_connect_swapped(G_OBJECT(eb), "button-press-event", G_CALLBACK(scroll_left_cb), scroll_book);

	gtk_box_pack_start(GTK_BOX(scroll_book), scroll_book->hbox, FALSE, FALSE, 0);

	scroll_book->notebook = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(scroll_book->notebook), FALSE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(scroll_book->notebook), FALSE);

	gtk_box_pack_start(GTK_BOX(scroll_book), scroll_book->notebook, TRUE, TRUE, 0);

	g_signal_connect_swapped(G_OBJECT(scroll_book->notebook), "remove", G_CALLBACK(page_count_change_cb), scroll_book);
	g_signal_connect(G_OBJECT(scroll_book->notebook), "switch-page", G_CALLBACK(switch_page_cb), scroll_book);
	gtk_widget_show_all(scroll_book->notebook);

	/*
	 * Show the nav header's children once so that when refresh_scroll_box()
	 * later shows the hbox (pages present) they are already visible. Then
	 * hide the hbox and mark it no-show-all so an empty nav bar is never
	 * revealed -- not at startup, and not by any parent gtk_widget_show_all()
	 * on the buddy list. Its visibility is driven solely by
	 * refresh_scroll_box() based on the page count.
	 */
	gtk_widget_show_all(scroll_book->hbox);
	gtk_widget_hide(scroll_book->hbox);
	gtk_widget_set_no_show_all(scroll_book->hbox, TRUE);
}

GtkWidget *
pidgin_scroll_book_new()
{
	return g_object_new(PIDGIN_TYPE_SCROLL_BOOK, NULL);
}

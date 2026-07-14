/*
 * @file gtkdnd-hints.c GTK+ Drag-and-Drop arrow hints
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
 * the Free Software Foundation; either version 2, or(at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301, USA.
 */

#include "gtkdnd-hints.h"
#include "gtkutils.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#ifdef _WIN32
#include "win32dep.h"
#endif

typedef struct
{
	GtkWidget *widget;
	gchar *filename;
	gint ox;
	gint oy;

} HintWindowInfo;

/**
 * Info about each hint widget. See DndHintWindowId enum.
 */
static HintWindowInfo hint_windows[] = {
	{ NULL, "arrow-up.xpm",   -13/2,     0 },
	{ NULL, "arrow-down.xpm", -13/2,   -16 },
	{ NULL, "arrow-left.xpm",     0, -13/2 },
	{ NULL, "arrow-right.xpm",  -16, -13/2 },
	{ NULL, NULL, 0, 0 }
};

static GtkWidget *
dnd_hints_init_window(const gchar *fname)
{
	GdkPixbuf *pixbuf;
	cairo_region_t *shape;
	cairo_surface_t *surface;
	cairo_t *cr;
	GtkWidget *pix;
	GtkWidget *win;
	GdkScreen *screen;
	GdkVisual *visual;

	pixbuf = gdk_pixbuf_new_from_file(fname, NULL);
	g_return_val_if_fail(pixbuf, NULL);

	win = gtk_window_new(GTK_WINDOW_POPUP);

	/* Give the popup an RGBA visual so the arrow can be alpha-shaped. */
	screen = gtk_widget_get_screen(win);
	visual = gdk_screen_get_rgba_visual(screen);
	if (visual != NULL) {
		gtk_widget_set_visual(win, visual);
		gtk_widget_set_app_paintable(win, TRUE);
	}

	pix = gtk_image_new_from_pixbuf(pixbuf);
	gtk_container_add(GTK_CONTAINER(win), pix);

	/* Build a shape region from the pixbuf's alpha channel so the window
	 * takes the arrow silhouette, matching the old bitmap-mask behavior. */
	surface = cairo_image_surface_create(CAIRO_FORMAT_A1,
	                                     gdk_pixbuf_get_width(pixbuf),
	                                     gdk_pixbuf_get_height(pixbuf));
	cr = cairo_create(surface);
	gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	shape = gdk_cairo_region_create_from_surface(surface);
	cairo_surface_destroy(surface);

	gtk_widget_realize(win);
	gtk_widget_shape_combine_region(win, shape);
	cairo_region_destroy(shape);

	g_object_unref(G_OBJECT(pixbuf));

	gtk_widget_show_all(pix);

	return win;
}

static void
get_widget_coords(GtkWidget *w, gint *x1, gint *y1, gint *x2, gint *y2)
{
	gint ox, oy, width, height;
	GtkWidget *parent = gtk_widget_get_parent(w);
	GdkWindow *win = gtk_widget_get_window(w);
	GtkAllocation alloc;

	gtk_widget_get_allocation(w, &alloc);

	if (parent && gtk_widget_get_window(parent) == win)
	{
		get_widget_coords(parent, &ox, &oy, NULL, NULL);
		height = alloc.height;
		width = alloc.width;
	}
	else
	{
		gdk_window_get_origin(win, &ox, &oy);
		pidgin_gdk_window_get_size(win, &width, &height);
	}

	if (x1) *x1 = ox;
	if (y1) *y1 = oy;
	if (x2) *x2 = ox + width;
	if (y2) *y2 = oy + height;
}

static void
dnd_hints_init(void)
{
	static gboolean done = FALSE;
	gint i;

	if (done)
		return;

	done = TRUE;

	for (i = 0; hint_windows[i].filename != NULL; i++) {
		gchar *fname;

		fname = g_build_filename(DATADIR, "pixmaps", "pidgin",
								 hint_windows[i].filename, NULL);

		hint_windows[i].widget = dnd_hints_init_window(fname);

		g_free(fname);
	}
}

void
dnd_hints_hide_all(void)
{
	gint i;

	for (i = 0; hint_windows[i].filename != NULL; i++)
		dnd_hints_hide(i);
}

void
dnd_hints_hide(DndHintWindowId i)
{
	GtkWidget *w = hint_windows[i].widget;

	if (w && GTK_IS_WIDGET(w))
		gtk_widget_hide(w);
}

void
dnd_hints_show(DndHintWindowId id, gint x, gint y)
{
	GtkWidget *w;

	dnd_hints_init();

	w = hint_windows[id].widget;

	if (w && GTK_IS_WIDGET(w))
	{
		gtk_window_move(GTK_WINDOW(w), hint_windows[id].ox + x,
								 hint_windows[id].oy + y);
		gtk_widget_show(w);
	}
}

void
dnd_hints_show_relative(DndHintWindowId id, GtkWidget *widget,
						DndHintPosition horiz, DndHintPosition vert)
{
	gint x1, x2, y1, y2;
	gint x = 0, y = 0;

	get_widget_coords(widget, &x1, &y1, &x2, &y2);
	{
		GtkAllocation alloc;
		gtk_widget_get_allocation(widget, &alloc);
		x1 += alloc.x;	x2 += alloc.x;
		y1 += alloc.y;	y2 += alloc.y;
	}

	switch (horiz)
	{
		case HINT_POSITION_RIGHT:  x = x2;            break;
		case HINT_POSITION_LEFT:   x = x1;            break;
		case HINT_POSITION_CENTER: x = (x1 + x2) / 2; break;
		default:
			/* should not happen */
			g_warning("Invalid parameter to dnd_hints_show_relative");
			break;
	}

	switch (vert)
	{
		case HINT_POSITION_TOP:    y = y1;            break;
		case HINT_POSITION_BOTTOM: y = y2;            break;
		case HINT_POSITION_CENTER: y = (y1 + y2) / 2; break;
		default:
			/* should not happen */
			g_warning("Invalid parameter to dnd_hints_show_relative");
			break;
	}

	dnd_hints_show(id, x, y);
}


#!/usr/bin/env python3
"""GTK2->GTK3 mechanical transform, PASS 2.
Idempotent, safe to run once on the pass-1 tree. Only the unambiguous
global renames; structural cases (->window, ->allocation, ->style,
GtkTooltips struct fields, combo/option menus) are done by hand.
"""
import re, pathlib

def convert(src: str) -> str:
    # GtkObject -> GObject family
    src = re.sub(r'\bgtk_object_sink\s*\(', r'g_object_ref_sink(', src)
    src = re.sub(r'\bGTK_OBJECT\s*\(', r'G_OBJECT(', src)
    src = re.sub(r'\bGTK_TYPE_OBJECT\b', r'G_TYPE_OBJECT', src)
    src = re.sub(r'\bGtkObject\b', r'GObject', src)

    # Removed GtkDialog separator setter -> no-op
    src = re.sub(r'gtk_dialog_set_has_separator\s*\([^;]*?\)\s*;',
                 r'/* set_has_separator removed */;', src)

    # gtk_tooltips_new() -> NULL (handle now unused; struct fields removed by hand)
    src = re.sub(r'gtk_tooltips_new\s*\(\s*\)', r'NULL', src)

    # gtk_tooltips_set_tip(tips, widget, text, priv) -> gtk_widget_set_tooltip_text(widget, text)
    src = re.sub(
        r'gtk_tooltips_set_tip\s*\(\s*[^,]+,\s*([^,]+?),\s*([^,]+?),\s*[^)]*?\)',
        r'gtk_widget_set_tooltip_text(\1, \2)', src)

    # gdk_drawable_get_size(win, &w, &h) -> gdk_window_get_geometry-ish helper
    src = re.sub(r'\bgdk_drawable_get_size\s*\(', r'pidgin_gdk_window_get_size(', src)
    src = re.sub(r'\bgdk_drawable_get_display\s*\(', r'gdk_window_get_display(', src)

    return src

def main():
    root = pathlib.Path('pidgin')
    changed = 0
    for f in list(root.glob('*.c')) + list(root.glob('*.h')):
        if 'win32' in str(f):
            continue
        txt = f.read_text()
        new = convert(txt)
        if new != txt:
            f.write_text(new)
            changed += 1
            print(f"  {f}")
    print(f"changed {changed} files")

if __name__ == '__main__':
    main()

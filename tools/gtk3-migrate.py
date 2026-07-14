#!/usr/bin/env python3
"""GTK2->GTK3 mechanical transform pass for Pidgin.
Handles the regular, unambiguous rewrites. Structural cases
(GtkItemFactory, cell renderers, custom draw) are done by hand.
"""
import re, sys, pathlib

def convert(src: str) -> str:
    # --- boxes: gtk_hbox_new(homog, spacing) / gtk_vbox_new(...) ---
    # FALSE homogeneous is the overwhelming majority -> direct map.
    src = re.sub(r'gtk_hbox_new\s*\(\s*FALSE\s*,\s*([^)]*)\)',
                 r'gtk_box_new(GTK_ORIENTATION_HORIZONTAL, \1)', src)
    src = re.sub(r'gtk_vbox_new\s*\(\s*FALSE\s*,\s*([^)]*)\)',
                 r'gtk_box_new(GTK_ORIENTATION_VERTICAL, \1)', src)
    # TRUE homogeneous -> wrap with a homogeneous setter via a comma helper.
    # Rare; mark with a helper call the codebase provides.
    src = re.sub(r'gtk_hbox_new\s*\(\s*TRUE\s*,\s*([^)]*)\)',
                 r'pidgin_box_new_homogeneous(GTK_ORIENTATION_HORIZONTAL, \1)', src)
    src = re.sub(r'gtk_vbox_new\s*\(\s*TRUE\s*,\s*([^)]*)\)',
                 r'pidgin_box_new_homogeneous(GTK_ORIENTATION_VERTICAL, \1)', src)

    # --- separators ---
    src = re.sub(r'gtk_hseparator_new\s*\(\s*\)',
                 r'gtk_separator_new(GTK_ORIENTATION_HORIZONTAL)', src)
    src = re.sub(r'gtk_vseparator_new\s*\(\s*\)',
                 r'gtk_separator_new(GTK_ORIENTATION_VERTICAL)', src)

    # --- scales ---
    src = re.sub(r'gtk_hscale_new\s*\(', r'gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, ', src)
    src = re.sub(r'gtk_vscale_new\s*\(', r'gtk_scale_new(GTK_ORIENTATION_VERTICAL, ', src)
    src = re.sub(r'gtk_hscale_new_with_range\s*\(',
                 r'gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, ', src)
    src = re.sub(r'gtk_vscale_new_with_range\s*\(',
                 r'gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, ', src)

    # --- paned ---
    src = re.sub(r'gtk_hpaned_new\s*\(\s*\)',
                 r'gtk_paned_new(GTK_ORIENTATION_HORIZONTAL)', src)
    src = re.sub(r'gtk_vpaned_new\s*\(\s*\)',
                 r'gtk_paned_new(GTK_ORIENTATION_VERTICAL)', src)

    # --- button boxes ---
    src = re.sub(r'gtk_hbutton_box_new\s*\(\s*\)',
                 r'gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL)', src)
    src = re.sub(r'gtk_vbutton_box_new\s*\(\s*\)',
                 r'gtk_button_box_new(GTK_ORIENTATION_VERTICAL)', src)

    # --- widget flag macros -> accessors ---
    src = re.sub(r'GTK_WIDGET_HAS_FOCUS\s*\(([^)]*)\)', r'gtk_widget_has_focus(\1)', src)
    src = re.sub(r'GTK_WIDGET_HAS_DEFAULT\s*\(([^)]*)\)', r'gtk_widget_has_default(\1)', src)
    src = re.sub(r'GTK_WIDGET_VISIBLE\s*\(([^)]*)\)', r'gtk_widget_get_visible(\1)', src)
    src = re.sub(r'GTK_WIDGET_REALIZED\s*\(([^)]*)\)', r'gtk_widget_get_realized(\1)', src)
    src = re.sub(r'GTK_WIDGET_MAPPED\s*\(([^)]*)\)', r'gtk_widget_get_mapped(\1)', src)
    src = re.sub(r'GTK_WIDGET_SENSITIVE\s*\(([^)]*)\)', r'gtk_widget_get_sensitive(\1)', src)
    src = re.sub(r'GTK_WIDGET_IS_SENSITIVE\s*\(([^)]*)\)', r'gtk_widget_is_sensitive(\1)', src)
    src = re.sub(r'GTK_WIDGET_CAN_FOCUS\s*\(([^)]*)\)', r'gtk_widget_get_can_focus(\1)', src)
    src = re.sub(r'GTK_WIDGET_CAN_DEFAULT\s*\(([^)]*)\)', r'gtk_widget_get_can_default(\1)', src)
    src = re.sub(r'GTK_WIDGET_STATE\s*\(([^)]*)\)', r'gtk_widget_get_state(\1)', src)
    src = re.sub(r'GTK_WIDGET_DRAWABLE\s*\(([^)]*)\)', r'gtk_widget_is_drawable(\1)', src)

    # --- set flags: GTK_WIDGET_SET_FLAGS(w, GTK_CAN_DEFAULT) etc ---
    src = re.sub(r'GTK_WIDGET_SET_FLAGS\s*\(([^,]+),\s*GTK_CAN_DEFAULT\s*\)',
                 r'gtk_widget_set_can_default(\1, TRUE)', src)
    src = re.sub(r'GTK_WIDGET_UNSET_FLAGS\s*\(([^,]+),\s*GTK_CAN_DEFAULT\s*\)',
                 r'gtk_widget_set_can_default(\1, FALSE)', src)
    src = re.sub(r'GTK_WIDGET_SET_FLAGS\s*\(([^,]+),\s*GTK_CAN_FOCUS\s*\)',
                 r'gtk_widget_set_can_focus(\1, TRUE)', src)
    src = re.sub(r'GTK_WIDGET_UNSET_FLAGS\s*\(([^,]+),\s*GTK_CAN_FOCUS\s*\)',
                 r'gtk_widget_set_can_focus(\1, FALSE)', src)

    # --- size request ---
    src = re.sub(r'gtk_widget_size_request\s*\(', r'gtk_widget_get_preferred_size_compat(', src)

    # --- hide_all -> hide (GTK3 hide is recursive-effective for our uses) ---
    src = re.sub(r'gtk_widget_hide_all\s*\(', r'gtk_widget_hide(', src)

    # --- GDK key syms: GDK_Foo -> GDK_KEY_Foo ---
    # Keysyms are the ONLY GDK_ constants that contain a lowercase letter
    # (GDK_Escape, GDK_Return, GDK_Page_Up, GDK_KP_Enter, GDK_z). All-caps
    # GDK_ names are event masks / cursor types / enums and must be left alone.
    def _keysym(m):
        name = m.group(1)
        if name.startswith('KEY_'):
            return m.group(0)
        if any(c.islower() for c in name):
            return 'GDK_KEY_' + name
        return m.group(0)
    src = re.sub(r'\bGDK_([A-Za-z][A-Za-z0-9_]*)\b', _keysym, src)

    return src

def main():
    root = pathlib.Path('pidgin')
    files = list(root.glob('*.c')) + list(root.glob('*.h'))
    changed = 0
    for f in files:
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

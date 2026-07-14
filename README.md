# 🐦 Pidgin 2.x — GTK 3 port

> **Keep the classic Pidgin 2 buddy list alive on modern Linux desktops.**

<p align="center">
  <a href="https://github.com/1ay1/pidgin/stargazers"><img src="https://img.shields.io/github/stars/1ay1/pidgin?style=for-the-badge&logo=github&color=gold" alt="Stars"></a>
  <a href="https://github.com/1ay1/pidgin/network/members"><img src="https://img.shields.io/github/forks/1ay1/pidgin?style=for-the-badge&logo=github&color=blue" alt="Forks"></a>
  <a href="https://github.com/1ay1/pidgin/issues"><img src="https://img.shields.io/github/issues/1ay1/pidgin?style=for-the-badge&logo=github&color=success" alt="Issues"></a>
  <img src="https://img.shields.io/badge/GTK-3-729FCF?style=for-the-badge&logo=gnome&logoColor=white" alt="GTK 3">
  <img src="https://img.shields.io/badge/language-C-555?style=for-the-badge&logo=c&logoColor=white" alt="C">
  <img src="https://img.shields.io/badge/license-GPL--2.0-orange?style=for-the-badge" alt="License">
</p>

A fork of **Pidgin 2.15.0dev** (the `2.x` line) ported from GTK+ 2 to
**GTK 3**. It keeps the classic Pidgin 2 experience — the same buddy list,
conversation windows, status box, plugins and protocols — while running on a
modern, still-maintained toolkit.

> This is **not** Pidgin 3. The goal is to keep the mature 2.x UI alive on
> current systems, not to migrate to the 3.x codebase.

> **⭐ Like keeping Pidgin 2 alive? [Star the repo](https://github.com/1ay1/pidgin) — it genuinely helps others find it.**

See `AUTHORS` and `COPYRIGHT` for the list of contributors.

## Components

| Component    | Description                                                              |
|--------------|--------------------------------------------------------------------------|
| `libpurple`  | Core library for IM clients that connect to many networks (XMPP, etc.).  |
| **Pidgin**   | Graphical IM client written in C, using the **GTK 3** toolkit.           |
| `finch`      | Text-based IM client written in C, using the ncurses toolkit.            |

## What the GTK 3 port changes

The port replaces the deprecated GTK 2 APIs that either warn loudly or render
nothing under GTK 3, and fixes the layout and teardown regressions that came
with the toolkit change:

- **Theming** moved from GTK 2 RC files to **GTK 3 CSS providers** and live
  style contexts — buddy-list colors, dark-mode detection, the `pidginrc`
  plugin, per-tab conversation colors, and custom fonts.
- **Deprecated widgets/APIs replaced** — `GtkArrow`, `GtkMisc`/`GtkAlignment`
  alignment, `gtk_widget_modify_font`, `gdk_cairo_create`,
  `gdk_screen_width/height` (now per-monitor geometry), tree-view rules hints,
  and stock item / item-factory menus.
- **Layout fixes** — collapsed conversation history, dead voids below the entry
  box, `GtkPaned` child collapse in the log viewer, a buddy list that now
  shrinks horizontally, and the status box no longer stretching over the list.
- **Status box** — self-painted dropdown arrow (`gtk_render_arrow()`, always
  visible under any theme), restored avatar box, and fixes for the
  status-dropdown hang and shutdown `GObject` assertions.
- **Menu icons** — restored under GTK 3 (which hides `GtkImageMenuItem` icons by
  default) and aligned into one column with check items.
- **Built-in tray icon** — an Ayatana AppIndicator (SNI) docklet backend that
  works on modern desktops out of the box.

## Build

Pidgin is built with **Meson** and **Ninja**:

```sh
meson setup build
ninja -C build
```

Run `meson configure build` to see the available options, and set them with,
for example:

```sh
meson setup build -Dgtk=enabled -Ddbus=disabled
```

Building Pidgin requires **GTK 3** and its development files; Meson fails to
configure without them. Install them via your distribution's package manager.

Optional dependencies:

- **Sound** — GStreamer.
- **Spellchecking** — GtkSpell.
- **Tray icon** — `ayatana-appindicator3-0.1` (falls back to
  `appindicator3-0.1`); auto-detected.

Read the `INSTALL` file for more detailed directions.

## Run

Install so plugins and other files land where they are expected:

```sh
sudo ninja -C build install
```

Then run `pidgin` (or `finch`) and add a new account to get started.

To run straight from the build tree without installing:

```sh
env LD_LIBRARY_PATH=build/libpurple \
    PURPLE_PLUGIN_PATH="$PWD/build/libpurple/plugins:$PWD/build/pidgin/plugins" \
    ./build/pidgin/pidgin
```

## Plugins

Plugin support is enabled by default. To disable it entirely, configure with
`-Dplugins=false` and rebuild.

`ninja -C build install` puts plugins in `$PREFIX/lib/purple` (`$PREFIX`
defaults to `/usr/local`, set it with `meson setup build --prefix=...`).
libpurple looks there by default. Plugins may also be installed per-user in
`~/.purple/plugins`. Pidgin and Finch additionally look in `$PREFIX/lib/pidgin`
and `$PREFIX/lib/finch` for UI-specific plugins.

To build a plugin from a `.c` file, add it to the appropriate `plugins/`
directory's `meson.build` and rebuild with `ninja -C build`.

## Reporting bugs

For upstream Pidgin issues, see <https://issues.imfreedom.org/>. For problems
specific to this GTK 3 fork, use this repository's issue tracker.

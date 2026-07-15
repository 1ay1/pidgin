# Developing this Pidgin / libpurple tree

This document describes how to **build, install, run, and debug** the
`better-libpurple` tree so that the binary you launch actually contains the
code you just compiled. It also documents the single most common footgun on a
machine that already has a distro Pidgin installed: **plugin shadowing**.

For the *design* of the modernization work (IRCv3, the protocol subsystem, DNS,
GTK3 fixes, …) see [`../libpurple/ARCHITECTURE.md`](../libpurple/ARCHITECTURE.md).
This file is purely about the mechanics of the edit → build → run loop.

---

## TL;DR

```sh
# one-time configure (installs into a private dev prefix, see below)
meson setup build --prefix="$HOME/.local/pidgin-dev"

# after any code change
ninja -C build                 # build in place
ninja -C build install         # copy into the dev prefix

# run the build you just made (NOT the system pidgin)
LD_LIBRARY_PATH="$HOME/.local/pidgin-dev/lib" \
    "$HOME/.local/pidgin-dev/bin/pidgin" -d
```

If a feature you *know* you added (a new `/command`, a new account option, an
IRCv3 capability) is missing at runtime, you are almost certainly running the
**system** Pidgin and loading the **system** plugin. Jump to
[Plugin shadowing](#the-big-footgun-plugin-shadowing).

---

## Prerequisites / build system

The tree uses **Meson + Ninja** (there is no autotools build here). The
configured build directory lives at `build/`.

```sh
meson setup build --prefix="$HOME/.local/pidgin-dev"   # first time
meson setup --reconfigure build                        # after adding SOURCES
ninja -C build                                         # incremental build
```

Notes:

- **Adding a *new* source file** to a `meson.build` requires
  `meson setup --reconfigure build` before `ninja` will pick it up. Editing an
  existing file does not.
- Ninja is change-driven. If it prints only
  `[1/N] Generating package_revision.h …` and nothing else, nothing it tracks
  changed. To *force* a plugin rebuild after editing its sources:

  ```sh
  touch libpurple/protocols/irc/*.c && ninja -C build
  ```

- The chosen `--prefix` is **`$HOME/.local/pidgin-dev`** on this machine — a
  private, unprivileged location. That is deliberate: it keeps our work
  completely separate from the distro's `/usr` install and needs no `sudo`.
  The prefix is baked into the binary at compile time (`LIBDIR`, `DATADIR`,
  …), so an *installed* dev binary looks for its plugins under
  `$HOME/.local/pidgin-dev/lib/purple-2/`.

---

## Two ways to run

### 1. Run installed (recommended)

`ninja -C build install` copies everything (the `pidgin` binary,
`libpurple.so*`, and every prpl under `lib/purple-2/`) into the dev prefix.
Then run **that** binary and point the dynamic linker at **that** libpurple:

```sh
LD_LIBRARY_PATH="$HOME/.local/pidgin-dev/lib" \
    "$HOME/.local/pidgin-dev/bin/pidgin" -d
```

`-d` enables debug output on stderr.

Because the binary's compiled-in `LIBDIR` is `$HOME/.local/pidgin-dev/lib`, it
finds the dev `libirc.so` (and every other prpl) automatically — **as long as
you launch this binary and not `/usr/bin/pidgin`.**

### 2. Run in-place (no install)

You can run the in-tree binary without installing, but then you must tell
libpurple where the freshly built plugins are, since the in-tree binary's
`LIBDIR` still points at the install prefix (which may hold an older copy).
Prepend the build-tree plugin dirs with `PURPLE_PLUGIN_PATH`:

```sh
LD_LIBRARY_PATH="$PWD/build/libpurple" \
PURPLE_PLUGIN_PATH="$PWD/build/libpurple/plugins:$PWD/build/pidgin/plugins:$PWD/build/libpurple/protocols/irc" \
    ./build/pidgin/pidgin -d
```

`PURPLE_PLUGIN_PATH` entries are searched **before** the compiled-in `LIBDIR`
(see `purple_plugins_add_search_paths_from_env()` in `libpurple/plugin.c`), so
they win over any installed plugin.

---

## The big footgun: plugin shadowing

**Symptom.** A `/command`, account option, or protocol behaviour you *just
added and built* is absent at runtime — e.g. typing `/cap` or `/chathistory` in
a conversation prints **"Unknown command."**, or a NickServ/chat window shows
the command text being sent as a literal message instead of executed.

**Cause.** libpurple loads the protocol plugin (`libirc.so`, `liboscar.so`, …)
from the *running binary's* plugin search path, which is:

1. anything in `PURPLE_PLUGIN_PATH` (if set), then
2. the compiled-in `LIBDIR` of **that binary**.

If you launch the **system** `/usr/bin/pidgin`, its `LIBDIR` is `/usr/lib`, so
it loads **`/usr/lib/purple-2/libirc.so`** — a distro/older build that has none
of your changes. Your freshly built `build/…/libirc.so` is never consulted.

There are therefore up to **three** copies of each plugin on disk, and only one
gets loaded:

| Copy                                                | Loaded by                                          |
|-----------------------------------------------------|----------------------------------------------------|
| `build/libpurple/protocols/irc/libirc.so`           | in-tree binary **only if** `PURPLE_PLUGIN_PATH` points here |
| `$HOME/.local/pidgin-dev/lib/purple-2/libirc.so`    | the **dev** binary (`~/.local/pidgin-dev/bin/pidgin`) after `ninja install` |
| `/usr/lib/purple-2/libirc.so`                        | the **system** binary (`/usr/bin/pidgin`, app menu) |

**Diagnosis.** Confirm which plugin actually loaded — the debug log prints the
path it probes:

```sh
… pidgin -d 2>&1 | grep -i 'probing.*libirc'
# (02:11:27) plugins: probing /home/ayush/.local/pidgin-dev/lib/purple-2/libirc.so
```

You can also check whether a given `.so` even contains your feature without
running anything:

```sh
strings /usr/lib/purple-2/libirc.so          | grep -ci chathistory   # -> 0  (stale)
strings ~/.local/pidgin-dev/lib/purple-2/libirc.so | grep -ci chathistory   # -> 7  (ours)
```

**Fixes (any one):**

- Launch the **dev** binary, not the system one (see
  [Run installed](#1-run-installed-recommended)).
- Or run in-place with `PURPLE_PLUGIN_PATH` (see
  [Run in-place](#2-run-in-place-no-install)).
- Or, if you really want the app-menu / bare `pidgin` command to pick up your
  work, install a launcher (below) so you never accidentally start the stale
  system build.

---

## A convenience launcher (optional)

To stop typing the `LD_LIBRARY_PATH …` incantation, drop a wrapper on your
`PATH`:

```sh
# ~/.local/bin/pidgin-dev
#!/bin/sh
exec env LD_LIBRARY_PATH="$HOME/.local/pidgin-dev/lib" \
    "$HOME/.local/pidgin-dev/bin/pidgin" "$@"
```

```sh
chmod +x ~/.local/bin/pidgin-dev
pidgin-dev -d          # runs the build with all our changes
```

An analogous `.desktop` file in `~/.local/share/applications/` (with
`Exec=/home/USER/.local/bin/pidgin-dev`) makes the dev build appear in the
application menu alongside — or instead of — the distro one.

---

## Standard smoke test

After every build, sanity-check that the daemon starts and loads plugins
without crashing. Run it as a **separate** shell step from the build, redirect
to a file, then grep the file (piping the live output through `grep` can buffer
or hang the GUI process):

```sh
timeout 8 env LD_LIBRARY_PATH="$HOME/.local/pidgin-dev/lib" \
    "$HOME/.local/pidgin-dev/bin/pidgin" -d >/tmp/smoke 2>&1
grep -icE "SIGABRT|abort|CRITICAL" /tmp/smoke     # expect 0
```

- **`timeout` exiting 124 is expected** — the GUI runs for the full duration
  and is then killed; that is not a failure.
- Expect **0** matches for `SIGABRT|abort|CRITICAL`.
- Benign teardown warnings during shutdown (`g_object_unref: G_IS_OBJECT
  failed`, a NULL class pointer after `blist: Destroying`) are pre-existing
  teardown-order noise, not regressions.

For anything that needs a live network exchange (e.g. observing an IRC CAP
negotiation against Libera), bump the timeout to 15–30s and make sure the
account's status is *available* so it actually connects.

---

## Debugging a hang

`ptrace_scope=1` on this system blocks attaching gdb to a running process, so
launch Pidgin **as a child of gdb** instead:

```sh
gdb -q --args "$HOME/.local/pidgin-dev/bin/pidgin" -d
# (gdb) run
# … reproduce the hang, Ctrl-C …
# (gdb) thread apply all bt
```

If the main thread is sitting in `gtk_main → g_main_context_iterate → ppoll`
while idle, the UI event loop is healthy — the hang is elsewhere (often a
blocking call on the main thread, e.g. a synchronous image decode). A libpurple
core deadlock would show the main thread blocked on a lock, not in `ppoll`.

---

## Testing pure logic (standalone)

Some pure decision logic (e.g. IRCv3 message-tag parsing/unescaping) has
standalone tests that are **glib-only** and are *not* wired into the Meson
build. They mirror the prpl's logic byte-for-byte (they can't link the plugin
`.o`), so keep them in sync when you change the real code. Build and run one
directly:

```sh
cc -o /tmp/test_irc_tags libpurple/protocols/irc/tests/test_irc_tags.c \
    $(pkg-config --cflags --libs glib-2.0)
/tmp/test_irc_tags
```

---

## Conventions recap

These mirror [`../libpurple/ARCHITECTURE.md`](../libpurple/ARCHITECTURE.md)'s
conventions section; repeated here for the build/run context:

- **Additive-first, no ABI breaks on the 2.x line.** Prefer new API that wraps
  the legacy path over changing a public struct or vtable.
- Wrap unavoidable deprecated GTK/GLib calls in
  `G_GNUC_BEGIN_IGNORE_DEPRECATIONS` / `G_GNUC_END_IGNORE_DEPRECATIONS`.
- Use `purple_strequal(a, b)` (NULL-safe) rather than hand-rolled `strcmp`.
- Use `purple_debug_info/_warning/_error(domain, …)` for logging.
- New public headers go in all three `meson.build` header lists and in
  `purple.h.in`.
- Every change should be self-contained, build clean, and pass the smoke test
  before it is committed.

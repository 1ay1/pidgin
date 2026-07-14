# libpurple architecture & modernization

This document describes the internal architecture of libpurple 2.x and the
modernization work carried on the `better-libpurple` branch. It is a working
map for contributors, not end-user documentation.

## Overview

libpurple is the ~93k-line C core beneath Pidgin and Finch. It provides the
account model, the buddy list (blist), conversations, connections, the plugin
and protocol (prpl) systems, a signal bus, and I/O / DNS / proxy plumbing. UIs
plug in through `*UiOps` vtables; protocols plug in as plugins exposing a
`PurplePluginProtocolInfo` vtable.

## Subsystem map

| Area          | Files                                            | Role |
|---------------|--------------------------------------------------|------|
| Core lifecycle| `core.c`                                         | Ordered init/teardown of every subsystem |
| Accounts      | `account.c`, `accountopt.c`                      | Credentials, per-account settings, presence |
| Buddy list    | `blist.c`                                        | Intrusive n-ary tree of buddies/contacts/groups |
| Conversations | `conversation.c`                                 | IM and chat sessions (tagged union) |
| Connections   | `connection.c`, `server.c`                       | Live sessions + `serv_*` dispatch helpers |
| Protocols     | `prpl.c`, `protocols.c`, `plugin.c`              | Protocol plugin vtable, registry, dispatch |
| Signals       | `signals.c`, `value.c`                           | Hand-rolled string-keyed signal bus |
| Async I/O     | `eventloop.c`, `proxy.c`, `dnsquery.c`, `dnssrv.c` | fd-watch callbacks over a pluggable event loop |
| Media         | `media*.c`, `media/`                             | GStreamer voice/video (GObject-based) |

## The protocol subsystem (modernized)

Historically a protocol was located with an O(n) linear scan
(`purple_find_prpl`), and its optional features were probed all over the tree
with struct-offset arithmetic (`PURPLE_PROTOCOL_PLUGIN_HAS_FUNC`). The
`protocols.{c,h}` subsystem modernizes this **without touching the legacy
`PurplePluginProtocolInfo` vtable** — it is purely additive.

### Registry (O(1))

```c
PurplePlugin *purple_protocols_find(const char *id);   /* hash-indexed */
guint         purple_protocols_get_count(void);
```

The index is a `GHashTable` kept in sync by the plugin loader (`plugin.c`
load / unload / register sites). `purple_find_prpl()` now delegates here.
Initialised in `core.c` immediately after the plugin subsystem so both static
and probed protocols are indexed as they register.

### Capability introspection

A single, exhaustive `PurpleProtocolCapability` enum names each optional
feature (IM, typing, chat, server-roster, privacy, roomlist, file-transfer,
media, attention, whiteboard, user-info, registration, password-change,
buddy-menu). The mapping from vtable function pointers to capabilities lives
in exactly one function (`compute_capabilities`), and the result is cached per
plugin.

```c
PurpleProtocolCapability purple_protocol_get_capabilities(PurplePlugin *);
gboolean  purple_protocol_has_capability(PurplePlugin *, PurpleProtocolCapability);
GList    *purple_protocols_find_with_capability(PurpleProtocolCapability);
const char *purple_protocol_capability_to_string(PurpleProtocolCapability);
```

This enum is the intended **seam** along which the ~90-slot god vtable should
eventually be split into segregated interfaces.

### Capability-checked dispatch facade

Collapses the ubiquitous prpl-resolution boilerplate:

```c
PurplePluginProtocolInfo *purple_connection_get_protocol_info(const PurpleConnection *);
gboolean purple_connection_can(const PurpleConnection *, PurpleProtocolCapability);
gboolean purple_account_protocol_can(const PurpleAccount *, PurpleProtocolCapability);
```

`server.c`'s `serv_*` helpers use the facade: one NULL-safe dispatch shape
instead of the repeated `prpl = ...; prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl); if (prpl_info->x) ...`.

## DNS resolution (modernized)

The Unix resolver historically `fork()`ed a dedicated child per lookup and
marshalled request/result over a `pipe()` pair, with a child pool, SIGCHLD
reaping and EINTR loops.

Since `getaddrinfo()` is universally available and Windows has long resolved
on a GLib worker thread, `dnsquery.c` now selects **`PURPLE_DNSQUERY_USE_THREAD`**
by default: the blocking lookup runs on a short-lived `g_thread_try_new()`
worker and the result is delivered back on the main thread. One portable
implementation replaces the Windows special case; `fork()` remains only as a
fallback for platforms lacking `getaddrinfo` (forceable with
`-DPURPLE_DNSQUERY_FORCE_FORK`).

## Known architectural debt (roadmap)

The following are real design smells, but each is an **ABI break** that
upstream explicitly deferred to a 3.0 line. They are intentionally *not*
attempted piecemeal on the 2.x branch, because a half-applied version is worse
architecture than the status quo:

1. **God vtable → segregated interfaces.** Split `PurplePluginProtocolInfo`
   (~90 pointers + a manual `struct_size` ABI-versioning kludge) into
   capability-aligned interfaces. The `PurpleProtocolCapability` enum already
   defines the split lines.
2. **Signal bus → GObject.** `signals.c` is a hand-rolled string-keyed bus
   with ~50 hand-written marshallers, duplicating `GSignal` + `glib-genmarshal`
   (already in the build). Migrate incrementally once core objects are
   GObjects.
3. **Opaque, refcounted core objects.** `PurpleAccount`, `PurpleConnection`,
   `PurpleConversation`, and the blist nodes are wide-open public structs any
   plugin can corrupt, with new/destroy instead of ref/unref. Making them
   opaque GObjects is the foundational change the other two build on.
4. **Permit/deny indexing.** Account permit/deny are O(n) `GSList` string
   scans; upstream's own TODO wants hash tables. Blocked on the lists no
   longer being a mutable public struct field (i.e. on item 3).

## Conventions

- Build: `meson setup build && ninja -C build`. Reconfigure after adding
  sources: `meson setup --reconfigure build`.
- New public headers go in all three `meson.build` header lists and in
  `purple.h.in`.
- Wrap unavoidable deprecated calls in
  `G_GNUC_BEGIN/END_IGNORE_DEPRECATIONS`.
- Additive-first: prefer new API that wraps the legacy path over ABI breaks on
  the 2.x line.

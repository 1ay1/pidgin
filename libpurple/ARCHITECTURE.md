# libpurple architecture & modernization

This document describes the internal architecture of libpurple 2.x and the
modernization work carried on the `better-libpurple` branch. It is a working
map for contributors, not end-user documentation.

> For the **build / install / run / debug workflow** (and the common
> "plugin shadowing" footgun where the system Pidgin loads a stale
> `libirc.so` instead of your build), see
> [`../docs/DEVELOPING.md`](../docs/DEVELOPING.md).

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
| Reconnection  | `reconnect.c`                                    | Core auto-reconnect with exponential backoff |
| Message queue | `msgqueue.c`                                      | Store-and-forward outgoing IMs across reconnects |
| Rate limiting | `ratelimit.c`                                     | Per-connection outbound token bucket (anti-flood) |
| Health        | `connhealth.c`                                    | Connection liveness/latency telemetry + signal |
| Signals       | `signals.c`, `value.c`                           | Hand-rolled string-keyed signal bus |
| Async I/O     | `eventloop.c`, `proxy.c`, `dnsquery.c`, `dnssrv.c` | fd-watch callbacks over a pluggable event loop |
| Media         | `media*.c`, `media/`                             | GStreamer voice/video (GObject-based) |

## Automatic reconnection (new)

Reconnection used to live in a separate "autorecon" UI plugin, so every
embedder shipped its own recovery logic with no shared backoff policy.
`reconnect.{c,h}` builds it into the core, entirely signal-driven:

- On a **non-fatal** `connection-error` it schedules a reconnect with capped
  exponential backoff: `delay = min(initial * 2^(n-1), max)` plus ~25% jitter
  (to avoid a thundering herd when many accounts drop together). **Fatal**
  errors (bad password, banned, name-in-use) are never retried, gated on
  `purple_connection_error_is_fatal()`.
- Reconnection is **suspended while the network is down** and resumes on the
  next `network-configuration-changed`; a successful `signed-on` resets the
  backoff; `account-removed`/`account-disabled` cancel and clear state.
- Policy/query API: `purple_reconnect_set_enabled` / `set_backoff` /
  `get_delay` / `get_attempts` / `cancel_account`. Defaults: 8s..8m,
  unlimited attempts, enabled.

The pure backoff calculation (`_purple_reconnect_backoff_base`) is factored
out jitter-free and covered by `tests/test_reconnect.c`.

## Outgoing message queue (new)

Before, an IM sent the instant a connection blipped was simply lost: `serv_send_im`
returned an error to the UI and the message was gone; the user had to notice
and retype it once the account came back. `msgqueue.{c,h}` adds core
store-and-forward, riding on top of the reconnect subsystem:

- When `serv_send_im` cannot deliver (protocol returned `< 0` and the
  connection is not `PURPLE_CONNECTED`), the message is offered to
  `purple_msgqueue_enqueue_im`. It is parked **only if a reconnect is actually
  pending** (`purple_reconnect_is_pending`) — a permanently-offline
  (disabled/fatal) account still surfaces the failure immediately, exactly as
  before. Auto-responses and system/no-log traffic are never queued.
- On the next `signed-on` the account's parked messages are flushed **in FIFO
  order** via `serv_send_im`. `account-removed`/`account-disabled` discard the
  queue rather than flush it out of the blue later.
- The queue is **bounded**: capped per account (oldest dropped once full) and
  messages expire after a max age, so a long outage cannot grow memory without
  bound. Policy API: `purple_msgqueue_set_enabled` / `set_limits` /
  `get_count` / `clear_account`. Defaults: 64 messages/account, 1h expiry.

The cap-eviction and stale-expiry invariants are covered by
`tests/test_msgqueue.c`.

## Outbound rate limiting (new)

Many servers enforce anti-flood policies — send too fast and they drop
messages, throttle you, or (IRC-style) disconnect with "Excess Flood".
Before, each protocol plugin either ignored this and got its users kicked or
hand-rolled its own pacing. `ratelimit.{c,h}` provides a shared **per-connection
token bucket** in the core:

- Each connection accrues send credits at a steady `refill_per_sec` up to a
  `burst` ceiling; a send spends one credit. `purple_ratelimit_try_consume`
  returns whether a credit was available; `purple_ratelimit_next_delay`
  reports the fractional seconds until the next credit, so a caller (server
  layer, protocol plugin, or the msgqueue flusher) can pace itself instead of
  tripping the server's detector. Buckets start **full**, so a normal
  conversation is never penalized.
- The limiter is **advisory and opt-in per caller** — no protocol change is
  required, and it is a no-op when disabled. A protocol that knows its
  server's exact threshold can set an explicit policy with
  `purple_ratelimit_set_connection`; otherwise a conservative default applies
  (burst 5, 0.75/s). Bucket state is keyed off the live `PurpleConnection` in
  a side table (no ABI change to the public struct) and reclaimed on
  `signed-off`.
- Policy API: `purple_ratelimit_set_enabled` / `set_default` /
  `set_connection` / `try_consume` / `next_delay` / `reset_connection`.

The token-accrual, burst, and next-delay math is covered by
`tests/test_ratelimit.c` against a synthetic clock.

## Connection health telemetry (new)

Before, libpurple exposed only two coarse connection states (connected /
disconnected) plus a private keepalive timer buried in `connection.c`. There
was no way for a UI or plugin to observe latency or notice a silently-wedged
link before the socket actually failed. `connhealth.{c,h}` adds lightweight,
protocol-agnostic observability:

- A 5s sampler walks every live connection and reads the inbound-liveness
  marker each prpl already maintains (`gc->last_received`). From it it derives
  an **idle time**, a coarse **health grade** (good / idle / stalled by
  tunable thresholds — defaults 45s / 90s, chosen so a healthy 30s-keepalive
  link is never worse than IDLE), and a smoothed **latency** estimate (EWMA of
  each silence→next-activity gap — a keepalive-round-trip proxy that needs no
  protocol cooperation).
- It registers a `connection-health-changed` signal
  `(PurpleConnection*, int old, int new)` emitted on every grade transition,
  so a UI can show a signal-strength indicator or a "reconnecting soon" hint
  without polling. Query API: `purple_connection_get_health` /
  `health_to_string` / `get_idle_time` / `get_latency`; policy:
  `purple_connhealth_set_enabled` / `set_thresholds`.
- Per-connection state lives in a side table keyed off the live
  `PurpleConnection` (no ABI change to the public struct) and is reclaimed on
  `signed-off`. Everything is derived from existing state — no protocol, ABI,
  or wire change.

The idle-to-grade thresholds and latency EWMA are covered by
`tests/test_connhealth.c`.

## Existing object modernization: PurpleAccount settings

The most exercised object machinery in the tree is the per-account
protocol/UI settings store (`PurpleAccountSetting` in `account.c`), read and
written thousands of times per login. It had three long-standing rough
edges, all now fixed **without any ABI or storage-format change**:

- **Redundant-write elision.** `set_int` / `set_string` / `set_bool` (and
  their `set_ui_*` twins) previously freed and reallocated the setting,
  notified the UI, and rescheduled an `accounts.xml` flush on *every* call —
  including the very common case of a prpl re-applying a value it already
  holds on reconnect. A pure `setting_unchanged()` helper now short-circuits
  those no-op writes (covered by `tests/test_account_setting.c`).
- **Change signal.** Setting a value now emits `account-setting-changed`
  `(PurpleAccount*, const char *name)`, so a UI or plugin can react to a
  changed account option instead of polling. The signal fires only on an
  actual change, precisely because of the elision above.
- **Type-mismatch tolerance.** The getters used `g_return_val_if_fail` on the
  stored type, which spat a CRITICAL into the log whenever a setting was read
  as a different type than it was stored (e.g. after a prpl changed an
  option's type between versions). They now log a single `debug_warning` and
  quietly return the caller's default.

## Existing object modernization: PurpleBuddyIcon

`purple_buddy_icon_set_data()` (buddyicon.c) is called every time a protocol
delivers a buddy's avatar — which prpls do afresh on **every reconnect**, for
every buddy. It previously always re-ran the full path: `purple_buddy_icon_data_new()`
SHA-1-hashes the entire image payload to derive its cache filename, and
`purple_buddy_icon_update()` makes the buddy list and every open conversation
re-render the icon and re-take references.

The protocol's own **checksum** is precisely its "has this changed?" token.
The setter now short-circuits when the incoming checksum equals the one
already held **and** the image is already in memory: it frees the (redundant)
incoming data and returns, skipping both the re-hash and the re-render. The
guard is deliberately conservative — it never skips a first load
(`img == NULL`, the `purple_buddy_icons_find` disk path), a checksum change, a
checksumless update (can't prove equality), or an icon *removal*
(`data == NULL`/`len == 0`). Those boundary conditions are covered by
`tests/test_buddyicon.c`.

## Frontend hardening: image decode can no longer freeze the UI

`pidgin_pixbuf_from_data()` / `pidgin_pixbuf_anim_from_data()`
(`pidgin/gtkutils.c`) decode **protocol-supplied, untrusted** bytes — buddy
icons and inline images — by feeding them straight into a `GdkPixbufLoader` on
the **GTK main thread**. On a modern gdk-pixbuf backed by **glycin**, that
decode forks a sandboxed subprocess and blocks on `wait4()`; a huge, animated,
or malformed avatar (or a slow sandbox spawn) therefore stalls the whole event
loop — observed as Pidgin *"hanging suddenly"* right after a buddy's icon
arrives. A backtrace of the frozen process shows the main thread parked in the
loader while `gly-hdl-loader` / `async-io` / `typefind:sink` worker threads sit
in `wait4()`.

`pidgin_pixbuf_from_data_helper()` now guards the decode without changing any
public signature or the on-disk cache format:

1. **Encoded-size gate.** `NULL`/empty payloads and anything larger than
   `PIDGIN_PIXBUF_MAX_ENCODED_BYTES` (16 MiB — far above any real avatar) are
   rejected *before* the loader is created, so a hostile multi-hundred-MB blob
   never reaches the decoder.
2. **Dimension clamp against decompression bombs.** A `size-prepared` handler
   caps decoded dimensions at `PIDGIN_PIXBUF_MAX_DIMENSION` (4096 px) via
   `gdk_pixbuf_loader_set_size()`. This is **shrink-only** and
   **aspect-preserving** (and floors each side at 1 px), so a tiny encoded
   payload that declares 60000×60000 can no longer make the loader try to
   allocate gigabytes up front. Real icons are scaled down for display anyway,
   so the clamp is loss-free in practice.

The two pure decision functions (the size gate and the shrink-only clamp) are
covered exactly by `tests/test_pixbuf_guard.c`.

## Frontend hardening: GTK3 relayout loop could freeze the conversation UI

`resize_imhtml_cb()` (`pidgin/gtkconv.c`) keeps the message-entry box sized to
its text. It is wired to the entry's **`size-allocate`** signal *and* ends by
calling `gtk_widget_set_size_request()` on that same entry -- which queues a
fresh `size-allocate`, re-entering the callback.

Under GTK2 this terminated because the old `gtkimhtml` size-request machinery
(`gtk_imhtml_size_allocate`) bumped a `"resize-count"` object-data counter that
the callback's guard checked against `GTK_IMHTML_MAX_CONSEC_RESIZES`. Under
GTK3's height-for-width geometry that counter no longer advances in step with
the handler, so the guard can never fire: any 1-pixel oscillation in the
computed height (focus-width / padding rounding) produces an unbounded
`allocate -> request -> allocate` storm that pegs the CPU and **freezes the
UI**.

The fix is self-contained and needs no counter cooperation:

1. **Re-entrancy flag.** A `"resize-in-progress"` marker is set on the entry
   across the `set_size_request()` call; the handler returns immediately if it
   sees the marker, so the nested allocate can't recurse.
2. **Change-gated request.** The size request is only issued when the target
   height actually differs from the entry's current request
   (`gtk_widget_get_size_request()`), making the steady state a true no-op
   instead of a redundant resize that still queues a relayout.

The legacy `"resize-count"` damper is left in place as a secondary cap; the two
guards above are what make the loop provably terminate.

## Frontend hardening: Wayland could not position parentless tooltip popups

Pidgin's link/buddy tooltips are override-redirect `GTK_WINDOW_POPUP`
toplevels created on demand (`pidgin/pidgintooltip.c`,
`gtk_imhtml_tip()` in `pidgin/gtkimhtml.c`). Under X11 a popup can float
unparented and be positioned by absolute root coordinates, so these windows
never set a transient parent.

Wayland has no global coordinate space: an override-redirect surface must be a
**subsurface/popup of a real parent** to be placed at all. Mapping a
parentless one makes GDK log

```
Window ... is a temporary window without parent, application will not be able
to position it on screen.
gdk_wayland_window_handle_configure_popup: assertion 'impl->transient_for' failed
```

and the tooltip either fails to appear or spams the assertion on every hover.

The fix anchors each tooltip popup to the toplevel `GtkWindow` of the widget it
describes via `gtk_window_set_transient_for()` before it is mapped
(guarded by `GTK_IS_WINDOW`). The compositor then positions the popup relative
to that surface; the placement math (which still computes absolute pointer
coordinates) is unchanged and remains correct on X11.

## Frontend hardening: status-box message entry claimed too much height

The blist status selector (`PidginStatusBox`, `pidgin/gtkstatusbox.c`) is a
custom-allocated widget: a toggle button on top and, when the selected status
carries a message, an editable `gtkimhtml` entry (`status_box->vbox`) below it.
`update_size()` computes how tall that entry should be (one to four text lines)
and pins it with `gtk_widget_set_size_request()`.

Under GTK3, `gtk_widget_set_size_request()` sets only the widget's *minimum*
size; a widget's *natural* size can still be larger. The text view's natural
height is unbounded, so when `pidgin_status_box_get_requisition()` measured the
vbox with `gtk_widget_get_preferred_size()` it read that large natural height.
The status box is packed `expand=FALSE` in the buddy-list vbox, so it received
exactly its requested height -- yielding a message entry several lines tall for
a one-line "I'm not here right now".

The fix clamps the measured vbox height in the requisition to the height
`update_size()` explicitly requested (`gtk_widget_get_size_request()`), when one
is set. The entry now stays exactly as tall as the text it holds; if no
request is pinned yet the old natural-size behaviour is used unchanged.

## The IRC protocol plugin, dragged into the IRCv3 era

> User-facing reference for the features below (capabilities, account
> options, and slash-commands like `/cap` and `/chathistory`) lives in
> [`../docs/IRC.md`](../docs/IRC.md). This section covers the *internals*.

The `irc` prpl (`libpurple/protocols/irc`) was a faithful but frozen RFC1459
client: it understood only the classic `:prefix COMMAND args` line grammar and
its sole nod to modern IRC was a bespoke `CAP REQ sasl` hack bolted onto the
login path. Everything downstream paid for that -- every message was stamped
with the local `time(NULL)`, presence was learned by polling `ISON`/`WHO`, and
any line the server tagged (a `@`-prefixed blob) was dropped as unparseable.

The modernization is additive and centred on a real **capability-negotiation
core** plus a **message-tag layer**, both living in `parse.c`/`msgs.c` behind a
small API in `irc.h` (`irc_cap_have`, `irc_msg_tag`, `irc_msg_tag_time`).

### Message tags (the foundation)

`irc_parse_msg()` now strips a leading `@key=value;...` blob before the rest of
the line is parsed, decoding the IRCv3 escape alphabet (`\:`->`;`, `\s`->space,
`\\`, `\r`, `\n`) into a per-connection `irc->tags` hash that stays live for
exactly one dispatched message. Handlers read it through `irc_msg_tag()`;
`irc_msg_tag_time()` turns an IRCv3 `server-time` tag (ISO 8601) into a
`time_t`, falling back to receive-time when absent. This is what makes bouncer
backlog and `chathistory` replay render with the **original** timestamps
instead of "now".

### Capability negotiation

`irc_cap_ls_begin()` (called from the login path before `USER`/`NICK`) sends
`CAP LS 302` and holds registration open (`in_cap`) while we intersect the
server's offer with `irc_wanted_caps[]` and `CAP REQ` the subset we can use.
The dispatcher `irc_msg_cap_v3()` handles `LS/ACK/NAK/NEW/DEL/LIST`, tracking
outstanding REQ round-trips in `cap_reqs`; `irc_cap_maybe_end()` emits `CAP END`
**exactly once**, only after the LS is parsed, every REQ is answered, and any
SASL exchange has finished. SASL is now just one negotiated capability -- the
old `irc_msg_cap` was split into `irc_cap_start_sasl()` (driven by the ACK) and
the generic core, and the SASL completion paths route through
`irc_cap_maybe_end()` instead of sending their own `CAP END`. `cap-notify`
(`CAP NEW`/`CAP DEL`) lets the server add/remove capabilities mid-session.

### Capabilities consumed

| Capability      | Effect in the prpl                                          |
|-----------------|-------------------------------------------------------------|
| `server-time`   | real timestamps on replayed/bounced messages                |
| `echo-message`  | own messages shown from the server's echo (correctly ordered/stamped); the local optimistic echo is suppressed to avoid duplicates |
| `extended-join` | services account announced on JOIN without a manual WHOIS    |
| `account-notify`| live "logged in as X" / "logged out" in channels             |
| `away-notify`   | buddy away/back reflected instantly, no ISON/WHO polling     |
| `batch` + `draft/chathistory` | on join, up to N recent lines are replayed into the channel with their original timestamps (see below) |
| `multi-prefix`, `userhost-in-names`, `chghost`, `setname`, `invite-notify`, `message-tags` | negotiated and accepted (no longer logged as junk); ready for host/realname-aware features |

The design deliberately keeps every consumer guarded on `irc_cap_have()` and
every tag read guarded on presence, so a bare RFC1459 server behaves exactly as
before. Pure tag logic is covered by
`libpurple/protocols/irc/tests/test_irc_tags.c`.

### Backlog on join (chathistory)

When the server offers both `batch` and `draft/chathistory`, joining a channel
no longer drops you into an empty window. Immediately after `serv_got_joined_chat`
(and the existing `WHO`), `irc_msg_join` sends
`CHATHISTORY LATEST <channel> * <limit>`. The server replays the newest
messages inside a `BATCH` frame; each replayed line carries its own
`server-time` tag, so the existing PRIVMSG path renders it with the *original*
timestamp and ordering — no special-casing of the replay is needed. The `BATCH`
open/close framing lines are accepted silently by `irc_msg_batch` so they are
not logged as junk. The limit is the per-account **"Backlog messages to fetch
on join"** option (`chathistory-limit`, default 50, `0` disables the fetch).

Two slash-commands expose this interactively: **`/cap`** prints the list of
capabilities actually enabled on the current connection (client-side
introspection, no server round-trip), and **`/chathistory`** fetches more
backlog on demand — bare, it re-pulls the latest N lines for the current
channel; with an argument it forwards a raw chathistory subcommand
(`LATEST` / `BEFORE` / `AFTER` / `AROUND` / `BETWEEN`) to the server.

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

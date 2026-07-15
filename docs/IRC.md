# The IRC protocol plugin (`prpl-irc`)

This is the feature reference for Pidgin's / libpurple's **IRC** protocol
plugin as it stands on the `better-libpurple` branch. It covers what the
plugin now speaks, the per-account options, the slash-commands, and how the
modern **IRCv3** features behave in practice.

For the *internal design* (how CAP negotiation is structured, the message-tag
parser, the message/command dispatch tables) see the "IRC protocol plugin,
dragged into the IRCv3 era" section of
[`../libpurple/ARCHITECTURE.md`](../libpurple/ARCHITECTURE.md). For the
build/run workflow see [`DEVELOPING.md`](DEVELOPING.md).

---

## What changed, in one paragraph

The IRC prpl used to speak frozen **RFC1459**: bare `:prefix COMMAND args`
lines, timestamps stamped with the local clock, presence discovered by polling
(`ISON`/`WHO`), SASL bolted on as a one-off hack, and any modern `@tag`-prefixed
line dropped as unparseable. It now negotiates and uses **IRCv3** capabilities:
real server timestamps, live account/away notifications, reliable message echo,
richer JOIN data, message tags, and on-join **chat history** backlog — all
behind capability guards so a plain RFC1459 server still works exactly as
before.

---

## IRCv3 capabilities

On connect the plugin sends `CAP LS 302`, holds registration open, requests the
intersection of what the server offers and what it can use, and sends `CAP END`
only once every request is answered (and any SASL exchange has finished).
`cap-notify` means the server can add/remove capabilities mid-session and the
plugin reacts.

You can see exactly what got enabled on a live connection with
[`/cap`](#cap).

### Capabilities and what they do for you

| Capability | Effect you can see |
|---|---|
| **`server-time`** | Replayed/bounced messages (from a bouncer, or chat-history playback) show their **original** timestamp instead of the moment you happened to receive them. |
| **`echo-message`** | Your own channel messages are displayed from the server's echo, so they appear in the exact order and with the exact timestamp everyone else sees. The local optimistic echo is suppressed so you don't see duplicates. (For 1:1 IMs the local echo is kept and the server echo suppressed, since libpurple already rendered your line.) |
| **`extended-join`** | When someone joins a channel, their services **account** and real name arrive with the JOIN — the plugin announces "*X joined, logged in as Y*" without you running a manual `/whois`. |
| **`account-notify`** | Live "*X is now logged in as Y*" / "*X logged out of their account*" notices in every channel where you can see that user — no polling. |
| **`away-notify`** | A buddy's away/back state is reflected **instantly** in their status, with no periodic `ISON`/`WHO` traffic. |
| **`batch`** + **`draft/chathistory`** | On join, recent channel backlog is replayed into the window so you rejoin mid-conversation instead of to an empty room. See [Chat history](#chat-history-backlog). |
| **`multi-prefix`** | Full `@+%&~` prefix set in `NAMES`/`WHO` (all of a user's channel privileges, not just the highest). |
| **`userhost-in-names`** | Full `nick!user@host` in `NAMES`. |
| **`chghost`** | A user's `user@host` changing in place is accepted quietly instead of showing a fake quit/join. |
| **`setname`** | Real-name changes without a reconnect. |
| **`invite-notify`** | You can see invites others send in a channel. |
| **`message-tags`** | Generic client-only tag transport (the foundation the tagged features above ride on). |
| **`sasl`** | Requested only when you enable SASL in the account options **and** libpurple was built with Cyrus SASL. |

Everything is guarded: against a bare RFC1459 server, none of the above is
requested and the plugin behaves like the classic IRC prpl.

---

## Chat history (backlog)

When the server offers both `batch` and `draft/chathistory`, **joining a
channel no longer drops you into an empty window** — the plugin fetches recent
scrollback automatically.

**How it works.** Right after you join, the plugin sends:

```
CHATHISTORY LATEST <channel> * <limit>
```

The server replays the newest messages inside a `BATCH`. Each replayed line
carries its own `server-time` tag, so every message is rendered with its
**original** timestamp and in the right order — no special-casing on the client.
The `BATCH` framing lines are accepted silently.

**How much.** Controlled by the per-account option **"Backlog messages to fetch
on join"** (`chathistory-limit`), default **50**. Set it to **0** to disable
the on-join fetch entirely.

**On demand.** Use [`/chathistory`](#chathistory) to pull more backlog at any
time — the latest N again, or an explicit slice by timestamp.

---

## SASL authentication

If libpurple was built with Cyrus SASL, enable **"Authenticate with SASL"** in
the account options to authenticate during CAP negotiation (before you're fully
registered), which is the modern, secure way to identify to services. The old
behaviour of messaging NickServ after connecting still works, but SASL is
preferred where the network supports it. Related options: **"SASL login name"**
and **"Allow plaintext SASL auth over unencrypted connection"**.

---

## Account options

Set these in **Accounts → (your IRC account) → Modify → Advanced**.

| Option | Setting key | Default | Meaning |
|---|---|---|---|
| Port | `port` | 6667 | Server port (use with **Use SSL** for 6697). |
| Encodings | `encoding` | UTF-8 | Comma-separated fallback charsets for decoding. |
| Auto-detect incoming UTF-8 | `autodetect_utf8` | on | Prefer UTF-8 when a line validates as such. |
| **Backlog messages to fetch on join** | `chathistory-limit` | 50 | On-join chat-history size; **0 disables**. |
| Ident name | `username` | *(nick)* | The `USER`/ident name. |
| Real name | `realname` | | Your GECOS / real-name field. |
| Use SSL | `ssl` | off | TLS connection. |
| Authenticate with SASL | `sasl` | off | *(only if built with Cyrus SASL)* |
| SASL login name | `saslname` | | Overrides the nick as the SASL authname. |
| Allow plaintext SASL over unencrypted | `auth_plain_in_clear` | off | Permit PLAIN without TLS (discouraged). |
| Seconds between sending messages | `ratelimit-interval` | *(default)* | Outbound anti-flood spacing. |
| Maximum messages to send at once | `ratelimit-burst` | *(default)* | Outbound anti-flood burst size. |

---

## Slash-commands

Type these in any IRC conversation. `<…>` is required, `[…]` is optional.

### IRCv3-era commands (new)

#### `/cap`

Show the IRCv3 capabilities actually negotiated with the server on **this**
connection. Purely local introspection — no round-trip — so it's the quickest
way to confirm whether `server-time`, `echo-message`, SASL, chat history, etc.
are live. Output is a single line listing the enabled capabilities (sorted), or
a note that none are enabled.

#### `/chathistory`

Fetch more channel backlog on demand. Requires the `draft/chathistory`
capability (otherwise you're told the server doesn't support it).

- **`/chathistory`** — re-pull the latest *N* lines (`chathistory-limit`) for
  the current channel.
- **`/chathistory <subcommand …>`** — forward a raw chathistory subcommand
  verbatim. The standard subcommands are `LATEST`, `BEFORE`, `AFTER`, `AROUND`,
  and `BETWEEN`, e.g.:

  ```
  /chathistory LATEST #channel * 100
  /chathistory BEFORE #channel timestamp=2024-01-01T00:00:00.000Z 50
  ```

### Classic commands (unchanged)

Messaging & presence: `/msg <nick> <msg>`, `/query <nick> [msg]`,
`/notice <target> <msg>`, `/me <action>` (a.k.a. `/action`),
`/away [message]`, `/back`, `/ping [nick]`.

Channels: `/join` (`/j`) `<#room>[,room2] [key…]`, `/part [room] [message]`,
`/topic [new topic]`, `/names [channel]`, `/invite <nick> [room]`,
`/list`.

Moderation (needs channel-operator status): `/op`, `/deop`, `/voice`,
`/devoice` `<nick…>`, `/kick <nick> [message]`, `/remove <nick> [message]`,
`/mode <±flags> [target]`, `/umode <±flags>`.

Identity & info: `/nick <newnick>`, `/whois [server] <nick>`,
`/whowas <nick>`, `/version [nick]`, `/time`.

Services shortcuts: `/nickserv`, `/chanserv`, `/memoserv`, `/operserv`,
`/authserv` `<command>`.

Operator / raw: `/wallops <message>`, `/operwall <message>`,
`/quote <raw IRC line>`, `/ctcp <nick> <msg>`, `/quit [message]`.

> Tip: `/help` in a conversation lists every command with its one-line help.

---

## Not seeing a new command or feature?

If `/cap` or `/chathistory` reports **"Unknown command."**, you are almost
certainly running the **system** Pidgin, which loads a stale
`/usr/lib/purple-2/libirc.so` from before this work — not your build. See
[DEVELOPING.md → Plugin shadowing](DEVELOPING.md#the-big-footgun-plugin-shadowing).

---

## Roadmap (not yet implemented)

Natural next steps that build on the current IRCv3 foundation:

- **msgid-based dedup** — use the `msgid` tag to drop bouncer double-delivery.
- **labeled-response + standard-replies** — structured `FAIL`/`WARN`/`NOTE`
  error handling instead of free-text notices.
- **`+draft/react`** — message reactions.
- **`AROUND`/context-aware history** wired to scrollback navigation in the UI.

/**
 * @file protocols.h Modern protocol registry, capability introspection and
 *                   typed dispatch facade for libpurple.
 * @ingroup core
 *
 * purple 2.x historically located protocol plugins with an O(n) linear scan
 * over a GList (purple_find_prpl), probed optional protocol features with
 * error-prone struct-offset arithmetic (PURPLE_PROTOCOL_PLUGIN_HAS_FUNC), and
 * spread protocol dispatch across server.c / prpl.c / conversation.c.
 *
 * This subsystem modernizes that story without breaking the existing
 * PurplePluginProtocolInfo vtable or any shipped protocol:
 *
 *   - An O(1) hashed protocol-id -> plugin index (purple_protocols_find),
 *     kept in sync as plugins load/unload.
 *
 *   - A single, self-documenting capability enum (PurpleProtocolCapability)
 *     plus purple_protocol_has_capability(), so callers ask
 *     "does this protocol support X?" by name instead of poking at struct
 *     offsets. New capabilities are added in exactly one place.
 *
 *   - Iteration / lookup-by-capability helpers so UI and core code can, e.g.,
 *     enumerate every protocol that can initiate media.
 *
 * It is purely additive: legacy code keeps working, and callers migrate to the
 * cleaner API incrementally.
 */
#ifndef _PURPLE_PROTOCOLS_H_
#define _PURPLE_PROTOCOLS_H_

#include "plugin.h"
#include "connection.h"
#include "prpl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A single, exhaustive description of an optional protocol capability.
 *
 * Each value names a coherent feature a protocol may or may not implement.
 * This replaces scattered PURPLE_PROTOCOL_PLUGIN_HAS_FUNC(prpl, member) checks
 * with purple_protocol_has_capability(plugin, PURPLE_PROTOCOL_CAP_...).
 *
 * These are interface groupings, deliberately coarser than the ~90-slot
 * PurplePluginProtocolInfo vtable: they are the seam along which that god
 * vtable should eventually be split into segregated interfaces.
 */
typedef enum
{
	PURPLE_PROTOCOL_CAP_NONE            = 0,

	/** Can send instant messages (send_im).                              */
	PURPLE_PROTOCOL_CAP_IM              = 1 <<  0,
	/** Can send typing notifications (send_typing).                      */
	PURPLE_PROTOCOL_CAP_TYPING          = 1 <<  1,
	/** Supports multi-user chats (join_chat / chat_send).                */
	PURPLE_PROTOCOL_CAP_CHAT            = 1 <<  2,
	/** Maintains a server-side buddy list / roster (add_buddy).          */
	PURPLE_PROTOCOL_CAP_SERVER_ROSTER   = 1 <<  3,
	/** Supports server-side aliasing (alias_buddy).                      */
	PURPLE_PROTOCOL_CAP_SERVER_ALIAS    = 1 <<  4,
	/** Supports privacy / permit-deny lists (add_permit).                */
	PURPLE_PROTOCOL_CAP_PRIVACY         = 1 <<  5,
	/** Can browse rooms (roomlist_get_list).                             */
	PURPLE_PROTOCOL_CAP_ROOMLIST        = 1 <<  6,
	/** Can send files (can_receive_file / send_file).                    */
	PURPLE_PROTOCOL_CAP_FILE_TRANSFER   = 1 <<  7,
	/** Supports voice / video media (initiate_media).                    */
	PURPLE_PROTOCOL_CAP_MEDIA           = 1 <<  8,
	/** Supports "buzz" / "nudge" attention (send_attention).             */
	PURPLE_PROTOCOL_CAP_ATTENTION       = 1 <<  9,
	/** Supports a collaborative whiteboard (whiteboard_prpl_ops).        */
	PURPLE_PROTOCOL_CAP_WHITEBOARD      = 1 << 10,
	/** Can fetch extended user info / profiles (get_info).               */
	PURPLE_PROTOCOL_CAP_USER_INFO       = 1 << 11,
	/** Supports account registration (register_user).                    */
	PURPLE_PROTOCOL_CAP_REGISTRATION    = 1 << 12,
	/** Supports changing the account password (change_passwd).           */
	PURPLE_PROTOCOL_CAP_PASSWORD_CHANGE = 1 << 13,
	/** Offers per-buddy right-click actions (blist_node_menu).           */
	PURPLE_PROTOCOL_CAP_BUDDY_MENU      = 1 << 14

} PurpleProtocolCapability;

/**************************************************************************/
/** @name Protocol Registry API                                           */
/**************************************************************************/
/*@{*/

/**
 * Locate a loaded protocol plugin by its protocol id in O(1).
 *
 * This is the modern, hash-indexed replacement for purple_find_prpl().
 * It honours the same libpurple3-forward id aliases (prpl-xmpp / prpl-gtalk
 * -> prpl-jabber).
 *
 * @param id The protocol id, e.g. "prpl-jabber".
 *
 * @return The matching PurplePlugin, or @c NULL if none is registered.
 */
PurplePlugin *purple_protocols_find(const char *id);

/**
 * Return the number of currently registered protocol plugins.
 */
guint purple_protocols_get_count(void);

/**
 * Compute the full capability bitmask a protocol plugin implements.
 *
 * The result is cached per plugin, so repeated queries are cheap.
 *
 * @param plugin A protocol PurplePlugin.
 *
 * @return A bitwise-or of PurpleProtocolCapability values.
 */
PurpleProtocolCapability purple_protocol_get_capabilities(PurplePlugin *plugin);

/**
 * Ask whether a protocol plugin implements a given capability.
 *
 * @param plugin A protocol PurplePlugin.
 * @param cap    A single PurpleProtocolCapability (or a mask; all bits must
 *               be present).
 *
 * @return @c TRUE if @a plugin provides every requested capability.
 */
gboolean purple_protocol_has_capability(PurplePlugin *plugin,
                                        PurpleProtocolCapability cap);

/**
 * Return a NUL-terminated, human-readable name for a single capability bit
 * (e.g. PURPLE_PROTOCOL_CAP_MEDIA -> "media"). Useful for debug output and
 * UI. Returns "unknown" for an unrecognised or composite value.
 */
const char *purple_protocol_capability_to_string(PurpleProtocolCapability cap);

/**
 * Build a newly-allocated GList of every registered protocol plugin that
 * provides all of the requested capabilities. The caller frees the list with
 * g_list_free() (the plugins themselves are borrowed, not owned).
 *
 * @param cap The capability mask to filter by (PURPLE_PROTOCOL_CAP_NONE
 *            matches everything).
 */
GList *purple_protocols_find_with_capability(PurpleProtocolCapability cap);

/*@}*/

/**************************************************************************/
/** @name Capability-checked dispatch facade                              */
/**************************************************************************/
/*@{*/

/**
 * Resolve the PurplePluginProtocolInfo vtable for a connection in one call.
 *
 * Replaces the ubiquitous three-line dance
 *     prpl = purple_connection_get_prpl(gc);
 *     prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
 * with a single, NULL-safe accessor.
 *
 * @param gc A connection (may be @c NULL).
 *
 * @return The protocol vtable, or @c NULL if @a gc has no protocol.
 */
PurplePluginProtocolInfo *purple_connection_get_protocol_info(const PurpleConnection *gc);

/**
 * Ask whether the protocol backing a live connection provides a capability.
 *
 * @param gc  A connection.
 * @param cap A PurpleProtocolCapability mask (all bits must be present).
 *
 * @return @c TRUE if @a gc is backed by a protocol offering @a cap.
 */
gboolean purple_connection_can(const PurpleConnection *gc,
                               PurpleProtocolCapability cap);

/**
 * Ask whether the protocol behind an account provides a capability, even
 * when the account is offline (no live connection needed).
 *
 * @param account An account.
 * @param cap     A PurpleProtocolCapability mask.
 *
 * @return @c TRUE if @a account's protocol offers @a cap.
 */
gboolean purple_account_protocol_can(const PurpleAccount *account,
                                     PurpleProtocolCapability cap);

/*@}*/

/**************************************************************************/
/** @name Registry maintenance (internal)                                 */
/**************************************************************************/
/*@{*/

/**
 * Register a protocol plugin with the index. Called by the plugin loader the
 * moment a protocol becomes available; not intended for protocol authors.
 */
void purple_protocols_add(PurplePlugin *plugin);

/**
 * Drop a protocol plugin from the index. Called by the plugin loader on
 * unload.
 */
void purple_protocols_remove(PurplePlugin *plugin);

/**
 * Return the protocols subsystem handle, for signal registration.
 */
void *purple_protocols_get_handle(void);

/**
 * Initialise the protocols subsystem.
 */
void purple_protocols_init(void);

/**
 * Tear down the protocols subsystem.
 */
void purple_protocols_uninit(void);

/*@}*/

#ifdef __cplusplus
}
#endif

#endif /* _PURPLE_PROTOCOLS_H_ */

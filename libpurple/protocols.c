/**
 * @file protocols.c Modern protocol registry, capability introspection and
 *                   typed dispatch facade for libpurple.
 * @ingroup core
 */

/* purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
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
#include "internal.h"

#include "debug.h"
#include "prpl.h"
#include "protocols.h"
#include "signals.h"

/*
 * O(1) protocol-id -> PurplePlugin* index. Values are borrowed pointers owned
 * by the plugin subsystem; we never free them here, we just index them.
 */
static GHashTable *protocol_index = NULL;

/*
 * Per-plugin cache of the computed capability bitmask. Keyed by PurplePlugin*.
 * The stored value is (PurpleProtocolCapability | PURPLE_PROTOCOL_CAP_CACHED)
 * so we can distinguish "not computed yet" from "computed and happens to be 0".
 */
#define PURPLE_PROTOCOL_CAP_CACHED (1u << 31)
static GHashTable *capability_cache = NULL;

static void *protocols_handle = NULL;

/*
 * libpurple3-forward id aliasing, matching the historic purple_find_prpl()
 * behaviour so callers can key on the future id names.
 */
static const char *
canonical_protocol_id(const char *id)
{
	if (purple_strequal(id, "prpl-xmpp") || purple_strequal(id, "prpl-gtalk"))
		return "prpl-jabber";
	return id;
}

/**************************************************************************
 * Registry
 **************************************************************************/

PurplePlugin *
purple_protocols_find(const char *id)
{
	g_return_val_if_fail(id != NULL, NULL);

	if (protocol_index == NULL)
		return NULL;

	return g_hash_table_lookup(protocol_index, canonical_protocol_id(id));
}

guint
purple_protocols_get_count(void)
{
	if (protocol_index == NULL)
		return 0;

	return g_hash_table_size(protocol_index);
}

void
purple_protocols_add(PurplePlugin *plugin)
{
	const char *id;

	g_return_if_fail(plugin != NULL);
	g_return_if_fail(plugin->info != NULL);
	g_return_if_fail(plugin->info->id != NULL);

	if (protocol_index == NULL)
		return;

	id = plugin->info->id;

	/* Refresh any stale capability cache for this plugin. */
	if (capability_cache != NULL)
		g_hash_table_remove(capability_cache, plugin);

	g_hash_table_replace(protocol_index, g_strdup(id), plugin);

	purple_debug_info("protocols", "indexed protocol '%s' (%u total)\n",
			id, g_hash_table_size(protocol_index));

	purple_signal_emit(purple_protocols_get_handle(),
			"protocol-added", plugin);
}

void
purple_protocols_remove(PurplePlugin *plugin)
{
	const char *id;

	g_return_if_fail(plugin != NULL);
	g_return_if_fail(plugin->info != NULL);
	g_return_if_fail(plugin->info->id != NULL);

	id = plugin->info->id;

	if (capability_cache != NULL)
		g_hash_table_remove(capability_cache, plugin);

	if (protocol_index == NULL)
		return;

	/* Only remove the mapping if it still points at this plugin -- guards
	 * against a re-registered id clobbering the wrong entry. */
	if (g_hash_table_lookup(protocol_index, id) == plugin) {
		g_hash_table_remove(protocol_index, id);
		purple_signal_emit(purple_protocols_get_handle(),
				"protocol-removed", plugin);
	}
}

/**************************************************************************
 * Capability introspection
 **************************************************************************/

/*
 * Compute the capability mask from the legacy vtable. This is the single
 * place that maps prpl function pointers to coherent capabilities; every
 * post-struct_size member is probed through PURPLE_PROTOCOL_PLUGIN_HAS_FUNC so
 * the offset arithmetic lives here and nowhere else.
 */
static PurpleProtocolCapability
compute_capabilities(PurplePlugin *plugin)
{
	PurplePluginProtocolInfo *prpl_info;
	PurpleProtocolCapability caps = PURPLE_PROTOCOL_CAP_NONE;

	prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(plugin);
	if (prpl_info == NULL)
		return caps;

#define CAP_IF(cond, bit) do { if (cond) caps |= (bit); } while (0)

	CAP_IF(prpl_info->send_im,        PURPLE_PROTOCOL_CAP_IM);
	CAP_IF(prpl_info->send_typing,    PURPLE_PROTOCOL_CAP_TYPING);
	CAP_IF(prpl_info->join_chat && prpl_info->chat_send,
	                                  PURPLE_PROTOCOL_CAP_CHAT);
	CAP_IF(prpl_info->add_buddy,      PURPLE_PROTOCOL_CAP_SERVER_ROSTER);
	CAP_IF(prpl_info->alias_buddy,    PURPLE_PROTOCOL_CAP_SERVER_ALIAS);
	CAP_IF(prpl_info->add_permit || prpl_info->set_permit_deny,
	                                  PURPLE_PROTOCOL_CAP_PRIVACY);
	CAP_IF(prpl_info->roomlist_get_list, PURPLE_PROTOCOL_CAP_ROOMLIST);
	CAP_IF(prpl_info->can_receive_file && prpl_info->send_file,
	                                  PURPLE_PROTOCOL_CAP_FILE_TRANSFER);
	CAP_IF(prpl_info->get_info,       PURPLE_PROTOCOL_CAP_USER_INFO);
	CAP_IF(prpl_info->register_user,  PURPLE_PROTOCOL_CAP_REGISTRATION);
	CAP_IF(prpl_info->change_passwd,  PURPLE_PROTOCOL_CAP_PASSWORD_CHANGE);
	CAP_IF(prpl_info->blist_node_menu,PURPLE_PROTOCOL_CAP_BUDDY_MENU);
	CAP_IF(prpl_info->whiteboard_prpl_ops, PURPLE_PROTOCOL_CAP_WHITEBOARD);

	/* Members added after the struct_size marker must be probed with
	 * PURPLE_PROTOCOL_PLUGIN_HAS_FUNC so an older, smaller prpl struct isn't
	 * read out of bounds. */
	CAP_IF(PURPLE_PROTOCOL_PLUGIN_HAS_FUNC(prpl_info, send_attention),
	                                  PURPLE_PROTOCOL_CAP_ATTENTION);
	CAP_IF(PURPLE_PROTOCOL_PLUGIN_HAS_FUNC(prpl_info, initiate_media),
	                                  PURPLE_PROTOCOL_CAP_MEDIA);

#undef CAP_IF

	return caps;
}

PurpleProtocolCapability
purple_protocol_get_capabilities(PurplePlugin *plugin)
{
	gpointer cached;
	PurpleProtocolCapability caps;

	g_return_val_if_fail(plugin != NULL, PURPLE_PROTOCOL_CAP_NONE);
	g_return_val_if_fail(plugin->info != NULL, PURPLE_PROTOCOL_CAP_NONE);
	g_return_val_if_fail(plugin->info->type == PURPLE_PLUGIN_PROTOCOL,
			PURPLE_PROTOCOL_CAP_NONE);

	if (capability_cache != NULL &&
	    g_hash_table_lookup_extended(capability_cache, plugin, NULL, &cached))
	{
		return (PurpleProtocolCapability)
			(GPOINTER_TO_UINT(cached) & ~PURPLE_PROTOCOL_CAP_CACHED);
	}

	caps = compute_capabilities(plugin);

	if (capability_cache != NULL) {
		g_hash_table_insert(capability_cache, plugin,
			GUINT_TO_POINTER((guint)caps | PURPLE_PROTOCOL_CAP_CACHED));
	}

	return caps;
}

gboolean
purple_protocol_has_capability(PurplePlugin *plugin,
                               PurpleProtocolCapability cap)
{
	PurpleProtocolCapability have;

	g_return_val_if_fail(plugin != NULL, FALSE);

	have = purple_protocol_get_capabilities(plugin);

	return (have & cap) == cap;
}

const char *
purple_protocol_capability_to_string(PurpleProtocolCapability cap)
{
	switch (cap) {
		case PURPLE_PROTOCOL_CAP_NONE:            return "none";
		case PURPLE_PROTOCOL_CAP_IM:              return "im";
		case PURPLE_PROTOCOL_CAP_TYPING:          return "typing";
		case PURPLE_PROTOCOL_CAP_CHAT:            return "chat";
		case PURPLE_PROTOCOL_CAP_SERVER_ROSTER:   return "server-roster";
		case PURPLE_PROTOCOL_CAP_SERVER_ALIAS:    return "server-alias";
		case PURPLE_PROTOCOL_CAP_PRIVACY:         return "privacy";
		case PURPLE_PROTOCOL_CAP_ROOMLIST:        return "roomlist";
		case PURPLE_PROTOCOL_CAP_FILE_TRANSFER:   return "file-transfer";
		case PURPLE_PROTOCOL_CAP_MEDIA:           return "media";
		case PURPLE_PROTOCOL_CAP_ATTENTION:       return "attention";
		case PURPLE_PROTOCOL_CAP_WHITEBOARD:      return "whiteboard";
		case PURPLE_PROTOCOL_CAP_USER_INFO:       return "user-info";
		case PURPLE_PROTOCOL_CAP_REGISTRATION:    return "registration";
		case PURPLE_PROTOCOL_CAP_PASSWORD_CHANGE: return "password-change";
		case PURPLE_PROTOCOL_CAP_BUDDY_MENU:      return "buddy-menu";
		default:                                  return "unknown";
	}
}

static void
collect_with_capability(gpointer key, gpointer value, gpointer user_data)
{
	PurplePlugin *plugin = value;
	gpointer *ctx = user_data;
	PurpleProtocolCapability want = GPOINTER_TO_UINT(ctx[0]);
	GList **out = ctx[1];

	if (purple_protocol_has_capability(plugin, want))
		*out = g_list_prepend(*out, plugin);
}

GList *
purple_protocols_find_with_capability(PurpleProtocolCapability cap)
{
	GList *result = NULL;
	gpointer ctx[2];

	if (protocol_index == NULL)
		return NULL;

	ctx[0] = GUINT_TO_POINTER((guint)cap);
	ctx[1] = &result;

	g_hash_table_foreach(protocol_index, collect_with_capability, ctx);

	return result;
}

/**************************************************************************
 * Subsystem lifecycle
 **************************************************************************/

void *
purple_protocols_get_handle(void)
{
	return &protocols_handle;
}

void
purple_protocols_init(void)
{
	void *handle = purple_protocols_get_handle();

	protocol_index = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, NULL);
	capability_cache = g_hash_table_new(g_direct_hash, g_direct_equal);

	purple_signal_register(handle, "protocol-added",
			purple_marshal_VOID__POINTER, NULL, 1,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_PLUGIN));

	purple_signal_register(handle, "protocol-removed",
			purple_marshal_VOID__POINTER, NULL, 1,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_PLUGIN));
}

void
purple_protocols_uninit(void)
{
	purple_signals_unregister_by_instance(purple_protocols_get_handle());

	if (protocol_index != NULL) {
		g_hash_table_destroy(protocol_index);
		protocol_index = NULL;
	}
	if (capability_cache != NULL) {
		g_hash_table_destroy(capability_cache);
		capability_cache = NULL;
	}
}

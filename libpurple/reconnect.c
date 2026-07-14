/**
 * @file reconnect.c Automatic reconnection with exponential backoff.
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

#include "account.h"
#include "connection.h"
#include "core.h"
#include "debug.h"
#include "eventloop.h"
#include "network.h"
#include "reconnect.h"
#include "signals.h"

/* Defaults: first retry after 8s, doubling up to 8 minutes, forever. */
#define DEFAULT_INITIAL_DELAY   8
#define DEFAULT_MAX_DELAY       (8 * 60)
#define DEFAULT_MAX_ATTEMPTS    0

typedef struct {
	guint     timeout;      /* purple_timeout handle, 0 if none pending    */
	guint     attempts;     /* consecutive failed reconnect attempts       */
	guint     delay;        /* seconds this pending attempt was scheduled  */
	gboolean  waiting_net;  /* deferred because the network was down       */
} PurpleReconnectData;

static gboolean reconnect_enabled = TRUE;
static guint    cfg_initial_delay = DEFAULT_INITIAL_DELAY;
static guint    cfg_max_delay     = DEFAULT_MAX_DELAY;
static guint    cfg_max_attempts  = DEFAULT_MAX_ATTEMPTS;

/* PurpleAccount* -> PurpleReconnectData* */
static GHashTable *account_data = NULL;

static void *reconnect_handle = NULL;

static PurpleReconnectData *
data_for(PurpleAccount *account, gboolean create)
{
	PurpleReconnectData *rd;

	if (account_data == NULL)
		return NULL;

	rd = g_hash_table_lookup(account_data, account);
	if (rd == NULL && create) {
		rd = g_new0(PurpleReconnectData, 1);
		g_hash_table_insert(account_data, account, rd);
	}
	return rd;
}

static void
clear_pending(PurpleReconnectData *rd)
{
	if (rd == NULL)
		return;
	if (rd->timeout > 0) {
		purple_timeout_remove(rd->timeout);
		rd->timeout = 0;
	}
	rd->delay = 0;
	rd->waiting_net = FALSE;
}

/*
 * Deterministic base delay before the Nth (1-based) attempt:
 *   min(initial * 2^(N-1), max).
 * Kept jitter-free and side-effect-free so it can be unit tested; the live
 * scheduler layers random jitter on top (see compute_delay).
 *
 * Not part of the public API -- exposed with a leading underscore for the
 * test suite only.
 */
guint
_purple_reconnect_backoff_base(guint attempt, guint initial, guint max)
{
	guint64 delay;
	guint shift = attempt > 0 ? attempt - 1 : 0;

	if (initial < 1)
		initial = 1;
	if (max < initial)
		max = initial;

	/* Cap the shift so the doubling can't overflow before the clamp. */
	if (shift > 20)
		shift = 20;
	delay = (guint64)initial << shift;

	if (delay > max)
		delay = max;

	return (guint)delay;
}

/*
 * Compute the delay before the Nth (1-based) attempt: the deterministic base
 * plus up to ~25% random jitter. Jitter de-synchronises many accounts that
 * dropped together (e.g. a laptop waking up), avoiding a reconnect
 * thundering herd.
 */
static guint
compute_delay(guint attempt)
{
	guint delay = _purple_reconnect_backoff_base(attempt,
			cfg_initial_delay, cfg_max_delay);
	guint jitter;

	jitter = (delay / 4) > 0 ? g_random_int_range(0, (gint32)(delay / 4) + 1) : 0;

	return delay + jitter;
}

static gboolean
should_reconnect(PurpleAccount *account)
{
	if (!reconnect_enabled)
		return FALSE;

	/* Only accounts the user still wants online. */
	if (!purple_account_get_enabled(account, purple_core_get_ui()))
		return FALSE;

	/* Don't fight a connection that's already up or coming up. */
	if (purple_account_is_connected(account) ||
	    purple_account_is_connecting(account))
		return FALSE;

	return TRUE;
}

static gboolean
do_reconnect(gpointer data)
{
	PurpleAccount *account = data;
	PurpleReconnectData *rd = data_for(account, FALSE);

	if (rd != NULL)
		rd->timeout = 0;

	if (!should_reconnect(account)) {
		if (rd != NULL)
			clear_pending(rd);
		return FALSE;
	}

	/* If the network is still down, park until connectivity returns instead
	 * of burning an attempt on a lookup that can't succeed. */
	if (!purple_network_is_available()) {
		if (rd != NULL) {
			rd->waiting_net = TRUE;
			rd->delay = 0;
		}
		purple_debug_info("reconnect",
				"network unavailable; deferring reconnect of %s\n",
				purple_account_get_username(account));
		return FALSE;
	}

	purple_debug_info("reconnect", "reconnecting %s (attempt %u)\n",
			purple_account_get_username(account),
			rd ? rd->attempts : 0);

	purple_account_connect(account);
	return FALSE;
}

static void
schedule_reconnect(PurpleAccount *account)
{
	PurpleReconnectData *rd;
	guint delay;

	if (!should_reconnect(account))
		return;

	rd = data_for(account, TRUE);

	clear_pending(rd);

	rd->attempts++;

	if (cfg_max_attempts != 0 && rd->attempts > cfg_max_attempts) {
		purple_debug_info("reconnect",
				"giving up on %s after %u attempts\n",
				purple_account_get_username(account), rd->attempts - 1);
		return;
	}

	delay = compute_delay(rd->attempts);
	rd->delay = delay;

	purple_debug_info("reconnect",
			"scheduling reconnect of %s in %u s (attempt %u)\n",
			purple_account_get_username(account), delay, rd->attempts);

	rd->timeout = purple_timeout_add_seconds(delay, do_reconnect, account);
}

/**************************************************************************
 * Signal handlers
 **************************************************************************/

static void
connection_error_cb(PurpleConnection *gc, PurpleConnectionError reason,
                    const char *description, gpointer unused)
{
	PurpleAccount *account = purple_connection_get_account(gc);

	if (account == NULL)
		return;

	/* Never retry fatal errors: bad password, account banned, name in use,
	 * unsupported protocol version, etc. The user must intervene. */
	if (purple_connection_error_is_fatal(reason)) {
		purple_reconnect_cancel_account(account);
		return;
	}

	schedule_reconnect(account);
}

static void
signed_on_cb(PurpleConnection *gc, gpointer unused)
{
	PurpleAccount *account = purple_connection_get_account(gc);
	PurpleReconnectData *rd = data_for(account, FALSE);

	/* Success: forget the backoff so the next drop starts fresh. */
	if (rd != NULL) {
		clear_pending(rd);
		rd->attempts = 0;
	}
}

static void
account_removed_cb(PurpleAccount *account, gpointer unused)
{
	purple_reconnect_cancel_account(account);
}

static void
account_disabled_cb(PurpleAccount *account, gpointer unused)
{
	purple_reconnect_cancel_account(account);
}

/*
 * When the network comes back, immediately service every account that was
 * parked waiting for connectivity.
 */
static void
network_available_cb(gpointer unused)
{
	GHashTableIter iter;
	gpointer key, value;

	if (account_data == NULL)
		return;

	g_hash_table_iter_init(&iter, account_data);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		PurpleAccount *account = key;
		PurpleReconnectData *rd = value;

		if (rd->waiting_net) {
			rd->waiting_net = FALSE;
			/* Retry promptly but with a hair of jitter. */
			rd->timeout = purple_timeout_add_seconds(
					1 + g_random_int_range(0, 3), do_reconnect, account);
		}
	}
}

/**************************************************************************
 * Public API
 **************************************************************************/

void
purple_reconnect_set_enabled(gboolean enabled)
{
	reconnect_enabled = enabled;

	if (!enabled && account_data != NULL) {
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init(&iter, account_data);
		while (g_hash_table_iter_next(&iter, &key, &value))
			clear_pending((PurpleReconnectData *)value);
	}
}

gboolean
purple_reconnect_get_enabled(void)
{
	return reconnect_enabled;
}

void
purple_reconnect_set_backoff(guint initial_delay, guint max_delay,
                             guint max_attempts)
{
	if (initial_delay < 1)
		initial_delay = 1;
	if (max_delay < initial_delay)
		max_delay = initial_delay;

	cfg_initial_delay = initial_delay;
	cfg_max_delay = max_delay;
	cfg_max_attempts = max_attempts;
}

guint
purple_reconnect_get_delay(PurpleAccount *account)
{
	PurpleReconnectData *rd = data_for(account, FALSE);
	return (rd != NULL && rd->timeout > 0) ? rd->delay : 0;
}

guint
purple_reconnect_get_attempts(PurpleAccount *account)
{
	PurpleReconnectData *rd = data_for(account, FALSE);
	return rd != NULL ? rd->attempts : 0;
}

void
purple_reconnect_cancel_account(PurpleAccount *account)
{
	PurpleReconnectData *rd = data_for(account, FALSE);

	if (rd != NULL) {
		clear_pending(rd);
		rd->attempts = 0;
	}
}

void *
purple_reconnect_get_handle(void)
{
	return &reconnect_handle;
}

void
purple_reconnect_init(void)
{
	void *handle = purple_reconnect_get_handle();
	void *conn_handle = purple_connections_get_handle();
	void *acct_handle = purple_accounts_get_handle();
	void *net_handle = purple_network_get_handle();

	account_data = g_hash_table_new_full(g_direct_hash, g_direct_equal,
			NULL, g_free);

	purple_signal_connect(conn_handle, "connection-error", handle,
			PURPLE_CALLBACK(connection_error_cb), NULL);
	purple_signal_connect(conn_handle, "signed-on", handle,
			PURPLE_CALLBACK(signed_on_cb), NULL);

	purple_signal_connect(acct_handle, "account-removed", handle,
			PURPLE_CALLBACK(account_removed_cb), NULL);
	purple_signal_connect(acct_handle, "account-disabled", handle,
			PURPLE_CALLBACK(account_disabled_cb), NULL);

	purple_signal_connect(net_handle, "network-configuration-changed", handle,
			PURPLE_CALLBACK(network_available_cb), NULL);
}

void
purple_reconnect_uninit(void)
{
	purple_signals_disconnect_by_handle(purple_reconnect_get_handle());

	if (account_data != NULL) {
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init(&iter, account_data);
		while (g_hash_table_iter_next(&iter, &key, &value))
			clear_pending((PurpleReconnectData *)value);

		g_hash_table_destroy(account_data);
		account_data = NULL;
	}
}

/**
 * @file msgqueue.c Outgoing message queue that survives reconnects.
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
#include "conversation.h"
#include "core.h"
#include "debug.h"
#include "msgqueue.h"
#include "reconnect.h"
#include "server.h"
#include "signals.h"

/* Defaults: keep up to 64 messages per account, discard after one hour. */
#define DEFAULT_MAX_PER_ACCOUNT  64
#define DEFAULT_MAX_AGE_SECS     (60 * 60)

typedef struct {
	char                *who;
	char                *message;
	PurpleMessageFlags   flags;
	time_t               queued_at;
} PurpleQueuedMessage;

static gboolean queue_enabled     = TRUE;
static guint    cfg_max_per_acct  = DEFAULT_MAX_PER_ACCOUNT;
static guint    cfg_max_age       = DEFAULT_MAX_AGE_SECS;

/* PurpleAccount* -> GQueue* of PurpleQueuedMessage* (oldest at head) */
static GHashTable *account_queues = NULL;

static void *msgqueue_handle = NULL;

static void
queued_message_free(PurpleQueuedMessage *qm)
{
	if (qm == NULL)
		return;
	g_free(qm->who);
	g_free(qm->message);
	g_free(qm);
}

static void
queue_destroy(gpointer data)
{
	GQueue *q = data;
	PurpleQueuedMessage *qm;

	while ((qm = g_queue_pop_head(q)) != NULL)
		queued_message_free(qm);
	g_queue_free(q);
}

static GQueue *
queue_for(PurpleAccount *account, gboolean create)
{
	GQueue *q;

	if (account_queues == NULL)
		return NULL;

	q = g_hash_table_lookup(account_queues, account);
	if (q == NULL && create) {
		q = g_queue_new();
		g_hash_table_insert(account_queues, account, q);
	}
	return q;
}

/* Drop messages that have outlived cfg_max_age. */
static void
expire_stale(GQueue *q)
{
	time_t now;
	PurpleQueuedMessage *qm;

	if (q == NULL || cfg_max_age == 0)
		return;

	now = time(NULL);
	while ((qm = g_queue_peek_head(q)) != NULL) {
		if (now - qm->queued_at < (time_t)cfg_max_age)
			break;
		g_queue_pop_head(q);
		purple_debug_info("msgqueue",
				"discarding stale queued message to %s (aged %lds)\n",
				qm->who ? qm->who : "?",
				(long)(now - qm->queued_at));
		queued_message_free(qm);
	}
}

/**************************************************************************/
/* Public: policy & queries                                               */
/**************************************************************************/

void
purple_msgqueue_set_enabled(gboolean enabled)
{
	queue_enabled = enabled;

	/* If the queue is being turned off, drop everything we were holding so
	 * we don't flush it out of the blue later. */
	if (!enabled && account_queues != NULL)
		g_hash_table_remove_all(account_queues);
}

gboolean
purple_msgqueue_get_enabled(void)
{
	return queue_enabled;
}

void
purple_msgqueue_set_limits(guint max_per_account, guint max_age_secs)
{
	cfg_max_per_acct = max_per_account < 1 ? 1 : max_per_account;
	cfg_max_age = max_age_secs;
}

gboolean
purple_msgqueue_enqueue_im(PurpleAccount *account, const char *who,
                           const char *message, PurpleMessageFlags flags)
{
	GQueue *q;
	PurpleQueuedMessage *qm;

	g_return_val_if_fail(account != NULL, FALSE);
	g_return_val_if_fail(who != NULL, FALSE);
	g_return_val_if_fail(message != NULL, FALSE);

	if (!queue_enabled)
		return FALSE;

	/* Auto-responses and system-generated traffic are not worth deferring;
	 * they are only meaningful in the moment. */
	if (flags & (PURPLE_MESSAGE_AUTO_RESP | PURPLE_MESSAGE_SYSTEM |
			PURPLE_MESSAGE_NO_LOG))
		return FALSE;

	/* Only park a message if the framework actually intends to bring this
	 * account back. A permanently-offline (disabled/fatal) account should
	 * report the failure immediately, exactly as before. */
	if (!purple_reconnect_is_pending(account))
		return FALSE;

	q = queue_for(account, TRUE);
	expire_stale(q);

	/* Enforce the per-account cap by dropping the oldest messages. */
	while (g_queue_get_length(q) >= cfg_max_per_acct) {
		PurpleQueuedMessage *old = g_queue_pop_head(q);
		purple_debug_info("msgqueue",
				"queue for %s full; dropping oldest message to %s\n",
				purple_account_get_username(account),
				old && old->who ? old->who : "?");
		queued_message_free(old);
	}

	qm = g_new0(PurpleQueuedMessage, 1);
	qm->who = g_strdup(who);
	qm->message = g_strdup(message);
	qm->flags = flags;
	qm->queued_at = time(NULL);
	g_queue_push_tail(q, qm);

	purple_debug_info("msgqueue",
			"parked outgoing message to %s on %s (%u queued)\n",
			who, purple_account_get_username(account),
			g_queue_get_length(q));

	return TRUE;
}

guint
purple_msgqueue_get_count(PurpleAccount *account)
{
	GQueue *q = queue_for(account, FALSE);
	return q != NULL ? g_queue_get_length(q) : 0;
}

void
purple_msgqueue_clear_account(PurpleAccount *account)
{
	if (account_queues != NULL)
		g_hash_table_remove(account_queues, account);
}

/**************************************************************************/
/* Flush on sign-on                                                       */
/**************************************************************************/

static void
flush_account(PurpleAccount *account)
{
	PurpleConnection *gc;
	GQueue *q;
	PurpleQueuedMessage *qm;
	guint sent = 0;

	q = queue_for(account, FALSE);
	if (q == NULL || g_queue_is_empty(q))
		return;

	gc = purple_account_get_connection(account);
	if (gc == NULL || purple_connection_get_state(gc) != PURPLE_CONNECTED) {
		/* Not actually ready; leave the queue intact and try next time. */
		return;
	}

	expire_stale(q);

	while ((qm = g_queue_pop_head(q)) != NULL) {
		purple_debug_info("msgqueue",
				"flushing queued message to %s on %s\n",
				qm->who, purple_account_get_username(account));
		serv_send_im(gc, qm->who, qm->message, qm->flags);
		sent++;
		queued_message_free(qm);
	}

	if (sent > 0)
		purple_debug_info("msgqueue",
				"flushed %u queued message(s) for %s\n", sent,
				purple_account_get_username(account));

	/* Drop the now-empty queue container. */
	purple_msgqueue_clear_account(account);
}

static void
signed_on_cb(PurpleConnection *gc, void *data)
{
	flush_account(purple_connection_get_account(gc));
}

static void
account_removed_cb(PurpleAccount *account, void *data)
{
	purple_msgqueue_clear_account(account);
}

static void
account_disabled_cb(PurpleAccount *account, void *data)
{
	/* A user-disabled account is not coming back on its own; anything we
	 * were holding for it should not be silently flushed later. */
	purple_msgqueue_clear_account(account);
}

/**************************************************************************/
/* Subsystem                                                              */
/**************************************************************************/

void *
purple_msgqueue_get_handle(void)
{
	return &msgqueue_handle;
}

void
purple_msgqueue_init(void)
{
	void *handle = purple_msgqueue_get_handle();
	void *conn_handle = purple_connections_get_handle();
	void *acct_handle = purple_accounts_get_handle();

	account_queues = g_hash_table_new_full(g_direct_hash, g_direct_equal,
			NULL, queue_destroy);

	purple_signal_connect(conn_handle, "signed-on", handle,
			PURPLE_CALLBACK(signed_on_cb), NULL);

	purple_signal_connect(acct_handle, "account-removed", handle,
			PURPLE_CALLBACK(account_removed_cb), NULL);
	purple_signal_connect(acct_handle, "account-disabled", handle,
			PURPLE_CALLBACK(account_disabled_cb), NULL);
}

void
purple_msgqueue_uninit(void)
{
	purple_signals_disconnect_by_handle(purple_msgqueue_get_handle());

	if (account_queues != NULL) {
		g_hash_table_destroy(account_queues);
		account_queues = NULL;
	}
}

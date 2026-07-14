/**
 * @file ratelimit.c Outbound message flood control (token bucket).
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

#include "connection.h"
#include "core.h"
#include "debug.h"
#include "ratelimit.h"
#include "signals.h"

/*
 * Defaults: a conservative ceiling that suits chatty text protocols without
 * tripping typical server flood detectors. Five messages back-to-back, then
 * one every ~1.3s sustained -- comfortably under IRC's classic
 * "2 seconds + 1s per line" allowance.
 */
#define DEFAULT_BURST           5
#define DEFAULT_REFILL_PER_SEC  0.75

typedef struct {
	gdouble  tokens;        /* credits currently available (0 .. burst)   */
	gdouble  burst;         /* ceiling                                     */
	gdouble  refill;        /* credits gained per second                   */
	gint64   last;          /* monotonic time (us) of last refill          */
} PurpleRateBucket;

static gboolean ratelimit_enabled = TRUE;
static guint    cfg_burst   = DEFAULT_BURST;
static gdouble  cfg_refill  = DEFAULT_REFILL_PER_SEC;

/* PurpleConnection* -> PurpleRateBucket* */
static GHashTable *conn_buckets = NULL;

static void *ratelimit_handle = NULL;

static PurpleRateBucket *
bucket_for(PurpleConnection *gc, gboolean create)
{
	PurpleRateBucket *b;

	if (conn_buckets == NULL)
		return NULL;

	b = g_hash_table_lookup(conn_buckets, gc);
	if (b == NULL && create) {
		b = g_new0(PurpleRateBucket, 1);
		b->burst = cfg_burst < 1 ? 1 : cfg_burst;
		b->refill = cfg_refill > 0.0 ? cfg_refill : DEFAULT_REFILL_PER_SEC;
		b->tokens = b->burst;   /* start full: no penalty for the first burst */
		b->last = g_get_monotonic_time();
		g_hash_table_insert(conn_buckets, gc, b);
	}
	return b;
}

/* Bring a bucket's token count up to date for the elapsed wall time. */
static void
refill(PurpleRateBucket *b)
{
	gint64 now = g_get_monotonic_time();
	gint64 elapsed_us = now - b->last;

	if (elapsed_us <= 0) {
		b->last = now;
		return;
	}

	b->tokens += (elapsed_us / (gdouble)G_USEC_PER_SEC) * b->refill;
	if (b->tokens > b->burst)
		b->tokens = b->burst;
	b->last = now;
}

/**************************************************************************/
/* Public: policy & queries                                               */
/**************************************************************************/

void
purple_ratelimit_set_enabled(gboolean enabled)
{
	ratelimit_enabled = enabled;
}

gboolean
purple_ratelimit_get_enabled(void)
{
	return ratelimit_enabled;
}

void
purple_ratelimit_set_default(guint burst, gdouble refill_per_sec)
{
	cfg_burst = burst < 1 ? 1 : burst;
	cfg_refill = refill_per_sec > 0.0 ? refill_per_sec : DEFAULT_REFILL_PER_SEC;
}

void
purple_ratelimit_set_connection(PurpleConnection *gc, guint burst,
                                gdouble refill_per_sec)
{
	PurpleRateBucket *b;

	g_return_if_fail(gc != NULL);

	b = bucket_for(gc, TRUE);
	if (b == NULL)
		return;

	b->burst = burst < 1 ? 1 : burst;
	b->refill = refill_per_sec > 0.0 ? refill_per_sec : DEFAULT_REFILL_PER_SEC;
	if (b->tokens > b->burst)
		b->tokens = b->burst;
}

gboolean
purple_ratelimit_try_consume(PurpleConnection *gc)
{
	PurpleRateBucket *b;

	if (!ratelimit_enabled)
		return TRUE;

	g_return_val_if_fail(gc != NULL, TRUE);

	b = bucket_for(gc, TRUE);
	if (b == NULL)
		return TRUE;

	refill(b);

	if (b->tokens >= 1.0) {
		b->tokens -= 1.0;
		return TRUE;
	}

	return FALSE;
}

gdouble
purple_ratelimit_next_delay(PurpleConnection *gc)
{
	PurpleRateBucket *b;
	gdouble deficit;

	if (!ratelimit_enabled)
		return 0.0;

	g_return_val_if_fail(gc != NULL, 0.0);

	b = bucket_for(gc, FALSE);
	if (b == NULL)
		return 0.0;

	refill(b);

	if (b->tokens >= 1.0)
		return 0.0;

	/* Seconds of refill needed to reach one whole credit. */
	deficit = 1.0 - b->tokens;
	return b->refill > 0.0 ? deficit / b->refill : 0.0;
}

void
purple_ratelimit_reset_connection(PurpleConnection *gc)
{
	if (conn_buckets != NULL)
		g_hash_table_remove(conn_buckets, gc);
}

/**************************************************************************/
/* Signal handlers                                                        */
/**************************************************************************/

static void
signed_off_cb(PurpleConnection *gc, void *data)
{
	purple_ratelimit_reset_connection(gc);
}

/**************************************************************************/
/* Subsystem                                                              */
/**************************************************************************/

void *
purple_ratelimit_get_handle(void)
{
	return &ratelimit_handle;
}

void
purple_ratelimit_init(void)
{
	void *handle = purple_ratelimit_get_handle();
	void *conn_handle = purple_connections_get_handle();

	conn_buckets = g_hash_table_new_full(g_direct_hash, g_direct_equal,
			NULL, g_free);

	purple_signal_connect(conn_handle, "signed-off", handle,
			PURPLE_CALLBACK(signed_off_cb), NULL);
}

void
purple_ratelimit_uninit(void)
{
	purple_signals_disconnect_by_handle(purple_ratelimit_get_handle());

	if (conn_buckets != NULL) {
		g_hash_table_destroy(conn_buckets);
		conn_buckets = NULL;
	}
}

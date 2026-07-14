/**
 * @file connhealth.c Connection health telemetry & observability.
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
#include "connhealth.h"
#include "core.h"
#include "debug.h"
#include "eventloop.h"
#include "signals.h"
#include "value.h"

/* How often we sample every live connection's liveness marker. */
#define SAMPLE_INTERVAL_SECS  5

/*
 * Defaults: a connection is GOOD while it has produced inbound traffic within
 * the last 45s (keepalive fires at 30s, so a healthy link is never graded
 * worse than IDLE just for being quiet), IDLE up to 90s, STALLED beyond.
 */
#define DEFAULT_IDLE_SECS      45
#define DEFAULT_STALLED_SECS   90

/* EWMA smoothing factor for the latency proxy (higher = snappier). */
#define LATENCY_ALPHA          0.3

typedef struct {
	PurpleConnectionHealth  health;      /* last-reported grade            */
	time_t                  last_seen;   /* last_received we saw last tick  */
	time_t                  quiet_since; /* when the link first went quiet  */
	gdouble                 latency;     /* EWMA silence->response, seconds */
} PurpleHealthData;

static gboolean health_enabled = TRUE;
static guint    cfg_idle_secs    = DEFAULT_IDLE_SECS;
static guint    cfg_stalled_secs = DEFAULT_STALLED_SECS;

/* PurpleConnection* -> PurpleHealthData* */
static GHashTable *health_data = NULL;

static guint  sample_timer = 0;
static void  *connhealth_handle = NULL;

static PurpleHealthData *
data_for(PurpleConnection *gc, gboolean create)
{
	PurpleHealthData *hd;

	if (health_data == NULL)
		return NULL;

	hd = g_hash_table_lookup(health_data, gc);
	if (hd == NULL && create) {
		hd = g_new0(PurpleHealthData, 1);
		hd->health = PURPLE_CONN_HEALTH_UNKNOWN;
		hd->last_seen = 0;
		g_hash_table_insert(health_data, gc, hd);
	}
	return hd;
}

static PurpleConnectionHealth
grade_for_idle(guint idle)
{
	if (idle < cfg_idle_secs)
		return PURPLE_CONN_HEALTH_GOOD;
	if (idle < cfg_stalled_secs)
		return PURPLE_CONN_HEALTH_IDLE;
	return PURPLE_CONN_HEALTH_STALLED;
}

/*
 * Evaluate one connection: fold in any fresh inbound activity, update the
 * latency estimate, recompute the grade, and emit a signal on transition.
 */
static void
sample_connection(PurpleConnection *gc, time_t now)
{
	PurpleHealthData *hd;
	guint idle;
	PurpleConnectionHealth grade;

	if (purple_connection_get_state(gc) != PURPLE_CONNECTED)
		return;

	hd = data_for(gc, TRUE);
	if (hd == NULL)
		return;

	/* Did the peer send us something since the last tick? */
	if (gc->last_received > hd->last_seen) {
		if (hd->quiet_since != 0) {
			/*
			 * The link had gone quiet and has now spoken again. The length
			 * of that silence, up to when data arrived, is a coarse proxy
			 * for how long the peer took to respond -- fold it into the
			 * smoothed latency estimate.
			 */
			gdouble observed = (gdouble)(gc->last_received - hd->quiet_since);
			if (observed < 0.0)
				observed = 0.0;
			if (hd->latency <= 0.0)
				hd->latency = observed;
			else
				hd->latency = (LATENCY_ALPHA * observed) +
						((1.0 - LATENCY_ALPHA) * hd->latency);
			hd->quiet_since = 0;
		}
		hd->last_seen = gc->last_received;
	} else if (hd->quiet_since == 0 && hd->last_seen != 0) {
		/* First tick in which no new data has arrived: mark the silence. */
		hd->quiet_since = hd->last_seen;
	}

	idle = (hd->last_seen != 0 && now >= hd->last_seen)
			? (guint)(now - hd->last_seen) : 0;
	grade = grade_for_idle(idle);

	if (grade != hd->health) {
		PurpleConnectionHealth old = hd->health;
		hd->health = grade;
		purple_debug_info("connhealth",
				"%s health %s -> %s (idle %us, latency %.1fs)\n",
				purple_account_get_username(purple_connection_get_account(gc)),
				purple_connection_health_to_string(old),
				purple_connection_health_to_string(grade),
				idle, hd->latency);
		purple_signal_emit(purple_connhealth_get_handle(),
				"connection-health-changed", gc, (int)old, (int)grade);
	}
}

static gboolean
sample_all(gpointer data)
{
	GList *l;
	time_t now = time(NULL);

	if (!health_enabled)
		return TRUE;

	for (l = purple_connections_get_all(); l != NULL; l = l->next)
		sample_connection((PurpleConnection *)l->data, now);

	return TRUE;
}

/**************************************************************************/
/* Public: queries                                                        */
/**************************************************************************/

PurpleConnectionHealth
purple_connection_get_health(PurpleConnection *gc)
{
	PurpleHealthData *hd;

	g_return_val_if_fail(gc != NULL, PURPLE_CONN_HEALTH_UNKNOWN);

	if (!health_enabled)
		return PURPLE_CONN_HEALTH_UNKNOWN;

	hd = data_for(gc, FALSE);
	return hd != NULL ? hd->health : PURPLE_CONN_HEALTH_UNKNOWN;
}

const char *
purple_connection_health_to_string(PurpleConnectionHealth health)
{
	switch (health) {
		case PURPLE_CONN_HEALTH_GOOD:    return "good";
		case PURPLE_CONN_HEALTH_IDLE:    return "idle";
		case PURPLE_CONN_HEALTH_STALLED: return "stalled";
		case PURPLE_CONN_HEALTH_UNKNOWN:
		default:                         return "unknown";
	}
}

guint
purple_connection_get_idle_time(PurpleConnection *gc)
{
	PurpleHealthData *hd;
	time_t now;

	g_return_val_if_fail(gc != NULL, 0);

	hd = data_for(gc, FALSE);
	if (hd == NULL || hd->last_seen == 0)
		return 0;

	now = time(NULL);
	return now >= hd->last_seen ? (guint)(now - hd->last_seen) : 0;
}

gdouble
purple_connection_get_latency(PurpleConnection *gc)
{
	PurpleHealthData *hd;

	g_return_val_if_fail(gc != NULL, 0.0);

	hd = data_for(gc, FALSE);
	return hd != NULL ? hd->latency : 0.0;
}

/**************************************************************************/
/* Public: policy                                                         */
/**************************************************************************/

void
purple_connhealth_set_enabled(gboolean enabled)
{
	health_enabled = enabled;
}

gboolean
purple_connhealth_get_enabled(void)
{
	return health_enabled;
}

void
purple_connhealth_set_thresholds(guint idle_secs, guint stalled_secs)
{
	cfg_idle_secs = idle_secs < 1 ? 1 : idle_secs;
	cfg_stalled_secs = stalled_secs > cfg_idle_secs
			? stalled_secs : cfg_idle_secs + 1;
}

/**************************************************************************/
/* Signal handlers                                                        */
/**************************************************************************/

static void
signed_off_cb(PurpleConnection *gc, void *data)
{
	if (health_data != NULL)
		g_hash_table_remove(health_data, gc);
}

/**************************************************************************/
/* Subsystem                                                              */
/**************************************************************************/

void *
purple_connhealth_get_handle(void)
{
	return &connhealth_handle;
}

void
purple_connhealth_init(void)
{
	void *handle = purple_connhealth_get_handle();
	void *conn_handle = purple_connections_get_handle();

	health_data = g_hash_table_new_full(g_direct_hash, g_direct_equal,
			NULL, g_free);

	purple_signal_register(handle, "connection-health-changed",
			purple_marshal_VOID__POINTER_INT_INT, NULL, 3,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_CONNECTION),
			purple_value_new(PURPLE_TYPE_INT),
			purple_value_new(PURPLE_TYPE_INT));

	purple_signal_connect(conn_handle, "signed-off", handle,
			PURPLE_CALLBACK(signed_off_cb), NULL);

	sample_timer = purple_timeout_add_seconds(SAMPLE_INTERVAL_SECS,
			sample_all, NULL);
}

void
purple_connhealth_uninit(void)
{
	if (sample_timer > 0) {
		purple_timeout_remove(sample_timer);
		sample_timer = 0;
	}

	purple_signals_unregister_by_instance(purple_connhealth_get_handle());
	purple_signals_disconnect_by_handle(purple_connhealth_get_handle());

	if (health_data != NULL) {
		g_hash_table_destroy(health_data);
		health_data = NULL;
	}
}

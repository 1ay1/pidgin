/**
 * @file connhealth.h Connection health telemetry & observability.
 * @ingroup core
 *
 * A best-in-class IM framework should let an embedder *see* the health of a
 * live connection -- is it responsive, has it gone quiet, is it about to
 * drop? Historically libpurple exposed only two coarse states (connected /
 * disconnected) plus a private keepalive timer buried in connection.c; there
 * was no way for a UI or plugin to observe latency or notice a silently-wedged
 * link before the socket actually failed.
 *
 * This subsystem adds lightweight, protocol-agnostic health telemetry. It
 * watches the inbound-liveness marker every connection already maintains
 * (@c last_received, advanced by each prpl when it reads data) and derives:
 *
 *  - an @b idle time (seconds since the last inbound byte),
 *  - a coarse @b health grade (good / idle / stalled), and
 *  - a smoothed @b latency estimate (EWMA of the gap between a connection
 *    going quiet and the next inbound activity -- a keepalive-round-trip
 *    proxy that needs no protocol cooperation).
 *
 * It emits a @c "connection-health-changed" signal whenever a connection's
 * health grade transitions, so a UI can surface a "reconnecting soon" hint or
 * a signal-strength style indicator without polling. Everything is derived
 * from existing state; no protocol, ABI, or wire change is required.
 */
#ifndef _PURPLE_CONNHEALTH_H_
#define _PURPLE_CONNHEALTH_H_

#include "connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Coarse health grade for a live connection.
 */
typedef enum
{
	PURPLE_CONN_HEALTH_UNKNOWN = 0, /**< Not connected or not yet sampled. */
	PURPLE_CONN_HEALTH_GOOD,        /**< Recent inbound activity.          */
	PURPLE_CONN_HEALTH_IDLE,        /**< Quiet, but within tolerance.      */
	PURPLE_CONN_HEALTH_STALLED      /**< Silent well past the keepalive
	                                     window; likely wedged/dropping.   */
} PurpleConnectionHealth;

/**************************************************************************/
/** @name Health queries                                                  */
/**************************************************************************/
/*@{*/

/**
 * @return @a gc's current coarse health grade.
 */
PurpleConnectionHealth purple_connection_get_health(PurpleConnection *gc);

/**
 * @return A short, non-localized label for a health grade ("good", "idle",
 *         "stalled", "unknown"), suitable for logs or debug UIs.
 */
const char *purple_connection_health_to_string(PurpleConnectionHealth health);

/**
 * @return The number of seconds since @a gc last received data from the
 *         server, or 0 if unknown.
 */
guint purple_connection_get_idle_time(PurpleConnection *gc);

/**
 * @return A smoothed estimate, in seconds, of @a gc's server round-trip
 *         latency (how long it takes the peer to break a silence), or 0.0 if
 *         no estimate has been formed yet.
 */
gdouble purple_connection_get_latency(PurpleConnection *gc);

/*@}*/

/**************************************************************************/
/** @name Policy                                                          */
/**************************************************************************/
/*@{*/

/**
 * Enable or disable health telemetry globally. Enabled by default. When
 * disabled, no sampling occurs and every connection reports
 * @c PURPLE_CONN_HEALTH_UNKNOWN.
 */
void purple_connhealth_set_enabled(gboolean enabled);

/**
 * @return @c TRUE if health telemetry is currently enabled.
 */
gboolean purple_connhealth_get_enabled(void);

/**
 * Tune the idle thresholds, in seconds.
 *
 * A connection is graded GOOD while idle < @a idle_secs, IDLE between that
 * and @a stalled_secs, and STALLED beyond @a stalled_secs.
 *
 * @param idle_secs    Idle seconds after which a connection drops to IDLE
 *                     (clamped >= 1).
 * @param stalled_secs Idle seconds after which it is graded STALLED (clamped
 *                     > idle_secs).
 */
void purple_connhealth_set_thresholds(guint idle_secs, guint stalled_secs);

/*@}*/

/**************************************************************************/
/** @name Subsystem                                                       */
/**************************************************************************/
/*@{*/

/**
 * @return The health subsystem handle (for signal connection). It registers
 *         the @c "connection-health-changed" signal:
 *         @code (PurpleConnection *gc, int old_health, int new_health) @endcode
 */
void *purple_connhealth_get_handle(void);

/**
 * Initialise the connection health subsystem.
 */
void purple_connhealth_init(void);

/**
 * Tear down the connection health subsystem.
 */
void purple_connhealth_uninit(void);

/*@}*/

#ifdef __cplusplus
}
#endif

#endif /* _PURPLE_CONNHEALTH_H_ */

/**
 * @file ratelimit.h Outbound message flood control (token bucket).
 * @ingroup core
 *
 * Many IM servers enforce an anti-flood policy: send messages faster than
 * some threshold and the server silently drops them, throttles you, or -- on
 * IRC and similar -- disconnects the client with an "Excess Flood" error.
 * Historically each libpurple protocol plugin either ignored this (and got its
 * users kicked) or hand-rolled its own pacing queue. There was no shared,
 * principled outbound rate limiter in the core.
 *
 * This subsystem provides one: a per-connection token bucket. Each connection
 * accrues send credits at a steady refill rate up to a burst ceiling; sending
 * a message spends a credit. When credits run out, the send path can ask the
 * limiter how long to wait before the next message would be allowed, and pace
 * accordingly instead of tripping the server's flood detector.
 *
 * The bucket is advisory: it exposes @c purple_ratelimit_try_consume (spend a
 * credit if available) and @c purple_ratelimit_next_delay (seconds until the
 * next credit) so callers -- the server layer, a protocol plugin, or the
 * message queue flusher -- can pace themselves without any protocol changes.
 * Per-connection parameters can be tuned by a protocol that knows its server's
 * exact policy; otherwise a conservative default applies.
 */
#ifndef _PURPLE_RATELIMIT_H_
#define _PURPLE_RATELIMIT_H_

#include "connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************/
/** @name Rate limit policy & queries                                     */
/**************************************************************************/
/*@{*/

/**
 * Enable or disable outbound rate limiting globally. Enabled by default.
 *
 * When disabled, @c purple_ratelimit_try_consume always succeeds and
 * @c purple_ratelimit_next_delay always returns 0, so the framework behaves
 * exactly as it did before this subsystem existed.
 */
void purple_ratelimit_set_enabled(gboolean enabled);

/**
 * @return @c TRUE if outbound rate limiting is currently enabled.
 */
gboolean purple_ratelimit_get_enabled(void);

/**
 * Set the default token-bucket parameters used for connections that have not
 * been given an explicit policy.
 *
 * A bucket refills at @a refill_per_sec credits per second up to a ceiling of
 * @a burst credits, and each message consumes one credit. Thus the sustained
 * rate is @a refill_per_sec messages/second and up to @a burst messages may be
 * sent back-to-back after an idle period.
 *
 * @param burst          Maximum credits the bucket can hold (clamped >= 1).
 * @param refill_per_sec Steady refill rate in credits/second (clamped > 0).
 */
void purple_ratelimit_set_default(guint burst, gdouble refill_per_sec);

/**
 * Give a specific connection its own token-bucket policy, overriding the
 * default. A protocol plugin that knows its server's exact flood threshold
 * should call this from its login/connect path.
 *
 * @param gc             The connection.
 * @param burst          Maximum credits (clamped >= 1).
 * @param refill_per_sec Steady refill rate in credits/second (clamped > 0).
 */
void purple_ratelimit_set_connection(PurpleConnection *gc, guint burst,
                                     gdouble refill_per_sec);

/**
 * Attempt to spend one send credit for @a gc.
 *
 * @return @c TRUE if a credit was available and has been consumed (the caller
 *         may send now); @c FALSE if the bucket is empty (the caller should
 *         defer, consulting @c purple_ratelimit_next_delay). Always @c TRUE
 *         when rate limiting is disabled.
 */
gboolean purple_ratelimit_try_consume(PurpleConnection *gc);

/**
 * @return The number of seconds until @a gc's bucket will next hold at least
 *         one credit (fractional; 0.0 if a credit is available right now or
 *         rate limiting is disabled).
 */
gdouble purple_ratelimit_next_delay(PurpleConnection *gc);

/**
 * Forget any bucket state associated with @a gc. Called automatically when a
 * connection is destroyed; exposed for protocols that want to reset pacing.
 */
void purple_ratelimit_reset_connection(PurpleConnection *gc);

/*@}*/

/**************************************************************************/
/** @name Subsystem                                                       */
/**************************************************************************/
/*@{*/

/**
 * @return The rate limit subsystem handle (for signal connection).
 */
void *purple_ratelimit_get_handle(void);

/**
 * Initialise the rate limit subsystem.
 */
void purple_ratelimit_init(void);

/**
 * Tear down the rate limit subsystem, discarding all bucket state.
 */
void purple_ratelimit_uninit(void);

/*@}*/

#ifdef __cplusplus
}
#endif

#endif /* _PURPLE_RATELIMIT_H_ */

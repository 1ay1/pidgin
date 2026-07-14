/**
 * @file reconnect.h Automatic reconnection with exponential backoff.
 * @ingroup core
 *
 * A best-in-class IM framework should recover from transient network drops
 * on its own. Historically libpurple pushed this responsibility onto a
 * separate "autorecon" UI plugin, so every embedder had to ship reconnection
 * logic and there was no principled, shared backoff policy.
 *
 * This subsystem builds automatic reconnection into the core. It observes the
 * connection lifecycle signals and, when an account drops with a *transient*
 * (non-fatal) error, schedules a reconnect using capped exponential backoff
 * with jitter. Fatal errors (bad password, account banned, ...) are never
 * retried. Reconnection is suspended while the network is unavailable and
 * resumes automatically when connectivity returns.
 *
 * It is entirely signal-driven and requires no protocol or UI cooperation.
 */
#ifndef _PURPLE_RECONNECT_H_
#define _PURPLE_RECONNECT_H_

#include "account.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************/
/** @name Reconnection policy & queries                                   */
/**************************************************************************/
/*@{*/

/**
 * Enable or disable automatic reconnection globally. Enabled by default.
 *
 * When disabled, any pending reconnect timers are cancelled and no new ones
 * are scheduled; the account simply stays offline after a drop, as it did
 * before this subsystem existed.
 */
void purple_reconnect_set_enabled(gboolean enabled);

/**
 * @return @c TRUE if automatic reconnection is currently enabled.
 */
gboolean purple_reconnect_get_enabled(void);

/**
 * Tune the backoff schedule.
 *
 * The delay before the Nth consecutive reconnect attempt is
 *     min(initial_delay * 2^(N-1), max_delay)
 * plus a small random jitter, and attempts stop after @a max_attempts
 * consecutive failures (0 = retry forever).
 *
 * @param initial_delay First-attempt delay, in seconds (clamped to >= 1).
 * @param max_delay     Ceiling for the delay, in seconds (clamped to
 *                      >= initial_delay).
 * @param max_attempts  Consecutive-failure cap, or 0 for unlimited.
 */
void purple_reconnect_set_backoff(guint initial_delay, guint max_delay,
                                  guint max_attempts);

/**
 * @return The number of seconds until @a account's next scheduled reconnect,
 *         or 0 if none is pending.
 */
guint purple_reconnect_get_delay(PurpleAccount *account);

/**
 * @return The number of consecutive failed reconnect attempts recorded for
 *         @a account (reset to 0 once it signs on), or 0 if none.
 */
guint purple_reconnect_get_attempts(PurpleAccount *account);

/**
 * @return @c TRUE if a reconnect is currently armed or parked for
 *         @a account (i.e. the account dropped with a transient error and the
 *         framework intends to bring it back). This is the signal other
 *         subsystems (e.g. the outgoing message queue) use to decide whether
 *         a transient-offline account is worth waiting for.
 */
gboolean purple_reconnect_is_pending(PurpleAccount *account);

/**
 * Cancel any pending reconnect for @a account and clear its failure counter.
 */
void purple_reconnect_cancel_account(PurpleAccount *account);

/*@}*/

/**************************************************************************/
/** @name Subsystem                                                       */
/**************************************************************************/
/*@{*/

/**
 * @return The reconnect subsystem handle (for signal connection).
 */
void *purple_reconnect_get_handle(void);

/**
 * Initialise the reconnect subsystem.
 */
void purple_reconnect_init(void);

/**
 * Tear down the reconnect subsystem, cancelling all pending reconnects.
 */
void purple_reconnect_uninit(void);

/*@}*/

#ifdef __cplusplus
}
#endif

#endif /* _PURPLE_RECONNECT_H_ */

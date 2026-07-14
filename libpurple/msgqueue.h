/**
 * @file msgqueue.h Outgoing message queue that survives reconnects.
 * @ingroup core
 *
 * A best-in-class IM framework should not silently drop a message the user
 * hit "send" on just because the connection blipped a fraction of a second
 * earlier. Historically libpurple's @c serv_send_im returned an error to the
 * UI whenever the connection was not @c PURPLE_CONNECTED, and the message was
 * lost; the user had to notice, and manually resend, once the account came
 * back.
 *
 * This subsystem introduces a per-account store-and-forward queue. When an IM
 * is sent while the account is mid-reconnect (or otherwise transiently
 * offline), the message is parked instead of dropped, and automatically
 * flushed in order once the account signs back on. It integrates with the
 * @c reconnect subsystem: messages are only held while a reconnect is
 * actually pending, so a permanently-offline account still reports the send
 * failure immediately rather than swallowing it forever.
 *
 * Parked messages expire after a bounded lifetime and the queue is capped per
 * account, so a long outage cannot grow memory without bound. It is entirely
 * signal-driven and requires no protocol or UI cooperation.
 */
#ifndef _PURPLE_MSGQUEUE_H_
#define _PURPLE_MSGQUEUE_H_

#include "account.h"
#include "conversation.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************/
/** @name Message queue policy & queries                                  */
/**************************************************************************/
/*@{*/

/**
 * Enable or disable the outgoing message queue globally. Enabled by default.
 *
 * When disabled, an IM sent while the account is offline fails immediately as
 * it did before this subsystem existed; already-queued messages are dropped.
 */
void purple_msgqueue_set_enabled(gboolean enabled);

/**
 * @return @c TRUE if the outgoing message queue is currently enabled.
 */
gboolean purple_msgqueue_get_enabled(void);

/**
 * Tune the queue limits.
 *
 * @param max_per_account Maximum number of parked messages retained per
 *                        account; older messages beyond this are dropped
 *                        (clamped to >= 1).
 * @param max_age_secs    Maximum time, in seconds, a message may sit parked
 *                        before it is discarded as stale (0 = no expiry).
 */
void purple_msgqueue_set_limits(guint max_per_account, guint max_age_secs);

/**
 * Try to park an outgoing IM for later delivery.
 *
 * This is invoked by the server layer when @c serv_send_im cannot deliver a
 * message because the account is transiently offline. If the account is
 * eligible (a reconnect is pending or in progress and queueing is enabled),
 * the message is copied into the queue and will be flushed on the next
 * successful sign-on.
 *
 * @param account The account the message was to be sent from.
 * @param who     The destination buddy name.
 * @param message The message body.
 * @param flags   The original @c PurpleMessageFlags.
 *
 * @return @c TRUE if the message was queued (the caller should treat the send
 *         as deferred, not failed); @c FALSE if it was not (the caller should
 *         report the failure as before).
 */
gboolean purple_msgqueue_enqueue_im(PurpleAccount *account, const char *who,
                                    const char *message,
                                    PurpleMessageFlags flags);

/**
 * @return The number of messages currently parked for @a account.
 */
guint purple_msgqueue_get_count(PurpleAccount *account);

/**
 * Discard every parked message for @a account without sending.
 */
void purple_msgqueue_clear_account(PurpleAccount *account);

/*@}*/

/**************************************************************************/
/** @name Subsystem                                                       */
/**************************************************************************/
/*@{*/

/**
 * @return The message queue subsystem handle (for signal connection).
 */
void *purple_msgqueue_get_handle(void);

/**
 * Initialise the message queue subsystem.
 */
void purple_msgqueue_init(void);

/**
 * Tear down the message queue subsystem, discarding all parked messages.
 */
void purple_msgqueue_uninit(void);

/*@}*/

#ifdef __cplusplus
}
#endif

#endif /* _PURPLE_MSGQUEUE_H_ */

/**
 * @file test_msgqueue.c Standalone unit test for the outgoing message queue.
 *
 * The msgqueue subsystem's flush/enqueue path is signal-driven and needs a
 * full core, but its two load-bearing invariants -- per-account cap
 * enforcement (drop the OLDEST message once full) and stale-message expiry
 * (discard from the head while the front is too old) -- are pure GQueue
 * operations. This test mirrors those exact operations so the intended
 * behaviour is provable without the (currently un-wired) check(1) harness.
 *
 * Build & run:
 *   cc -o /tmp/test_msgqueue test_msgqueue.c $(pkg-config --cflags --libs glib-2.0)
 *   /tmp/test_msgqueue
 */
#include <glib.h>
#include <stdio.h>
#include <time.h>

typedef struct {
	char   *message;
	time_t  queued_at;
} TestMsg;

static TestMsg *
msg_new(const char *body, time_t when)
{
	TestMsg *m = g_new0(TestMsg, 1);
	m->message = g_strdup(body);
	m->queued_at = when;
	return m;
}

static void
msg_free(TestMsg *m)
{
	if (m == NULL)
		return;
	g_free(m->message);
	g_free(m);
}

/* Mirror of expire_stale() in msgqueue.c: discard from the head while the
 * front message has outlived max_age (0 = never expire). */
static void
expire_stale(GQueue *q, guint max_age, time_t now)
{
	TestMsg *m;

	if (max_age == 0)
		return;

	while ((m = g_queue_peek_head(q)) != NULL) {
		if (now - m->queued_at < (time_t)max_age)
			break;
		g_queue_pop_head(q);
		msg_free(m);
	}
}

/* Mirror of the enqueue cap logic in purple_msgqueue_enqueue_im(): before
 * pushing, drop oldest messages while at/over the cap. */
static void
enqueue(GQueue *q, guint cap, const char *body, time_t now)
{
	while (g_queue_get_length(q) >= cap) {
		TestMsg *old = g_queue_pop_head(q);
		msg_free(old);
	}
	g_queue_push_tail(q, msg_new(body, now));
}

static int failures = 0;

static void
check(gboolean cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

int
main(void)
{
	GQueue *q;
	TestMsg *front;

	printf("test_msgqueue: cap enforcement drops OLDEST, keeps order\n");
	q = g_queue_new();
	/* cap 3: push five, expect the last three survive, FIFO order. */
	enqueue(q, 3, "m1", 100);
	enqueue(q, 3, "m2", 101);
	enqueue(q, 3, "m3", 102);
	enqueue(q, 3, "m4", 103); /* evicts m1 */
	enqueue(q, 3, "m5", 104); /* evicts m2 */
	check(g_queue_get_length(q) == 3, "length capped at 3");
	front = g_queue_peek_head(q);
	check(g_strcmp0(front->message, "m3") == 0, "oldest survivor is m3");
	front = g_queue_peek_tail(q);
	check(g_strcmp0(front->message, "m5") == 0, "newest is m5");
	g_queue_free_full(q, (GDestroyNotify)msg_free);

	printf("test_msgqueue: stale expiry drops from head only\n");
	q = g_queue_new();
	enqueue(q, 64, "old1", 0);    /* very old */
	enqueue(q, 64, "old2", 10);
	enqueue(q, 64, "fresh", 1000);
	/* now=1005, max_age=100: old1 (age 1005) and old2 (age 995) expire,
	 * fresh (age 5) stays. */
	expire_stale(q, 100, 1005);
	check(g_queue_get_length(q) == 1, "two stale messages expired");
	front = g_queue_peek_head(q);
	check(front && g_strcmp0(front->message, "fresh") == 0,
			"survivor is the fresh message");
	g_queue_free_full(q, (GDestroyNotify)msg_free);

	printf("test_msgqueue: max_age 0 disables expiry\n");
	q = g_queue_new();
	enqueue(q, 64, "ancient", 0);
	expire_stale(q, 0, 999999);
	check(g_queue_get_length(q) == 1, "nothing expired when max_age==0");
	g_queue_free_full(q, (GDestroyNotify)msg_free);

	printf("test_msgqueue: expiry stops at first fresh (no gap skipping)\n");
	q = g_queue_new();
	enqueue(q, 64, "stale", 0);
	enqueue(q, 64, "fresh", 1000);
	enqueue(q, 64, "stale2", 0); /* out-of-order timestamp behind a fresh one */
	expire_stale(q, 100, 1050);
	/* Only the head 'stale' expires; the scan stops at 'fresh' and never
	 * reaches the trailing 'stale2'. This documents the FIFO assumption. */
	check(g_queue_get_length(q) == 2, "expiry halts at first fresh head");
	front = g_queue_peek_head(q);
	check(front && g_strcmp0(front->message, "fresh") == 0,
			"head is now fresh");
	g_queue_free_full(q, (GDestroyNotify)msg_free);

	if (failures == 0)
		printf("\nAll msgqueue tests PASSED.\n");
	else
		printf("\n%d msgqueue test(s) FAILED.\n", failures);

	return failures == 0 ? 0 : 1;
}

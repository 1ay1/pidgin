/**
 * @file test_ratelimit.c Standalone unit test for the token-bucket math.
 *
 * The ratelimit subsystem's plumbing is signal-driven and needs a full core,
 * but its heart -- token accrual, burst consumption, and next-credit timing --
 * is pure arithmetic over a bucket struct. This test mirrors that arithmetic
 * exactly (feeding a synthetic clock so it is deterministic) so the intended
 * behaviour is provable without the (currently un-wired) check(1) harness.
 *
 * Build & run:
 *   cc -o /tmp/test_ratelimit test_ratelimit.c $(pkg-config --cflags --libs glib-2.0)
 *   /tmp/test_ratelimit
 */
#include <glib.h>
#include <stdio.h>

/* Mirror of PurpleRateBucket + refill()/consume() from ratelimit.c, but with
 * an injected monotonic clock (now_us) so the test is deterministic. */
typedef struct {
	gdouble tokens;
	gdouble burst;
	gdouble refill;
	gint64  last;
} Bucket;

static void
bucket_init(Bucket *b, gdouble burst, gdouble refill, gint64 now)
{
	b->burst = burst < 1 ? 1 : burst;
	b->refill = refill > 0.0 ? refill : 0.75;
	b->tokens = b->burst;
	b->last = now;
}

static void
do_refill(Bucket *b, gint64 now)
{
	gint64 elapsed = now - b->last;
	if (elapsed <= 0) { b->last = now; return; }
	b->tokens += (elapsed / (gdouble)G_USEC_PER_SEC) * b->refill;
	if (b->tokens > b->burst)
		b->tokens = b->burst;
	b->last = now;
}

static gboolean
try_consume(Bucket *b, gint64 now)
{
	do_refill(b, now);
	if (b->tokens >= 1.0) {
		b->tokens -= 1.0;
		return TRUE;
	}
	return FALSE;
}

static gdouble
next_delay(Bucket *b, gint64 now)
{
	do_refill(b, now);
	if (b->tokens >= 1.0)
		return 0.0;
	return (1.0 - b->tokens) / b->refill;
}

static int failures = 0;

static void
check(gboolean cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

#define SEC(n) ((gint64)((n) * G_USEC_PER_SEC))

int
main(void)
{
	Bucket b;
	gint64 t = 0;
	gdouble d;
	int i, ok;

	printf("test_ratelimit: full bucket allows exactly 'burst' back-to-back\n");
	bucket_init(&b, 5, 1.0, t);   /* burst 5, 1 credit/sec */
	ok = 0;
	for (i = 0; i < 5; i++)
		if (try_consume(&b, t)) ok++;   /* same instant, no refill */
	check(ok == 5, "5 immediate sends succeed");
	check(!try_consume(&b, t), "6th immediate send is throttled");

	printf("test_ratelimit: next_delay reports time to one credit\n");
	d = next_delay(&b, t);
	/* Empty bucket, 1 credit/sec -> ~1s until next credit. */
	check(d > 0.99 && d < 1.01, "next_delay ~= 1.0s at 1 credit/sec");

	printf("test_ratelimit: a credit becomes available after the delay\n");
	t += SEC(1);
	check(try_consume(&b, t), "send succeeds one second later");

	printf("test_ratelimit: bucket refills but never exceeds burst\n");
	bucket_init(&b, 5, 1.0, t);
	/* drain it */
	for (i = 0; i < 5; i++) try_consume(&b, t);
	/* idle for 100s -- would accrue 100 credits, must cap at burst=5 */
	t += SEC(100);
	do_refill(&b, t);
	check(b.tokens <= 5.0 + 1e-9, "tokens capped at burst after long idle");
	ok = 0;
	for (i = 0; i < 5; i++) if (try_consume(&b, t)) ok++;
	check(ok == 5, "exactly burst credits available after long idle");
	check(!try_consume(&b, t), "no extra credit beyond burst");

	printf("test_ratelimit: sustained rate matches refill\n");
	bucket_init(&b, 1, 2.0, t);   /* burst 1, 2 credits/sec */
	check(try_consume(&b, t), "first send ok");
	check(!try_consume(&b, t), "immediate second throttled");
	t += SEC(0.5);                /* 0.5s * 2/s = 1 credit */
	check(try_consume(&b, t), "send ok after 0.5s at 2/sec");

	if (failures == 0)
		printf("\nAll ratelimit tests PASSED.\n");
	else
		printf("\n%d ratelimit test(s) FAILED.\n", failures);

	return failures == 0 ? 0 : 1;
}

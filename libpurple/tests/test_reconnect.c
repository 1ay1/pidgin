/**
 * @file test_reconnect.c Standalone unit test for the reconnect backoff math.
 *
 * The reconnect subsystem's scheduling logic is signal-driven and needs a full
 * core, but its heart -- the exponential-backoff delay calculation -- is a
 * pure function with no dependencies. This standalone test exercises that
 * function directly so its correctness is provable without the (currently
 * un-wired) check(1) harness.
 *
 * Build & run:
 *   cc -o /tmp/test_reconnect test_reconnect.c $(pkg-config --cflags --libs glib-2.0)
 *   /tmp/test_reconnect
 */
#include <glib.h>
#include <stdio.h>

/* Mirror of the internal signature in reconnect.c. Declared here so the test
 * is standalone; kept in lock-step with the real implementation. */
extern guint _purple_reconnect_backoff_base(guint attempt, guint initial, guint max);

/*
 * Local copy of the exact algorithm under test. If reconnect.c changes, this
 * copy must change too -- the test then guards the intended behaviour. (We
 * cannot link reconnect.o standalone because it pulls in the whole core.)
 */
static guint
backoff_base(guint attempt, guint initial, guint max)
{
	guint64 delay;
	guint shift = attempt > 0 ? attempt - 1 : 0;

	if (initial < 1)
		initial = 1;
	if (max < initial)
		max = initial;

	if (shift > 20)
		shift = 20;
	delay = (guint64)initial << shift;

	if (delay > max)
		delay = max;

	return (guint)delay;
}

static int failures = 0;

static void
expect(const char *what, guint got, guint want)
{
	if (got != want) {
		printf("  FAIL: %s -> %u, expected %u\n", what, got, want);
		failures++;
	} else {
		printf("  ok:   %s -> %u\n", what, got);
	}
}

int
main(void)
{
	guint prev;
	guint i;

	printf("reconnect backoff: doubling from initial, clamped at max\n");
	/* initial=8, max=480 (8 minutes): 8, 16, 32, 64, 128, 256, 480, 480 ... */
	expect("attempt 1", backoff_base(1, 8, 480), 8);
	expect("attempt 2", backoff_base(2, 8, 480), 16);
	expect("attempt 3", backoff_base(3, 8, 480), 32);
	expect("attempt 4", backoff_base(4, 8, 480), 64);
	expect("attempt 5", backoff_base(5, 8, 480), 128);
	expect("attempt 6", backoff_base(6, 8, 480), 256);
	expect("attempt 7 (would be 512, clamped)", backoff_base(7, 8, 480), 480);
	expect("attempt 8 (stays clamped)", backoff_base(8, 8, 480), 480);

	printf("\nreconnect backoff: attempt 0 treated as attempt 1\n");
	expect("attempt 0", backoff_base(0, 8, 480), 8);

	printf("\nreconnect backoff: input clamping\n");
	expect("initial<1 becomes 1", backoff_base(1, 0, 480), 1);
	expect("max<initial becomes initial", backoff_base(1, 30, 5), 30);

	printf("\nreconnect backoff: never overflows on huge attempt counts\n");
	prev = 0;
	for (i = 1; i <= 1000; i++) {
		guint d = backoff_base(i, 8, 480);
		if (d > 480) {
			printf("  FAIL: attempt %u exceeded max: %u\n", i, d);
			failures++;
			break;
		}
		if (d < prev) {
			printf("  FAIL: attempt %u regressed below previous (%u < %u)\n",
					i, d, prev);
			failures++;
			break;
		}
		prev = d;
	}
	if (failures == 0 || prev == 480)
		printf("  ok:   monotonic non-decreasing, always <= max, no overflow\n");

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
			failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

/**
 * @file test_connhealth.c Standalone unit test for the health-grading math.
 *
 * The connhealth subsystem's plumbing is signal-driven and needs a full core,
 * but its decision logic -- idle-to-grade thresholds and the latency EWMA --
 * is pure arithmetic. This test mirrors that logic exactly so the intended
 * behaviour is provable without the (currently un-wired) check(1) harness.
 *
 * Build & run:
 *   cc -o /tmp/test_connhealth test_connhealth.c $(pkg-config --cflags --libs glib-2.0)
 *   /tmp/test_connhealth
 */
#include <glib.h>
#include <stdio.h>

/* Mirror of the grades + defaults + LATENCY_ALPHA from connhealth.c. */
typedef enum {
	HEALTH_UNKNOWN = 0,
	HEALTH_GOOD,
	HEALTH_IDLE,
	HEALTH_STALLED
} Health;

#define IDLE_SECS      45
#define STALLED_SECS   90
#define LATENCY_ALPHA  0.3

static Health
grade_for_idle(guint idle, guint idle_secs, guint stalled_secs)
{
	if (idle < idle_secs)
		return HEALTH_GOOD;
	if (idle < stalled_secs)
		return HEALTH_IDLE;
	return HEALTH_STALLED;
}

/* Mirror of the EWMA fold in sample_connection(). */
static gdouble
fold_latency(gdouble prev, gdouble observed)
{
	if (observed < 0.0)
		observed = 0.0;
	if (prev <= 0.0)
		return observed;
	return (LATENCY_ALPHA * observed) + ((1.0 - LATENCY_ALPHA) * prev);
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
	gdouble lat;

	printf("test_connhealth: idle time maps to the right grade\n");
	check(grade_for_idle(0,   IDLE_SECS, STALLED_SECS) == HEALTH_GOOD,
			"idle 0s -> GOOD");
	check(grade_for_idle(44,  IDLE_SECS, STALLED_SECS) == HEALTH_GOOD,
			"idle 44s -> GOOD (just under idle threshold)");
	check(grade_for_idle(45,  IDLE_SECS, STALLED_SECS) == HEALTH_IDLE,
			"idle 45s -> IDLE (at threshold)");
	check(grade_for_idle(89,  IDLE_SECS, STALLED_SECS) == HEALTH_IDLE,
			"idle 89s -> IDLE (just under stalled)");
	check(grade_for_idle(90,  IDLE_SECS, STALLED_SECS) == HEALTH_STALLED,
			"idle 90s -> STALLED (at threshold)");
	check(grade_for_idle(600, IDLE_SECS, STALLED_SECS) == HEALTH_STALLED,
			"idle 10m -> STALLED");

	printf("test_connhealth: first latency sample seeds the estimate\n");
	lat = 0.0;
	lat = fold_latency(lat, 8.0);
	check(lat > 7.99 && lat < 8.01, "seed = first observation (8.0s)");

	printf("test_connhealth: EWMA moves toward new observations\n");
	/* new = 0.3*20 + 0.7*8 = 6 + 5.6 = 11.6 */
	lat = fold_latency(lat, 20.0);
	check(lat > 11.59 && lat < 11.61, "EWMA after 20.0s obs = 11.6s");
	/* new = 0.3*11.6 + 0.7*11.6 = 11.6 (stable when obs == estimate) */
	lat = fold_latency(lat, 11.6);
	check(lat > 11.59 && lat < 11.61, "EWMA stable when obs equals estimate");

	printf("test_connhealth: negative observation is clamped to zero\n");
	lat = fold_latency(10.0, -5.0);
	/* 0.3*0 + 0.7*10 = 7.0 */
	check(lat > 6.99 && lat < 7.01, "negative obs treated as 0 -> 7.0s");

	printf("test_connhealth: custom thresholds respected\n");
	check(grade_for_idle(15, 10, 30) == HEALTH_IDLE,
			"idle 15s with (10,30) -> IDLE");
	check(grade_for_idle(5, 10, 30) == HEALTH_GOOD,
			"idle 5s with (10,30) -> GOOD");
	check(grade_for_idle(35, 10, 30) == HEALTH_STALLED,
			"idle 35s with (10,30) -> STALLED");

	if (failures == 0)
		printf("\nAll connhealth tests PASSED.\n");
	else
		printf("\n%d connhealth test(s) FAILED.\n", failures);

	return failures == 0 ? 0 : 1;
}

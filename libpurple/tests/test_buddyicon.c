/**
 * @file test_buddyicon.c Standalone unit test for the buddy-icon
 *       checksum short-circuit.
 *
 * purple_buddy_icon_set_data() now skips re-hashing the image and re-running
 * purple_buddy_icon_update() (which forces the buddy list and every open
 * conversation to re-render) when a protocol re-delivers a buddy's icon whose
 * checksum matches the one already held AND the image is already in memory --
 * the extremely common case on every reconnect. This test mirrors that guard
 * exactly so its correctness (and its careful boundary conditions) is provable
 * without a full core.
 *
 * Build & run:
 *   cc -o /tmp/test_buddyicon test_buddyicon.c $(pkg-config --cflags --libs glib-2.0)
 *   /tmp/test_buddyicon
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

static gboolean
strequal(const char *a, const char *b)
{
	return (a == NULL && b == NULL) ||
	       (a != NULL && b != NULL && strcmp(a, b) == 0);
}

/*
 * Mirror of the short-circuit guard in purple_buddy_icon_set_data():
 * returns TRUE when the incoming (data,len,checksum) is redundant given the
 * icon's current (have_img, cur_checksum) and the set should be skipped.
 */
static gboolean
should_skip(gboolean have_data, size_t len, gboolean have_img,
            const char *cur_checksum, const char *new_checksum)
{
	return have_data && len > 0 && have_img &&
	       new_checksum != NULL && strequal(cur_checksum, new_checksum);
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
	printf("test_buddyicon: skip when checksum matches and image present\n");
	check(should_skip(TRUE, 1024, TRUE, "abc123", "abc123"),
			"same checksum + have image -> skip");

	printf("test_buddyicon: never skip when the image isn't loaded yet\n");
	/* This is the purple_buddy_icons_find() path: fresh icon, img==NULL,
	 * checksum loaded from the blist. It MUST load, not skip. */
	check(!should_skip(TRUE, 1024, FALSE, "abc123", "abc123"),
			"no image in memory -> must not skip");

	printf("test_buddyicon: never skip when the checksum changed\n");
	check(!should_skip(TRUE, 1024, TRUE, "abc123", "def456"),
			"different checksum -> must not skip");

	printf("test_buddyicon: never skip a checksumless update\n");
	/* Some protocols don't provide a checksum; without one we can't prove
	 * the data is unchanged, so we must always go through the full path. */
	check(!should_skip(TRUE, 1024, TRUE, "abc123", NULL),
			"incoming NULL checksum -> must not skip");
	check(!should_skip(TRUE, 1024, TRUE, NULL, NULL),
			"both NULL checksum -> must not skip");

	printf("test_buddyicon: never skip an icon *removal*\n");
	/* data==NULL / len==0 means \"clear the icon\": must go through so the
	 * buddy list and conversations drop it. */
	check(!should_skip(FALSE, 0, TRUE, "abc123", "abc123"),
			"no incoming data (removal) -> must not skip");
	check(!should_skip(TRUE, 0, TRUE, "abc123", "abc123"),
			"zero-length data -> must not skip");

	printf("test_buddyicon: first-ever set is never skipped\n");
	/* Fresh icon from purple_buddy_icon_new(): checksum NULL, img NULL. */
	check(!should_skip(TRUE, 1024, FALSE, NULL, "abc123"),
			"first set (no prior checksum/img) -> must not skip");

	if (failures == 0)
		printf("\nAll buddyicon tests PASSED.\n");
	else
		printf("\n%d buddyicon test(s) FAILED.\n", failures);

	return failures == 0 ? 0 : 1;
}

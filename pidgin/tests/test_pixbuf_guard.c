/*
 * Standalone test for the image-decode safety guards added to
 * pidgin_pixbuf_from_data_helper() in pidgin/gtkutils.c.
 *
 * The guards exist to stop a hostile / corrupt / oversized buddy icon or
 * inline image from wedging the GTK main loop (on a glycin-backed gdk-pixbuf
 * the decode forks a sandbox subprocess and blocks on wait4()). This test
 * mirrors the two pure decision functions exactly:
 *
 *   1. the encoded-size gate (reject before we ever touch the loader), and
 *   2. the size-prepared dimension clamp (shrink-only, aspect-preserving).
 *
 * Build:
 *   cc -o /tmp/test_pixbuf_guard pidgin/tests/test_pixbuf_guard.c \
 *       $(pkg-config --cflags --libs glib-2.0)
 */

#include <glib.h>

#define PIDGIN_PIXBUF_MAX_ENCODED_BYTES (16 * 1024 * 1024)
#define PIDGIN_PIXBUF_MAX_DIMENSION 4096

/* Mirrors the up-front reject in pidgin_pixbuf_from_data_helper(). Returns
 * TRUE when the payload should be handed to the loader, FALSE when rejected. */
static gboolean
encoded_ok(const void *buf, gsize count)
{
	if (buf == NULL || count == 0)
		return FALSE;
	if (count > PIDGIN_PIXBUF_MAX_ENCODED_BYTES)
		return FALSE;
	return TRUE;
}

/* Mirrors pidgin_pixbuf_size_prepared_cb(): clamp shrink-only, aspect-
 * preserving, floor at 1px. Writes the resulting size through out params. */
static void
clamp_size(gint width, gint height, gint *out_w, gint *out_h)
{
	if (width <= PIDGIN_PIXBUF_MAX_DIMENSION &&
	    height <= PIDGIN_PIXBUF_MAX_DIMENSION) {
		*out_w = width;
		*out_h = height;
		return;
	}

	if (width >= height) {
		height = (gint)((gint64)height * PIDGIN_PIXBUF_MAX_DIMENSION / width);
		width = PIDGIN_PIXBUF_MAX_DIMENSION;
	} else {
		width = (gint)((gint64)width * PIDGIN_PIXBUF_MAX_DIMENSION / height);
		height = PIDGIN_PIXBUF_MAX_DIMENSION;
	}
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;

	*out_w = width;
	*out_h = height;
}

static int failures = 0;

static void
check(gboolean cond, const char *what)
{
	if (cond) {
		g_print("  ok: %s\n", what);
	} else {
		g_print("  FAIL: %s\n", what);
		failures++;
	}
}

int
main(void)
{
	char dummy[8] = {0};
	gint w, h;

	g_print("encoded-size gate:\n");
	check(!encoded_ok(NULL, 100),   "NULL buffer rejected");
	check(!encoded_ok(dummy, 0),    "zero-length rejected");
	check(encoded_ok(dummy, 8),     "tiny valid buffer accepted");
	check(encoded_ok(dummy, PIDGIN_PIXBUF_MAX_ENCODED_BYTES),
	      "exactly-at-cap accepted");
	check(!encoded_ok(dummy, PIDGIN_PIXBUF_MAX_ENCODED_BYTES + 1),
	      "one-over-cap rejected");
	check(!encoded_ok(dummy, (gsize)1 << 30),
	      "1 GiB payload rejected");

	g_print("dimension clamp (shrink-only, aspect-preserving):\n");

	/* Normal small icon: untouched. */
	clamp_size(64, 64, &w, &h);
	check(w == 64 && h == 64, "64x64 left alone");

	/* Exactly at the cap: untouched. */
	clamp_size(4096, 4096, &w, &h);
	check(w == 4096 && h == 4096, "4096x4096 left alone");

	/* Square bomb: clamped to the cap. */
	clamp_size(60000, 60000, &w, &h);
	check(w == 4096 && h == 4096, "60000x60000 -> 4096x4096");

	/* Wide bomb: width pinned, height scaled proportionally, never grows. */
	clamp_size(40000, 10000, &w, &h);
	check(w == 4096 && h == 1024, "40000x10000 -> 4096x1024 (aspect kept)");

	/* Tall bomb: height pinned, width scaled. */
	clamp_size(10000, 40000, &w, &h);
	check(w == 1024 && h == 4096, "10000x40000 -> 1024x4096 (aspect kept)");

	/* Extreme aspect ratio must never floor a dimension to 0. */
	clamp_size(1000000, 1, &w, &h);
	check(w == 4096 && h == 1, "1000000x1 -> 4096x1 (no zero dimension)");

	/* Only one side over the cap: still clamped, aspect kept. */
	clamp_size(8192, 2048, &w, &h);
	check(w == 4096 && h == 1024, "8192x2048 -> 4096x1024");

	if (failures == 0)
		g_print("\nALL PASS\n");
	else
		g_print("\n%d FAILURE(S)\n", failures);

	return failures == 0 ? 0 : 1;
}

/*
 * Standalone unit test for the IRCv3 message-tag parsing logic used by the
 * IRC prpl (mirrors irc_tag_unescape / irc_tags_parse / server-time decode in
 * parse.c).  glib-only; not linked against the prpl object.
 *
 *   cc -o /tmp/test_irc_tags libpurple/protocols/irc/tests/test_irc_tags.c \
 *        $(pkg-config --cflags --libs glib-2.0) && /tmp/test_irc_tags
 */
#include <glib.h>
#include <string.h>
#include <time.h>

/* --- copies of the pure logic under test (kept byte-identical to parse.c) --- */

static char *irc_tag_unescape(const char *value)
{
	GString *out = g_string_new(NULL);
	const char *p;

	for (p = value; *p; p++) {
		if (*p != '\\') {
			g_string_append_c(out, *p);
			continue;
		}
		p++;
		switch (*p) {
		case ':': g_string_append_c(out, ';'); break;
		case 's': g_string_append_c(out, ' '); break;
		case 'r': g_string_append_c(out, '\r'); break;
		case 'n': g_string_append_c(out, '\n'); break;
		case '\\': g_string_append_c(out, '\\'); break;
		case '\0': return g_string_free(out, FALSE);
		default: g_string_append_c(out, *p); break;
		}
	}
	return g_string_free(out, FALSE);
}

static GHashTable *irc_tags_parse(const char *blob)
{
	GHashTable *tags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	gchar **pairs, **it;

	pairs = g_strsplit(blob, ";", -1);
	for (it = pairs; it && *it; it++) {
		char *eq;
		if (**it == '\0')
			continue;
		eq = strchr(*it, '=');
		if (eq == NULL)
			g_hash_table_insert(tags, g_strdup(*it), g_strdup(""));
		else {
			*eq = '\0';
			g_hash_table_insert(tags, g_strdup(*it), irc_tag_unescape(eq + 1));
		}
	}
	g_strfreev(pairs);
	return tags;
}

static time_t irc_tag_time(GHashTable *tags)
{
	const char *st = g_hash_table_lookup(tags, "time");
	GDateTime *dt;
	time_t result;

	if (st == NULL || *st == '\0')
		return (time_t)-1;
	dt = g_date_time_new_from_iso8601(st, NULL);
	if (dt == NULL)
		return (time_t)-1;
	result = (time_t)g_date_time_to_unix(dt);
	g_date_time_unref(dt);
	return result;
}

/* --- tests --- */

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (cond) { g_print("  ok   %s\n", msg); } \
	else { g_print("  FAIL %s\n", msg); failures++; } \
} while (0)

static void test_unescape(void)
{
	char *r;
	g_print("unescape:\n");

	r = irc_tag_unescape("hello\\sworld");
	CHECK(g_strcmp0(r, "hello world") == 0, "\\s -> space"); g_free(r);

	r = irc_tag_unescape("a\\:b");
	CHECK(g_strcmp0(r, "a;b") == 0, "\\: -> semicolon"); g_free(r);

	r = irc_tag_unescape("c\\\\d");
	CHECK(g_strcmp0(r, "c\\d") == 0, "\\\\ -> backslash"); g_free(r);

	r = irc_tag_unescape("plain");
	CHECK(g_strcmp0(r, "plain") == 0, "no escapes passthrough"); g_free(r);

	r = irc_tag_unescape("trailing\\");
	CHECK(g_strcmp0(r, "trailing") == 0, "lone trailing backslash dropped"); g_free(r);

	r = irc_tag_unescape("\\a");
	CHECK(g_strcmp0(r, "a") == 0, "unknown escape keeps char"); g_free(r);
}

static void test_parse(void)
{
	GHashTable *t;
	g_print("parse:\n");

	t = irc_tags_parse("time=2021-01-02T15:04:05.000Z;account=alice;msgid=abc");
	CHECK(g_strcmp0(g_hash_table_lookup(t, "account"), "alice") == 0, "account value");
	CHECK(g_strcmp0(g_hash_table_lookup(t, "msgid"), "abc") == 0, "msgid value");
	CHECK(g_hash_table_lookup(t, "time") != NULL, "time present");
	g_hash_table_destroy(t);

	t = irc_tags_parse("draft/label;key=val");
	CHECK(g_strcmp0(g_hash_table_lookup(t, "draft/label"), "") == 0, "valueless tag -> empty");
	CHECK(g_strcmp0(g_hash_table_lookup(t, "key"), "val") == 0, "mixed valued tag");
	g_hash_table_destroy(t);

	t = irc_tags_parse("k=a\\sb\\sc");
	CHECK(g_strcmp0(g_hash_table_lookup(t, "k"), "a b c") == 0, "escaped value decoded on parse");
	g_hash_table_destroy(t);
}

static void test_time(void)
{
	GHashTable *t;
	time_t got;
	g_print("server-time:\n");

	/* 2021-01-02T15:04:05Z == 1609599845 UTC */
	t = irc_tags_parse("time=2021-01-02T15:04:05.000Z");
	got = irc_tag_time(t);
	CHECK(got == (time_t)1609599845, "ISO8601 -> correct unix time");
	g_hash_table_destroy(t);

	t = irc_tags_parse("account=bob");
	CHECK(irc_tag_time(t) == (time_t)-1, "no time tag -> sentinel");
	g_hash_table_destroy(t);

	t = irc_tags_parse("time=garbage");
	CHECK(irc_tag_time(t) == (time_t)-1, "unparseable time -> sentinel");
	g_hash_table_destroy(t);
}

int main(void)
{
	test_unescape();
	test_parse();
	test_time();
	g_print("\n%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
	return failures == 0 ? 0 : 1;
}

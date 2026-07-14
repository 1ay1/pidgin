/**
 * @file test_account_setting.c Standalone unit test for account-setting
 *       change detection.
 *
 * purple_account_set_int/string/bool now skip the write entirely (no realloc,
 * no UI notify, no accounts.xml flush, no signal) when the setting already
 * holds the value being written -- an extremely common case on every login,
 * where a prpl re-applies its stored options. The decision is made by the
 * pure helper setting_unchanged(). This test mirrors that helper exactly
 * (over a real GHashTable of the same PurpleAccountSetting shape) so its
 * correctness is provable without a full core.
 *
 * Build & run:
 *   cc -o /tmp/test_account_setting test_account_setting.c $(pkg-config --cflags --libs glib-2.0)
 *   /tmp/test_account_setting
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

/* Mirror of PurplePrefType's relevant members + the setting struct. */
typedef enum {
	PREF_NONE = 0,
	PREF_BOOLEAN,
	PREF_INT,
	PREF_STRING
} PrefType;

typedef struct {
	PrefType type;
	char *ui;
	union {
		int integer;
		char *string;
		gboolean boolean;
	} value;
} Setting;

static gboolean
strequal(const char *a, const char *b)
{
	return (a == NULL && b == NULL) ||
	       (a != NULL && b != NULL && strcmp(a, b) == 0);
}

/* Exact mirror of setting_unchanged() from account.c. */
static gboolean
setting_unchanged(GHashTable *table, const char *name, PrefType type,
                  gconstpointer value)
{
	Setting *setting = g_hash_table_lookup(table, name);

	if (setting == NULL || setting->type != type)
		return FALSE;

	switch (type) {
		case PREF_INT:     return setting->value.integer == *(const int *)value;
		case PREF_BOOLEAN: return setting->value.boolean == *(const gboolean *)value;
		case PREF_STRING:  return strequal(setting->value.string, (const char *)value);
		default:           return FALSE;
	}
}

static void
put_int(GHashTable *t, const char *name, int v)
{
	Setting *s = g_new0(Setting, 1);
	s->type = PREF_INT;
	s->value.integer = v;
	g_hash_table_insert(t, g_strdup(name), s);
}

static void
put_string(GHashTable *t, const char *name, const char *v)
{
	Setting *s = g_new0(Setting, 1);
	s->type = PREF_STRING;
	s->value.string = g_strdup(v);
	g_hash_table_insert(t, g_strdup(name), s);
}

static void
put_bool(GHashTable *t, const char *name, gboolean v)
{
	Setting *s = g_new0(Setting, 1);
	s->type = PREF_BOOLEAN;
	s->value.boolean = v;
	g_hash_table_insert(t, g_strdup(name), s);
}

static void
free_setting(void *data)
{
	Setting *s = data;
	if (s->type == PREF_STRING)
		g_free(s->value.string);
	g_free(s->ui);
	g_free(s);
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
	GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, free_setting);
	int iv;
	gboolean bv;

	printf("test_account_setting: absent setting always counts as changed\n");
	iv = 5;
	check(!setting_unchanged(t, "port", PREF_INT, &iv),
			"unset int -> changed");

	printf("test_account_setting: identical value is unchanged\n");
	put_int(t, "port", 5194);
	iv = 5194;
	check(setting_unchanged(t, "port", PREF_INT, &iv), "same int -> unchanged");
	iv = 6667;
	check(!setting_unchanged(t, "port", PREF_INT, &iv),
			"different int -> changed");

	printf("test_account_setting: string equality (incl. NULL)\n");
	put_string(t, "server", "irc.libera.chat");
	check(setting_unchanged(t, "server", PREF_STRING, "irc.libera.chat"),
			"same string -> unchanged");
	check(!setting_unchanged(t, "server", PREF_STRING, "irc.oftc.net"),
			"different string -> changed");
	put_string(t, "nick", NULL);
	check(setting_unchanged(t, "nick", PREF_STRING, NULL),
			"NULL == NULL -> unchanged");
	check(!setting_unchanged(t, "nick", PREF_STRING, "ayush"),
			"NULL vs value -> changed");

	printf("test_account_setting: bool equality\n");
	put_bool(t, "ssl", TRUE);
	bv = TRUE;
	check(setting_unchanged(t, "ssl", PREF_BOOLEAN, &bv), "same bool -> unchanged");
	bv = FALSE;
	check(!setting_unchanged(t, "ssl", PREF_BOOLEAN, &bv), "different bool -> changed");

	printf("test_account_setting: type mismatch counts as changed\n");
	/* 'port' is stored as INT; asking as STRING/BOOL must not claim unchanged. */
	check(!setting_unchanged(t, "port", PREF_STRING, "5194"),
			"int stored, string queried -> changed (no false-skip)");
	bv = TRUE;
	check(!setting_unchanged(t, "port", PREF_BOOLEAN, &bv),
			"int stored, bool queried -> changed");

	g_hash_table_destroy(t);

	if (failures == 0)
		printf("\nAll account-setting tests PASSED.\n");
	else
		printf("\n%d account-setting test(s) FAILED.\n", failures);

	return failures == 0 ? 0 : 1;
}

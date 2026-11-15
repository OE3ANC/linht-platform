// SPDX-License-Identifier: GPL-3.0-only
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../tests/check.h"
#include "store.h"

static char dir[] = "/tmp/test-store-XXXXXX";
static char path[600];

static void write_file(const char *p, const char *text)
{
	FILE *f = fopen(p, "w");
	fputs(text, f);
	fclose(f);
}

static char *read_file(const char *p)
{
	static char buf[4096];
	FILE *f = fopen(p, "r");
	if (!f)
		return NULL;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

static struct lschema *schema(void)
{
	char sp[600], err[256];
	snprintf(sp, sizeof(sp), "%s/schema.yaml", dir);
	write_file(sp, "namespace: t\nversion: 1\ntitle: T\nkeys:\n"
	               "  n: {type: int, default: 5, min: 1, max: 10, label: N, order: 1}\n"
	               "  b: {type: bool, default: false, label: B, order: 2}\n"
	               "  f: {type: float, default: 0.5, label: F, order: 3}\n"
	               "  s: {type: string, default: x, label: S, order: 4}\n");
	struct lschema *s = lschema_load(sp, err, sizeof(err));
	if (!s)
		fprintf(stderr, "schema: %s\n", err);
	return s;
}
enum { N, B, F, S };

static int set_int(struct lstore *st, int64_t v, bool *changed)
{
	char err[256];
	struct lupdate u = {.key = N, .value = {.type = LT_INT, .i = v}};
	return lstore_commit(st, &u, 1, changed, err, sizeof(err));
}

static void test_defaults_without_file(const struct lschema *s)
{
	struct lstore *st = lstore_open(s, dir);
	CHECK(st && lstore_get(st, N)->i == 5 && lstore_get(st, S)->type == LT_STRING);
	CHECK(st && strcmp(lstore_get(st, S)->s, "x") == 0);
	lstore_free(st);
}

static void test_set_persists_and_reports_changes(const struct lschema *s)
{
	bool changed[4] = {0};
	struct lstore *st = lstore_open(s, dir);
	CHECK(set_int(st, 7, changed) == 0);
	CHECK(lstore_get(st, N)->i == 7);
	CHECK(changed[N] && !changed[B] && !changed[F] && !changed[S]);
	lstore_free(st);

	st = lstore_open(s, dir); // reload from disk
	CHECK(lstore_get(st, N)->i == 7);

	memset(changed, 0, sizeof(changed));
	CHECK(set_int(st, 7, changed) == 0); // same value again: nothing changes
	CHECK(!changed[N]);

	memset(changed, 0, sizeof(changed));
	CHECK(set_int(st, 5, changed) == 0); // back to the default: removed from the file
	CHECK(changed[N] && lstore_get(st, N)->i == 5);
	CHECK(read_file(path) && !strstr(read_file(path), "n:"));
	lstore_free(st);
}

static void test_commit_is_all_or_nothing(const struct lschema *s)
{
	bool changed[4] = {0};
	char err[256] = "";
	struct lstore *st = lstore_open(s, dir);
	struct lupdate u[] = {
		{.key = B, .value = {.type = LT_BOOL, .b = true}},
		{.key = N, .value = {.type = LT_INT, .i = 11}}, // above max
	};
	CHECK(lstore_commit(st, u, 2, changed, err, sizeof(err)) == -EINVAL);
	CHECK(strstr(err, "n") && strstr(err, "max")); // names the key and the rule
	CHECK(lstore_get(st, B)->b == false && !changed[B]);
	lstore_free(st);
	st = lstore_open(s, dir);
	CHECK(lstore_get(st, B)->b == false); // nothing was written
	lstore_free(st);
}

static void test_reset(const struct lschema *s)
{
	bool changed[4] = {0};
	char err[256];
	struct lstore *st = lstore_open(s, dir);
	CHECK(set_int(st, 9, changed) == 0);
	memset(changed, 0, sizeof(changed));
	struct lupdate u = {.key = N, .reset = true};
	CHECK(lstore_commit(st, &u, 1, changed, err, sizeof(err)) == 0);
	CHECK(changed[N] && lstore_get(st, N)->i == 5);
	lstore_free(st);
	st = lstore_open(s, dir);
	CHECK(lstore_get(st, N)->i == 5);
	lstore_free(st);
}

static void test_values_round_trip(const struct lschema *s)
{
	bool changed[4];
	char err[256];
	// U+0080 and U+FFFE are valid UTF-8 but not allowed raw in a YAML file: they must be escaped,
	// or the next start can't parse the file at all. U+0085, U+2028 and U+2029 are allowed raw, but
	// a YAML parser folds them as line breaks.
	const char *tricky = "quote \" back \\ newline \n tab \t ctrl \x01 umlaut \xc3\xa4 # not a comment: x"
	                     " c1 \xc2\x80 nonchar \xef\xbf\xbe nel \xc2\x85 ls \xe2\x80\xa8 ps \xe2\x80\xa9 end";
	struct lstore *st = lstore_open(s, dir);
	struct lupdate u[] = {
		{.key = S, .value = {.type = LT_STRING, .s = (char *)tricky}},
		{.key = F, .value = {.type = LT_FLOAT, .d = 0.1}},
		{.key = B, .value = {.type = LT_BOOL, .b = true}},
	};
	CHECK(lstore_commit(st, u, 3, changed, err, sizeof(err)) == 0);
	lstore_free(st);
	st = lstore_open(s, dir);
	CHECK(strcmp(lstore_get(st, S)->s, tricky) == 0);
	CHECK(lstore_get(st, F)->d == 0.1);
	CHECK(lstore_get(st, B)->b == true);
	struct stat sb;
	CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0644);
	lstore_free(st);
}

static void test_forgiving_load(const struct lschema *s)
{
	write_file(path, "n: 11\nb: maybe\nunknown: 1\ns: kept\nf: [1]\n");
	struct lstore *st = lstore_open(s, dir);
	CHECK(st != NULL);
	CHECK(lstore_get(st, N)->i == 5);              // out of range: default
	CHECK(lstore_get(st, B)->b == false);          // not a bool: default
	CHECK(lstore_get(st, F)->d == 0.5);            // not a scalar: default
	CHECK(strcmp(lstore_get(st, S)->s, "kept") == 0); // valid entries survive
	lstore_free(st);

	char broken[700];
	snprintf(broken, sizeof(broken), "%s.broken", path);
	write_file(path, "n: [unclosed\n");
	st = lstore_open(s, dir);
	CHECK(st && lstore_get(st, N)->i == 5);
	CHECK(access(broken, F_OK) == 0); // the unreadable file is kept for inspection
	CHECK(access(path, F_OK) != 0);
	lstore_free(st);
	unlink(broken);
}

static void test_unopenable_file_is_kept(const struct lschema *s)
{
	if (geteuid() == 0) {
		fprintf(stderr, "skipping unopenable-file test: root ignores file permissions\n");
		return;
	}
	char broken[700];
	snprintf(broken, sizeof(broken), "%s.broken", path);
	write_file(path, "n: 8\nb: true\n");
	chmod(path, 0);
	struct lstore *st = lstore_open(s, dir);
	CHECK(st && lstore_get(st, N)->i == 5);
	CHECK(access(broken, F_OK) == 0); // moved aside, so the next write cannot replace the stored values
	CHECK(access(path, F_OK) != 0);
	lstore_free(st);
	unlink(broken);
}

static void test_write_failure_keeps_old_values(const struct lschema *s)
{
	if (geteuid() == 0) {
		fprintf(stderr, "skipping write-failure test: root ignores directory permissions\n");
		return;
	}
	bool changed[4] = {0};
	char err[256] = "";
	struct lstore *st = lstore_open(s, dir);
	CHECK(set_int(st, 3, changed) == 0);
	chmod(dir, 0555);
	memset(changed, 0, sizeof(changed));
	struct lupdate u = {.key = N, .value = {.type = LT_INT, .i = 4}};
	CHECK(lstore_commit(st, &u, 1, changed, err, sizeof(err)) == -EIO);
	CHECK(lstore_get(st, N)->i == 3 && !changed[N]);
	chmod(dir, 0755);
	lstore_free(st);
}

int main(void)
{
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(path, sizeof(path), "%s/t.yaml", dir);
	struct lschema *s = schema();
	CHECK(s != NULL);
	if (s) {
		test_defaults_without_file(s);
		test_set_persists_and_reports_changes(s);
		test_commit_is_all_or_nothing(s);
		test_reset(s);
		test_values_round_trip(s);
		test_forgiving_load(s);
		test_unopenable_file_is_kept(s);
		test_write_failure_keeps_old_values(s);
		lschema_free(s);
	}
	char cmd[700];
	snprintf(cmd, sizeof(cmd), "chmod -R u+w %s && rm -rf %s", dir, dir);
	if (system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", dir);
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

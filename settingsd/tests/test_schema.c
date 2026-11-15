// SPDX-License-Identifier: GPL-3.0-only
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../tests/check.h"
#include "schema.h"

static char tmpdir[] = "/tmp/test-schema-XXXXXX";

// Writes a schema file and loads it; err receives the loader's message.
static struct lschema *load_text(const char *text, char *err, size_t errlen)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/test.yaml", tmpdir);
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
	err[0] = 0;
	return lschema_load(path, err, errlen);
}

#define HEAD "namespace: test\nversion: 1\ntitle: Test\nkeys:\n"

static void test_real_display_schema(void)
{
	char err[256];
	struct lschema *s = lschema_load(SCHEMA_DIR "/system.display.yaml", err, sizeof(err));
	CHECK(s != NULL);
	if (!s)
		return;
	CHECK(strcmp(s->ns, "system.display") == 0);
	CHECK(s->version == 1);
	CHECK(strcmp(s->title, "Display") == 0);
	CHECK(s->n_keys == 2);
	CHECK(strcmp(s->keys[0].name, "brightness") == 0); // order 1 before order 2
	CHECK(strcmp(s->keys[1].name, "timeout") == 0);
	const struct lkey *b = lschema_key(s, "brightness");
	CHECK(b && b->type == LT_INT && b->def.i == 255 && b->has_min && b->min.i == 32);
	CHECK(b && b->has_max && b->max.i == 255 && b->access == LA_USER && strcmp(b->apply, "live") == 0);
	const struct lkey *t = lschema_key(s, "timeout");
	CHECK(t && t->n_choices == 6 && t->choices[0].i == 0 && t->choices[5].i == 60);
	CHECK(t && t->unit && strcmp(t->unit, "s") == 0);
	CHECK(lschema_key(s, "nope") == NULL);
	lschema_free(s);
}

static void test_device_only_access(void)
{
	char err[256];
	struct lschema *s = lschema_load(SCHEMA_DIR "/system.developer.yaml", err, sizeof(err));
	CHECK(s != NULL);
	if (!s)
		return;
	const struct lkey *k = lschema_key(s, "enabled");
	CHECK(k && k->type == LT_BOOL && k->def.b == false && k->access == LA_DEVICE_ONLY);
	lschema_free(s);
}

static void test_keys_without_order_sort_last_by_name(void)
{
	char err[256];
	struct lschema *s = load_text(HEAD
	                              "  zeta: {type: bool, default: true, label: Z}\n"
	                              "  alpha: {type: bool, default: true, label: A}\n"
	                              "  last: {type: bool, default: true, label: L, order: 5}\n",
	                              err, sizeof(err));
	CHECK(s != NULL);
	if (!s)
		return;
	CHECK(strcmp(s->keys[0].name, "last") == 0);
	CHECK(strcmp(s->keys[1].name, "alpha") == 0);
	CHECK(strcmp(s->keys[2].name, "zeta") == 0);
	lschema_free(s);
}

// Every rejected schema must fail to load and name the reason.
static void expect_reject(const char *keys, const char *reason)
{
	char text[2048], err[256];
	snprintf(text, sizeof(text), HEAD "%s", keys);
	struct lschema *s = load_text(text, err, sizeof(err));
	CHECK(s == NULL);
	if (s) {
		fprintf(stderr, "  accepted: %s", keys);
		lschema_free(s);
	} else if (!strstr(err, reason)) {
		fprintf(stderr, "  error \"%s\" does not mention \"%s\"\n", err, reason);
		failures++;
	}
}

static void test_rejects_bad_schemas(void)
{
	expect_reject("  a: {type: int, default: 1, lable: A}\n", "lable");             // typo in field name
	expect_reject("  a: {type: integer, default: 1, label: A}\n", "integer");       // unknown type
	expect_reject("  a: {default: 1, label: A}\n", "type");                         // missing type
	expect_reject("  a: {type: int, label: A}\n", "default");                       // missing default
	expect_reject("  a: {type: int, default: 1}\n", "label");                       // missing label
	expect_reject("  a: {type: int, default: 5, min: 10, label: A}\n", "default");  // default below min
	expect_reject("  a: {type: int, default: 7, choices: [1, 2], label: A}\n", "default");
	expect_reject("  a: {type: int, default: x, label: A}\n", "default");           // not an int
	expect_reject("  a: {type: bool, default: true, min: 0, label: A}\n", "min");   // min on a bool
	expect_reject("  a: {type: int, default: 1, access: root, label: A}\n", "access");
	expect_reject("  a: {type: int, default: 1, apply: never, label: A}\n", "apply");
	expect_reject("  Bad-Name: {type: int, default: 1, label: A}\n", "Bad-Name");   // key name pattern
	expect_reject("  a: {type: int, type: bool, default: 1, label: A}\n", "duplicate"); // libyaml allows these
	expect_reject("  a: {type: int, default: 1, label: A}\n  a: {type: int, default: 2, label: B}\n", "duplicate");

	char err[256];
	struct lschema *s = load_text("namespace: Bad.NS\nversion: 1\ntitle: T\nkeys: {}\n", err, sizeof(err));
	CHECK(s == NULL && strstr(err, "namespace"));
	lschema_free(s);
	s = load_text("namespace: test\nversion: 1\ntitle: T\nkeys: {}\nextra: 1\n", err, sizeof(err));
	CHECK(s == NULL && strstr(err, "extra"));
	lschema_free(s);
	s = load_text("namespace: test\nversion: 1\n", err, sizeof(err)); // no title, no keys
	CHECK(s == NULL);
	lschema_free(s);
	s = load_text("namespace: test\nversion: 0\ntitle: T\nkeys: {}\n", err, sizeof(err));
	CHECK(s == NULL && strstr(err, "version"));
	lschema_free(s);
	// "\0" in a double-quoted scalar would silently truncate the string in C
	expect_reject("  a: {type: string, default: \"a\\0b\", label: A}\n", "default");
}

static void test_value_parse(void)
{
	struct lvalue v;
	CHECK(lvalue_parse(LT_INT, "12", &v) && v.type == LT_INT && v.i == 12);
	CHECK(lvalue_parse(LT_INT, "-3", &v) && v.i == -3);
	CHECK(!lvalue_parse(LT_INT, "12.5", &v));
	CHECK(!lvalue_parse(LT_INT, "", &v));
	CHECK(!lvalue_parse(LT_INT, "99999999999999999999", &v)); // overflow
	CHECK(lvalue_parse(LT_FLOAT, "1e3", &v) && v.type == LT_FLOAT && v.d == 1000.0);
	CHECK(!lvalue_parse(LT_FLOAT, "nan", &v));
	CHECK(!lvalue_parse(LT_FLOAT, "inf", &v));
	CHECK(lvalue_parse(LT_BOOL, "true", &v) && v.b == true);
	CHECK(lvalue_parse(LT_BOOL, "false", &v) && v.b == false);
	CHECK(!lvalue_parse(LT_BOOL, "yes", &v));
	CHECK(lvalue_parse(LT_STRING, "hello", &v) && v.type == LT_STRING && strcmp(v.s, "hello") == 0);
	lvalue_clear(&v);
}

static void test_value_check(void)
{
	char err[256];
	struct lschema *s = load_text(HEAD
	                              "  n: {type: int, default: 5, min: 1, max: 10, label: N}\n"
	                              "  c: {type: string, default: a, choices: [a, b], label: C}\n"
	                              "  f: {type: float, default: 0.5, label: F}\n"
	                              "  s: {type: string, default: x, label: S}\n",
	                              err, sizeof(err));
	CHECK(s != NULL);
	if (!s)
		return;
	const struct lkey *n = lschema_key(s, "n"), *c = lschema_key(s, "c");
	const struct lkey *f = lschema_key(s, "f"), *str = lschema_key(s, "s");
	struct lvalue v = {.type = LT_INT, .i = 1};
	CHECK(lkey_check(n, &v) == NULL); // min is inclusive
	v.i = 10;
	CHECK(lkey_check(n, &v) == NULL); // max is inclusive
	v.i = 0;
	CHECK(lkey_check(n, &v) && strstr(lkey_check(n, &v), "min"));
	v.i = 11;
	CHECK(lkey_check(n, &v) && strstr(lkey_check(n, &v), "max"));
	struct lvalue wrong = {.type = LT_BOOL, .b = true};
	CHECK(lkey_check(n, &wrong) && strstr(lkey_check(n, &wrong), "type"));
	struct lvalue fv = {.type = LT_FLOAT, .d = NAN};
	CHECK(lkey_check(f, &fv) != NULL);

	struct lvalue sv = {.type = LT_STRING, .s = "b"};
	CHECK(lkey_check(c, &sv) == NULL);
	sv.s = "z";
	CHECK(lkey_check(c, &sv) && strstr(lkey_check(c, &sv), "choices"));
	sv.s = "\xff\xfe";
	CHECK(lkey_check(str, &sv) && strstr(lkey_check(str, &sv), "UTF-8"));
	char *big = malloc(LSTRING_MAX + 2);
	memset(big, 'a', LSTRING_MAX + 1);
	big[LSTRING_MAX + 1] = 0;
	sv.s = big;
	CHECK(lkey_check(str, &sv) && strstr(lkey_check(str, &sv), "long"));
	big[LSTRING_MAX] = 0;
	CHECK(lkey_check(str, &sv) == NULL); // exactly the limit is fine
	free(big);
	lschema_free(s);
}

int main(void)
{
	if (!mkdtemp(tmpdir)) {
		perror("mkdtemp");
		return 2;
	}
	test_real_display_schema();
	test_device_only_access();
	test_keys_without_order_sort_last_by_name();
	test_rejects_bad_schemas();
	test_value_parse();
	test_value_check();
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	if (system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", tmpdir);
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

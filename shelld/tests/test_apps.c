// SPDX-License-Identifier: GPL-3.0-only
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../tests/check.h"
#include "apps.h"

static char root[] = "/tmp/test-apps-XXXXXX";

// Creates <base>/<dir>/manifest.yaml and returns the app directory.
static const char *write_app_in(const char *base, const char *dir, const char *manifest)
{
	static char appdir[600];
	char path[700];
	snprintf(appdir, sizeof(appdir), "%s/%s", base, dir);
	mkdir(appdir, 0755);
	snprintf(path, sizeof(path), "%s/manifest.yaml", appdir);
	FILE *f = fopen(path, "w");
	fputs(manifest, f);
	fclose(f);
	return appdir;
}

static const char *write_app(const char *dir, const char *manifest) { return write_app_in(root, dir, manifest); }

static void expect_reject(const char *dir, const char *manifest, const char *reason)
{
	char err[256] = "";
	struct app *a = app_load(write_app(dir, manifest), err, sizeof(err));
	CHECK(a == NULL);
	if (a) {
		fprintf(stderr, "  accepted:\n%s", manifest);
		app_free(a);
	} else if (!strstr(err, reason)) {
		fprintf(stderr, "  error \"%s\" does not mention \"%s\"\n", err, reason);
		failures++;
	}
}

static void test_exec_app(void)
{
	char err[256], want[700];
	const char *dir = write_app("org.example.hello",
	                            "id: org.example.hello\nname: Hello\nversion: 1.0.0\napi: 1\ncategory: tools\n"
	                            "icons: {small: icons/16.png, large: icons/32.png}\n"
	                            "permissions: [notifications]\n"
	                            "components:\n  - {id: beacon, type: service, exec: bin/beacon}\n"
	                            "  - {id: ui, type: ui, exec: bin/hello}\n");
	struct app *a = app_load(dir, err, sizeof(err));
	CHECK(a != NULL);
	if (!a) {
		fprintf(stderr, "  %s\n", err);
		return;
	}
	CHECK(strcmp(a->id, "org.example.hello") == 0 && strcmp(a->name, "Hello") == 0);
	CHECK(strcmp(a->category, "tools") == 0);
	snprintf(want, sizeof(want), "%s/bin/hello", dir); // the ui component, not the first component
	CHECK(a->exec && strcmp(a->exec, want) == 0 && a->unit == NULL);
	snprintf(want, sizeof(want), "%s/icons/32.png", dir);
	CHECK(a->icon_large && strcmp(a->icon_large, want) == 0);
	app_free(a);
}

static void test_unit_app(void)
{
	char err[256];
	struct app *a = app_load(write_app("org.example.radio", "id: org.example.radio\nname: Radio\n"
	                                                         "components:\n  - {type: ui, unit: linht-gui-test.service}\n"),
	                         err, sizeof(err));
	CHECK(a && a->unit && strcmp(a->unit, "linht-gui-test.service") == 0 && a->exec == NULL);
	CHECK(a && strcmp(a->category, "") == 0 && a->icon_small == NULL);
	app_free(a);
}

static void test_rejects_bad_manifests(void)
{
	expect_reject("org.example.a", "id: org.example.b\nname: A\ncomponents: [{type: ui, exec: bin/a}]\n", "id");
	expect_reject("Bad.Id", "id: Bad.Id\nname: A\ncomponents: [{type: ui, exec: bin/a}]\n", "id");
	expect_reject("single", "id: single\nname: A\ncomponents: [{type: ui, exec: bin/a}]\n", "id"); // not reverse-DNS
	expect_reject("org.example.c", "id: org.example.c\ncomponents: [{type: ui, exec: bin/a}]\n", "name");
	expect_reject("org.example.d", "id: org.example.d\nname: D\nnmae: typo\ncomponents: [{type: ui, exec: bin/a}]\n",
	              "nmae");
	expect_reject("org.example.e", "id: org.example.e\nname: E\ncomponents: [{type: ui, exec: ../../bin/sh}]\n", "exec");
	expect_reject("org.example.f", "id: org.example.f\nname: F\ncomponents: [{type: ui, exec: /bin/sh}]\n", "exec");
	expect_reject("org.example.g", "id: org.example.g\nname: G\ncomponents: [{type: ui, exec: bin/g, unit: g.service}]\n",
	              "exactly one");
	expect_reject("org.example.h", "id: org.example.h\nname: H\ncomponents: [{type: ui}]\n", "exactly one");
	expect_reject("org.example.i", "id: org.example.i\nname: I\ncomponents: [{type: ui, unit: \"x; rm -rf /\"}]\n", "unit");
	expect_reject("org.example.j", "id: org.example.j\nname: J\ncomponents: [{type: service, exec: bin/j}]\n", "ui");
	expect_reject("org.example.k", "id: org.example.k\nname: K\nicons: {small: ../secret}\ncomponents: [{type: ui, exec: bin/k}]\n",
	              "icons");
}

static void test_load_dir_skips_bad_and_sorts_by_name(void)
{
	char dir[600];
	snprintf(dir, sizeof(dir), "%s/list", root);
	mkdir(dir, 0755);
	write_app_in(dir, "org.example.zebra", "id: org.example.zebra\nname: Zebra\ncomponents: [{type: ui, exec: bin/z}]\n");
	write_app_in(dir, "org.example.alpha", "id: org.example.alpha\nname: Alpha\ncomponents: [{type: ui, exec: bin/a}]\n");
	write_app_in(dir, "org.example.broken", "id: [\n");

	struct app_list l = {0};
	CHECK(apps_load(dir, &l) == 0);
	CHECK(l.n == 2);
	if (l.n == 2) {
		CHECK(strcmp(l.apps[0].name, "Alpha") == 0 && strcmp(l.apps[1].name, "Zebra") == 0);
		CHECK(apps_find(&l, "org.example.zebra") == &l.apps[1]);
	}
	CHECK(apps_find(&l, "org.example.broken") == NULL);
	apps_free(&l);
}

int main(void)
{
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 2;
	}
	test_exec_app();
	test_unit_app();
	test_rejects_bad_manifests();
	test_load_dir_skips_bad_and_sorts_by_name();
	char cmd[700];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	if (system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", root);
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

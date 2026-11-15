// SPDX-License-Identifier: GPL-3.0-only
// App manifests (architecture §9.2): <apps dir>/<id>/manifest.yaml. Milestone 1 reads id, name,
// category, icons and the first component of type "ui".
#pragma once
#include <stddef.h>

struct app {
	char *id, *name;
	char *category;               // "" if not given
	char *icon_small, *icon_large; // absolute paths, or NULL
	char *exec;                   // absolute path of the UI program, or NULL when unit is set
	char *unit;                   // existing systemd unit that runs the app (e.g. linht-gui-test.service), or NULL
};

struct app_list {
	struct app *apps; // sorted by name
	size_t n;
};

// Loads <appdir>/manifest.yaml. The directory name must equal the manifest id.
// On error returns NULL and writes a message to err.
struct app *app_load(const char *appdir, char *err, size_t errlen);
void app_free(struct app *a);

// Loads every app below dir; bad manifests are skipped with a message on stderr.
int apps_load(const char *dir, struct app_list *out);
void apps_free(struct app_list *l);
const struct app *apps_find(const struct app_list *l, const char *id);

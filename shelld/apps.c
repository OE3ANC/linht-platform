// SPDX-License-Identifier: GPL-3.0-only
#include "apps.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "yaml-util.h"

#define N(a) (sizeof(a) / sizeof(*(a)))

// Fields of architecture §9.2; milestone 1 uses a subset, but accepts all of them.
static const char *const TOP[] = {"id",       "name",     "version",     "api",   "arch",       "license",    "maintainer",
                                  "description", "icons", "category",    "permissions", "components", "settings"};
static const char *const COMPONENT[] = {"id", "type", "exec", "unit", "autostart", "file"};

struct ctx {
	yaml_document_t *doc;
	char *err;
	size_t errlen;
};

static void fail(struct ctx *c, const char *fmt, ...)
{
	if (c->err[0])
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->err, c->errlen, fmt, ap);
	va_end(ap);
}

static yaml_node_t *node(struct ctx *c, int id) { return yaml_document_get_node(c->doc, id); }

static bool in(const char *s, const char *const *list, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(s, list[i]) == 0)
			return true;
	return false;
}

// Looks up key in mapping m. Unknown or repeated keys are an error, so typos never pass silently.
static yaml_node_t *field(struct ctx *c, yaml_node_t *m, const char *key, const char *const *allowed, size_t n,
                          const char *where)
{
	yaml_node_t *found = NULL;
	for (yaml_node_pair_t *p = m->data.mapping.pairs.start; p < m->data.mapping.pairs.top; p++) {
		const char *k = yaml_text(node(c, p->key));
		if (!k || !in(k, allowed, n)) {
			fail(c, "%s: unknown field '%s'", where, k ? k : "?");
			return NULL;
		}
		if (strcmp(k, key) == 0) {
			if (found) {
				fail(c, "%s: duplicate field '%s'", where, k);
				return NULL;
			}
			found = node(c, p->value);
		}
	}
	return found;
}

// reverse-DNS: at least two [a-z0-9_-]+ segments joined by '.'
static bool id_ok(const char *s)
{
	int segments = 1;
	if (!*s || *s == '.')
		return false;
	for (const char *p = s; *p; p++) {
		if (*p == '.') {
			if (p[1] == '.' || !p[1])
				return false;
			segments++;
		} else if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
			return false;
		}
	}
	return segments >= 2;
}

// A path inside the app directory: relative, without ".." components.
static bool inside(const char *p)
{
	if (!*p || *p == '/')
		return false;
	for (const char *s = p; *s;) {
		size_t len = strcspn(s, "/");
		if (len == 2 && s[0] == '.' && s[1] == '.')
			return false;
		s += len;
		s += *s == '/';
	}
	return true;
}

static bool unit_ok(const char *u)
{
	size_t n = strlen(u);
	if (n < 9 || n > 255 || strcmp(u + n - 8, ".service") != 0)
		return false;
	for (const char *p = u; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("@._:-", *p)))
			return false;
	return true;
}

static char *join(struct ctx *c, const char *dir, const char *rel, const char *what)
{
	char *p;
	if (!inside(rel)) {
		fail(c, "%s '%s' must be a relative path inside the app directory", what, rel);
		return NULL;
	}
	if (asprintf(&p, "%s/%s", dir, rel) < 0) {
		fail(c, "out of memory");
		return NULL;
	}
	return p;
}

static bool parse(struct ctx *c, const char *appdir, const char *dirname, struct app *a)
{
	yaml_node_t *root = yaml_document_get_root_node(c->doc);
	if (!root || root->type != YAML_MAPPING_NODE) {
		fail(c, "manifest must be a mapping");
		return false;
	}
	const char *id = yaml_text(field(c, root, "id", TOP, N(TOP), "manifest"));
	const char *name = yaml_text(field(c, root, "name", TOP, N(TOP), "manifest"));
	yaml_node_t *cat = field(c, root, "category", TOP, N(TOP), "manifest");
	yaml_node_t *icons = field(c, root, "icons", TOP, N(TOP), "manifest");
	yaml_node_t *comps = field(c, root, "components", TOP, N(TOP), "manifest");
	if (c->err[0])
		return false;
	if (!id || !id_ok(id) || strcmp(id, dirname) != 0) {
		fail(c, "id '%s' must be reverse-DNS ([a-z0-9_-] segments joined by '.') and equal the directory name '%s'",
		     id ? id : "?", dirname);
		return false;
	}
	if (!name) {
		fail(c, "missing name");
		return false;
	}
	if (!(a->id = strdup(id)) || !(a->name = strdup(name)) || !(a->category = strdup(cat ? yaml_text(cat) ?: "" : ""))) {
		fail(c, "out of memory");
		return false;
	}

	if (icons) {
		static const char *const ICON[] = {"small", "large"};
		if (icons->type != YAML_MAPPING_NODE) {
			fail(c, "icons must be a mapping");
			return false;
		}
		const char *s = yaml_text(field(c, icons, "small", ICON, N(ICON), "icons"));
		const char *l = yaml_text(field(c, icons, "large", ICON, N(ICON), "icons"));
		if (c->err[0] || (s && !(a->icon_small = join(c, appdir, s, "icons"))) ||
		    (l && !(a->icon_large = join(c, appdir, l, "icons"))))
			return false;
	}

	if (!comps || comps->type != YAML_SEQUENCE_NODE) {
		fail(c, "components must be a list");
		return false;
	}
	for (yaml_node_item_t *it = comps->data.sequence.items.start; it < comps->data.sequence.items.top; it++) {
		yaml_node_t *comp = node(c, *it);
		if (!comp || comp->type != YAML_MAPPING_NODE) {
			fail(c, "every component must be a mapping");
			return false;
		}
		const char *type = yaml_text(field(c, comp, "type", COMPONENT, N(COMPONENT), "component"));
		if (c->err[0])
			return false;
		if (!type || strcmp(type, "ui") != 0)
			continue;
		const char *exec = yaml_text(field(c, comp, "exec", COMPONENT, N(COMPONENT), "component"));
		const char *unit = yaml_text(field(c, comp, "unit", COMPONENT, N(COMPONENT), "component"));
		if (!exec == !unit) {
			fail(c, "the ui component needs exactly one of exec and unit");
			return false;
		}
		if (unit && !unit_ok(unit)) {
			fail(c, "unit '%s' must be a .service name of [A-Za-z0-9@._:-]", unit);
			return false;
		}
		if (exec)
			return (a->exec = join(c, appdir, exec, "exec")) != NULL;
		if (!(a->unit = strdup(unit)))
			fail(c, "out of memory");
		return a->unit != NULL;
	}
	fail(c, "no component of type ui");
	return false;
}

struct app *app_load(const char *appdir, char *err, size_t errlen)
{
	char dummy[1], *path;
	struct ctx c = {.err = errlen ? err : dummy, .errlen = errlen ? errlen : sizeof(dummy)};
	c.err[0] = 0;
	const char *slash = strrchr(appdir, '/');
	const char *dirname = slash ? slash + 1 : appdir;
	if (asprintf(&path, "%s/manifest.yaml", appdir) < 0)
		return NULL;
	FILE *f = fopen(path, "r");
	free(path);
	if (!f) {
		fail(&c, "manifest.yaml: %s", strerror(errno));
		return NULL;
	}
	yaml_parser_t p;
	yaml_document_t doc;
	struct app *a = calloc(1, sizeof(*a));
	yaml_parser_initialize(&p);
	yaml_parser_set_input_file(&p, f);
	if (!a) {
		fail(&c, "out of memory");
	} else if (!yaml_parser_load(&p, &doc)) {
		fail(&c, "YAML error at line %zu: %s", p.problem_mark.line + 1, p.problem ? p.problem : "unknown");
	} else {
		c.doc = &doc;
		parse(&c, appdir, dirname, a);
		yaml_document_delete(&doc);
	}
	yaml_parser_delete(&p);
	fclose(f);
	if (c.err[0]) {
		app_free(a);
		return NULL;
	}
	return a;
}

static void app_clear(struct app *a)
{
	free(a->id);
	free(a->name);
	free(a->category);
	free(a->icon_small);
	free(a->icon_large);
	free(a->exec);
	free(a->unit);
}

void app_free(struct app *a)
{
	if (a)
		app_clear(a);
	free(a);
}

static int by_name(const void *pa, const void *pb)
{
	const struct app *a = pa, *b = pb;
	int r = strcmp(a->name, b->name);
	return r ? r : strcmp(a->id, b->id);
}

int apps_load(const char *dir, struct app_list *out)
{
	struct dirent **ent;
	int n = scandir(dir, &ent, NULL, alphasort);
	*out = (struct app_list){0};
	if (n < 0)
		return -errno;
	out->apps = calloc(n ? n : 1, sizeof(*out->apps));
	for (int i = 0; i < n; i++) {
		char appdir[4096], err[512];
		if (out->apps && ent[i]->d_name[0] != '.') {
			snprintf(appdir, sizeof(appdir), "%s/%s", dir, ent[i]->d_name);
			struct app *a = app_load(appdir, err, sizeof(err));
			if (a) {
				out->apps[out->n++] = *a;
				free(a); // the fields moved into the list
			} else {
				fprintf(stderr, "skipping app %s: %s\n", appdir, err);
			}
		}
		free(ent[i]);
	}
	free(ent);
	if (!out->apps)
		return -ENOMEM;
	qsort(out->apps, out->n, sizeof(*out->apps), by_name);
	return 0;
}

void apps_free(struct app_list *l)
{
	for (size_t i = 0; i < l->n; i++)
		app_clear(&l->apps[i]);
	free(l->apps);
	*l = (struct app_list){0};
}

const struct app *apps_find(const struct app_list *l, const char *id)
{
	for (size_t i = 0; i < l->n; i++)
		if (strcmp(l->apps[i].id, id) == 0)
			return &l->apps[i];
	return NULL;
}

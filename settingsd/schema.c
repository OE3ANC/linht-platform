// SPDX-License-Identifier: GPL-3.0-only
#include "schema.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "yaml-util.h"

static const char *const TYPES[] = {"bool", "int", "float", "string"};
static const char *const ACCESS[] = {"user", "admin", "device-only", "factory"};
static const char *const APPLY[] = {"live", "restart-app", "restart-service", "reboot"};
#define N(a) (sizeof(a) / sizeof(*(a)))

const char *ltype_name(enum ltype t) { return TYPES[t]; }
const char *laccess_name(enum laccess a) { return ACCESS[a]; }

void lvalue_clear(struct lvalue *v)
{
	if (v->type == LT_STRING) {
		free(v->s);
		v->s = NULL;
	}
}

int lvalue_copy(struct lvalue *dst, const struct lvalue *src)
{
	*dst = *src;
	if (src->type == LT_STRING && !(dst->s = strdup(src->s)))
		return -ENOMEM;
	return 0;
}

bool lvalue_eq(const struct lvalue *a, const struct lvalue *b)
{
	if (a->type != b->type)
		return false;
	switch (a->type) {
	case LT_BOOL: return a->b == b->b;
	case LT_INT: return a->i == b->i;
	case LT_FLOAT: return a->d == b->d;
	case LT_STRING: return strcmp(a->s, b->s) == 0;
	}
	return false;
}

bool lvalue_parse(enum ltype t, const char *text, struct lvalue *out)
{
	char *end;
	out->type = t;
	switch (t) {
	case LT_BOOL:
		if (strcmp(text, "true") == 0 || strcmp(text, "false") == 0) {
			out->b = text[0] == 't';
			return true;
		}
		return false;
	case LT_INT: {
		if (!*text)
			return false;
		errno = 0;
		long long n = strtoll(text, &end, 10);
		if (errno || *end)
			return false;
		out->i = n;
		return true;
	}
	case LT_FLOAT: {
		if (!*text)
			return false;
		errno = 0;
		double d = strtod(text, &end);
		if (errno || *end || !isfinite(d))
			return false;
		out->d = d;
		return true;
	}
	case LT_STRING:
		return (out->s = strdup(text)) != NULL;
	}
	return false;
}

// Rejects overlong forms, surrogates and code points above U+10FFFF.
static bool utf8_valid(const char *str)
{
	const unsigned char *s = (const unsigned char *)str;
	while (*s) {
		unsigned c = *s, n;
		uint32_t cp;
		if (c < 0x80) {
			s++;
			continue;
		} else if ((c & 0xe0) == 0xc0) {
			n = 1, cp = c & 0x1f;
		} else if ((c & 0xf0) == 0xe0) {
			n = 2, cp = c & 0x0f;
		} else if ((c & 0xf8) == 0xf0) {
			n = 3, cp = c & 0x07;
		} else {
			return false;
		}
		for (unsigned i = 1; i <= n; i++) {
			if ((s[i] & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (s[i] & 0x3f);
		}
		static const uint32_t min[] = {0, 0x80, 0x800, 0x10000};
		if (cp < min[n] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return false;
		s += n + 1;
	}
	return true;
}

static int lvalue_cmp(const struct lvalue *a, const struct lvalue *b)
{
	if (a->type == LT_INT)
		return (a->i > b->i) - (a->i < b->i);
	return (a->d > b->d) - (a->d < b->d);
}

const char *lkey_check(const struct lkey *k, const struct lvalue *v)
{
	if (v->type != k->type)
		return "wrong type";
	if (v->type == LT_FLOAT && !isfinite(v->d))
		return "not a finite number";
	if (v->type == LT_STRING) {
		if (strlen(v->s) > LSTRING_MAX)
			return "string too long";
		if (!utf8_valid(v->s))
			return "not valid UTF-8";
	}
	if (k->has_min && lvalue_cmp(v, &k->min) < 0)
		return "below min";
	if (k->has_max && lvalue_cmp(v, &k->max) > 0)
		return "above max";
	if (k->n_choices) {
		for (size_t i = 0; i < k->n_choices; i++)
			if (lvalue_eq(v, &k->choices[i]))
				return NULL;
		return "not one of the choices";
	}
	return NULL;
}

// --- schema files ---

struct ctx {
	yaml_document_t *doc;
	char *err;
	size_t errlen;
};

static void fail(struct ctx *c, const char *fmt, ...)
{
	if (c->err[0])
		return; // keep the first error
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->err, c->errlen, fmt, ap);
	va_end(ap);
}

static yaml_node_t *node(struct ctx *c, int id) { return yaml_document_get_node(c->doc, id); }

static bool name_ok(const char *s, bool dots)
{
	if (!*s)
		return false;
	for (const char *p = s; *p; p++) {
		bool word = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
		if (!word && !(dots && *p == '.' && p != s && p[1] && p[1] != '.'))
			return false;
	}
	return true;
}

static int pick(const char *s, const char *const *list, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(s, list[i]) == 0)
			return (int)i;
	return -1;
}

static char *dup_scalar(struct ctx *c, yaml_node_t *n, const char *what)
{
	const char *s = yaml_text(n);
	char *d = s ? strdup(s) : NULL;
	if (!d)
		fail(c, "%s must be a string", what);
	return d;
}

static bool parse_into(struct ctx *c, const struct lkey *k, yaml_node_t *n, const char *field, struct lvalue *out)
{
	const char *s = yaml_text(n);
	if (!s || !lvalue_parse(k->type, s, out)) {
		fail(c, "key '%s': %s '%s' is not a valid %s", k->name, field, s ? s : "?", ltype_name(k->type));
		return false;
	}
	return true;
}

static void free_key(struct lkey *k)
{
	lvalue_clear(&k->def);
	for (size_t i = 0; i < k->n_choices; i++)
		lvalue_clear(&k->choices[i]);
	free(k->choices);
	free(k->name);
	free(k->unit);
	free(k->label);
	free(k->description);
	free(k->apply);
}

static const char *const KEY_FIELDS[] = {"type", "default", "min", "max", "choices", "step",
                                         "unit", "label", "description", "order", "apply", "access"};

static bool parse_key(struct ctx *c, const char *name, yaml_node_t *map, struct lkey *k)
{
	if (!name_ok(name, false)) {
		fail(c, "key name '%s' must match [a-z0-9_]+", name);
		return false;
	}
	if (!(k->name = strdup(name)) || !map || map->type != YAML_MAPPING_NODE) {
		fail(c, "key '%s' must be a mapping", name);
		return false;
	}
	yaml_node_t *field[N(KEY_FIELDS)] = {0};
	for (yaml_node_pair_t *p = map->data.mapping.pairs.start; p < map->data.mapping.pairs.top; p++) {
		const char *f = yaml_text(node(c, p->key));
		int i = f ? pick(f, KEY_FIELDS, N(KEY_FIELDS)) : -1;
		if (i < 0) {
			fail(c, "key '%s': unknown field '%s'", name, f ? f : "?");
			return false;
		}
		if (field[i]) {
			fail(c, "key '%s': duplicate field '%s'", name, f);
			return false;
		}
		field[i] = node(c, p->value);
	}
	enum { TYPE, DEFAULT, MIN, MAX, CHOICES, STEP, UNIT, LABEL, DESCRIPTION, ORDER, APPLYF, ACCESSF };

	const char *t = yaml_text(field[TYPE]);
	int type = t ? pick(t, TYPES, N(TYPES)) : -1;
	if (type < 0) {
		fail(c, t ? "key '%s': unknown type '%s'" : "key '%s': missing type", name, t);
		return false;
	}
	k->type = (enum ltype)type;
	if (!field[DEFAULT]) {
		fail(c, "key '%s': missing default", name);
		return false;
	}
	if (!parse_into(c, k, field[DEFAULT], "default", &k->def))
		return false;

	bool numeric = k->type == LT_INT || k->type == LT_FLOAT;
	static const char *const bound_name[] = {"min", "max", "step"};
	struct lvalue *bound[] = {&k->min, &k->max, &k->step};
	bool *has[] = {&k->has_min, &k->has_max, &k->has_step};
	int bound_field[] = {MIN, MAX, STEP};
	for (int i = 0; i < 3; i++) {
		if (!field[bound_field[i]])
			continue;
		if (!numeric) {
			fail(c, "key '%s': %s is only allowed for int and float", name, bound_name[i]);
			return false;
		}
		if (!parse_into(c, k, field[bound_field[i]], bound_name[i], bound[i]))
			return false;
		*has[i] = true;
	}

	if (field[CHOICES]) {
		yaml_node_t *seq = field[CHOICES];
		if (k->type == LT_BOOL || seq->type != YAML_SEQUENCE_NODE) {
			fail(c, "key '%s': choices must be a list, and is not allowed for bool", name);
			return false;
		}
		size_t n = seq->data.sequence.items.top - seq->data.sequence.items.start;
		if (!(k->choices = calloc(n ? n : 1, sizeof(*k->choices)))) {
			fail(c, "out of memory");
			return false;
		}
		for (yaml_node_item_t *it = seq->data.sequence.items.start; it < seq->data.sequence.items.top; it++) {
			if (!parse_into(c, k, node(c, *it), "choice", &k->choices[k->n_choices]))
				return false;
			k->n_choices++;
		}
	}

	if (!field[LABEL]) {
		fail(c, "key '%s': missing label", name);
		return false;
	}
	if (!(k->label = dup_scalar(c, field[LABEL], "label")) ||
	    (field[UNIT] && !(k->unit = dup_scalar(c, field[UNIT], "unit"))) ||
	    (field[DESCRIPTION] && !(k->description = dup_scalar(c, field[DESCRIPTION], "description"))))
		return false;

	if (field[ORDER]) {
		struct lvalue o;
		const char *s = yaml_text(field[ORDER]);
		if (!s || !lvalue_parse(LT_INT, s, &o)) {
			fail(c, "key '%s': order must be an int", name);
			return false;
		}
		k->has_order = true;
		k->order = o.i;
	}

	const char *a = field[APPLYF] ? yaml_text(field[APPLYF]) : "live";
	if (!a || pick(a, APPLY, N(APPLY)) < 0) {
		fail(c, "key '%s': unknown apply '%s'", name, a ? a : "?");
		return false;
	}
	if (!(k->apply = strdup(a))) {
		fail(c, "out of memory");
		return false;
	}

	const char *ac = field[ACCESSF] ? yaml_text(field[ACCESSF]) : "user";
	int access = ac ? pick(ac, ACCESS, N(ACCESS)) : -1;
	if (access < 0) {
		fail(c, "key '%s': unknown access '%s'", name, ac ? ac : "?");
		return false;
	}
	k->access = (enum laccess)access;

	const char *why = lkey_check(k, &k->def);
	if (why) {
		fail(c, "key '%s': default is invalid: %s", name, why);
		return false;
	}
	return true;
}

static int key_order(const void *pa, const void *pb)
{
	const struct lkey *a = pa, *b = pb;
	if (a->has_order != b->has_order)
		return a->has_order ? -1 : 1;
	if (a->has_order && a->order != b->order)
		return a->order < b->order ? -1 : 1;
	return strcmp(a->name, b->name);
}

static struct lschema *parse_schema(struct ctx *c)
{
	yaml_node_t *root = yaml_document_get_root_node(c->doc);
	if (!root || root->type != YAML_MAPPING_NODE) {
		fail(c, "schema must be a mapping");
		return NULL;
	}
	static const char *const TOP[] = {"namespace", "version", "title", "keys"};
	yaml_node_t *top[N(TOP)] = {0};
	for (yaml_node_pair_t *p = root->data.mapping.pairs.start; p < root->data.mapping.pairs.top; p++) {
		const char *f = yaml_text(node(c, p->key));
		int i = f ? pick(f, TOP, N(TOP)) : -1;
		if (i < 0 || top[i]) {
			fail(c, i < 0 ? "unknown field '%s'" : "duplicate field '%s'", f ? f : "?");
			return NULL;
		}
		top[i] = node(c, p->value);
	}
	for (size_t i = 0; i < N(TOP); i++)
		if (!top[i]) {
			fail(c, "missing %s", TOP[i]);
			return NULL;
		}

	struct lschema *s = calloc(1, sizeof(*s));
	if (!s) {
		fail(c, "out of memory");
		return NULL;
	}
	struct lvalue version;
	const char *ns = yaml_text(top[0]), *v = yaml_text(top[1]);
	if (!ns || !name_ok(ns, true)) {
		fail(c, "namespace '%s' must be [a-z0-9_]+ segments joined by '.'", ns ? ns : "?");
		goto err;
	}
	if (!v || !lvalue_parse(LT_INT, v, &version) || version.i < 1) {
		fail(c, "version must be an int >= 1");
		goto err;
	}
	s->version = version.i;
	if (!(s->ns = strdup(ns)) || !(s->title = dup_scalar(c, top[2], "title")))
		goto err;

	yaml_node_t *keys = top[3];
	if (keys->type != YAML_MAPPING_NODE) {
		fail(c, "keys must be a mapping");
		goto err;
	}
	size_t n = keys->data.mapping.pairs.top - keys->data.mapping.pairs.start;
	if (!(s->keys = calloc(n ? n : 1, sizeof(*s->keys)))) {
		fail(c, "out of memory");
		goto err;
	}
	for (yaml_node_pair_t *p = keys->data.mapping.pairs.start; p < keys->data.mapping.pairs.top; p++) {
		const char *name = yaml_text(node(c, p->key));
		if (!name) {
			fail(c, "key names must be strings");
			goto err;
		}
		if (lschema_key(s, name)) {
			fail(c, "duplicate key '%s'", name);
			goto err;
		}
		struct lkey *k = &s->keys[s->n_keys++];
		if (!parse_key(c, name, node(c, p->value), k))
			goto err;
	}
	qsort(s->keys, s->n_keys, sizeof(*s->keys), key_order);
	return s;
err:
	lschema_free(s);
	return NULL;
}

struct lschema *lschema_load(const char *path, char *err, size_t errlen)
{
	char dummy[1];
	struct ctx c = {.err = errlen ? err : dummy, .errlen = errlen ? errlen : sizeof(dummy)};
	c.err[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f) {
		fail(&c, "%s: %s", path, strerror(errno));
		return NULL;
	}
	yaml_parser_t parser;
	yaml_document_t doc;
	struct lschema *s = NULL;
	yaml_parser_initialize(&parser);
	yaml_parser_set_input_file(&parser, f);
	if (!yaml_parser_load(&parser, &doc)) {
		fail(&c, "YAML error at line %zu: %s", parser.problem_mark.line + 1,
		     parser.problem ? parser.problem : "unknown");
	} else {
		c.doc = &doc;
		s = parse_schema(&c);
		yaml_document_delete(&doc);
	}
	yaml_parser_delete(&parser);
	fclose(f);
	return s;
}

void lschema_free(struct lschema *s)
{
	if (!s)
		return;
	for (size_t i = 0; i < s->n_keys; i++)
		free_key(&s->keys[i]);
	free(s->keys);
	free(s->ns);
	free(s->title);
	free(s);
}

const struct lkey *lschema_key(const struct lschema *s, const char *name)
{
	for (size_t i = 0; i < s->n_keys; i++)
		if (s->keys[i].name && strcmp(s->keys[i].name, name) == 0)
			return &s->keys[i];
	return NULL;
}

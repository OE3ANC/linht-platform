// SPDX-License-Identifier: GPL-3.0-only
// Settings schemas and values, as specified in docs/settings-schema.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LSTRING_MAX 4096

enum ltype { LT_BOOL, LT_INT, LT_FLOAT, LT_STRING };
enum laccess { LA_USER, LA_ADMIN, LA_DEVICE_ONLY, LA_FACTORY };

struct lvalue {
	enum ltype type;
	union {
		bool b;
		int64_t i;
		double d;
		char *s; // owned, NUL-terminated
	};
};

struct lkey {
	char *name;
	enum ltype type;
	struct lvalue def;
	bool has_min, has_max, has_step;
	struct lvalue min, max, step;
	struct lvalue *choices;
	size_t n_choices;
	char *unit, *label, *description; // unit, description may be NULL
	char *apply;                      // "live" unless the schema says otherwise
	bool has_order;
	int64_t order;
	enum laccess access;
};

struct lschema {
	char *ns;
	int64_t version;
	char *title;
	struct lkey *keys; // sorted by order, keys without order last, then by name
	size_t n_keys;
};

// Loads and checks one schema file. On error returns NULL and writes a message to err.
struct lschema *lschema_load(const char *path, char *err, size_t errlen);
void lschema_free(struct lschema *s);
const struct lkey *lschema_key(const struct lschema *s, const char *name);

// NULL if v is a valid value for k, otherwise a short message naming the failed rule.
const char *lkey_check(const struct lkey *k, const struct lvalue *v);

// Parses the text of a YAML scalar as type t. Strings are taken as they are.
bool lvalue_parse(enum ltype t, const char *text, struct lvalue *out);
bool lvalue_eq(const struct lvalue *a, const struct lvalue *b);
int lvalue_copy(struct lvalue *dst, const struct lvalue *src); // 0 or -ENOMEM
void lvalue_clear(struct lvalue *v);
const char *ltype_name(enum ltype t);
const char *laccess_name(enum laccess a);

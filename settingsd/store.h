// SPDX-License-Identifier: GPL-3.0-only
// The user layer of one namespace: /var/lib/linht/settings/<namespace>.yaml (docs/settings-schema.md, "Store").
#pragma once
#include "schema.h"

struct lstore {
	const struct lschema *schema;
	char *path;
	struct lvalue *user; // user value per schema key, valid where set[i]
	bool *set;
};

struct lupdate {
	size_t key;          // index into schema->keys
	bool reset;          // true: back to the default; value is ignored
	struct lvalue value; // borrowed
};

// Loads <dir>/<namespace>.yaml. Never fails on bad content: invalid entries are dropped, and an
// unparsable file is kept as <namespace>.yaml.broken. Returns NULL only when out of memory.
struct lstore *lstore_open(const struct lschema *s, const char *dir);
void lstore_free(struct lstore *st);

// Effective value of key i: the stored user value, or the schema default.
const struct lvalue *lstore_get(const struct lstore *st, size_t i);

// Applies all updates or none. Validates first (-EINVAL), then writes the file atomically
// (-EIO, old values stay in effect). On success changed[i] tells whether the effective value of
// key i changed; it must have room for schema->n_keys entries. err receives a message on failure.
int lstore_commit(struct lstore *st, const struct lupdate *u, size_t n, bool *changed, char *err, size_t errlen);

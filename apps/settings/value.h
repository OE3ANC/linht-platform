// SPDX-License-Identifier: GPL-3.0-only
// One setting as the Settings app shows it, and what LEFT/RIGHT do to it (docs/settings-schema.md).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum stype {
	ST_BOOL,
	ST_INT,
	ST_OTHER, // string and float: shown, not editable on the radio yet
};

struct setting {
	char *key, *label, *unit; // unit may be NULL
	enum stype type;
	bool has_min, has_max;
	int64_t min, max, step; // step: 1 if the schema gives none
	int64_t *choices;
	size_t n_choices;
	bool b;      // current value of a bool
	int64_t i;   // current value of an int
	char *text;  // current value of anything else, as shown
};

// The value after LEFT (dir = -1) or RIGHT (dir = +1). Returns false if it cannot change.
bool setting_step(const struct setting *s, int dir, int64_t *out_i, bool *out_b);
// The current value as shown on screen, e.g. "224", "15 s", "on".
void setting_format(const struct setting *s, char *buf, size_t len);

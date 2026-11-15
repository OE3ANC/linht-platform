// SPDX-License-Identifier: GPL-3.0-only
#include "value.h"

#include <inttypes.h>
#include <stdio.h>

bool setting_step(const struct setting *s, int dir, int64_t *out_i, bool *out_b)
{
	if (s->type == ST_BOOL) {
		*out_b = !s->b;
		return true;
	}
	if (s->type != ST_INT)
		return false;
	if (s->n_choices) { // the nearest choice in that direction, no wrap-around
		bool found = false;
		int64_t best = 0;
		for (size_t i = 0; i < s->n_choices; i++) {
			int64_t c = s->choices[i];
			if ((dir > 0 ? c > s->i : c < s->i) && (!found || (dir > 0 ? c < best : c > best))) {
				best = c;
				found = true;
			}
		}
		if (found)
			*out_i = best;
		return found;
	}
	int64_t step = s->step > 0 ? s->step : 1, v;
	if (__builtin_add_overflow(s->i, dir > 0 ? step : -step, &v))
		v = dir > 0 ? INT64_MAX : INT64_MIN;
	if (s->has_min && v < s->min)
		v = s->min;
	if (s->has_max && v > s->max)
		v = s->max;
	if (v == s->i)
		return false;
	*out_i = v;
	return true;
}

void setting_format(const struct setting *s, char *buf, size_t len)
{
	switch (s->type) {
	case ST_BOOL: snprintf(buf, len, "%s", s->b ? "on" : "off"); break;
	case ST_INT: snprintf(buf, len, "%" PRId64 "%s%s", s->i, s->unit ? " " : "", s->unit ? s->unit : ""); break;
	case ST_OTHER: snprintf(buf, len, "%s", s->text ? s->text : ""); break;
	}
}

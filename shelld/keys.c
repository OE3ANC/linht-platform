// SPDX-License-Identifier: GPL-3.0-only
#include "keys.h"

#include <string.h>

static bool bit(const uint8_t *set, uint16_t code) { return code < KEY_CNT && (set[code / 8] >> (code % 8)) & 1; }

static void set_bit(uint8_t *set, uint16_t code, bool on)
{
	if (code >= KEY_CNT)
		return;
	if (on)
		set[code / 8] |= 1u << (code % 8);
	else
		set[code / 8] &= ~(1u << (code % 8));
}

static bool any_down(const struct key_router *r)
{
	for (size_t i = 0; i < sizeof(r->down); i++)
		if (r->down[i])
			return true;
	return false;
}

static void emit(struct kr_out *o, enum kr_type type, uint16_t code, int32_t value)
{
	if (o->n < (int)(sizeof(o->a) / sizeof(*o->a)))
		o->a[o->n++] = (struct kr_action){type, code, value};
}

void kr_init(struct key_router *r, uint64_t now_ms)
{
	uint32_t s = r->screen_timeout_ms, k = r->kbd_timeout_ms;
	memset(r, 0, sizeof(*r));
	r->screen_timeout_ms = s;
	r->kbd_timeout_ms = k;
	r->screen_on = r->kbd_on = true;
	r->last_activity = now_ms;
}

void kr_key(struct key_router *r, uint16_t code, int32_t value, uint64_t now_ms, struct kr_out *o)
{
	r->last_activity = now_ms;
	if (value != 2)
		set_bit(r->down, code, value == 1);
	if (!r->kbd_on) {
		r->kbd_on = true;
		emit(o, KR_KBD, 0, 1);
	}
	if (!r->screen_on && value == 1) {
		r->screen_on = true;
		emit(o, KR_SCREEN, 0, 1);
		if (code != KEY_P) { // PTT must work with the screen off; any other key only wakes it
			set_bit(r->swallow, code, true);
			return;
		}
	}
	if (bit(r->swallow, code)) {
		if (value == 0)
			set_bit(r->swallow, code, false);
		return;
	}
	if (code == KEY_ESC) { // held back until we know whether it is a long press
		if (value == 1) {
			r->esc_down = true;
			r->esc_long = false;
			r->esc_since = now_ms;
		} else if (value == 0) {
			if (r->esc_down && !r->esc_long) {
				emit(o, KR_FORWARD, KEY_ESC, 1);
				emit(o, KR_FORWARD, KEY_ESC, 0);
			}
			r->esc_down = false;
		}
		return;
	}
	emit(o, KR_FORWARD, code, value);
}

void kr_tick(struct key_router *r, uint64_t now_ms, struct kr_out *o)
{
	if (r->esc_down && !r->esc_long && now_ms - r->esc_since >= KR_LONG_MS) {
		r->esc_long = true;
		emit(o, KR_HOME, 0, 0); // refused while PTT is held (fg_request, D16)
	}
	if (any_down(r))
		return; // no timeouts while a key is held, e.g. a long transmission
	uint64_t idle = now_ms - r->last_activity;
	if (r->screen_on && r->screen_timeout_ms && idle >= r->screen_timeout_ms) {
		r->screen_on = false;
		emit(o, KR_SCREEN, 0, 0);
	}
	if (r->kbd_on && r->kbd_timeout_ms && idle >= r->kbd_timeout_ms) {
		r->kbd_on = false;
		emit(o, KR_KBD, 0, 0);
	}
}

static int64_t earliest(int64_t a, int64_t b) { return a < 0 ? b : b < 0 ? a : a < b ? a : b; }

int64_t kr_deadline(const struct key_router *r)
{
	int64_t d = -1;
	if (r->esc_down && !r->esc_long)
		d = (int64_t)(r->esc_since + KR_LONG_MS);
	if (any_down(r))
		return d;
	if (r->screen_on && r->screen_timeout_ms)
		d = earliest(d, (int64_t)(r->last_activity + r->screen_timeout_ms));
	if (r->kbd_on && r->kbd_timeout_ms)
		d = earliest(d, (int64_t)(r->last_activity + r->kbd_timeout_ms));
	return d;
}

bool kr_held(const struct key_router *r, uint16_t code) { return bit(r->down, code); }

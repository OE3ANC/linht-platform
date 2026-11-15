// SPDX-License-Identifier: GPL-3.0-only
// Keypad routing and idle policy of linht-shelld, without any I/O (architecture §6.2, D11, D16).
// Feed raw keypad events and clock ticks; act on the returned actions.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <linux/input-event-codes.h>

#define KR_LONG_MS 1000 // long-press ESC: go home (D11)

enum kr_type {
	KR_FORWARD, // pass code/value to the foreground app (uinput)
	KR_HOME,    // the "go home" gesture
	KR_SCREEN,  // screen backlight on (value 1) or off (value 0)
	KR_KBD,     // keypad backlight on (value 1) or off (value 0)
};

struct kr_action {
	enum kr_type type;
	uint16_t code;
	int32_t value;
};

struct kr_out {
	int n;
	struct kr_action a[6];
};

struct key_router {
	uint32_t screen_timeout_ms, kbd_timeout_ms; // 0 = never
	bool screen_on, kbd_on;
	uint64_t last_activity;
	bool esc_down, esc_long;
	uint64_t esc_since;
	uint8_t down[KEY_CNT / 8];    // keys currently held
	uint8_t swallow[KEY_CNT / 8]; // keys whose press only woke the screen; their release is dropped too
};

void kr_init(struct key_router *r, uint64_t now_ms);
// value: 1 = pressed, 0 = released, 2 = repeat (the keypad driver fix in contrib/yocto makes it so).
void kr_key(struct key_router *r, uint16_t code, int32_t value, uint64_t now_ms, struct kr_out *out);
void kr_tick(struct key_router *r, uint64_t now_ms, struct kr_out *out);
bool kr_held(const struct key_router *r, uint16_t code); // pressed right now
// Absolute time of the next kr_tick() that can produce an action, or -1 if none is pending.
int64_t kr_deadline(const struct key_router *r);

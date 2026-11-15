// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>

#include "../../tests/check.h"
#include "keys.h"

static struct key_router r;
static struct kr_out out;

static void key(uint16_t code, int32_t value, uint64_t t)
{
	out.n = 0;
	kr_key(&r, code, value, t, &out);
}

static void tick(uint64_t t)
{
	out.n = 0;
	kr_tick(&r, t, &out);
}

static bool has(enum kr_type type, uint16_t code, int32_t value)
{
	for (int i = 0; i < out.n; i++)
		if (out.a[i].type == type && out.a[i].code == code && out.a[i].value == value)
			return true;
	return false;
}

static void setup(uint32_t screen_ms, uint32_t kbd_ms)
{
	kr_init(&r, 0);
	r.screen_timeout_ms = screen_ms;
	r.kbd_timeout_ms = kbd_ms;
}

static void test_plain_keys_are_forwarded(void)
{
	setup(0, 0);
	key(KEY_UP, 1, 10);
	CHECK(out.n == 1 && has(KR_FORWARD, KEY_UP, 1));
	key(KEY_UP, 0, 20);
	CHECK(out.n == 1 && has(KR_FORWARD, KEY_UP, 0));
	key(KEY_P, 1, 30); // PTT is an ordinary key for the app in milestone 1 (gui_test keys the radio)
	CHECK(out.n == 1 && has(KR_FORWARD, KEY_P, 1));
	key(KEY_P, 0, 40);
	CHECK(out.n == 1 && has(KR_FORWARD, KEY_P, 0));
}

static void test_short_esc_is_delivered_on_release(void)
{
	setup(0, 0);
	key(KEY_ESC, 1, 100);
	CHECK(out.n == 0); // held back until we know it is not a long press
	tick(500);
	CHECK(out.n == 0);
	key(KEY_ESC, 0, 600);
	CHECK(out.n == 2 && out.a[0].type == KR_FORWARD && out.a[0].code == KEY_ESC && out.a[0].value == 1 &&
	      out.a[1].type == KR_FORWARD && out.a[1].code == KEY_ESC && out.a[1].value == 0);
}

static void test_long_esc_goes_home_and_app_never_sees_it(void)
{
	setup(0, 0);
	key(KEY_ESC, 1, 100);
	CHECK(kr_deadline(&r) == 100 + KR_LONG_MS);
	tick(100 + KR_LONG_MS - 1);
	CHECK(out.n == 0);
	tick(100 + KR_LONG_MS);
	CHECK(out.n == 1 && out.a[0].type == KR_HOME);
	tick(3000);
	CHECK(!has(KR_HOME, 0, 0)); // fires once per press
	key(KEY_ESC, 0, 3100);
	CHECK(out.n == 0);
}

static void test_ptt_is_held_until_released(void)
{
	setup(0, 0);
	key(KEY_P, 1, 0);
	CHECK(kr_held(&r, KEY_P)); // shelld refuses every app switch while this is true (fg_request, D16)
	key(KEY_P, 2, 100);
	CHECK(kr_held(&r, KEY_P)); // a repeat is not a release
	key(KEY_P, 0, 200);
	CHECK(has(KR_FORWARD, KEY_P, 0));
	CHECK(!kr_held(&r, KEY_P));
}

static void test_screen_timeout_and_wake(void)
{
	setup(15000, 0);
	CHECK(kr_deadline(&r) == 15000);
	tick(14999);
	CHECK(out.n == 0);
	tick(15000);
	CHECK(has(KR_SCREEN, 0, 0) && r.screen_on == false);
	key(KEY_5, 1, 20000);
	CHECK(has(KR_SCREEN, 0, 1) && !has(KR_FORWARD, KEY_5, 1)); // the waking key is swallowed
	key(KEY_5, 0, 20100);
	CHECK(out.n == 0); // and so is its release
	key(KEY_5, 1, 20200);
	CHECK(has(KR_FORWARD, KEY_5, 1)); // the next press is normal again
	CHECK(kr_deadline(&r) == -1);     // KEY_5 is still held
}

static void test_ptt_works_with_the_screen_off(void)
{
	setup(1000, 0);
	tick(1000);
	CHECK(!r.screen_on);
	key(KEY_P, 1, 2000);
	CHECK(has(KR_SCREEN, 0, 1) && has(KR_FORWARD, KEY_P, 1));
	key(KEY_P, 0, 2500);
	CHECK(has(KR_FORWARD, KEY_P, 0));
}

static void test_no_timeout_while_a_key_is_held(void)
{
	setup(1000, 1000);
	key(KEY_P, 1, 0);
	CHECK(kr_deadline(&r) == -1);
	tick(60000); // a long transmission
	CHECK(out.n == 0 && r.screen_on && r.kbd_on);
	key(KEY_P, 0, 60000);
	CHECK(kr_deadline(&r) == 61000); // counted from the release
}

static void test_keypad_light_times_out_separately(void)
{
	setup(15000, 5000);
	tick(5000);
	CHECK(out.n == 1 && has(KR_KBD, 0, 0) && r.screen_on);
	key(KEY_DOWN, 1, 6000);
	CHECK(has(KR_KBD, 0, 1) && has(KR_FORWARD, KEY_DOWN, 1)); // the screen was on: not swallowed
	CHECK(kr_deadline(&r) == -1);
	key(KEY_DOWN, 0, 6100);
	CHECK(kr_deadline(&r) == 6100 + 5000); // the earlier of the two timeouts
}

static void test_opt_press_and_release_leaves_nothing_held(void)
{
	setup(1000, 0);
	key(KEY_O, 1, 0);
	CHECK(has(KR_FORWARD, KEY_O, 1) && kr_deadline(&r) == -1);
	key(KEY_O, 0, 100);
	CHECK(has(KR_FORWARD, KEY_O, 0));
	CHECK(kr_deadline(&r) == 1100); // not stuck as "held": the screen can still time out
	tick(1100);
	CHECK(has(KR_SCREEN, 0, 0));
}

static void test_timeout_zero_means_never(void)
{
	setup(0, 0);
	CHECK(kr_deadline(&r) == -1);
	tick(1u << 30);
	CHECK(out.n == 0 && r.screen_on && r.kbd_on);
}

int main(void)
{
	test_plain_keys_are_forwarded();
	test_short_esc_is_delivered_on_release();
	test_long_esc_goes_home_and_app_never_sees_it();
	test_ptt_is_held_until_released();
	test_screen_timeout_and_wake();
	test_ptt_works_with_the_screen_off();
	test_no_timeout_while_a_key_is_held();
	test_keypad_light_times_out_separately();
	test_opt_press_and_release_leaves_nothing_held();
	test_timeout_zero_means_never();
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

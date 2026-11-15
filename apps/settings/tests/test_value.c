// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>

#include "../../../tests/check.h"
#include "value.h"

static int64_t timeouts[] = {0, 5, 10, 15, 30, 60};

static struct setting choice(int64_t cur)
{
	return (struct setting){.type = ST_INT, .choices = timeouts, .n_choices = 6, .step = 1, .i = cur};
}

static struct setting range(int64_t cur)
{
	return (struct setting){.type = ST_INT, .has_min = true, .has_max = true, .min = 32, .max = 255, .step = 32, .i = cur};
}

static void test_choices_move_to_neighbours_without_wrapping(void)
{
	int64_t v;
	struct setting s = choice(15);
	CHECK(setting_step(&s, +1, &v, NULL) && v == 30);
	CHECK(setting_step(&s, -1, &v, NULL) && v == 10);
	s = choice(60);
	CHECK(!setting_step(&s, +1, &v, NULL));
	s = choice(0);
	CHECK(!setting_step(&s, -1, &v, NULL));
	s = choice(7); // not one of the choices (e.g. set remotely before the schema changed)
	CHECK(setting_step(&s, +1, &v, NULL) && v == 10);
	CHECK(setting_step(&s, -1, &v, NULL) && v == 5);
}

static void test_range_steps_and_clamps(void)
{
	int64_t v;
	struct setting s = range(255);
	CHECK(setting_step(&s, -1, &v, NULL) && v == 223);
	CHECK(!setting_step(&s, +1, &v, NULL));
	s = range(224);
	CHECK(setting_step(&s, +1, &v, NULL) && v == 255); // clamped to max
	s = range(40);
	CHECK(setting_step(&s, -1, &v, NULL) && v == 32); // clamped to min
	s = range(32);
	CHECK(!setting_step(&s, -1, &v, NULL));
	s = (struct setting){.type = ST_INT, .step = 1, .i = 7}; // no bounds
	CHECK(setting_step(&s, +1, &v, NULL) && v == 8);
	s.i = INT64_MAX;
	CHECK(!setting_step(&s, +1, &v, NULL)); // no overflow
}

static void test_bool_toggles_and_other_types_are_read_only(void)
{
	bool b;
	int64_t v;
	struct setting s = {.type = ST_BOOL, .b = false};
	CHECK(setting_step(&s, +1, NULL, &b) && b == true);
	s.b = true;
	CHECK(setting_step(&s, -1, NULL, &b) && b == false);
	s = (struct setting){.type = ST_OTHER, .text = "org.m17project.linht.radio"};
	CHECK(!setting_step(&s, +1, &v, &b));
}

static void test_format(void)
{
	char buf[64];
	struct setting s = choice(15);
	s.unit = "s";
	setting_format(&s, buf, sizeof(buf));
	CHECK(strcmp(buf, "15 s") == 0);
	s = range(224);
	setting_format(&s, buf, sizeof(buf));
	CHECK(strcmp(buf, "224") == 0);
	s = (struct setting){.type = ST_BOOL, .b = true};
	setting_format(&s, buf, sizeof(buf));
	CHECK(strcmp(buf, "on") == 0);
	s = (struct setting){.type = ST_OTHER, .text = "radio"};
	setting_format(&s, buf, sizeof(buf));
	CHECK(strcmp(buf, "radio") == 0);
}

int main(void)
{
	test_choices_move_to_neighbours_without_wrapping();
	test_range_steps_and_clamps();
	test_bool_toggles_and_other_types_are_read_only();
	test_format();
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

// SPDX-License-Identifier: GPL-3.0-only
#include "../../tests/check.h"
#include "fg.h"

static struct app radio = {.id = "org.m17project.linht.radio"};
static struct app launcher = {.id = "org.m17project.linht.launcher"};
static struct app settings = {.id = "org.m17project.linht.settings"};
static struct fg f;

static bool is(struct fg_cmd c, enum fg_cmd_type type, const struct app *app) { return c.type == type && c.app == app; }

// Starts app from idle and lets its start job succeed.
static void running(const struct app *app)
{
	fg_init(&f, &radio, &launcher);
	CHECK(is(fg_request(&f, app, false), FG_START, app));
	CHECK(fg_foreground(&f) == NULL); // not before systemd says the job is done
	CHECK(is(fg_job_done(&f, true, 0), FG_NONE, NULL));
	CHECK(fg_foreground(&f) == app);
}

static void test_switch_stops_first_then_starts(void)
{
	running(&radio);
	CHECK(is(fg_request(&f, &launcher, false), FG_STOP, &radio));
	CHECK(fg_foreground(&f) == NULL);
	CHECK(is(fg_job_done(&f, true, 0), FG_START, &launcher));
	CHECK(is(fg_job_done(&f, true, 0), FG_NONE, NULL));
	CHECK(fg_foreground(&f) == &launcher);
}

static void test_request_for_the_running_app_does_nothing(void)
{
	running(&launcher);
	CHECK(is(fg_request(&f, &launcher, false), FG_NONE, NULL));
	CHECK(fg_foreground(&f) == &launcher);
}

static void test_newest_request_wins_during_a_switch(void)
{
	running(&radio);
	CHECK(is(fg_request(&f, &launcher, false), FG_STOP, &radio));
	CHECK(is(fg_request(&f, &settings, false), FG_NONE, NULL)); // queued, not acted on yet
	CHECK(is(fg_job_done(&f, true, 0), FG_START, &settings));
	CHECK(is(fg_job_done(&f, true, 0), FG_NONE, NULL));
	CHECK(fg_foreground(&f) == &settings);

	// a request while an app is still starting: let it start, then switch
	running(&radio);
	CHECK(is(fg_request(&f, &launcher, false), FG_STOP, &radio));
	CHECK(is(fg_job_done(&f, true, 0), FG_START, &launcher));
	CHECK(is(fg_request(&f, &settings, false), FG_NONE, NULL));
	CHECK(is(fg_job_done(&f, true, 0), FG_STOP, &launcher));
	CHECK(is(fg_job_done(&f, true, 0), FG_START, &settings));
}

static void test_crashed_app_falls_back(void)
{
	running(&settings);
	CHECK(is(fg_ended(&f, 1000), FG_START, &launcher)); // an app: back to the menu
	CHECK(is(fg_job_done(&f, true, 1000), FG_NONE, NULL));
	CHECK(is(fg_ended(&f, 2000), FG_START, &radio)); // the launcher: back home
	CHECK(is(fg_job_done(&f, true, 2000), FG_NONE, NULL));
	CHECK(is(fg_ended(&f, 3000), FG_START, &radio)); // the home app is restarted
}

static void test_failed_start_falls_back(void)
{
	fg_init(&f, &radio, &launcher);
	CHECK(is(fg_request(&f, &settings, false), FG_START, &settings));
	CHECK(is(fg_job_done(&f, false, 0), FG_START, &launcher));
	CHECK(is(fg_job_done(&f, false, 0), FG_START, &radio));
	CHECK(is(fg_job_done(&f, true, 0), FG_NONE, NULL));
	CHECK(fg_foreground(&f) == &radio);
}

static void test_restart_loop_gives_up(void)
{
	running(&radio);
	struct fg_cmd c = {0};
	int starts = 0;
	for (int i = 0; i < 20; i++) { // the home app dies right after every start
		c = fg_ended(&f, 100 + i);
		if (c.type != FG_START)
			break;
		starts++;
		fg_job_done(&f, true, 100 + i);
	}
	CHECK(starts == FG_MAX_AUTO_STARTS);
	CHECK(c.type == FG_NONE && f.state == FG_IDLE);
	CHECK(is(fg_request(&f, &launcher, false), FG_START, &launcher)); // the user can always start something
	CHECK(is(fg_job_done(&f, true, 200), FG_NONE, NULL));
	CHECK(is(fg_ended(&f, 300), FG_START, &radio)); // and the user request reset the limit
}

static void test_limit_window_expires(void)
{
	running(&radio);
	for (int i = 0; i < FG_MAX_AUTO_STARTS; i++) {
		CHECK(fg_ended(&f, 1000).type == FG_START);
		fg_job_done(&f, true, 1000);
	}
	CHECK(is(fg_ended(&f, 1000 + FG_WINDOW_MS), FG_START, &radio)); // a new window
}

static void test_exit_while_stopping_is_expected(void)
{
	running(&radio);
	CHECK(is(fg_request(&f, &launcher, false), FG_STOP, &radio));
	CHECK(is(fg_ended(&f, 0), FG_NONE, NULL)); // we asked for it
	CHECK(is(fg_job_done(&f, true, 0), FG_START, &launcher));
}

static void test_no_switch_while_ptt_is_held(void)
{
	running(&radio);
	CHECK(is(fg_request(&f, &launcher, true), FG_REFUSED, NULL)); // D16: the app in front may be transmitting
	CHECK(fg_foreground(&f) == &radio);
	CHECK(is(fg_request(&f, &launcher, false), FG_STOP, &radio));
	CHECK(is(fg_request(&f, &settings, true), FG_REFUSED, NULL)); // also while switching
	CHECK(is(fg_job_done(&f, true, 0), FG_START, &launcher));     // the refused request left no trace

	fg_init(&f, &radio, &launcher);
	CHECK(is(fg_request(&f, &radio, true), FG_START, &radio)); // nothing in front, nothing transmitting
}

int main(void)
{
	test_switch_stops_first_then_starts();
	test_no_switch_while_ptt_is_held();
	test_request_for_the_running_app_does_nothing();
	test_newest_request_wins_during_a_switch();
	test_crashed_app_falls_back();
	test_failed_start_falls_back();
	test_restart_loop_gives_up();
	test_limit_window_expires();
	test_exit_while_stopping_is_expected();
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}

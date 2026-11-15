// SPDX-License-Identifier: GPL-3.0-only
#include "fg.h"

void fg_init(struct fg *f, const struct app *home, const struct app *launcher)
{
	*f = (struct fg){.home = home, .launcher = launcher};
}

static struct fg_cmd start(struct fg *f, const struct app *app)
{
	f->cur = app;
	f->state = app ? FG_STARTING : FG_IDLE;
	return (struct fg_cmd){app ? FG_START : FG_NONE, app};
}

// Starts app on our own initiative (after a crash or a failed start), within the rate limit.
static struct fg_cmd auto_start(struct fg *f, const struct app *app, uint64_t now_ms)
{
	if (f->auto_starts == 0 || now_ms - f->window_start >= FG_WINDOW_MS) {
		f->window_start = now_ms;
		f->auto_starts = 0;
	}
	if (f->auto_starts++ >= FG_MAX_AUTO_STARTS)
		app = NULL; // give up until the user asks for something
	return start(f, app);
}

struct fg_cmd fg_request(struct fg *f, const struct app *app, bool ptt)
{
	if (ptt && f->state != FG_IDLE) // the app in front, or the one starting, may be transmitting
		return (struct fg_cmd){FG_REFUSED, NULL};
	f->auto_starts = 0; // the user's choice resets the restart limit
	switch (f->state) {
	case FG_IDLE:
		return start(f, app);
	case FG_RUNNING:
		if (app == f->cur)
			return (struct fg_cmd){FG_NONE, NULL};
		f->target = app;
		f->state = FG_STOPPING;
		return (struct fg_cmd){FG_STOP, f->cur};
	case FG_STARTING:
		f->target = app == f->cur ? NULL : app;
		break;
	case FG_STOPPING:
		f->target = app;
		break;
	}
	return (struct fg_cmd){FG_NONE, NULL};
}

struct fg_cmd fg_job_done(struct fg *f, bool ok, uint64_t now_ms)
{
	const struct app *t = f->target;
	switch (f->state) {
	case FG_STOPPING:
		f->target = NULL;
		return start(f, t);
	case FG_STARTING:
		if (!ok) {
			f->target = NULL;
			if (t)
				return start(f, t); // the user already asked for something else
			// an app that cannot start: the menu; the menu itself: home
			return auto_start(f, f->cur == f->launcher || !f->launcher ? f->home : f->launcher, now_ms);
		}
		if (t) { // a newer request came in while this app was starting
			f->state = FG_STOPPING;
			return (struct fg_cmd){FG_STOP, f->cur};
		}
		f->state = FG_RUNNING;
		break;
	default:
		break;
	}
	return (struct fg_cmd){FG_NONE, NULL};
}

struct fg_cmd fg_ended(struct fg *f, uint64_t now_ms)
{
	if (f->state != FG_RUNNING)
		return (struct fg_cmd){FG_NONE, NULL}; // while stopping, we asked for it
	// the home app is restarted; the launcher goes home; any other app goes back to the menu
	const struct app *next = f->cur == f->home || f->cur == f->launcher || !f->launcher ? f->home : f->launcher;
	return auto_start(f, next, now_ms);
}

const struct app *fg_foreground(const struct fg *f) { return f->state == FG_RUNNING ? f->cur : NULL; }

// SPDX-License-Identifier: GPL-3.0-only
// Foreground app state of linht-shelld, without any I/O (architecture §5, §6.1, D1, D10).
// One UI app at a time: stop the current one, wait for its job, then start the next one.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "apps.h"

#define FG_MAX_AUTO_STARTS 5  // automatic restarts/fallbacks allowed ...
#define FG_WINDOW_MS 60000     // ... per window, so a broken app cannot loop forever

enum fg_cmd_type { FG_NONE, FG_STOP, FG_START, FG_REFUSED };

struct fg_cmd {
	enum fg_cmd_type type;
	const struct app *app; // FG_STOP: the app to stop; FG_START: the app to start
};

enum fg_state { FG_IDLE, FG_STARTING, FG_RUNNING, FG_STOPPING };

struct fg {
	enum fg_state state;
	const struct app *cur;    // the app being started, running or being stopped
	const struct app *target; // what to start once the current transition is over
	const struct app *home, *launcher;
	unsigned auto_starts;
	uint64_t window_start;
};

void fg_init(struct fg *f, const struct app *home, const struct app *launcher);
// The user (Launch, Home, long-press ESC) wants app in the foreground. While ptt (PTT held) is true,
// the app in front keeps the screen and the request is FG_REFUSED (D16).
struct fg_cmd fg_request(struct fg *f, const struct app *app, bool ptt);
// The stop or start job last returned has finished; ok is false if systemd reported a failure.
struct fg_cmd fg_job_done(struct fg *f, bool ok, uint64_t now_ms);
// The running app exited by itself (crash, or it quit).
struct fg_cmd fg_ended(struct fg *f, uint64_t now_ms);
// The app owning the screen, or NULL while switching or idle.
const struct app *fg_foreground(const struct fg *f);

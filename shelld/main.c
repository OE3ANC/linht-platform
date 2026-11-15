// SPDX-License-Identifier: GPL-3.0-only
// linht-shelld: runs one UI app at a time, routes the keypad and applies the display and keypad
// settings (architecture §5, §6; interfaces/org.m17project.LinHT.Shell1.xml).
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>
#include <time.h>
#include <unistd.h>

#include "apps.h"
#include "fg.h"
#include "keys.h"

#define SHELL "org.m17project.LinHT.Shell"
#define SHELL_PATH "/org/m17project/LinHT/Shell"
#define SHELL_IFACE "org.m17project.LinHT.Shell1"
#define SHELL_ERR SHELL_IFACE ".Error."
#define SETTINGS "org.m17project.LinHT.Settings"
#define SETTINGS_PATH "/org/m17project/LinHT/Settings"
#define SETTINGS_IFACE "org.m17project.LinHT.Settings1"
#define SYSTEMD "org.freedesktop.systemd1"
#define SYSTEMD_PATH "/org/freedesktop/systemd1"
#define MANAGER SYSTEMD ".Manager"
#define APP_USER "linht" // transient app units run as the on-device UI user (D2)
#define KEYS_LINK "/dev/input/linht-keys"

struct shell {
	sd_event *ev;
	sd_bus *bus;
	struct app_list apps;
	struct key_router kr;
	struct fg fg;
	int keypad, uinput;
	sd_event_source *timer;
	char *job;                 // pending job of the foreground transition
	char *unit;                // unit of the app in the foreground (or being started)
	sd_bus_slot *unit_watch;   // its PropertiesChanged match
	char *startup_jobs[64];    // stop jobs of leftover app units; the home app starts when they are done
	size_t n_startup;
	const struct app *shown;   // last Foreground value we announced
	const char *backlight, *kbd_led, *home_fallback;
	int64_t brightness, kbd_brightness; // -1 until settingsd has told us
};

static uint64_t now_ms(struct shell *s)
{
	uint64_t us = 0;
	sd_event_now(s->ev, CLOCK_MONOTONIC, &us);
	return us / 1000;
}

// --- display and keypad backlight ---

static void write_attr(const char *dir, const char *attr, long v)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	FILE *f = fopen(path, "w");
	int ok = f && fprintf(f, "%ld", v) > 0;
	if ((f && fclose(f) != 0) || !ok)
		fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
}

static void screen(struct shell *s, bool on)
{
	if (on && s->brightness >= 0)
		write_attr(s->backlight, "brightness", s->brightness);
	write_attr(s->backlight, "bl_power", on ? 0 : 4); // FB_BLANK_UNBLANK, FB_BLANK_POWERDOWN
}

static void kbd(struct shell *s, bool on)
{
	if (s->kbd_brightness >= 0)
		write_attr(s->kbd_led, "brightness", on ? s->kbd_brightness : 0);
}

// --- foreground apps through systemd ---

static char *unit_of(const struct app *a)
{
	char *u = NULL;
	if (a->unit)
		u = strdup(a->unit);
	else if (asprintf(&u, "linht-app-%s.service", a->id) < 0)
		u = NULL;
	return u;
}

static void run(struct shell *s, struct fg_cmd c);

static struct fg_cmd request(struct shell *s, const struct app *a)
{
	return fg_request(&s->fg, a, kr_held(&s->kr, KEY_P));
}

static void announce(struct shell *s)
{
	const struct app *now = fg_foreground(&s->fg);
	if (now == s->shown)
		return;
	s->shown = now;
	sd_bus_emit_properties_changed(s->bus, SHELL_PATH, SHELL_IFACE, "Foreground", NULL);
}

static void watch(struct shell *s);

static void job_finished(struct shell *s, bool ok)
{
	free(s->job);
	s->job = NULL;
	bool was_running = fg_foreground(&s->fg) != NULL;
	struct fg_cmd c = fg_job_done(&s->fg, ok, now_ms(s));
	if (!was_running && fg_foreground(&s->fg))
		watch(s);
	run(s, c);
}

static int on_job_reply(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	struct shell *s = data;
	const char *job;
	if (sd_bus_message_is_method_error(m, NULL)) {
		const sd_bus_error *err = sd_bus_message_get_error(m);
		fprintf(stderr, "systemd: %s\n", err->message ? err->message : err->name);
		job_finished(s, false); // e.g. stopping a unit that is already gone: the stop is done
	} else if (sd_bus_message_read(m, "o", &job) >= 0) {
		free(s->job);
		s->job = strdup(job);
	}
	return 0;
}

// True while systemd has a job queued for our own unit, i.e. we are being stopped or restarted.
static bool going_away(struct shell *s)
{
	sd_bus_message *reply = NULL;
	uint32_t id = 0;
	const char *job;
	if (sd_bus_get_property(s->bus, SYSTEMD, SYSTEMD_PATH "/unit/self", SYSTEMD ".Unit", "Job", NULL, &reply, "(uo)") >= 0)
		sd_bus_message_read(reply, "(uo)", &id, &job);
	sd_bus_message_unref(reply);
	return id != 0;
}

static void ended(struct shell *s, const char *state)
{
	if (fg_foreground(&s->fg) && (strcmp(state, "inactive") == 0 || strcmp(state, "failed") == 0)) {
		fprintf(stderr, "%s ended (%s)\n", s->unit, state);
		// Stopped along with us (BindsTo=, shutdown): starting it again would cancel our own stop job.
		if (!going_away(s))
			run(s, fg_ended(&s->fg, now_ms(s)));
	}
}

static int on_unit_changed(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	const char *iface, *name, *state;
	if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, SYSTEMD ".Unit") != 0 ||
	    sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		if (sd_bus_message_read(m, "s", &name) < 0)
			return 0;
		if (strcmp(name, "ActiveState") == 0 && sd_bus_message_read(m, "v", "s", &state) >= 0)
			ended(data, state);
		else
			sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	return 0;
}

static int on_state_reply(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	const char *state;
	if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read(m, "v", "s", &state) >= 0)
		ended(data, state);
	return 0;
}

// Notices when the foreground app exits by itself.
static void watch(struct shell *s)
{
	char *path = NULL;
	s->unit_watch = sd_bus_slot_unref(s->unit_watch);
	if (sd_bus_path_encode(SYSTEMD_PATH "/unit", s->unit, &path) < 0)
		return;
	sd_bus_match_signal_async(s->bus, &s->unit_watch, SYSTEMD, path, "org.freedesktop.DBus.Properties",
	                          "PropertiesChanged", on_unit_changed, NULL, s);
	// it may have exited before the match was in place
	sd_bus_call_method_async(s->bus, NULL, SYSTEMD, path, "org.freedesktop.DBus.Properties", "Get", on_state_reply, s,
	                         "ss", SYSTEMD ".Unit", "ActiveState");
	free(path);
}

static int start_transient(struct shell *s, const struct app *a, const char *unit)
{
	sd_bus_message *m = NULL;
	char *desc = NULL, *env = NULL;
	int r = asprintf(&desc, "LinHT app %s", a->name) < 0 || asprintf(&env, "LINHT_APP_ID=%s", a->id) < 0 ? -ENOMEM : 0;
	if (r >= 0)
		r = sd_bus_message_new_method_call(s->bus, &m, SYSTEMD, SYSTEMD_PATH, MANAGER, "StartTransientUnit");
	if (r >= 0)
		r = sd_bus_message_append(m, "ss", unit, "replace");
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "(sv)");
	if (r >= 0)
		r = sd_bus_message_append(m, "(sv)(sv)(sv)(sv)(sv)(sv)(sv)", "Description", "s", desc, //
		                          "ExecStart", "a(sasb)", 1, a->exec, 1, a->exec, 0,        //
		                          "User", "s", APP_USER,                                      //
		                          "SupplementaryGroups", "as", 2, "video", "input",          // DRM, linht-keys
		                          "Environment", "as", 1, env,                                //
		                          "CollectMode", "s", "inactive-or-failed",                   //
		                          "TimeoutStopUSec", "t", (uint64_t)2000000);                  // SIGTERM, 2 s, SIGKILL
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r >= 0)
		r = sd_bus_message_append(m, "a(sa(sv))", 0);
	if (r >= 0)
		r = sd_bus_call_async(s->bus, NULL, m, on_job_reply, s, 0);
	sd_bus_message_unref(m);
	free(desc);
	free(env);
	return r;
}

static void run(struct shell *s, struct fg_cmd c)
{
	int r = 0;
	char *unit = c.app ? unit_of(c.app) : NULL;
	switch (c.type) {
	case FG_NONE:
	case FG_REFUSED:
		break;
	case FG_STOP:
		s->unit_watch = sd_bus_slot_unref(s->unit_watch);
		r = sd_bus_call_method_async(s->bus, NULL, SYSTEMD, SYSTEMD_PATH, MANAGER, "StopUnit", on_job_reply, s, "ss",
		                             unit, "replace");
		break;
	case FG_START:
		fprintf(stderr, "starting %s\n", c.app->id);
		free(s->unit);
		s->unit = strdup(unit);
		r = c.app->unit ? sd_bus_call_method_async(s->bus, NULL, SYSTEMD, SYSTEMD_PATH, MANAGER, "StartUnit",
		                                           on_job_reply, s, "ss", unit, "replace")
		                : start_transient(s, c.app, unit);
		break;
	}
	free(unit);
	if (r < 0) {
		fprintf(stderr, "systemd call: %s\n", strerror(-r));
		job_finished(s, false);
		return;
	}
	announce(s);
}

static int on_job_removed(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	struct shell *s = data;
	uint32_t id;
	const char *job, *unit, *result;
	if (sd_bus_message_read(m, "uoss", &id, &job, &unit, &result) < 0)
		return 0;
	for (size_t i = 0; i < s->n_startup; i++) {
		if (strcmp(job, s->startup_jobs[i]) == 0) {
			free(s->startup_jobs[i]);
			s->startup_jobs[i] = s->startup_jobs[--s->n_startup];
			if (s->n_startup == 0)
				run(s, request(s, s->fg.home));
			return 0;
		}
	}
	if (s->job && strcmp(job, s->job) == 0) {
		job_finished(s, strcmp(result, "done") == 0);
	}
	return 0;
}

static int on_unit_removed(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	struct shell *s = data;
	const char *unit;
	if (sd_bus_message_read(m, "s", &unit) >= 0 && s->unit && strcmp(unit, s->unit) == 0)
		ended(s, "inactive");
	return 0;
}

// Stops app units left from an earlier shelld (they hold a stale keypad), then starts the home app.
static void startup(struct shell *s)
{
	for (size_t i = 0; i < s->apps.n && s->n_startup < sizeof(s->startup_jobs) / sizeof(*s->startup_jobs); i++) {
		char *unit = unit_of(&s->apps.apps[i]);
		const char *job;
		sd_bus_message *reply = NULL;
		// NoSuchUnit (a transient app that is not running) just fails the call
		if (unit && sd_bus_call_method(s->bus, SYSTEMD, SYSTEMD_PATH, MANAGER, "StopUnit", NULL, &reply, "ss", unit,
		                               "replace") >= 0 &&
		    sd_bus_message_read(reply, "o", &job) >= 0 && (s->startup_jobs[s->n_startup] = strdup(job)))
			s->n_startup++;
		sd_bus_message_unref(reply);
		free(unit);
	}
	if (!s->n_startup)
		run(s, request(s, s->fg.home));
}

// --- keypad ---

static void forward(struct shell *s, uint16_t code, int32_t value)
{
	struct input_event ev[2] = {{.type = EV_KEY, .code = code, .value = value}, {.type = EV_SYN, .code = SYN_REPORT}};
	if (write(s->uinput, ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		fprintf(stderr, "uinput: %s\n", strerror(errno));
}

static void go_home(struct shell *s)
{
	const struct app *launcher = s->fg.launcher, *now = fg_foreground(&s->fg);
	// D10: the gesture opens the launcher; from the launcher it returns to the home app
	run(s, request(s, !launcher || now == launcher ? s->fg.home : launcher));
}

static void reschedule(struct shell *s)
{
	int64_t d = kr_deadline(&s->kr);
	if (d < 0) {
		sd_event_source_set_enabled(s->timer, SD_EVENT_OFF);
		return;
	}
	sd_event_source_set_time(s->timer, (uint64_t)d * 1000);
	sd_event_source_set_enabled(s->timer, SD_EVENT_ONESHOT);
}

static void act(struct shell *s, const struct kr_out *o)
{
	for (int i = 0; i < o->n; i++) {
		const struct kr_action *a = &o->a[i];
		switch (a->type) {
		case KR_FORWARD: forward(s, a->code, a->value); break;
		case KR_HOME: go_home(s); break;
		case KR_SCREEN: screen(s, a->value); break;
		case KR_KBD: kbd(s, a->value); break;
		}
	}
	reschedule(s);
}

static int on_timer(sd_event_source *src, uint64_t usec, void *data)
{
	(void)src;
	(void)usec;
	struct shell *s = data;
	struct kr_out o = {0};
	kr_tick(&s->kr, now_ms(s), &o);
	act(s, &o);
	return 0;
}

static int on_keypad(sd_event_source *src, int fd, uint32_t revents, void *data)
{
	(void)src;
	(void)revents;
	struct shell *s = data;
	struct input_event ev;
	ssize_t n;
	while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;
		struct kr_out o = {0};
		kr_key(&s->kr, ev.code, ev.value, now_ms(s), &o);
		act(s, &o);
	}
	if (n < 0 && errno != EAGAIN) {
		fprintf(stderr, "keypad: %s\n", strerror(errno));
		return sd_event_exit(s->ev, 1); // systemd restarts us
	}
	return 0;
}

static int open_keys(struct shell *s, const char *keypad)
{
	uint8_t bits[KEY_CNT / 8 + 1] = {0};
	if ((s->keypad = open(keypad, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0 || ioctl(s->keypad, EVIOCGRAB, 1) < 0 ||
	    ioctl(s->keypad, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
		fprintf(stderr, "%s: %s\n", keypad, strerror(errno));
		return -errno;
	}
	struct uinput_setup us = {.id = {.bustype = BUS_VIRTUAL, .version = 1}};
	snprintf(us.name, sizeof(us.name), "linht-keys");
	// No EV_REP: gui_test treats every value other than a press, including a repeat (2), as a release,
	// so a held PTT would stop transmitting. LVGL repeats held keys itself.
	if ((s->uinput = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC)) < 0 ||
	    ioctl(s->uinput, UI_SET_EVBIT, EV_KEY) < 0)
		goto err;
	for (int code = 0; code < KEY_CNT; code++) // the same keys as the keypad, so toolkits see the same device
		if (((bits[code / 8] >> (code % 8)) & 1) && ioctl(s->uinput, UI_SET_KEYBIT, code) < 0)
			goto err;
	if (ioctl(s->uinput, UI_DEV_SETUP, &us) < 0 || ioctl(s->uinput, UI_DEV_CREATE) < 0)
		goto err;
	for (int i = 0; i < 100 && access(KEYS_LINK, F_OK) != 0; i++) // udev creates the link (70-linht-keys.rules)
		usleep(20 * 1000);
	if (access(KEYS_LINK, F_OK) != 0)
		fprintf(stderr, "%s did not appear; apps will not get keys\n", KEYS_LINK);
	return 0;
err:
	fprintf(stderr, "/dev/uinput: %s\n", strerror(errno));
	return -errno;
}

// --- settings (docs/settings-schema.md) ---

// Reads one value of a{sv} for namespace ns and applies it.
static void apply(struct shell *s, const char *ns, const char *key, sd_bus_message *m)
{
	const char *contents = NULL, *str;
	int64_t v;
	sd_bus_message_peek_type(m, NULL, &contents);
	if (contents && strcmp(contents, "x") == 0 && sd_bus_message_read(m, "v", "x", &v) >= 0) {
		if (strcmp(ns, "system.display") == 0 && strcmp(key, "brightness") == 0) {
			s->brightness = v;
			if (s->kr.screen_on)
				screen(s, true);
		} else if (strcmp(ns, "system.display") == 0 && strcmp(key, "timeout") == 0) {
			s->kr.screen_timeout_ms = v > 0 ? (uint32_t)v * 1000 : 0;
		} else if (strcmp(ns, "system.keypad") == 0 && strcmp(key, "backlight_brightness") == 0) {
			s->kbd_brightness = v;
			if (s->kr.kbd_on)
				kbd(s, true);
		} else if (strcmp(ns, "system.keypad") == 0 && strcmp(key, "backlight_timeout") == 0) {
			s->kr.kbd_timeout_ms = v > 0 ? (uint32_t)v * 1000 : 0;
		}
	} else if (contents && strcmp(contents, "s") == 0 && sd_bus_message_read(m, "v", "s", &str) >= 0) {
		if (strcmp(ns, "system.shell") == 0 && strcmp(key, "home_app") == 0) {
			const struct app *h = apps_find(&s->apps, str);
			if (!h)
				fprintf(stderr, "home app %s is not installed\n", str);
			s->fg.home = h ? h : apps_find(&s->apps, s->home_fallback) ?: s->fg.launcher;
		}
	} else {
		sd_bus_message_skip(m, "v");
	}
}

static void apply_all(struct shell *s, const char *ns, sd_bus_message *m)
{
	const char *key;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		if (sd_bus_message_read(m, "s", &key) >= 0)
			apply(s, ns, key, m);
		sd_bus_message_exit_container(m);
	}
	reschedule(s);
}

static int on_settings_changed(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	const char *ns;
	if (sd_bus_message_read(m, "s", &ns) >= 0)
		apply_all(data, ns, m);
	return 0;
}

static void read_settings(struct shell *s)
{
	static const char *const ns[] = {"system.display", "system.keypad", "system.shell"};
	for (size_t i = 0; i < sizeof(ns) / sizeof(*ns); i++) {
		sd_bus_message *reply = NULL;
		sd_bus_error err = SD_BUS_ERROR_NULL;
		if (sd_bus_call_method(s->bus, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "GetAll", &err, &reply, "s", ns[i]) >= 0)
			apply_all(s, ns[i], reply);
		else // without settingsd the hardware stays as it is and nothing times out, until it appears
			fprintf(stderr, "settings %s: %s\n", ns[i], err.message ? err.message : "unavailable");
		sd_bus_error_free(&err);
		sd_bus_message_unref(reply);
	}
}

static int on_settings_owner(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	const char *name, *old, *new;
	if (sd_bus_message_read(m, "sss", &name, &old, &new) >= 0 && *new)
		read_settings(data); // settingsd (re)started
	return 0;
}

// --- org.m17project.LinHT.Shell1 ---

static int m_list(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	struct shell *s = data;
	sd_bus_message *reply = NULL;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0)
		r = sd_bus_message_open_container(reply, 'a', "a{sv}");
	for (size_t i = 0; r >= 0 && i < s->apps.n; i++) {
		const struct app *a = &s->apps.apps[i];
		r = sd_bus_message_append(reply, "a{sv}", 5, "id", "s", a->id, "name", "s", a->name, "category", "s",
		                          a->category, "icon_small", "s", a->icon_small ?: "", "icon_large", "s",
		                          a->icon_large ?: "");
	}
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int launch(struct shell *s, sd_bus_message *m, const struct app *a, sd_bus_error *e)
{
	if (!a)
		return sd_bus_error_set(e, SHELL_ERR "UnknownApp", "no such app"), -ENOENT;
	struct fg_cmd c = request(s, a);
	if (c.type == FG_REFUSED)
		return sd_bus_error_set(e, SHELL_ERR "LaunchFailed", "PTT is held"), -EBUSY;
	run(s, c);
	return sd_bus_reply_method_return(m, NULL);
}

static int m_launch(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct shell *s = data;
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	return r < 0 ? r : launch(s, m, apps_find(&s->apps, id), e);
}

static int m_home(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct shell *s = data;
	return launch(s, m, s->fg.home, e);
}

static int p_foreground(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                        void *data, sd_bus_error *e)
{
	(void)bus, (void)path, (void)iface, (void)prop, (void)e;
	const struct app *a = fg_foreground(&((struct shell *)data)->fg);
	return sd_bus_message_append(reply, "s", a ? a->id : "");
}

#define U SD_BUS_VTABLE_UNPRIVILEGED
static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_NAMES("ListApps", "", "", "aa{sv}", SD_BUS_PARAM(apps), m_list, U),
	SD_BUS_METHOD_WITH_NAMES("Launch", "s", SD_BUS_PARAM(app_id), "", "", m_launch, U),
	SD_BUS_METHOD_WITH_NAMES("Home", "", "", "", "", m_home, U),
	SD_BUS_PROPERTY("Foreground", "s", p_foreground, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_VTABLE_END,
};

int main(int argc, char **argv)
{
	struct shell s = {.keypad = -1, .uinput = -1, .brightness = -1, .kbd_brightness = -1};
	const char *apps_dir = "/usr/lib/linht/apps", *keypad = "/dev/input/keypad";
	const char *launcher_id = "org.m17project.linht.launcher";
	s.backlight = "/sys/class/backlight/backlight-lcd";
	s.kbd_led = "/sys/class/leds/linht::kbd_backlight";
	s.home_fallback = "org.m17project.linht.radio"; // used only while settingsd is unavailable
	static const struct option opts[] = {{"apps", required_argument, NULL, 'a'}, {"keypad", required_argument, NULL, 'k'},
	                                     {"help", no_argument, NULL, 'h'}, {0}};
	for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
		if (c == 'a')
			apps_dir = optarg;
		else if (c == 'k')
			keypad = optarg;
		else {
			fputs("usage: linht-shelld [--apps DIR] [--keypad DEVICE]\n", c == 'h' ? stdout : stderr);
			return c == 'h' ? 0 : 2;
		}
	}

	int r = apps_load(apps_dir, &s.apps);
	if (r < 0)
		fprintf(stderr, "%s: %s\n", apps_dir, strerror(-r)); // keep going: keys and backlight still work
	fg_init(&s.fg, apps_find(&s.apps, s.home_fallback), apps_find(&s.apps, launcher_id));
	if (!s.fg.home)
		s.fg.home = s.fg.launcher;

	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, SIGTERM);
	sigaddset(&ss, SIGINT);
	const char *step = "keypad";
	r = open_keys(&s, keypad);
	if (r >= 0 && (step = "event loop"))
		r = sigprocmask(SIG_BLOCK, &ss, NULL) < 0 ? -errno : sd_event_default(&s.ev);
	if (r >= 0)
		r = sd_event_add_signal(s.ev, NULL, SIGTERM, NULL, NULL);
	if (r >= 0)
		r = sd_event_add_signal(s.ev, NULL, SIGINT, NULL, NULL);
	if (r >= 0)
		r = sd_event_add_io(s.ev, NULL, s.keypad, EPOLLIN, on_keypad, &s);
	if (r >= 0)
		r = sd_event_add_time(s.ev, &s.timer, CLOCK_MONOTONIC, UINT64_MAX, 0, on_timer, &s);
	if (r >= 0 && (step = "system bus"))
		r = sd_bus_open_system(&s.bus);
	if (r >= 0)
		r = sd_bus_attach_event(s.bus, s.ev, 0);
	if (r >= 0 && (step = "systemd signals"))
		r = sd_bus_match_signal(s.bus, NULL, SYSTEMD, SYSTEMD_PATH, MANAGER, "JobRemoved", on_job_removed, &s);
	if (r >= 0)
		r = sd_bus_match_signal(s.bus, NULL, SYSTEMD, SYSTEMD_PATH, MANAGER, "UnitRemoved", on_unit_removed, &s);
	if (r >= 0)
		r = sd_bus_call_method(s.bus, SYSTEMD, SYSTEMD_PATH, MANAGER, "Subscribe", NULL, NULL, "");
	if (r >= 0 && (step = "settings signals"))
		r = sd_bus_match_signal(s.bus, NULL, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "Changed", on_settings_changed, &s);
	if (r >= 0) // only settingsd's name: every other bus client would trigger a blocking re-read
		r = sd_bus_add_match(s.bus, NULL,
		                     "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
		                     "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" SETTINGS "'",
		                     on_settings_owner, &s);
	if (r >= 0) {
		kr_init(&s.kr, now_ms(&s));
		read_settings(&s);
		screen(&s, true);
		kbd(&s, true);
		step = "exporting " SHELL;
		r = sd_bus_add_object_vtable(s.bus, NULL, SHELL_PATH, SHELL_IFACE, vtable, &s);
	}
	if (r >= 0)
		r = sd_bus_request_name(s.bus, SHELL, 0);
	if (r >= 0) {
		sd_notify(0, "READY=1");
		startup(&s);
		step = "running";
		r = sd_event_loop(s.ev);
	}
	if (r < 0)
		fprintf(stderr, "linht-shelld: %s: %s\n", step, strerror(-r));

	screen(&s, true); // never leave the radio with a dark screen and nobody to wake it
	kbd(&s, true);
	if (s.uinput >= 0) {
		ioctl(s.uinput, UI_DEV_DESTROY);
		close(s.uinput);
	}
	if (s.keypad >= 0)
		close(s.keypad);
	sd_bus_flush_close_unref(s.bus);
	sd_event_source_unref(s.timer);
	sd_event_unref(s.ev);
	apps_free(&s.apps);
	free(s.job);
	free(s.unit);
	for (size_t i = 0; i < s.n_startup; i++)
		free(s.startup_jobs[i]);
	return r < 0 || r == 1 ? 1 : 0;
}

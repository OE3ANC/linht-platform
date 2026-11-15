// SPDX-License-Identifier: GPL-3.0-only
#include "linht-ui.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SHELL "org.m17project.LinHT.Shell"
#define SHELL_PATH "/org/m17project/LinHT/Shell"
#define SHELL_IFACE "org.m17project.LinHT.Shell1"

static lv_obj_t *title_label, *clock_label;
static lv_indev_read_cb_t keypad_read;
static volatile sig_atomic_t quit;

int linht_bus_open(sd_bus **bus)
{
	const char *b = getenv("LINHT_BUS");
	int r = b && strcmp(b, "session") == 0 ? sd_bus_open_user(bus) : sd_bus_open_system(bus);
	if (r < 0)
		fprintf(stderr, "D-Bus: %s\n", strerror(-r));
	return r;
}

static uint32_t tick_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// The keypad has no Tab: UP/DOWN move the focus. LEFT/RIGHT/ENTER/ESC go to the focused object.
static void remap_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	keypad_read(indev, data);
	if (data->key == LV_KEY_UP)
		data->key = LV_KEY_PREV;
	else if (data->key == LV_KEY_DOWN)
		data->key = LV_KEY_NEXT;
}

static void update_clock(lv_timer_t *t)
{
	(void)t;
	char buf[8];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);
	lv_label_set_text(clock_label, buf);
}

lv_obj_t *linht_ui_init(const char *title)
{
	lv_init();
	lv_tick_set_cb(tick_ms);
#ifdef LINHT_SIM
	lv_display_t *disp = lv_sdl_window_create(LINHT_W, LINHT_H);
	lv_sdl_window_set_zoom(disp, 4);
	lv_sdl_window_set_title(disp, title);
	lv_indev_t *keys = lv_sdl_keyboard_create();
#else
	lv_display_t *disp = lv_linux_drm_create();
	// LVGL 9.1 creates the DRM display as 800x480 and hands it the panel-sized buffers before setting the
	// real resolution, so its buffer-size assertion fails and halts (while(1)) on any smaller panel.
	lv_display_set_resolution(disp, LINHT_W, LINHT_H);
	lv_linux_drm_set_file(disp, "/dev/dri/linht-lcd", -1); // ui/70-linht-lcd.rules
	lv_indev_t *keys = lv_evdev_create(LV_INDEV_TYPE_KEYPAD, "/dev/input/linht-keys");
#endif
	if (!keys) { // LVGL logging is compiled out on the radio, so say it here
		perror("keypad /dev/input/linht-keys");
		exit(1); // an app without keys can't even be left; shelld returns to the launcher or home
	}
	keypad_read = lv_indev_get_read_cb(keys);
	lv_indev_set_read_cb(keys, remap_read);
	lv_group_t *g = lv_group_create();
	lv_group_set_default(g);
	lv_indev_set_group(keys, g);

	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
	                                                 lv_palette_main(LV_PALETTE_ORANGE), true, LV_FONT_DEFAULT));
	lv_obj_t *scr = lv_screen_active();
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *bar = lv_obj_create(scr);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LINHT_W, LINHT_BAR_H);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x202020), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(bar, 3, 0);
	title_label = lv_label_create(bar);
	lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, 0);
	clock_label = lv_label_create(bar);
	lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, 0, 0);
	linht_ui_set_title(title);
	update_clock(NULL);
	lv_timer_create(update_clock, 5000, NULL); // shortcut: time only; battery/GNSS/TX come with hwd and radiod (phase 2)

	lv_obj_t *content = lv_obj_create(scr);
	lv_obj_remove_style_all(content);
	lv_obj_set_pos(content, 0, LINHT_BAR_H);
	lv_obj_set_size(content, LINHT_W, LINHT_H - LINHT_BAR_H);
	return content;
}

void linht_ui_set_title(const char *title) { lv_label_set_text(title_label, title); }

lv_obj_t *linht_ui_list(lv_obj_t *parent)
{
	lv_obj_t *list = lv_list_create(parent);
	lv_obj_set_size(list, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_row(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	return list;
}

lv_obj_t *linht_ui_list_add(lv_obj_t *list, const char *text)
{
	lv_obj_t *row = lv_list_add_button(list, NULL, text);
	lv_obj_set_style_pad_ver(row, 2, 0); // 7 rows of 16 px fit the content area
	lv_obj_set_style_pad_hor(row, 4, 0);
	return row;
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

int linht_ui_run(sd_bus *bus)
{
	struct sigaction sa = {.sa_handler = on_signal};
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	while (!quit) {
		uint32_t ms = lv_timer_handler();
		while (bus && sd_bus_process(bus, NULL) > 0)
			;
		if (ms > 20) // the keypad and SDL are polled by LVGL timers
			ms = 20;
		if (bus)
			sd_bus_wait(bus, (uint64_t)ms * 1000); // returns early on a signal
		else
			lv_delay_ms(ms);
	}
	return 0;
}

void linht_ui_launch(sd_bus *bus, const char *app_id)
{
	int r = sd_bus_call_method_async(bus, NULL, SHELL, SHELL_PATH, SHELL_IFACE, "Launch", NULL, NULL, "s", app_id);
	if (r < 0)
		fprintf(stderr, "Launch %s: %s\n", app_id, strerror(-r));
}

void linht_ui_home(sd_bus *bus)
{
	int r = sd_bus_call_method_async(bus, NULL, SHELL, SHELL_PATH, SHELL_IFACE, "Home", NULL, NULL, "");
	if (r < 0)
		fprintf(stderr, "Home: %s\n", strerror(-r));
}

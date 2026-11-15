// SPDX-License-Identifier: GPL-3.0-only
// linht-ui: what every LinHT UI app needs (architecture §6.3). The display (DRM on the radio, SDL in
// the simulator), /dev/input/linht-keys, the theme, the status bar, and one loop for LVGL and D-Bus.
#pragma once
#include <lvgl.h>
#include <systemd/sd-bus.h>

#define LINHT_W 160
#define LINHT_H 128
#define LINHT_BAR_H 16 // the system status bar at the top (D1)
#define LINHT_LAUNCHER "org.m17project.linht.launcher"

// The session bus if LINHT_BUS=session (simulator), otherwise the system bus.
int linht_bus_open(sd_bus **bus);

// Sets up display, keypad, theme and status bar. Returns the 160x112 content area below the bar.
lv_obj_t *linht_ui_init(const char *title);
void linht_ui_set_title(const char *title);

// A compact list filling parent, and a row in it; rows join the keypad focus group.
lv_obj_t *linht_ui_list(lv_obj_t *parent);
lv_obj_t *linht_ui_list_add(lv_obj_t *list, const char *text);

// Runs LVGL and the bus until SIGTERM or SIGINT. Returns 0.
int linht_ui_run(sd_bus *bus);

// Shell1.Launch / Shell1.Home (interfaces/org.m17project.LinHT.Shell1.xml).
void linht_ui_launch(sd_bus *bus, const char *app_id);
void linht_ui_home(sd_bus *bus);

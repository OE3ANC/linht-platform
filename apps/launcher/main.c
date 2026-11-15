// SPDX-License-Identifier: GPL-3.0-only
// The launcher (architecture §6.4, D10): the installed apps; ENTER opens one, ESC goes back home.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linht-ui.h"

static sd_bus *bus;

static void on_click(lv_event_t *e) { linht_ui_launch(bus, lv_event_get_user_data(e)); }

static void on_key(lv_event_t *e)
{
	if (lv_event_get_key(e) == LV_KEY_ESC)
		linht_ui_home(bus);
}

// Reads one app of ListApps (a{sv}) into id and name; both are borrowed from the message.
static int read_app(sd_bus_message *m, const char **id, const char **name)
{
	const char *key;
	int r;
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		if ((r = sd_bus_message_read(m, "s", &key)) < 0)
			return r;
		if (strcmp(key, "id") == 0)
			r = sd_bus_message_read(m, "v", "s", id);
		else if (strcmp(key, "name") == 0)
			r = sd_bus_message_read(m, "v", "s", name);
		else
			r = sd_bus_message_skip(m, "v");
		if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0)
			return r;
	}
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

static void add_apps(lv_obj_t *list)
{
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	size_t shown = 0;
	// shortcut: Shell1.ListApps through sd-bus directly, until liblinht wraps it (phase 1, D3, D19)
	int r = sd_bus_call_method(bus, "org.m17project.LinHT.Shell", "/org/m17project/LinHT/Shell",
	                           "org.m17project.LinHT.Shell1", "ListApps", &err, &reply, "");
	if (r >= 0)
		r = sd_bus_message_enter_container(reply, 'a', "a{sv}");
	while (r >= 0 && (r = sd_bus_message_peek_type(reply, NULL, NULL)) > 0) {
		const char *id = NULL, *name = NULL;
		if ((r = read_app(reply, &id, &name)) < 0)
			break;
		if (!id || !name || strcmp(id, LINHT_LAUNCHER) == 0)
			continue;
		lv_obj_t *row = linht_ui_list_add(list, name);
		lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, strdup(id)); // lives as long as the process
		lv_obj_add_event_cb(row, on_key, LV_EVENT_KEY, NULL);
		shown++;
	}
	if (r < 0)
		fprintf(stderr, "ListApps: %s\n", err.message ? err.message : strerror(-r));
	if (!shown) { // ESC must still go home
		lv_obj_t *row = linht_ui_list_add(list, "No apps installed");
		lv_obj_add_event_cb(row, on_key, LV_EVENT_KEY, NULL);
	}
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
}

int main(void)
{
	if (linht_bus_open(&bus) < 0)
		return 1;
	add_apps(linht_ui_list(linht_ui_init("Apps")));
	return linht_ui_run(bus);
}

// SPDX-License-Identifier: GPL-3.0-only
// The Settings app (D19): every namespace and key, generated from the schemas (Settings1.Describe).
// LEFT/RIGHT change a value, ENTER toggles a switch, ESC goes back.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linht-ui.h"
#include "value.h"

// shortcut: Settings1 through sd-bus directly, until liblinht wraps it (phase 1, D3, D19)
#define SETTINGS "org.m17project.LinHT.Settings"
#define SETTINGS_PATH "/org/m17project/LinHT/Settings"
#define SETTINGS_IFACE "org.m17project.LinHT.Settings1"

static sd_bus *bus;
static lv_obj_t *content;
static char **ns_name, **ns_title; // all namespaces, read once
static size_t n_ns;

static struct view { // the namespace on screen; ns < 0 on the namespace list
	ssize_t ns;
	struct setting *key;
	lv_obj_t **value;
	size_t n;
} view = {.ns = -1};

static void free_view(void)
{
	for (size_t i = 0; i < view.n; i++) {
		free(view.key[i].key);
		free(view.key[i].label);
		free(view.key[i].unit);
		free(view.key[i].choices);
		free(view.key[i].text);
	}
	free(view.key);
	free(view.value);
	view = (struct view){.ns = -1};
}

// --- Describe / GetAll / Changed ---

// m is at a variant holding the value of s.
static int read_value(sd_bus_message *m, struct setting *s)
{
	const char *contents = NULL, *str;
	char buf[32];
	int b, r = sd_bus_message_peek_type(m, NULL, &contents);
	double d;
	if (r < 0 || !contents)
		return r;
	if (strcmp(contents, "b") == 0 && (r = sd_bus_message_read(m, "v", "b", &b)) >= 0)
		s->b = b;
	else if (strcmp(contents, "x") == 0)
		r = sd_bus_message_read(m, "v", "x", &s->i);
	else if (strcmp(contents, "s") == 0 && (r = sd_bus_message_read(m, "v", "s", &str)) >= 0) {
		free(s->text);
		s->text = strdup(str);
	} else if (strcmp(contents, "d") == 0 && (r = sd_bus_message_read(m, "v", "d", &d)) >= 0) {
		snprintf(buf, sizeof(buf), "%g", d);
		free(s->text);
		s->text = strdup(buf);
	} else
		r = sd_bus_message_skip(m, "v");
	return r;
}

static int read_choices(sd_bus_message *m, struct setting *s)
{
	int r = sd_bus_message_enter_container(m, 'v', "av");
	if (r >= 0)
		r = sd_bus_message_enter_container(m, 'a', "v");
	while (r >= 0 && (r = sd_bus_message_peek_type(m, NULL, NULL)) > 0) {
		const char *contents = NULL;
		int64_t v;
		if ((r = sd_bus_message_peek_type(m, NULL, &contents)) < 0)
			break;
		if (contents && strcmp(contents, "x") == 0 && (r = sd_bus_message_read(m, "v", "x", &v)) >= 0) {
			int64_t *c = realloc(s->choices, (s->n_choices + 1) * sizeof(*c));
			if (!c)
				return -1;
			s->choices = c;
			s->choices[s->n_choices++] = v;
		} else if (r >= 0)
			r = sd_bus_message_skip(m, "v"); // choices of other types: shown, not editable yet
	}
	if (r >= 0 && (r = sd_bus_message_exit_container(m)) >= 0)
		r = sd_bus_message_exit_container(m);
	return r;
}

// One key of Describe: (s name, a{sv} properties).
static int read_key(sd_bus_message *m, struct setting *s)
{
	const char *name, *str, *prop;
	int r = sd_bus_message_enter_container(m, 'r', "sa{sv}");
	if (r >= 0 && (r = sd_bus_message_read(m, "s", &name)) >= 0)
		s->key = strdup(name);
	s->type = ST_OTHER;
	s->step = 1;
	if (r >= 0)
		r = sd_bus_message_enter_container(m, 'a', "{sv}");
	while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		struct setting n = {0};
		if ((r = sd_bus_message_read(m, "s", &prop)) < 0)
			break;
		if (strcmp(prop, "type") == 0 || strcmp(prop, "label") == 0 || strcmp(prop, "unit") == 0) {
			if ((r = sd_bus_message_read(m, "v", "s", &str)) < 0)
				break;
			if (prop[0] == 't')
				s->type = strcmp(str, "bool") == 0 ? ST_BOOL : strcmp(str, "int") == 0 ? ST_INT : ST_OTHER;
			else if (prop[0] == 'l')
				s->label = strdup(str);
			else
				s->unit = strdup(str);
		} else if (strcmp(prop, "choices") == 0) {
			r = read_choices(m, s);
		} else if (strcmp(prop, "min") == 0 || strcmp(prop, "max") == 0 || strcmp(prop, "step") == 0) {
			const char *contents = NULL;
			if ((r = sd_bus_message_peek_type(m, NULL, &contents)) < 0)
				break;
			if (!contents || strcmp(contents, "x") != 0) { // float bounds: the key is read-only here
				r = sd_bus_message_skip(m, "v");
			} else if ((r = read_value(m, &n)) >= 0) {
				if (prop[1] == 'i')
					s->has_min = true, s->min = n.i;
				else if (prop[1] == 'a')
					s->has_max = true, s->max = n.i;
				else
					s->step = n.i;
			}
		} else {
			r = sd_bus_message_skip(m, "v");
		}
		if (r >= 0)
			r = sd_bus_message_exit_container(m);
	}
	if (r >= 0 && (r = sd_bus_message_exit_container(m)) >= 0)
		r = sd_bus_message_exit_container(m);
	return r;
}

static void show_value(size_t i)
{
	char buf[64];
	setting_format(&view.key[i], buf, sizeof(buf));
	lv_label_set_text(view.value[i], buf);
}

static int on_changed(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)data, (void)e;
	const char *ns, *key;
	if (view.ns < 0 || sd_bus_message_read(m, "s", &ns) < 0 || strcmp(ns, ns_name[view.ns]) != 0 ||
	    sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		if (sd_bus_message_read(m, "s", &key) < 0)
			return 0;
		size_t i = 0;
		while (i < view.n && strcmp(view.key[i].key, key) != 0)
			i++;
		if (i < view.n && read_value(m, &view.key[i]) >= 0)
			show_value(i);
		else if (i == view.n)
			sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	return 0;
}

static int on_set_reply(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)data, (void)e;
	const sd_bus_error *err = sd_bus_message_get_error(m);
	if (err) // the row keeps its value: it only changes when Changed arrives
		fprintf(stderr, "Set: %s\n", err->message ? err->message : err->name);
	return 0;
}

// --- screens ---

static void show_namespaces(void *unused);

static void change(size_t i, int dir)
{
	struct setting *s = &view.key[i];
	int64_t v;
	bool b;
	if (!setting_step(s, dir, &v, &b))
		return;
	const char *ns = ns_name[view.ns];
	int r = s->type == ST_BOOL ? sd_bus_call_method_async(bus, NULL, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "Set",
	                                                      on_set_reply, NULL, "ssv", ns, s->key, "b", (int)b)
	                           : sd_bus_call_method_async(bus, NULL, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "Set",
	                                                      on_set_reply, NULL, "ssv", ns, s->key, "x", v);
	if (r < 0)
		fprintf(stderr, "Set %s.%s: %s\n", ns, s->key, strerror(-r));
}

static void on_key_row(lv_event_t *e)
{
	size_t i = (uintptr_t)lv_event_get_user_data(e);
	uint32_t key = lv_event_get_key(e);
	if (key == LV_KEY_ESC)
		lv_async_call(show_namespaces, NULL); // not from inside the event of a row we are about to delete
	else if (i < view.n && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT))
		change(i, key == LV_KEY_RIGHT ? 1 : -1);
}

static void on_click_row(lv_event_t *e)
{
	size_t i = (uintptr_t)lv_event_get_user_data(e);
	if (i < view.n && view.key[i].type == ST_BOOL)
		change(i, 1);
}

static void show_keys(void *arg)
{
	size_t ns = (uintptr_t)arg;
	sd_bus_message *desc = NULL, *values = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *title;
	free_view();
	lv_obj_clean(content);
	lv_obj_t *list = linht_ui_list(content);
	int r = sd_bus_call_method(bus, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "Describe", &err, &desc, "s", ns_name[ns]);
	if (r >= 0)
		r = sd_bus_message_read(desc, "s", &title);
	if (r >= 0)
		r = sd_bus_message_enter_container(desc, 'a', "(sa{sv})");
	while (r >= 0 && (r = sd_bus_message_peek_type(desc, NULL, NULL)) > 0) {
		struct setting *k = realloc(view.key, (view.n + 1) * sizeof(*k));
		if (!k)
			break;
		view.key = k;
		view.key[view.n] = (struct setting){0};
		if ((r = read_key(desc, &view.key[view.n++])) < 0)
			break;
	}
	view.ns = (ssize_t)ns;
	if (r >= 0)
		r = sd_bus_call_method(bus, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "GetAll", &err, &values, "s", ns_name[ns]);
	if (r >= 0)
		r = sd_bus_message_enter_container(values, 'a', "{sv}");
	while (r >= 0 && (r = sd_bus_message_enter_container(values, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(values, "s", &key)) < 0)
			break;
		size_t i = 0;
		while (i < view.n && strcmp(view.key[i].key, key) != 0)
			i++;
		r = i < view.n ? read_value(values, &view.key[i]) : sd_bus_message_skip(values, "v");
		if (r >= 0)
			r = sd_bus_message_exit_container(values);
	}
	if (r < 0)
		fprintf(stderr, "%s: %s\n", ns_name[ns], err.message ? err.message : strerror(-r));

	linht_ui_set_title(ns_title[ns]);
	view.value = calloc(view.n ? view.n : 1, sizeof(*view.value));
	for (size_t i = 0; view.value && i < view.n; i++) {
		lv_obj_t *row = linht_ui_list_add(list, view.key[i].label ? view.key[i].label : view.key[i].key);
		view.value[i] = lv_label_create(row); // right of the label, which takes the remaining width
		show_value(i);
		lv_obj_add_event_cb(row, on_key_row, LV_EVENT_KEY, (void *)(uintptr_t)i);
		lv_obj_add_event_cb(row, on_click_row, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
	}
	if (!view.n) { // ESC must still lead back
		lv_obj_t *row = linht_ui_list_add(list, "Unavailable");
		lv_obj_add_event_cb(row, on_key_row, LV_EVENT_KEY, (void *)(uintptr_t)SIZE_MAX);
	}
	sd_bus_error_free(&err);
	sd_bus_message_unref(desc);
	sd_bus_message_unref(values);
}

static void on_click_ns(lv_event_t *e) { lv_async_call(show_keys, lv_event_get_user_data(e)); }

static void on_key_ns(lv_event_t *e)
{
	if (lv_event_get_key(e) == LV_KEY_ESC)
		linht_ui_launch(bus, LINHT_LAUNCHER);
}

static void show_namespaces(void *unused)
{
	(void)unused;
	free_view();
	lv_obj_clean(content);
	linht_ui_set_title("Settings");
	lv_obj_t *list = linht_ui_list(content);
	for (size_t i = 0; i < n_ns; i++) {
		lv_obj_t *row = linht_ui_list_add(list, ns_title[i]);
		lv_obj_add_event_cb(row, on_click_ns, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
		lv_obj_add_event_cb(row, on_key_ns, LV_EVENT_KEY, NULL);
	}
	if (!n_ns) {
		lv_obj_t *row = linht_ui_list_add(list, "Settings unavailable");
		lv_obj_add_event_cb(row, on_key_ns, LV_EVENT_KEY, NULL);
	}
}

static void load_namespaces(void)
{
	sd_bus_message *reply = NULL;
	char **names = NULL;
	if (sd_bus_call_method(bus, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "ListNamespaces", NULL, &reply, "") < 0 ||
	    sd_bus_message_read_strv(reply, &names) < 0) {
		fprintf(stderr, "ListNamespaces failed\n");
		sd_bus_message_unref(reply);
		return;
	}
	sd_bus_message_unref(reply);
	for (n_ns = 0; names[n_ns]; n_ns++)
		;
	ns_name = names;
	ns_title = calloc(n_ns ? n_ns : 1, sizeof(*ns_title));
	for (size_t i = 0; ns_title && i < n_ns; i++) {
		const char *title;
		ns_title[i] = NULL;
		if (sd_bus_call_method(bus, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "Describe", NULL, &reply, "s", names[i]) >= 0 &&
		    sd_bus_message_read(reply, "s", &title) >= 0)
			ns_title[i] = strdup(title);
		if (!ns_title[i])
			ns_title[i] = strdup(names[i]);
		sd_bus_message_unref(reply);
		reply = NULL;
	}
}

int main(void)
{
	if (linht_bus_open(&bus) < 0)
		return 1;
	content = linht_ui_init("Settings");
	sd_bus_match_signal(bus, NULL, SETTINGS, SETTINGS_PATH, SETTINGS_IFACE, "Changed", on_changed, NULL);
	load_namespaces();
	show_namespaces(NULL);
	return linht_ui_run(bus);
}

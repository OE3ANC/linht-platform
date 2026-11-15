// SPDX-License-Identifier: GPL-3.0-only
// linht-settingsd: owns the settings store and exports org.m17project.LinHT.Settings1.
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-event.h>

#include "settingsd.h"

static bool ends_with(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);
	return a > b && strcmp(s + a - b, suffix) == 0;
}

// Loads every <namespace>.yaml in dir. A bad schema is skipped and logged; the others still load.
static int load(struct settingsd *d, const char *schemas, const char *store)
{
	struct dirent **ent;
	int n = scandir(schemas, &ent, NULL, alphasort);
	if (n < 0) {
		fprintf(stderr, "%s: %s\n", schemas, strerror(errno));
		return -errno;
	}
	d->schemas = calloc(n ? n : 1, sizeof(*d->schemas));
	d->stores = calloc(n ? n : 1, sizeof(*d->stores));
	int r = d->schemas && d->stores ? 0 : -ENOMEM;
	for (int i = 0; i < n; i++) {
		const char *name = ent[i]->d_name;
		char path[4096], err[512];
		if (r == 0 && ends_with(name, ".yaml")) {
			snprintf(path, sizeof(path), "%s/%s", schemas, name);
			struct lschema *s = lschema_load(path, err, sizeof(err));
			if (!s) {
				fprintf(stderr, "skipping schema %s: %s\n", path, err);
			} else if (strlen(name) != strlen(s->ns) + 5 || strncmp(name, s->ns, strlen(s->ns)) != 0) {
				fprintf(stderr, "skipping schema %s: file name must be %s.yaml\n", path, s->ns);
				lschema_free(s);
			} else if (!(d->stores[d->n] = lstore_open(s, store))) {
				lschema_free(s);
				r = -ENOMEM;
			} else {
				d->schemas[d->n++] = s;
			}
		}
		free(ent[i]);
	}
	free(ent);
	return r;
}

static bool lookup_uid(const char *user, uid_t *uid)
{
	struct passwd *pw = getpwnam(user);
	if (pw)
		*uid = pw->pw_uid;
	return pw != NULL;
}

static void usage(FILE *f)
{
	fputs("usage: linht-settingsd [--session] [--schemas DIR] [--store DIR] [--device-user NAME] [--web-user NAME]\n", f);
}

int main(int argc, char **argv)
{
	const char *schemas = "/usr/share/linht/schemas", *store = "/var/lib/linht/settings";
	const char *device_user = "linht", *web_user = "linht-web";
	bool session = false;
	static const struct option opts[] = {
		{"session", no_argument, NULL, 'S'},          {"schemas", required_argument, NULL, 's'},
		{"store", required_argument, NULL, 'd'},      {"device-user", required_argument, NULL, 'u'},
		{"web-user", required_argument, NULL, 'w'},   {"help", no_argument, NULL, 'h'},
		{0},
	};
	for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
		switch (c) {
		case 'S': session = true; break;
		case 's': schemas = optarg; break;
		case 'd': store = optarg; break;
		case 'u': device_user = optarg; break;
		case 'w': web_user = optarg; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	struct settingsd d = {0};
	d.has_device_uid = lookup_uid(device_user, &d.device_uid);
	d.has_web_uid = lookup_uid(web_user, &d.web_uid);
	int r = load(&d, schemas, store);

	sd_event *ev = NULL;
	sd_bus *bus = NULL;
	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, SIGTERM);
	sigaddset(&ss, SIGINT);
	const char *step = "loading schemas";
	if (r >= 0 && (step = "blocking signals"))
		r = sigprocmask(SIG_BLOCK, &ss, NULL) < 0 ? -errno : 0;
	if (r >= 0 && (step = "setting up the event loop"))
		r = sd_event_default(&ev);
	if (r >= 0) // NULL handlers end the event loop
		r = sd_event_add_signal(ev, NULL, SIGTERM, NULL, NULL);
	if (r >= 0)
		r = sd_event_add_signal(ev, NULL, SIGINT, NULL, NULL);
	if (r >= 0 && (step = "connecting to the bus"))
		r = session ? sd_bus_open_user(&bus) : sd_bus_open_system(&bus);
	if (r >= 0 && (step = "exporting the interface"))
		r = settingsd_bus_add(&d, bus);
	if (r >= 0)
		r = sd_bus_attach_event(bus, ev, 0);
	if (r >= 0 && (step = "requesting " SETTINGS_BUS_NAME)) // last: clients only see a fully set up service
		r = sd_bus_request_name(bus, SETTINGS_BUS_NAME, 0);
	if (r >= 0 && (step = "running"))
		r = sd_event_loop(ev);
	if (r < 0)
		fprintf(stderr, "linht-settingsd: %s: %s\n", step, strerror(-r));

	sd_bus_flush_close_unref(bus);
	sd_event_unref(ev);
	for (size_t i = 0; i < d.n; i++) {
		lstore_free(d.stores[i]);
		lschema_free(d.schemas[i]);
	}
	free(d.stores);
	free(d.schemas);
	return r < 0 ? 1 : 0;
}

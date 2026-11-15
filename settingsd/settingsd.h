// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdbool.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>

#include "store.h"

#define SETTINGS_BUS_NAME "org.m17project.LinHT.Settings"

struct settingsd {
	struct lschema **schemas;
	struct lstore **stores; // stores[i] holds the values of schemas[i]
	size_t n;
	bool has_device_uid, has_web_uid; // on-device UI user, linht-web user (docs/settings-schema.md, Access)
	uid_t device_uid, web_uid;
};

// Exports org.m17project.LinHT.Settings1 on the bus.
int settingsd_bus_add(struct settingsd *d, sd_bus *bus);

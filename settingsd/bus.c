// SPDX-License-Identifier: GPL-3.0-only
// org.m17project.LinHT.Settings1: interfaces/org.m17project.LinHT.Settings1.xml, docs/settings-schema.md
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "settingsd.h"

#define OBJ "/org/m17project/LinHT/Settings"
#define IFACE "org.m17project.LinHT.Settings1"
#define ERR IFACE ".Error."

static const char *const SIG[] = {[LT_BOOL] = "b", [LT_INT] = "x", [LT_FLOAT] = "d", [LT_STRING] = "s"};

static int find_ns(struct settingsd *d, const char *ns, sd_bus_error *e, size_t *out)
{
	for (size_t i = 0; i < d->n; i++)
		if (strcmp(d->schemas[i]->ns, ns) == 0) {
			*out = i;
			return 0;
		}
	sd_bus_error_setf(e, ERR "UnknownNamespace", "unknown namespace '%s'", ns);
	return -ENOENT; // explicit, so callers (and the compiler) know *out is set whenever this returns 0
}

static int find_key(const struct lschema *s, const char *key, sd_bus_error *e, size_t *out)
{
	const struct lkey *k = lschema_key(s, key);
	if (!k) {
		sd_bus_error_setf(e, ERR "UnknownKey", "unknown key '%s' in %s", key, s->ns);
		return -ENOENT;
	}
	*out = k - s->keys;
	return 0;
}

// Appends v as a variant.
static int append_value(sd_bus_message *m, const struct lvalue *v)
{
	switch (v->type) {
	case LT_BOOL: return sd_bus_message_append(m, "v", "b", (int)v->b);
	case LT_INT: return sd_bus_message_append(m, "v", "x", v->i);
	case LT_FLOAT: return sd_bus_message_append(m, "v", "d", v->d);
	case LT_STRING: return sd_bus_message_append(m, "v", "s", v->s);
	}
	return -EINVAL;
}

static int append_entry(sd_bus_message *m, const char *name, const struct lvalue *v)
{
	int r = sd_bus_message_open_container(m, 'e', "sv");
	if (r >= 0)
		r = sd_bus_message_append(m, "s", name);
	if (r >= 0)
		r = append_value(m, v);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	return r;
}

// Reads a variant that must hold exactly the key's type. Strings are borrowed from the message.
static int read_value(sd_bus_message *m, const struct lkey *k, struct lvalue *v, sd_bus_error *e)
{
	const char *contents = NULL, *want = SIG[k->type];
	int r = sd_bus_message_peek_type(m, NULL, &contents);
	if (r < 0)
		return r;
	if (!contents || strcmp(contents, want) != 0)
		return sd_bus_error_setf(e, ERR "InvalidValue", "%s: wrong type '%s', expected '%s'", k->name,
		                         contents ? contents : "?", want);
	if ((r = sd_bus_message_enter_container(m, 'v', want)) < 0)
		return r;
	v->type = k->type;
	switch (k->type) {
	case LT_BOOL: {
		int b;
		r = sd_bus_message_read_basic(m, 'b', &b);
		v->b = b;
		break;
	}
	case LT_INT: r = sd_bus_message_read_basic(m, 'x', &v->i); break;
	case LT_FLOAT: r = sd_bus_message_read_basic(m, 'd', &v->d); break;
	case LT_STRING: {
		const char *s;
		r = sd_bus_message_read_basic(m, 's', &s);
		v->s = (char *)s;
		break;
	}
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

static bool may_write(struct settingsd *d, sd_bus_message *m, const struct lkey *k)
{
	if (k->access == LA_USER)
		return true;
	sd_bus_creds *c = NULL;
	uid_t uid;
	int r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID, &c);
	if (r >= 0)
		r = sd_bus_creds_get_euid(c, &uid);
	sd_bus_creds_unref(c);
	if (r < 0)
		return false;
	bool device = d->has_device_uid && uid == d->device_uid;
	bool web = d->has_web_uid && uid == d->web_uid;
	switch (k->access) {
	case LA_ADMIN: return uid == 0 || device || web;
	case LA_DEVICE_ONLY: return uid == 0 || device;
	case LA_FACTORY: return uid == 0;
	case LA_USER: return true;
	}
	return false;
}

static int emit_changed(sd_bus *bus, const struct lstore *st, const bool *changed)
{
	sd_bus_message *sig = NULL;
	int r = sd_bus_message_new_signal(bus, &sig, OBJ, IFACE, "Changed");
	if (r >= 0)
		r = sd_bus_message_append(sig, "s", st->schema->ns);
	if (r >= 0)
		r = sd_bus_message_open_container(sig, 'a', "{sv}");
	for (size_t i = 0; r >= 0 && i < st->schema->n_keys; i++)
		if (changed[i])
			r = append_entry(sig, st->schema->keys[i].name, lstore_get(st, i));
	if (r >= 0)
		r = sd_bus_message_close_container(sig);
	if (r >= 0)
		r = sd_bus_send(bus, sig, NULL);
	sd_bus_message_unref(sig);
	return r;
}

// Checks access, applies the updates all-or-nothing and signals what changed.
static int commit(struct settingsd *d, size_t ni, const struct lupdate *u, size_t n, sd_bus_message *m, sd_bus_error *e)
{
	struct lstore *st = d->stores[ni];
	const struct lschema *s = d->schemas[ni];
	for (size_t j = 0; j < n; j++) {
		const struct lkey *k = &s->keys[u[j].key];
		if (!may_write(d, m, k))
			return sd_bus_error_setf(e, ERR "AccessDenied", "%s.%s needs access level %s", s->ns, k->name,
			                         laccess_name(k->access));
	}
	bool *changed = calloc(s->n_keys ? s->n_keys : 1, sizeof(*changed));
	if (!changed)
		return -ENOMEM;
	char err[512];
	int r = lstore_commit(st, u, n, changed, err, sizeof(err));
	if (r == -EINVAL)
		r = sd_bus_error_setf(e, ERR "InvalidValue", "%s", err);
	else if (r < 0)
		r = sd_bus_error_setf(e, ERR "StorageFailed", "%s", err);
	else {
		bool any = false;
		for (size_t i = 0; i < s->n_keys; i++)
			any |= changed[i];
		if (any && (r = emit_changed(sd_bus_message_get_bus(m), st, changed)) < 0)
			fprintf(stderr, "sending Changed for %s: %s\n", s->ns, strerror(-r));
		r = sd_bus_reply_method_return(m, NULL);
	}
	free(changed);
	return r;
}

static int m_list(sd_bus_message *m, void *data, sd_bus_error *e)
{
	(void)e;
	struct settingsd *d = data;
	sd_bus_message *reply = NULL;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0)
		r = sd_bus_message_open_container(reply, 'a', "s");
	for (size_t i = 0; r >= 0 && i < d->n; i++)
		r = sd_bus_message_append(reply, "s", d->schemas[i]->ns);
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int append_str(sd_bus_message *m, const char *name, const char *value)
{
	return value ? sd_bus_message_append(m, "{sv}", name, "s", value) : 0;
}

static int append_key(sd_bus_message *m, const struct lkey *k)
{
	int r = sd_bus_message_open_container(m, 'r', "sa{sv}");
	if (r >= 0)
		r = sd_bus_message_append(m, "s", k->name);
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (r >= 0)
		r = append_str(m, "type", ltype_name(k->type));
	if (r >= 0)
		r = append_entry(m, "default", &k->def);
	if (r >= 0 && k->has_min)
		r = append_entry(m, "min", &k->min);
	if (r >= 0 && k->has_max)
		r = append_entry(m, "max", &k->max);
	if (r >= 0 && k->has_step)
		r = append_entry(m, "step", &k->step);
	if (r >= 0 && k->n_choices) {
		r = sd_bus_message_open_container(m, 'e', "sv");
		if (r >= 0)
			r = sd_bus_message_append(m, "s", "choices");
		if (r >= 0)
			r = sd_bus_message_open_container(m, 'v', "av");
		if (r >= 0)
			r = sd_bus_message_open_container(m, 'a', "v");
		for (size_t i = 0; r >= 0 && i < k->n_choices; i++)
			r = append_value(m, &k->choices[i]);
		for (int i = 0; r >= 0 && i < 3; i++) // array, variant, entry
			r = sd_bus_message_close_container(m);
	}
	if (r >= 0)
		r = append_str(m, "unit", k->unit);
	if (r >= 0)
		r = append_str(m, "label", k->label);
	if (r >= 0)
		r = append_str(m, "description", k->description);
	if (r >= 0 && k->has_order)
		r = sd_bus_message_append(m, "{sv}", "order", "x", k->order);
	if (r >= 0)
		r = append_str(m, "apply", k->apply);
	if (r >= 0)
		r = append_str(m, "access", laccess_name(k->access));
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	return r;
}

static int m_describe(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct settingsd *d = data;
	const char *ns;
	size_t ni;
	int r = sd_bus_message_read(m, "s", &ns);
	if (r < 0 || (r = find_ns(d, ns, e, &ni)) < 0)
		return r;
	const struct lschema *s = d->schemas[ni];
	sd_bus_message *reply = NULL;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0)
		r = sd_bus_message_append(reply, "s", s->title);
	if (r >= 0)
		r = sd_bus_message_open_container(reply, 'a', "(sa{sv})");
	for (size_t i = 0; r >= 0 && i < s->n_keys; i++)
		r = append_key(reply, &s->keys[i]);
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int m_get(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct settingsd *d = data;
	const char *ns, *key;
	size_t ni, ki;
	int r = sd_bus_message_read(m, "ss", &ns, &key);
	if (r < 0 || (r = find_ns(d, ns, e, &ni)) < 0 || (r = find_key(d->schemas[ni], key, e, &ki)) < 0)
		return r;
	sd_bus_message *reply = NULL;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0)
		r = append_value(reply, lstore_get(d->stores[ni], ki));
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int m_get_all(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct settingsd *d = data;
	const char *ns;
	size_t ni;
	int r = sd_bus_message_read(m, "s", &ns);
	if (r < 0 || (r = find_ns(d, ns, e, &ni)) < 0)
		return r;
	const struct lstore *st = d->stores[ni];
	sd_bus_message *reply = NULL;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0)
		r = sd_bus_message_open_container(reply, 'a', "{sv}");
	for (size_t i = 0; r >= 0 && i < st->schema->n_keys; i++)
		r = append_entry(reply, st->schema->keys[i].name, lstore_get(st, i));
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int m_set(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct settingsd *d = data;
	const char *ns, *key;
	size_t ni;
	struct lupdate u = {0};
	int r = sd_bus_message_read(m, "ss", &ns, &key);
	if (r < 0 || (r = find_ns(d, ns, e, &ni)) < 0 || (r = find_key(d->schemas[ni], key, e, &u.key)) < 0 ||
	    (r = read_value(m, &d->schemas[ni]->keys[u.key], &u.value, e)) < 0)
		return r;
	return commit(d, ni, &u, 1, m, e);
}

static int m_set_many(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct settingsd *d = data;
	const char *ns;
	size_t ni, n = 0, cap = 0;
	struct lupdate *u = NULL;
	int r = sd_bus_message_read(m, "s", &ns);
	if (r < 0 || (r = find_ns(d, ns, e, &ni)) < 0 || (r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if (n == cap) {
			struct lupdate *nu = realloc(u, (cap = cap ? cap * 2 : 8) * sizeof(*u));
			if (!nu) {
				r = -ENOMEM;
				break;
			}
			u = nu;
		}
		u[n] = (struct lupdate){0};
		if ((r = sd_bus_message_read(m, "s", &key)) < 0 || (r = find_key(d->schemas[ni], key, e, &u[n].key)) < 0 ||
		    (r = read_value(m, &d->schemas[ni]->keys[u[n].key], &u[n].value, e)) < 0 ||
		    (r = sd_bus_message_exit_container(m)) < 0)
			break;
		n++;
	}
	if (r >= 0 && (r = sd_bus_message_exit_container(m)) >= 0)
		r = commit(d, ni, u, n, m, e);
	free(u);
	return r;
}

static int m_reset(sd_bus_message *m, void *data, sd_bus_error *e)
{
	struct settingsd *d = data;
	const char *ns, *key;
	size_t ni;
	struct lupdate u = {.reset = true};
	int r = sd_bus_message_read(m, "ss", &ns, &key);
	if (r < 0 || (r = find_ns(d, ns, e, &ni)) < 0 || (r = find_key(d->schemas[ni], key, e, &u.key)) < 0)
		return r;
	return commit(d, ni, &u, 1, m, e);
}

// Access is checked per key in commit(), so every method is callable by unprivileged clients.
#define U SD_BUS_VTABLE_UNPRIVILEGED
static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_NAMES("ListNamespaces", "", "", "as", SD_BUS_PARAM(namespaces), m_list, U),
	SD_BUS_METHOD_WITH_NAMES("Describe", "s", SD_BUS_PARAM(namespace), "sa(sa{sv})", SD_BUS_PARAM(title) SD_BUS_PARAM(keys),
	                         m_describe, U),
	SD_BUS_METHOD_WITH_NAMES("Get", "ss", SD_BUS_PARAM(namespace) SD_BUS_PARAM(key), "v", SD_BUS_PARAM(value), m_get, U),
	SD_BUS_METHOD_WITH_NAMES("GetAll", "s", SD_BUS_PARAM(namespace), "a{sv}", SD_BUS_PARAM(values), m_get_all, U),
	SD_BUS_METHOD_WITH_NAMES("Set", "ssv", SD_BUS_PARAM(namespace) SD_BUS_PARAM(key) SD_BUS_PARAM(value), "", "", m_set, U),
	SD_BUS_METHOD_WITH_NAMES("SetMany", "sa{sv}", SD_BUS_PARAM(namespace) SD_BUS_PARAM(values), "", "", m_set_many, U),
	SD_BUS_METHOD_WITH_NAMES("Reset", "ss", SD_BUS_PARAM(namespace) SD_BUS_PARAM(key), "", "", m_reset, U),
	SD_BUS_SIGNAL_WITH_NAMES("Changed", "sa{sv}", SD_BUS_PARAM(namespace) SD_BUS_PARAM(values), 0),
	SD_BUS_VTABLE_END,
};

int settingsd_bus_add(struct settingsd *d, sd_bus *bus)
{
	return sd_bus_add_object_vtable(bus, NULL, OBJ, IFACE, vtable, d);
}

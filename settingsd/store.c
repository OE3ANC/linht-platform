// SPDX-License-Identifier: GPL-3.0-only
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#include "yaml-util.h"

static void warn(const struct lstore *st, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "%s: ", st->path);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

const struct lvalue *lstore_get(const struct lstore *st, size_t i)
{
	return st->set[i] ? &st->user[i] : &st->schema->keys[i].def;
}

static void free_values(size_t n, struct lvalue *v, bool *set)
{
	for (size_t i = 0; v && i < n; i++)
		if (set[i])
			lvalue_clear(&v[i]);
	free(v);
	free(set);
}

void lstore_free(struct lstore *st)
{
	if (!st)
		return;
	free_values(st->schema->n_keys, st->user, st->set);
	free(st->path);
	free(st);
}

// Reads the user layer. Bad entries are dropped with a warning; returns false if the file
// can't be parsed at all.
static bool load(struct lstore *st, FILE *f)
{
	yaml_parser_t p;
	yaml_document_t doc;
	yaml_parser_initialize(&p);
	yaml_parser_set_input_file(&p, f);
	bool ok = yaml_parser_load(&p, &doc);
	if (!ok) {
		warn(st, "line %zu: %s", p.problem_mark.line + 1, p.problem ? p.problem : "YAML error");
		yaml_parser_delete(&p);
		return false;
	}
	yaml_node_t *root = yaml_document_get_root_node(&doc);
	if (root && root->type != YAML_MAPPING_NODE) {
		warn(st, "not a mapping");
		ok = false;
	}
	for (yaml_node_pair_t *pr = root && ok ? root->data.mapping.pairs.start : NULL;
	     pr && pr < root->data.mapping.pairs.top; pr++) {
		yaml_node_t *kn = yaml_document_get_node(&doc, pr->key), *vn = yaml_document_get_node(&doc, pr->value);
		const char *name = kn && kn->type == YAML_SCALAR_NODE ? (const char *)kn->data.scalar.value : "?";
		const struct lkey *k = lschema_key(st->schema, name);
		if (!k) {
			warn(st, "dropping unknown key '%s'", name);
			continue;
		}
		size_t i = k - st->schema->keys;
		struct lvalue v;
		const char *why = NULL;
		const char *text = yaml_text(vn);
		if (!text || !lvalue_parse(k->type, text, &v))
			why = "not a valid value";
		else if ((why = lkey_check(k, &v)) || lvalue_eq(&v, &k->def)) {
			lvalue_clear(&v);
			if (!why)
				continue; // equal to the default: not a user value
		}
		if (why) {
			warn(st, "dropping '%s': %s", name, why);
			continue;
		}
		if (st->set[i])
			lvalue_clear(&st->user[i]); // a repeated key: the last one wins
		st->user[i] = v;
		st->set[i] = true;
	}
	yaml_document_delete(&doc);
	yaml_parser_delete(&p);
	return ok;
}

struct lstore *lstore_open(const struct lschema *s, const char *dir)
{
	struct lstore *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;
	st->schema = s;
	st->user = calloc(s->n_keys ? s->n_keys : 1, sizeof(*st->user));
	st->set = calloc(s->n_keys ? s->n_keys : 1, sizeof(*st->set));
	if (!st->user || !st->set || asprintf(&st->path, "%s/%s.yaml", dir, s->ns) < 0) {
		st->path = NULL;
		lstore_free(st);
		return NULL;
	}
	FILE *f = fopen(st->path, "r");
	if (!f && errno == ENOENT)
		return st;
	const char *why = f ? "unreadable" : strerror(errno);
	bool ok = f && load(st, f);
	if (f)
		fclose(f);
	if (!ok) { // move it aside, or the next write would replace every stored value with the defaults
		char *broken;
		if (asprintf(&broken, "%s.broken", st->path) >= 0) {
			warn(st, rename(st->path, broken) == 0 ? "%s, kept as %s, using defaults"
			                                        : "%s, could not keep it as %s", why, broken);
			free(broken);
		}
	}
	return st;
}

// Code points we write unescaped: YAML 1.1 "printable", without the line breaks U+0085, U+2028 and
// U+2029, which a parser folds inside a quoted scalar.
static bool yaml_printable(uint32_t cp)
{
	return cp == 0x09 || cp == 0x0a || cp == 0x0d || (cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xd7ff &&
	       cp != 0x2028 && cp != 0x2029) || (cp >= 0xe000 && cp <= 0xfffd) || (cp >= 0x10000 && cp <= 0x10ffff);
}

// Writes s as a YAML double-quoted scalar. s is valid UTF-8 (lkey_check).
static void put_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		unsigned n = *p < 0x80 ? 1 : *p < 0xe0 ? 2 : *p < 0xf0 ? 3 : 4;
		uint32_t cp = n == 1 ? *p : *p & (0x7f >> n);
		for (unsigned i = 1; i < n; i++)
			cp = (cp << 6) | (p[i] & 0x3f);
		if (cp == '"' || cp == '\\')
			fprintf(f, "\\%c", (int)cp);
		else if (cp == '\n')
			fputs("\\n", f);
		else if (cp == '\t')
			fputs("\\t", f);
		else if (cp == '\r')
			fputs("\\r", f);
		else if (!yaml_printable(cp) || cp < 0x20)
			fprintf(f, "\\u%04" PRIX32, cp); // every non-printable code point is <= U+FFFF
		else
			fwrite(p, 1, n, f);
		p += n;
	}
	fputc('"', f);
}

static void put_value(FILE *f, const struct lvalue *v)
{
	switch (v->type) {
	case LT_BOOL: fputs(v->b ? "true" : "false", f); break;
	case LT_INT: fprintf(f, "%" PRId64, v->i); break;
	case LT_FLOAT: fprintf(f, "%.17g", v->d); break;
	case LT_STRING: put_string(f, v->s); break;
	}
}

// temp file in the same directory, fsync, rename, fsync of the directory
static int write_file(const struct lstore *st, const struct lvalue *v, const bool *set)
{
	char *tmp, *dir;
	if (asprintf(&tmp, "%s.XXXXXX", st->path) < 0)
		return -ENOMEM;
	int fd = mkstemp(tmp), r = -EIO;
	FILE *f = fd >= 0 && fchmod(fd, 0644) == 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		r = -errno;
		if (fd >= 0) {
			close(fd);
			unlink(tmp);
		}
		free(tmp);
		return r;
	}
	fputs("# Managed by linht-settingsd. Holds only values that differ from the schema default.\n", f);
	size_t count = 0;
	for (size_t i = 0; i < st->schema->n_keys; i++) {
		if (!set[i])
			continue;
		fprintf(f, "%s: ", st->schema->keys[i].name);
		put_value(f, &v[i]);
		fputc('\n', f);
		count++;
	}
	if (!count)
		fputs("{}\n", f);
	bool ok = fflush(f) == 0 && fsync(fd) == 0;
	ok = fclose(f) == 0 && ok;
	if (ok && rename(tmp, st->path) == 0) {
		r = 0;
		if ((dir = strdup(st->path))) {
			*strrchr(dir, '/') = 0;
			int dfd = open(dir, O_RDONLY | O_DIRECTORY);
			if (dfd < 0 || fsync(dfd) != 0)
				warn(st, "could not fsync the directory");
			if (dfd >= 0)
				close(dfd);
			free(dir);
		}
	} else {
		unlink(tmp);
	}
	free(tmp);
	return r;
}

int lstore_commit(struct lstore *st, const struct lupdate *u, size_t n, bool *changed, char *err, size_t errlen)
{
	const struct lschema *s = st->schema;
	for (size_t j = 0; j < n; j++) {
		if (u[j].key >= s->n_keys) {
			snprintf(err, errlen, "unknown key");
			return -EINVAL;
		}
		const char *why = u[j].reset ? NULL : lkey_check(&s->keys[u[j].key], &u[j].value);
		if (why) {
			snprintf(err, errlen, "%s: %s", s->keys[u[j].key].name, why);
			return -EINVAL;
		}
	}

	struct lvalue *nv = calloc(s->n_keys ? s->n_keys : 1, sizeof(*nv));
	bool *ns = calloc(s->n_keys ? s->n_keys : 1, sizeof(*ns));
	bool *ch = calloc(s->n_keys ? s->n_keys : 1, sizeof(*ch));
	int r = nv && ns && ch ? 0 : -ENOMEM;
	for (size_t i = 0; r == 0 && i < s->n_keys; i++)
		if ((ns[i] = st->set[i]))
			r = lvalue_copy(&nv[i], &st->user[i]);
	for (size_t j = 0; r == 0 && j < n; j++) {
		size_t i = u[j].key;
		if (ns[i])
			lvalue_clear(&nv[i]);
		ns[i] = !u[j].reset && !lvalue_eq(&u[j].value, &s->keys[i].def);
		if (ns[i])
			r = lvalue_copy(&nv[i], &u[j].value);
		if (r)
			ns[i] = false;
	}
	bool any = false;
	for (size_t i = 0; r == 0 && i < s->n_keys; i++) {
		const struct lvalue *was = lstore_get(st, i), *now = ns[i] ? &nv[i] : &s->keys[i].def;
		any |= ch[i] = !lvalue_eq(was, now);
	}
	if (r == 0 && any && (r = write_file(st, nv, ns)) != 0) {
		snprintf(err, errlen, "writing %s: %s", st->path, strerror(-r));
		r = -EIO;
	}
	if (r) {
		if (r == -ENOMEM)
			snprintf(err, errlen, "out of memory");
		free_values(s->n_keys, nv, ns);
		free(ch);
		return r;
	}
	free_values(s->n_keys, st->user, st->set);
	st->user = nv;
	st->set = ns;
	memcpy(changed, ch, s->n_keys * sizeof(*ch));
	free(ch);
	return 0;
}

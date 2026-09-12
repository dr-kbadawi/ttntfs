/* SPDX-License-Identifier: GPL-2.0 */
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

void json_write_string_n(FILE *f, const char *s, size_t n)
{
	fputc('"', f);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"': fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n", f); break;
		case '\r': fputs("\\r", f); break;
		case '\t': fputs("\\t", f); break;
		case '\b': fputs("\\b", f); break;
		case '\f': fputs("\\f", f); break;
		default:
			if (c < 0x20) fprintf(f, "\\u%04x", c);
			else fputc(c, f);
		}
	}
	fputc('"', f);
}

void json_write_string(FILE *f, const char *s) { json_write_string_n(f, s, strlen(s)); }

/* ---- parser ---- */
struct p { const char *s; size_t n, i; int err; };

static void skipws(struct p *p) { while (p->i < p->n && isspace((unsigned char)p->s[p->i])) p->i++; }
static struct json_value *parse_value(struct p *p);

static struct json_value *newv(enum json_type t)
{
	struct json_value *v = calloc(1, sizeof(*v));
	if (v) v->type = t;
	return v;
}

static void put_utf8(char **out, size_t *cap, size_t *len, uint32_t cp)
{
	char tmp[4]; int n;
	if (cp < 0x80) { tmp[0] = cp; n = 1; }
	else if (cp < 0x800) { tmp[0] = 0xC0 | (cp >> 6); tmp[1] = 0x80 | (cp & 63); n = 2; }
	else if (cp < 0x10000) { tmp[0] = 0xE0 | (cp >> 12); tmp[1] = 0x80 | ((cp >> 6) & 63); tmp[2] = 0x80 | (cp & 63); n = 3; }
	else { tmp[0] = 0xF0 | (cp >> 18); tmp[1] = 0x80 | ((cp >> 12) & 63); tmp[2] = 0x80 | ((cp >> 6) & 63); tmp[3] = 0x80 | (cp & 63); n = 4; }
	if (*len + n + 1 > *cap) { *cap = (*cap + n + 64) * 2; *out = realloc(*out, *cap); }
	memcpy(*out + *len, tmp, n); *len += n;
}

static char *parse_string_raw(struct p *p)
{
	char *out = NULL; size_t cap = 0, len = 0;
	if (p->s[p->i] != '"') { p->err = 1; return NULL; }
	p->i++;
	while (p->i < p->n) {
		unsigned char c = p->s[p->i++];
		if (c == '"') {
			if (len + 1 > cap) out = realloc(out, len + 1);
			out[len] = 0;
			return out;
		}
		if (c == '\\') {
			if (p->i >= p->n) break;
			c = p->s[p->i++];
			switch (c) {
			case '"': case '\\': case '/': put_utf8(&out, &cap, &len, c); break;
			case 'n': put_utf8(&out, &cap, &len, '\n'); break;
			case 'r': put_utf8(&out, &cap, &len, '\r'); break;
			case 't': put_utf8(&out, &cap, &len, '\t'); break;
			case 'b': put_utf8(&out, &cap, &len, '\b'); break;
			case 'f': put_utf8(&out, &cap, &len, '\f'); break;
			case 'u': {
				if (p->i + 4 > p->n) goto bad;
				uint32_t cp = strtoul((char[]){p->s[p->i], p->s[p->i+1], p->s[p->i+2], p->s[p->i+3], 0}, NULL, 16);
				p->i += 4;
				if (cp >= 0xD800 && cp < 0xDC00 && p->i + 6 <= p->n && p->s[p->i] == '\\' && p->s[p->i+1] == 'u') {
					uint32_t lo = strtoul((char[]){p->s[p->i+2], p->s[p->i+3], p->s[p->i+4], p->s[p->i+5], 0}, NULL, 16);
					if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p->i += 6; }
				}
				put_utf8(&out, &cap, &len, cp);
				break;
			}
			default: goto bad;
			}
		} else {
			put_utf8(&out, &cap, &len, c);
		}
	}
bad:
	p->err = 1; free(out); return NULL;
}

static struct json_value *parse_value(struct p *p)
{
	skipws(p);
	if (p->i >= p->n) { p->err = 1; return NULL; }
	char c = p->s[p->i];
	struct json_value *v;
	if (c == '{') {
		v = newv(JSON_OBJECT); p->i++;
		skipws(p);
		if (p->i < p->n && p->s[p->i] == '}') { p->i++; return v; }
		for (;;) {
			skipws(p);
			char *k = parse_string_raw(p);
			if (!k) goto fail;
			skipws(p);
			if (p->i >= p->n || p->s[p->i] != ':') { free(k); p->err = 1; goto fail; }
			p->i++;
			struct json_value *val = parse_value(p);
			if (!val) { free(k); goto fail; }
			v->u.obj.keys = realloc(v->u.obj.keys, (v->u.obj.n + 1) * sizeof(char *));
			v->u.obj.vals = realloc(v->u.obj.vals, (v->u.obj.n + 1) * sizeof(void *));
			v->u.obj.keys[v->u.obj.n] = k; v->u.obj.vals[v->u.obj.n] = val; v->u.obj.n++;
			skipws(p);
			if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
			if (p->i < p->n && p->s[p->i] == '}') { p->i++; return v; }
			p->err = 1; goto fail;
		}
	} else if (c == '[') {
		v = newv(JSON_ARRAY); p->i++;
		skipws(p);
		if (p->i < p->n && p->s[p->i] == ']') { p->i++; return v; }
		for (;;) {
			struct json_value *item = parse_value(p);
			if (!item) goto fail;
			v->u.arr.items = realloc(v->u.arr.items, (v->u.arr.n + 1) * sizeof(void *));
			v->u.arr.items[v->u.arr.n++] = item;
			skipws(p);
			if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
			if (p->i < p->n && p->s[p->i] == ']') { p->i++; return v; }
			p->err = 1; goto fail;
		}
	} else if (c == '"') {
		char *s = parse_string_raw(p);
		if (!s) return NULL;
		v = newv(JSON_STRING); v->u.str = s; return v;
	} else if (c == 't' && p->n - p->i >= 4 && !memcmp(p->s + p->i, "true", 4)) {
		p->i += 4; v = newv(JSON_BOOL); v->u.b = true; return v;
	} else if (c == 'f' && p->n - p->i >= 5 && !memcmp(p->s + p->i, "false", 5)) {
		p->i += 5; v = newv(JSON_BOOL); v->u.b = false; return v;
	} else if (c == 'n' && p->n - p->i >= 4 && !memcmp(p->s + p->i, "null", 4)) {
		p->i += 4; return newv(JSON_NULL);
	} else if (c == '-' || isdigit((unsigned char)c)) {
		char *end; errno = 0;
		double d = strtod(p->s + p->i, &end);
		if (end == p->s + p->i) { p->err = 1; return NULL; }
		p->i = end - p->s;
		v = newv(JSON_NUMBER); v->u.num = d; return v;
	}
	p->err = 1; return NULL;
fail:
	json_free(v); return NULL;
}

struct json_value *json_parse(const char *text, size_t len, size_t *err_off)
{
	struct p p = { text, len, 0, 0 };
	struct json_value *v = parse_value(&p);
	if (v) { skipws(&p); if (p.i != p.n) { json_free(v); v = NULL; p.err = 1; } }
	if (!v && err_off) *err_off = p.i;
	return v;
}

void json_free(struct json_value *v)
{
	if (!v) return;
	switch (v->type) {
	case JSON_STRING: free(v->u.str); break;
	case JSON_ARRAY: for (size_t i = 0; i < v->u.arr.n; i++) json_free(v->u.arr.items[i]); free(v->u.arr.items); break;
	case JSON_OBJECT:
		for (size_t i = 0; i < v->u.obj.n; i++) { free(v->u.obj.keys[i]); json_free(v->u.obj.vals[i]); }
		free(v->u.obj.keys); free(v->u.obj.vals); break;
	default: break;
	}
	free(v);
}

struct json_value *json_get(const struct json_value *v, const char *key)
{
	if (!v || v->type != JSON_OBJECT) return NULL;
	for (size_t i = 0; i < v->u.obj.n; i++)
		if (!strcmp(v->u.obj.keys[i], key)) return v->u.obj.vals[i];
	return NULL;
}
const char *json_get_str(const struct json_value *v, const char *key, const char *def)
{ struct json_value *x = json_get(v, key); return x && x->type == JSON_STRING ? x->u.str : def; }
double json_get_num(const struct json_value *v, const char *key, double def)
{ struct json_value *x = json_get(v, key); return x && x->type == JSON_NUMBER ? x->u.num : def; }
bool json_get_bool(const struct json_value *v, const char *key, bool def)
{ struct json_value *x = json_get(v, key); return x && x->type == JSON_BOOL ? x->u.b : def; }

char *json_read_file(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return NULL; }
	char *buf = malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	buf[n] = 0; fclose(f);
	if (len_out) *len_out = (size_t)n;
	return buf;
}

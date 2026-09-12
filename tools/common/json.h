/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal JSON: a writer with correct string escaping (used by mkfixtures)
 * and a small DOM parser (used by ntfscli verify). UTF-8 passes through.
 */
#ifndef TOOLS_JSON_H
#define TOOLS_JSON_H
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- writer --- */
void json_write_string(FILE *f, const char *s);		/* with quotes */
void json_write_string_n(FILE *f, const char *s, size_t n);

/* --- parser --- */
enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json_value {
	enum json_type type;
	union {
		bool b;
		double num;
		char *str;			/* NUL-terminated, unescaped UTF-8 */
		struct { struct json_value **items; size_t n; } arr;
		struct { char **keys; struct json_value **vals; size_t n; } obj;
	} u;
};

/* Parse @text; on error returns NULL and sets *err_off to the byte offset. */
struct json_value *json_parse(const char *text, size_t len, size_t *err_off);
void json_free(struct json_value *v);
/* Object lookup; NULL if absent or @v not an object. */
struct json_value *json_get(const struct json_value *v, const char *key);
/* Typed accessors with defaults. */
const char *json_get_str(const struct json_value *v, const char *key, const char *def);
double json_get_num(const struct json_value *v, const char *key, double def);
bool json_get_bool(const struct json_value *v, const char *key, bool def);
/* Read a whole file into a malloc'd buffer (NUL-terminated); NULL on error. */
char *json_read_file(const char *path, size_t *len_out);

#endif

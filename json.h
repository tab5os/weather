/* SPDX-License-Identifier: Apache-2.0
 * A small JSON tokenizer (jsmn style): one pass, no allocation of its own,
 * tokens point into the source text. Enough for API responses. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { J_OBJ = 1, J_ARR, J_STR, J_PRIM } jtype_t;

typedef struct {
    unsigned char type;     /* jtype_t */
    int start, end;         /* [start, end) in the source; strings without quotes */
    int size;               /* objects: keys, arrays: elements */
} jtok_t;

/* Tokens written, or -1 (bad JSON or out of tokens). */
int json_parse(const char *js, size_t len, jtok_t *t, int max);

/* The token after i and everything inside it. */
int json_skip(const jtok_t *t, int i);
/* The value for key in object obj, or -1. */
int json_get(const char *js, const jtok_t *t, int n, int obj, const char *key);
/* Element k of array arr, or -1. */
int json_at(const jtok_t *t, int n, int arr, int k);

/* A number (false for null, strings, objects). */
bool json_num(const char *js, const jtok_t *t, int i, double *out);
/* A string, unescaped to UTF-8 and cut to cap. Returns its length, -1 if i isn't a string. */
int json_str(const char *js, const jtok_t *t, int i, char *out, size_t cap);

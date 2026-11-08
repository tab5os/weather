/* SPDX-License-Identifier: Apache-2.0 */
#include "json.h"
#include <stdlib.h>
#include <string.h>

#define DEPTH 32

typedef struct {
    int tok;
    bool obj;
    bool want_key;          /* objects: the next token is a key */
} level_t;

static int add(jtok_t *t, int *n, int max, jtype_t type, int start, int end)
{
    if (*n >= max) return -1;
    t[*n] = (jtok_t){ (unsigned char)type, start, end, 0 };
    return (*n)++;
}

/* A new token starts inside the current container: count it if it's an
 * array element or an object key (not a key's value). */
static void count(jtok_t *t, level_t *st, int depth)
{
    if (!depth) return;
    level_t *l = &st[depth - 1];
    if (!l->obj || l->want_key) t[l->tok].size++;
}

int json_parse(const char *js, size_t len, jtok_t *t, int max)
{
    level_t st[DEPTH];
    int depth = 0, n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = js[i];
        switch (c) {
        case '{':
        case '[': {
            if (depth == DEPTH) return -1;
            count(t, st, depth);
            int k = add(t, &n, max, c == '{' ? J_OBJ : J_ARR, (int)i, -1);
            if (k < 0) return -1;
            st[depth++] = (level_t){ k, c == '{', c == '{' };
            break;
        }
        case '}':
        case ']':
            if (!depth || st[depth - 1].obj != (c == '}')) return -1;
            t[st[depth - 1].tok].end = (int)i + 1;
            depth--;
            break;
        case '"': {
            size_t s = ++i;
            while (i < len && js[i] != '"') {
                if (js[i] == '\\') i++;
                i++;
            }
            if (i >= len) return -1;
            count(t, st, depth);
            if (add(t, &n, max, J_STR, (int)s, (int)i) < 0) return -1;
            break;
        }
        case ':':
            if (depth && st[depth - 1].obj) st[depth - 1].want_key = false;
            break;
        case ',':
            if (depth && st[depth - 1].obj) st[depth - 1].want_key = true;
            break;
        case ' ': case '\t': case '\r': case '\n':
            break;
        default: {
            size_t s = i;
            while (i < len && !strchr(",]} \t\r\n", js[i])) i++;
            count(t, st, depth);
            if (add(t, &n, max, J_PRIM, (int)s, (int)i) < 0) return -1;
            i--;
            break;
        }
        }
    }
    return depth ? -1 : n;
}

int json_skip(const jtok_t *t, int i)
{
    int pending = 1;
    while (pending > 0) {
        pending--;
        if (t[i].type == J_OBJ) pending += t[i].size * 2;
        else if (t[i].type == J_ARR) pending += t[i].size;
        i++;
    }
    return i;
}

int json_get(const char *js, const jtok_t *t, int n, int obj, const char *key)
{
    if (obj < 0 || obj >= n || t[obj].type != J_OBJ) return -1;
    size_t kl = strlen(key);
    int j = obj + 1;
    for (int k = 0; k < t[obj].size && j + 1 < n; k++) {
        const jtok_t *kt = &t[j];
        if (kt->type == J_STR && (size_t)(kt->end - kt->start) == kl && !memcmp(js + kt->start, key, kl)) return j + 1;
        j = json_skip(t, j + 1);
    }
    return -1;
}

int json_at(const jtok_t *t, int n, int arr, int k)
{
    if (arr < 0 || arr >= n || t[arr].type != J_ARR || k >= t[arr].size) return -1;
    int j = arr + 1;
    while (k-- > 0) j = json_skip(t, j);
    return j < n ? j : -1;
}

bool json_num(const char *js, const jtok_t *t, int i, double *out)
{
    if (i < 0 || t[i].type != J_PRIM) return false;
    char c = js[t[i].start];
    if (!(c == '-' || (c >= '0' && c <= '9'))) return false;    /* null, true, false */
    char buf[32];
    int l = t[i].end - t[i].start;
    if (l <= 0 || l >= (int)sizeof buf) return false;
    memcpy(buf, js + t[i].start, (size_t)l);
    buf[l] = 0;
    *out = strtod(buf, NULL);
    return true;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

int json_str(const char *js, const jtok_t *t, int i, char *out, size_t cap)
{
    if (i < 0 || t[i].type != J_STR || !cap) return -1;
    size_t o = 0;
    for (int p = t[i].start; p < t[i].end && o + 1 < cap; p++) {
        char c = js[p];
        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        c = js[++p];
        unsigned cp = (unsigned char)c;
        switch (c) {
        case 'n': cp = '\n'; break;
        case 't': cp = '\t'; break;
        case 'r': cp = '\r'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'u': {
            int v = p + 4 < t[i].end ? hex4(js + p + 1) : -1;
            if (v < 0) { cp = '?'; break; }
            cp = (unsigned)v;
            p += 4;
            /* A surrogate pair. */
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < t[i].end && js[p + 1] == '\\' && js[p + 2] == 'u') {
                int lo = hex4(js + p + 3);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)lo - 0xDC00);
                    p += 6;
                }
            }
            break;
        }
        default: break;     /* \" \\ \/ */
        }
        /* UTF-8, never cutting a character in half. */
        int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (o + (size_t)need >= cap) break;
        if (need == 1) out[o++] = (char)cp;
        else if (need == 2) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (need == 3) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return (int)o;
}

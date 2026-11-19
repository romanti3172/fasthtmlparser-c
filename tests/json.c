#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct parser {
    const char *s;
    size_t len, p;
    int failed;
} parser;

static void ws(parser *p) {
    while (p->p < p->len && (p->s[p->p] == ' ' || p->s[p->p] == '\t' || p->s[p->p] == '\n' || p->s[p->p] == '\r'))
        p->p++;
}

static jvalue *newv(jtype t) {
    jvalue *v = (jvalue *)calloc(1, sizeof(jvalue));
    if (v) v->type = t;
    return v;
}

static void put_utf8(char **out, size_t *n, size_t *cap, unsigned cp) {
    char tmp[4];
    size_t k = 0, i;
    if (cp < 0x80) {
        tmp[k++] = (char)cp;
    } else if (cp < 0x800) {
        tmp[k++] = (char)(0xC0 | (cp >> 6));
        tmp[k++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        tmp[k++] = (char)(0xE0 | (cp >> 12));
        tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[k++] = (char)(0x80 | (cp & 0x3F));
    } else {
        tmp[k++] = (char)(0xF0 | (cp >> 18));
        tmp[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[k++] = (char)(0x80 | (cp & 0x3F));
    }
    for (i = 0; i < k; i++) {
        if (*n + 1 >= *cap) {
            *cap *= 2;
            *out = (char *)realloc(*out, *cap);
        }
        (*out)[(*n)++] = tmp[i];
    }
}

static unsigned hex4(parser *p) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p->p < p->len ? p->s[p->p++] : 0;
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            p->failed = 1;
    }
    return v;
}

static char *read_string(parser *p, size_t *out_len) {
    size_t cap = 32, n = 0;
    char *out = (char *)malloc(cap);
    p->p++; /* '"' */
    while (p->p < p->len && p->s[p->p] != '"') {
        char c = p->s[p->p++];
        if (c == '\\') {
            char e = p->p < p->len ? p->s[p->p++] : 0;
            unsigned cp;
            switch (e) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case '/': cp = '/'; break;
            case '\\': cp = '\\'; break;
            case '"': cp = '"'; break;
            case 'u':
                cp = hex4(p);
                if (cp >= 0xD800 && cp <= 0xDBFF && p->p + 1 < p->len && p->s[p->p] == '\\' && p->s[p->p + 1] == 'u') {
                    unsigned lo;
                    p->p += 2;
                    lo = hex4(p);
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else {
                        put_utf8(&out, &n, &cap, 0xFFFD);
                        cp = lo;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                break;
            default:
                p->failed = 1;
                cp = '?';
            }
            put_utf8(&out, &n, &cap, cp);
        } else {
            if (n + 1 >= cap) {
                cap *= 2;
                out = (char *)realloc(out, cap);
            }
            out[n++] = c;
        }
    }
    if (p->p >= p->len) p->failed = 1;
    p->p++;
    out[n] = 0;
    *out_len = n;
    return out;
}

static jvalue *value(parser *p) {
    jvalue *v;
    ws(p);
    if (p->p >= p->len) {
        p->failed = 1;
        return NULL;
    }
    switch (p->s[p->p]) {
    case '{': {
        size_t cap = 8;
        v = newv(J_OBJECT);
        v->items = (jvalue **)malloc(cap * sizeof(jvalue *));
        v->keys = (char **)malloc(cap * sizeof(char *));
        v->key_lens = (size_t *)malloc(cap * sizeof(size_t));
        p->p++;
        ws(p);
        if (p->p < p->len && p->s[p->p] == '}') {
            p->p++;
            return v;
        }
        for (;;) {
            size_t kl;
            char *key;
            ws(p);
            if (p->p >= p->len || p->s[p->p] != '"') {
                p->failed = 1;
                return v;
            }
            key = read_string(p, &kl);
            ws(p);
            if (p->p >= p->len || p->s[p->p] != ':') {
                free(key);
                p->failed = 1;
                return v;
            }
            p->p++;
            if (v->count == cap) {
                cap *= 2;
                v->items = (jvalue **)realloc(v->items, cap * sizeof(jvalue *));
                v->keys = (char **)realloc(v->keys, cap * sizeof(char *));
                v->key_lens = (size_t *)realloc(v->key_lens, cap * sizeof(size_t));
            }
            v->keys[v->count] = key;
            v->key_lens[v->count] = kl;
            v->items[v->count++] = value(p);
            if (p->failed) return v;
            ws(p);
            if (p->p < p->len && p->s[p->p] == ',') {
                p->p++;
                continue;
            }
            if (p->p < p->len && p->s[p->p] == '}') {
                p->p++;
                return v;
            }
            p->failed = 1;
            return v;
        }
    }
    case '[': {
        size_t cap = 8;
        v = newv(J_ARRAY);
        v->items = (jvalue **)malloc(cap * sizeof(jvalue *));
        p->p++;
        ws(p);
        if (p->p < p->len && p->s[p->p] == ']') {
            p->p++;
            return v;
        }
        for (;;) {
            if (v->count == cap) {
                cap *= 2;
                v->items = (jvalue **)realloc(v->items, cap * sizeof(jvalue *));
            }
            v->items[v->count++] = value(p);
            if (p->failed) return v;
            ws(p);
            if (p->p < p->len && p->s[p->p] == ',') {
                p->p++;
                continue;
            }
            if (p->p < p->len && p->s[p->p] == ']') {
                p->p++;
                return v;
            }
            p->failed = 1;
            return v;
        }
    }
    case '"':
        v = newv(J_STRING);
        v->str = read_string(p, &v->len);
        return v;
    case 't':
        p->p += 4;
        v = newv(J_BOOL);
        v->b = 1;
        return v;
    case 'f':
        p->p += 5;
        return newv(J_BOOL);
    case 'n':
        p->p += 4;
        return newv(J_NULL);
    default: {
        char *end;
        v = newv(J_NUMBER);
        v->num = strtod(p->s + p->p, &end);
        if (end == p->s + p->p) p->failed = 1;
        p->p = (size_t)(end - p->s);
        return v;
    }
    }
}

jvalue *json_parse(const char *s, size_t len) {
    parser p;
    jvalue *v;
    p.s = s;
    p.len = len;
    p.p = 0;
    p.failed = 0;
    v = value(&p);
    if (p.failed) {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(jvalue *v) {
    size_t i;
    if (!v) return;
    for (i = 0; i < v->count; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->key_lens);
    free(v->str);
    free(v);
}

const jvalue *json_get(const jvalue *obj, const char *key) {
    size_t i, kl = strlen(key);
    if (!obj || obj->type != J_OBJECT) return NULL;
    for (i = 0; i < obj->count; i++)
        if (obj->key_lens[i] == kl && memcmp(obj->keys[i], key, kl) == 0) return obj->items[i];
    return NULL;
}

static void out(char *buf, size_t size, size_t *n, const char *s, size_t len) {
    size_t i;
    for (i = 0; i < len && *n + 1 < size; i++) buf[(*n)++] = s[i];
    buf[*n] = 0;
}

static void print(const jvalue *v, char *buf, size_t size, size_t *n) {
    char tmp[64];
    size_t i;
    if (!v) {
        out(buf, size, n, "null", 4);
        return;
    }
    switch (v->type) {
    case J_NULL:
        out(buf, size, n, "null", 4);
        break;
    case J_BOOL:
        out(buf, size, n, v->b ? "true" : "false", v->b ? 4 : 5);
        break;
    case J_NUMBER:
        snprintf(tmp, sizeof tmp, "%.17g", v->num);
        out(buf, size, n, tmp, strlen(tmp));
        break;
    case J_STRING:
        out(buf, size, n, "\"", 1);
        for (i = 0; i < v->len; i++) {
            unsigned char c = (unsigned char)v->str[i];
            if (c == '"' || c == '\\') {
                out(buf, size, n, "\\", 1);
                out(buf, size, n, (const char *)&v->str[i], 1);
            } else if (c < 0x20) {
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                out(buf, size, n, tmp, 6);
            } else {
                out(buf, size, n, (const char *)&v->str[i], 1);
            }
        }
        out(buf, size, n, "\"", 1);
        break;
    case J_ARRAY:
        out(buf, size, n, "[", 1);
        for (i = 0; i < v->count; i++) {
            if (i) out(buf, size, n, ",", 1);
            print(v->items[i], buf, size, n);
        }
        out(buf, size, n, "]", 1);
        break;
    default:
        out(buf, size, n, "{", 1);
        for (i = 0; i < v->count; i++) {
            if (i) out(buf, size, n, ",", 1);
            out(buf, size, n, "\"", 1);
            out(buf, size, n, v->keys[i], v->key_lens[i]);
            out(buf, size, n, "\":", 2);
            print(v->items[i], buf, size, n);
        }
        out(buf, size, n, "}", 1);
    }
}

void json_print(const jvalue *v, char *buf, size_t size) {
    size_t n = 0;
    if (size) buf[0] = 0;
    print(v, buf, size, &n);
}

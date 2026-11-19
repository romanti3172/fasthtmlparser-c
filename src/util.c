/* Arenas, buffers, byte search and Unicode helpers. */
#include <stdio.h>
#include <stdlib.h>

#include "internal.h"

#if !defined(FHP_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64))
#include <emmintrin.h>
#define FHP_SSE2 1
#endif

#if defined(_MSC_VER)
#include <intrin.h>
static unsigned fhp_ctz(unsigned x) {
    unsigned long i;
    _BitScanForward(&i, x);
    return (unsigned)i;
}
#else
#define fhp_ctz(x) ((unsigned)__builtin_ctz(x))
#endif

/* ----- arena ----- */

#define CHUNK_HEADER ((sizeof(fhp_chunk) + 15) & ~(size_t)15)
#define CHUNK_DATA(c) ((char *)(c) + CHUNK_HEADER)

void fhp_arena_init(fhp_arena *a, size_t chunk_size) {
    a->first = a->cur = NULL;
    a->chunk_size = chunk_size ? chunk_size : 16384;
    a->used_before = 0;
}

void fhp_arena_destroy(fhp_arena *a) {
    fhp_chunk *c = a->first;
    while (c) {
        fhp_chunk *next = c->next;
        free(c);
        c = next;
    }
    a->first = a->cur = NULL;
    a->used_before = 0;
}

fhp_arena *fhp_arena_new(size_t chunk_size) {
    fhp_arena *a = (fhp_arena *)malloc(sizeof(fhp_arena));
    if (a) fhp_arena_init(a, chunk_size);
    return a;
}

void fhp_arena_reset(fhp_arena *a) {
    fhp_chunk *c;
    if (!a) return;
    for (c = a->first; c; c = c->next) c->used = 0;
    a->cur = a->first;
    a->used_before = 0;
}

void fhp_arena_free(fhp_arena *a) {
    if (!a) return;
    fhp_arena_destroy(a);
    free(a);
}

size_t fhp_arena_used(const fhp_arena *a) {
    return a ? a->used_before + (a->cur ? a->cur->used : 0) : 0;
}

void *fhp_alloc(fhp_arena *a, size_t size) {
    fhp_chunk *c = a->cur;
    size = (size + 7) & ~(size_t)7;
    if (c && c->cap - c->used >= size) {
        void *p = CHUNK_DATA(c) + c->used;
        c->used += size;
        return p;
    }
    /* the following chunks (kept by a reset) */
    while (c && c->next) {
        a->used_before += c->used;
        c = c->next;
        a->cur = c;
        if (c->cap - c->used >= size) {
            void *p = CHUNK_DATA(c) + c->used;
            c->used += size;
            return p;
        }
    }
    {
        size_t cap = a->chunk_size;
        fhp_chunk *n;
        if (cap < size) cap = size;
        /* the next chunks grow with the demand */
        if (a->chunk_size < ((size_t)1 << 20)) a->chunk_size *= 2;
        n = (fhp_chunk *)malloc(CHUNK_HEADER + cap);
        if (!n) return NULL;
        n->next = NULL;
        n->cap = cap;
        n->used = size;
        if (c) {
            a->used_before += c->used;
            c->next = n;
        } else {
            a->first = n;
        }
        a->cur = n;
        return CHUNK_DATA(n);
    }
}

void *fhp_realloc(fhp_arena *a, void *p, size_t old_size, size_t new_size) {
    fhp_chunk *c = a->cur;
    size_t old_aligned = (old_size + 7) & ~(size_t)7;
    size_t new_aligned = (new_size + 7) & ~(size_t)7;
    void *q;
    if (p && c && (char *)p + old_aligned == CHUNK_DATA(c) + c->used && (char *)p >= CHUNK_DATA(c) &&
        c->cap - (c->used - old_aligned) >= new_aligned) {
        c->used = c->used - old_aligned + new_aligned;
        return p;
    }
    q = fhp_alloc(a, new_size);
    if (q && p && old_size) memcpy(q, p, old_size < new_size ? old_size : new_size);
    return q;
}

char *fhp_strndup(fhp_arena *a, const char *s, size_t len) {
    char *p = (char *)fhp_alloc(a, len + 1);
    if (!p) return NULL;
    if (len) memcpy(p, s, len);
    p[len] = 0;
    return p;
}

fhp_mark fhp_arena_mark(fhp_arena *a) {
    fhp_mark m;
    m.chunk = a->cur;
    m.used = a->cur ? a->cur->used : 0;
    m.used_before = a->used_before;
    return m;
}

void fhp_arena_release(fhp_arena *a, fhp_mark m) {
    fhp_chunk *c;
    if (!m.chunk) {
        fhp_arena_reset(a);
        return;
    }
    for (c = m.chunk->next; c; c = c->next) c->used = 0;
    m.chunk->used = m.used;
    a->cur = m.chunk;
    a->used_before = m.used_before;
}

static FHP_TLS fhp_arena *tls_scratch;

fhp_arena *fhp_scratch(void) {
    if (!tls_scratch) {
        tls_scratch = fhp_arena_new(65536);
        if (!tls_scratch) abort(); /* out of memory for a 64 KB scratch: nothing sensible is left to do */
    }
    return tls_scratch;
}

void fhp_parse_cleanup(void);

void fhp_thread_cleanup(void) {
    fhp_arena_free(tls_scratch);
    tls_scratch = NULL;
    fhp_parse_cleanup();
}

/* ----- buffers ----- */

void fhp_buf_init(fhp_buf *b, fhp_arena *a, size_t cap) {
    b->a = a;
    b->len = 0;
    b->cap = 0;
    b->p = NULL;
    if (cap) {
        b->p = (char *)fhp_alloc(a, cap);
        if (b->p) b->cap = cap;
    }
}

void fhp_buf_grow(fhp_buf *b, size_t extra) {
    size_t cap = b->cap ? b->cap * 2 : 64;
    char *p;
    while (cap < b->len + extra) cap *= 2;
    p = (char *)fhp_realloc(b->a, b->p, b->cap, cap);
    if (!p) return;
    b->p = p;
    b->cap = cap;
}

void fhp_buf_add_cp(fhp_buf *b, uint32_t cp) {
    char tmp[4];
    fhp_buf_add(b, tmp, fhp_utf8_encode(cp, tmp));
}

void fhp_ivec_init(fhp_ivec *v, fhp_arena *a, size_t cap) {
    v->a = a;
    v->len = 0;
    v->cap = 0;
    v->p = NULL;
    if (cap) {
        v->p = (int32_t *)fhp_alloc(a, cap * sizeof(int32_t));
        if (v->p) v->cap = cap;
    }
}

void fhp_ivec_grow(fhp_ivec *v) {
    size_t cap = v->cap ? v->cap * 2 : 16;
    int32_t *p = (int32_t *)fhp_realloc(v->a, v->p, v->cap * sizeof(int32_t), cap * sizeof(int32_t));
    if (!p) return;
    v->p = p;
    v->cap = cap;
}

/* ----- byte search ----- */

const char *fhp_memchr2(const char *s, size_t n, char a, char b) {
    size_t i = 0;
#ifdef FHP_SSE2
    __m128i va = _mm_set1_epi8(a), vb = _mm_set1_epi8(b);
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(s + i));
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(x, va), _mm_cmpeq_epi8(x, vb)));
        if (m) return s + i + fhp_ctz(m);
    }
#endif
    for (; i < n; i++)
        if (s[i] == a || s[i] == b) return s + i;
    return NULL;
}

#ifndef FHP_SSE2
const char *fhp_memchr3(const char *s, size_t n, char a, char b, char c) {
    size_t i = 0;
#if 0
    __m128i va = _mm_set1_epi8(a), vb = _mm_set1_epi8(b), vc = _mm_set1_epi8(c);
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i e = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, va), _mm_cmpeq_epi8(x, vb)), _mm_cmpeq_epi8(x, vc));
        unsigned m = (unsigned)_mm_movemask_epi8(e);
        if (m) return s + i + fhp_ctz(m);
    }
#endif
    for (; i < n; i++)
        if (s[i] == a || s[i] == b || s[i] == c) return s + i;
    return NULL;
}

#endif

size_t fhp_count_byte(const char *s, size_t n, char c) {
    size_t i = 0, count = 0;
#ifdef FHP_SSE2
    __m128i vc = _mm_set1_epi8(c), zero = _mm_setzero_si128();
    while (i + 16 <= n) {
        __m128i acc = _mm_setzero_si128();
        size_t k = 0;
        for (; k < 255 && i + 16 <= n; k++, i += 16) {
            __m128i x = _mm_loadu_si128((const __m128i *)(s + i));
            acc = _mm_sub_epi8(acc, _mm_cmpeq_epi8(x, vc));
        }
        acc = _mm_sad_epu8(acc, zero);
        count += (size_t)_mm_cvtsi128_si32(acc) + (size_t)_mm_cvtsi128_si32(_mm_srli_si128(acc, 8));
    }
#endif
    for (; i < n; i++) count += s[i] == c;
    return count;
}

int fhp_eq_ignore_ascii_case(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        if (fhp_lower_ascii((unsigned char)a[i]) != fhp_lower_ascii((unsigned char)b[i])) return 0;
    return 1;
}

ptrdiff_t fhp_index_fold(const char *s, size_t n, const char *sub, size_t m) {
    unsigned char c, lower, upper;
    size_t i = 0, last;
    if (m == 0) return 0;
    if (m > n) return -1;
    c = (unsigned char)sub[0];
    lower = fhp_lower_ascii(c);
    upper = fhp_upper_ascii(c);
    last = n - m;
    while (i <= last) {
        const char *p = lower == upper ? (const char *)fhp_memchr(s + i, lower, last - i + 1)
                                       : fhp_memchr2(s + i, last - i + 1, (char)lower, (char)upper);
        if (!p) return -1;
        i = (size_t)(p - s);
        if (fhp_eq_ignore_ascii_case(s + i, sub, m)) return (ptrdiff_t)i;
        i++;
    }
    return -1;
}

ptrdiff_t fhp_index(const char *s, size_t n, const char *sub, size_t m) {
    size_t i = 0, last;
    if (m == 0) return 0;
    if (m > n) return -1;
    last = n - m;
    while (i <= last) {
        const char *p = (const char *)fhp_memchr(s + i, sub[0], last - i + 1);
        if (!p) return -1;
        i = (size_t)(p - s);
        if (memcmp(s + i, sub, m) == 0) return (ptrdiff_t)i;
        i++;
    }
    return -1;
}

int fhp_is_lower_ascii(fhp_str s) {
    size_t i;
    for (i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (c >= 0x80 || (unsigned)(c - 'A') < 26) return 0;
    }
    return 1;
}

/* ----- UTF-8 ----- */

uint32_t fhp_utf8_decode(const char *str, size_t len, size_t *size) {
    const unsigned char *s = (const unsigned char *)str;
    uint32_t c;
    if (len == 0) {
        *size = 0;
        return 0;
    }
    c = s[0];
    if (c < 0x80) {
        *size = 1;
        return c;
    }
    if (c >= 0xC2 && c <= 0xDF && len >= 2 && (s[1] & 0xC0) == 0x80) {
        *size = 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if (c >= 0xE0 && c <= 0xEF && len >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t v = ((c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (v >= 0x800 && (v < 0xD800 || v > 0xDFFF)) {
            *size = 3;
            return v;
        }
    }
    if (c >= 0xF0 && c <= 0xF4 && len >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        uint32_t v = ((c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) |
                     (s[3] & 0x3F);
        if (v >= 0x10000 && v <= 0x10FFFF) {
            *size = 4;
            return v;
        }
    }
    *size = 1;
    return 0xFFFD;
}

size_t fhp_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The length of the longest valid UTF-8 prefix. */
size_t fhp_utf8_valid_prefix(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
#ifdef FHP_SSE2
        while (i + 16 <= len && _mm_movemask_epi8(_mm_loadu_si128((const __m128i *)(s + i))) == 0) i += 16;
        if (i >= len) break;
#endif
        if ((unsigned char)s[i] < 0x80) {
            i++;
            continue;
        }
        {
            size_t size;
            uint32_t c = fhp_utf8_decode(s + i, len - i, &size);
            if (c == 0xFFFD && size == 1) {
                /* a real U+FFFD is 3 bytes: size 1 is an invalid byte */
                return i;
            }
            i += size;
        }
    }
    return len;
}

size_t fhp_utf16_len(fhp_str s) {
    size_t i = 0, n = 0;
    while (i < s.len) {
        size_t size;
        uint32_t c = fhp_utf8_decode(s.ptr + i, s.len - i, &size);
        n += c >= 0x10000 ? 2 : 1;
        i += size;
    }
    return n;
}

size_t fhp_char_count(fhp_str s) {
    size_t i = 0, n = 0;
    while (i < s.len) {
        size_t size;
        fhp_utf8_decode(s.ptr + i, s.len - i, &size);
        n++;
        i += size;
    }
    return n;
}

/* ----- Unicode properties ----- */

int fhp_is_white_space(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= 0x09 && c <= 0x0d);
    return c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

static int in_ranges(const unsigned (*r)[2], size_t n, uint32_t c) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < r[mid][0])
            hi = mid;
        else if (c > r[mid][1])
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

int fhp_is_alphabetic(uint32_t c) {
    if (c < 0x80) return fhp_is_alpha_ascii((unsigned char)c);
    return in_ranges(fhp_alpha_ranges, fhp_alpha_count, c);
}

int fhp_is_numeric(uint32_t c) {
    if (c < 0x80) return fhp_is_digit((unsigned char)c);
    return in_ranges(fhp_numeric_ranges, fhp_numeric_count, c);
}

int fhp_space_at(const char *s, size_t len, size_t i, size_t *size) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    if (c < 0x80) {
        *size = 1;
        return fhp_is_space(c);
    }
    cp = fhp_utf8_decode(s + i, len - i, size);
    return fhp_is_white_space(cp);
}

fhp_str fhp_trim_start(fhp_str s) {
    size_t i = 0;
    while (i < s.len) {
        size_t size;
        if (!fhp_space_at(s.ptr, s.len, i, &size)) break;
        i += size;
    }
    return fhp_s(s.ptr + i, s.len - i);
}

fhp_str fhp_trim_end(fhp_str s) {
    size_t end = s.len;
    while (end > 0) {
        unsigned char c = (unsigned char)s.ptr[end - 1];
        if (c < 0x80) {
            if (!fhp_is_space(c)) break;
            end--;
            continue;
        }
        {
            /* the start of the last char */
            size_t k = end - 1, size;
            uint32_t cp;
            while (k > 0 && end - k < 4 && ((unsigned char)s.ptr[k] & 0xC0) == 0x80) k--;
            cp = fhp_utf8_decode(s.ptr + k, end - k, &size);
            if (k + size != end || !fhp_is_white_space(cp)) break;
            end = k;
        }
    }
    return fhp_s(s.ptr, end);
}

fhp_str fhp_trim(fhp_str s) { return fhp_trim_end(fhp_trim_start(s)); }

fhp_str fhp_trim_ascii_end(fhp_str s) {
    while (s.len > 0 && fhp_is_space((unsigned char)s.ptr[s.len - 1])) s.len--;
    return fhp_s(s.ptr, s.len);
}

static const char *case_lookup(const fhp_case_map *m, size_t n, uint32_t c) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < m[mid].cp)
            hi = mid;
        else if (c > m[mid].cp)
            lo = mid + 1;
        else
            return m[mid].to;
    }
    return NULL;
}

/* The single-char lower case of c, or c (Rust: to_lowercase giving one char). */
static uint32_t fold(uint32_t c) {
    const char *to;
    size_t size;
    uint32_t l;
    if (c < 0x80) return fhp_lower_ascii((unsigned char)c);
    to = case_lookup(fhp_lower_map, fhp_lower_count, c);
    if (!to) return c;
    l = fhp_utf8_decode(to, strlen(to), &size);
    return to[size] == 0 ? l : c;
}

int fhp_equal_fold(fhp_str a, fhp_str b) {
    size_t i = 0, j = 0;
    int ascii = 1;
    for (i = 0; i < a.len && ascii; i++) ascii = (unsigned char)a.ptr[i] < 0x80;
    for (i = 0; i < b.len && ascii; i++) ascii = (unsigned char)b.ptr[i] < 0x80;
    if (ascii) return a.len == b.len && fhp_eq_ignore_ascii_case(a.ptr, b.ptr, a.len);
    i = 0;
    for (;;) {
        size_t sa, sb;
        uint32_t c, d;
        if (i >= a.len || j >= b.len) return i >= a.len && j >= b.len;
        c = fhp_utf8_decode(a.ptr + i, a.len - i, &sa);
        d = fhp_utf8_decode(b.ptr + j, b.len - j, &sb);
        if (c != d && fold(c) != fold(d)) return 0;
        i += sa;
        j += sb;
    }
}

static fhp_str map_case(fhp_arena *a, fhp_str s, int upper) {
    fhp_buf b;
    size_t i = 0;
    fhp_buf_init(&b, a, s.len + 8);
    while (i < s.len) {
        unsigned char c = (unsigned char)s.ptr[i];
        size_t size;
        uint32_t cp;
        const char *to;
        if (c < 0x80) {
            fhp_buf_addc(&b, (char)(upper ? fhp_upper_ascii(c) : fhp_lower_ascii(c)));
            i++;
            continue;
        }
        cp = fhp_utf8_decode(s.ptr + i, s.len - i, &size);
        to = upper ? case_lookup(fhp_upper_map, fhp_upper_count, cp) : case_lookup(fhp_lower_map, fhp_lower_count, cp);
        if (to)
            fhp_buf_add(&b, to, strlen(to));
        else
            fhp_buf_add_cp(&b, cp);
        i += size;
    }
    return fhp_buf_str(&b);
}

fhp_str fhp_to_lower(fhp_arena *a, fhp_str s) { return map_case(a, s, 0); }
fhp_str fhp_to_upper(fhp_arena *a, fhp_str s) { return map_case(a, s, 1); }

fhp_str fhp_lower_key(fhp_arena *a, fhp_str s) {
    if (fhp_is_lower_ascii(s)) return s;
    return map_case(a, s, 0);
}

/* ----- numbers ----- */

/* The shortest round-trip digits, written without an exponent (XPath has none), as Rust's "{}" writes an f64. */
void fhp_format_number(double d, fhp_buf *out) {
    char tmp[64], digits[32];
    int prec, exp10 = 0, nd = 0, neg = 0, i;
    char *e;
    if (d != d) {
        fhp_buf_add(out, "NaN", 3);
        return;
    }
    if (d > 1.7976931348623157e308) {
        fhp_buf_add(out, "Infinity", 8);
        return;
    }
    if (d < -1.7976931348623157e308) {
        fhp_buf_add(out, "-Infinity", 9);
        return;
    }
    if (d == 0) {
        fhp_buf_addc(out, '0');
        return;
    }
    for (prec = 1; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*e", prec - 1, d);
        if (strtod(tmp, NULL) == d) break;
    }
    /* tmp: [-]d.ddde[+-]xx */
    e = strchr(tmp, 'e');
    exp10 = atoi(e + 1);
    *e = 0;
    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '-')
            neg = 1;
        else if (fhp_is_digit((unsigned char)tmp[i]))
            digits[nd++] = tmp[i];
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    if (neg) fhp_buf_addc(out, '-');
    if (exp10 < 0) {
        fhp_buf_add(out, "0.", 2);
        for (i = 0; i < -exp10 - 1; i++) fhp_buf_addc(out, '0');
        fhp_buf_add(out, digits, (size_t)nd);
    } else if (exp10 + 1 >= nd) {
        fhp_buf_add(out, digits, (size_t)nd);
        for (i = nd; i < exp10 + 1; i++) fhp_buf_addc(out, '0');
    } else {
        fhp_buf_add(out, digits, (size_t)exp10 + 1);
        fhp_buf_addc(out, '.');
        fhp_buf_add(out, digits + exp10 + 1, (size_t)(nd - exp10 - 1));
    }
}

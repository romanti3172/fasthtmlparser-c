/* HTML5 character reference decoder (WHATWG "character reference state"): all 2125 named references, the legacy names
 * without ';', numeric references with or without ';', and the spec replacements (0, surrogates, > U+10FFFF ->
 * U+FFFD; 0x80-0x9F -> Windows-1252). */
#include "internal.h"

/* A value read from the page differs from its source when it has a character reference or a CR (HTML reads CR LF and
   a lone CR as LF). */
int fhp_needs_decoding(fhp_str s) { return s.len && fhp_memchr2(s.ptr, s.len, '&', '\r') != NULL; }

/* HTML input stream preprocessing: CR LF and a lone CR become LF. */
fhp_str fhp_normalize_newlines(fhp_arena *a, fhp_str s) {
    fhp_buf b;
    size_t i;
    if (!s.len || !fhp_memchr(s.ptr, '\r', s.len)) return s;
    fhp_buf_init(&b, a, s.len);
    for (i = 0; i < s.len; i++) {
        if (s.ptr[i] != '\r') {
            fhp_buf_addc(&b, s.ptr[i]);
        } else {
            fhp_buf_addc(&b, '\n');
            if (i + 1 < s.len && s.ptr[i + 1] == '\n') i++;
        }
    }
    return fhp_buf_str(&b);
}

/* Replacements for numeric references 0x80..0x9F; 0 = keep the code point as is. */
static const uint16_t windows_1252[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

size_t fhp_match_numeric(const char *s, size_t len, uint32_t *cp) {
    size_t i = 1, digits;
    int hex = i < len && ((unsigned char)s[i] | 0x20) == 'x';
    uint32_t value = 0;
    if (hex) i++;
    digits = i;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        uint32_t d;
        if (fhp_is_digit(c))
            d = c - '0';
        else if (hex && (unsigned)((c | 0x20) - 'a') < 6)
            d = (uint32_t)((c | 0x20) - 'a') + 10;
        else
            break;
        value = hex ? value * 16 + d : value * 10 + d;
        if (value > 0x10FFFF) value = 0x110000; /* saturate: no overflow on long digit runs */
        i++;
    }
    if (i == digits) return 0; /* "&#" / "&#x" without digits is plain text */
    if (i < len && s[i] == ';') i++;
    if (value == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        value = 0xFFFD;
    } else if (value >= 0x80 && value <= 0x9F) {
        uint32_t mapped = windows_1252[value - 0x80];
        if (mapped) value = mapped;
    }
    *cp = value;
    return i;
}

static const fhp_entity *lookup_entity(const char *name, size_t n) {
    size_t lo = 0, hi = FHP_ENTITY_COUNT;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const fhp_entity *e = &fhp_entities[mid];
        size_t m = e->name_len < n ? e->name_len : n;
        int c = memcmp(e->name, name, m);
        if (c == 0) c = e->name_len < n ? -1 : e->name_len > n ? 1 : 0;
        if (c < 0)
            lo = mid + 1;
        else if (c > 0)
            hi = mid;
        else
            return e;
    }
    return NULL;
}

FHP_INLINE int is_alnum(unsigned char c) { return fhp_is_alpha_ascii(c) || fhp_is_digit(c); }

size_t fhp_match_named(const char *s, size_t len, int in_attribute, fhp_str *value) {
    size_t limit = len < FHP_ENTITY_MAX_NAME + 1 ? len : FHP_ENTITY_MAX_NAME + 1;
    size_t n = 0, k;
    while (n < limit && is_alnum((unsigned char)s[n])) n++;
    if (n < 2) return 0;
    /* Full match: "&name;". */
    if (n < len && s[n] == ';') {
        const fhp_entity *e = lookup_entity(s, n);
        if (e) {
            *value = fhp_s(e->value, e->value_len);
            return n + 1;
        }
    }
    /* Longest legacy prefix without ';': "&copy2024" -> "(c)2024", "&notit;" -> "(not)it;". */
    k = n < FHP_ENTITY_MAX_LEGACY_NAME ? n : FHP_ENTITY_MAX_LEGACY_NAME;
    for (; k >= 2; k--) {
        const fhp_entity *e = lookup_entity(s, k);
        if (e && e->legacy) {
            if (in_attribute && k < len && (s[k] == '=' || is_alnum((unsigned char)s[k]))) break;
            *value = fhp_s(e->value, e->value_len);
            return k;
        }
    }
    return 0;
}

/* With in_attribute, a legacy name without ';' followed by '=' or an alphanumeric is left as is (so "?a=1&copy=2"
   stays intact). Without '&' and CR the input is returned as is. */
fhp_str fhp_decode(fhp_arena *a, fhp_str s, int in_attribute) {
    fhp_buf out;
    const char *amp;
    size_t pos = 0;
    /* Line breaks first, as a browser reads the page; a CR written as "&#13;" is decoded after that and stays. */
    s = fhp_normalize_newlines(a, s);
    amp = s.len ? (const char *)fhp_memchr(s.ptr, '&', s.len) : NULL;
    if (!amp) return s;
    fhp_buf_init(&out, a, s.len);
    for (;;) {
        size_t at = (size_t)(amp - s.ptr), consumed = 0;
        const char *rest = s.ptr + at + 1;
        size_t rest_len = s.len - at - 1;
        fhp_buf_add(&out, s.ptr + pos, at - pos);
        if (rest_len && rest[0] == '#') {
            uint32_t cp;
            consumed = fhp_match_numeric(rest, rest_len, &cp);
            if (consumed) fhp_buf_add_cp(&out, cp);
        } else {
            fhp_str value;
            consumed = fhp_match_named(rest, rest_len, in_attribute, &value);
            if (consumed) fhp_buf_adds(&out, value);
        }
        if (!consumed) fhp_buf_addc(&out, '&'); /* not a reference: keep '&' */
        pos = at + 1 + consumed;
        amp = pos < s.len ? (const char *)fhp_memchr(s.ptr + pos, '&', s.len - pos) : NULL;
        if (!amp) break;
    }
    fhp_buf_add(&out, s.ptr + pos, s.len - pos);
    return fhp_buf_str(&out);
}

fhp_str fhp_decode_html(const char *s, size_t len, fhp_arena *a) {
    if (!a) return FHP_EMPTY;
    return fhp_decode(a, fhp_s(s, len), 0);
}

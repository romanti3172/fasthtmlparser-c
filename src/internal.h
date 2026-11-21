/* Internal declarations shared by the sources of the library. */
#ifndef FHP_INTERNAL_H
#define FHP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fasthtmlparser.h"
#include "tables_gen.h"

#if defined(_MSC_VER)
#define FHP_INLINE static __inline
#define FHP_TLS __declspec(thread)
#else
#define FHP_INLINE static inline
#define FHP_TLS __thread
#endif

/* An internal tag key: the index into the tag tables (FHP_TAG_* - 1), -1 for a custom name. */
#define KEY(tag) ((tag) - 1)

enum {
    FHP_F_VOID = 1,
    FHP_F_CLOSES_P = 2,
    FHP_F_RULE_TARGET = 4,
    FHP_F_RAW_TEXT = 8
};

enum { LB_NONE = 0, LB_SPACE = 1, LB_LINE = 2, LB_PARAGRAPH = 3, LB_SKIP = 4 };

typedef struct fhp_rule {
    short target_keys[FHP_MAX_RULE_TARGETS];
    unsigned char target_count;
    unsigned char targets[FHP_TAG_COUNT];
    unsigned char stops[FHP_TAG_COUNT];
} fhp_rule;

typedef struct fhp_entity {
    const char *name;
    const char *value;
    unsigned char name_len;
    unsigned char value_len;
    unsigned char legacy;
} fhp_entity;

typedef struct fhp_case_map {
    unsigned cp;
    const char *to;
} fhp_case_map;

extern const char *const fhp_tag_names[FHP_TAG_COUNT];
extern const unsigned char fhp_tag_name_lens[FHP_TAG_COUNT];
extern const short fhp_tag_hash[FHP_TAG_HASH_SIZE];
extern const unsigned char fhp_tag_flags[FHP_TAG_COUNT];
extern const signed char fhp_tag_rule[FHP_TAG_COUNT];
extern const unsigned char fhp_tag_line_break[FHP_TAG_COUNT];
extern const fhp_rule fhp_rules[FHP_RULE_COUNT];
extern const fhp_entity fhp_entities[FHP_ENTITY_COUNT];
extern const unsigned fhp_alpha_ranges[][2];
extern const size_t fhp_alpha_count;
extern const unsigned fhp_numeric_ranges[][2];
extern const size_t fhp_numeric_count;
extern const fhp_case_map fhp_lower_map[];
extern const size_t fhp_lower_count;
extern const fhp_case_map fhp_upper_map[];
extern const size_t fhp_upper_count;

/* ----- arena ----- */

typedef struct fhp_chunk {
    struct fhp_chunk *next;
    size_t cap;
    size_t used;
    /* data follows, aligned to 16 */
} fhp_chunk;

struct fhp_arena {
    fhp_chunk *first;
    fhp_chunk *cur;
    size_t chunk_size;
    size_t used_before; /* bytes in the chunks before cur */
};

typedef struct fhp_mark {
    fhp_chunk *chunk;
    size_t used;
    size_t used_before;
} fhp_mark;

void fhp_arena_init(fhp_arena *a, size_t chunk_size);
void fhp_arena_destroy(fhp_arena *a);
void *fhp_alloc(fhp_arena *a, size_t size);
/* Grows the last allocation in place when it is at the top of the arena; otherwise allocates and copies. */
void *fhp_realloc(fhp_arena *a, void *p, size_t old_size, size_t new_size);
char *fhp_strndup(fhp_arena *a, const char *s, size_t len);
fhp_mark fhp_arena_mark(fhp_arena *a);
void fhp_arena_release(fhp_arena *a, fhp_mark m);

/* The scratch arena of the calling thread, for temporaries (release to a mark when done). */
fhp_arena *fhp_scratch(void);

/* ----- strings ----- */

static const char fhp_empty_string[1] = "";

FHP_INLINE fhp_str fhp_s(const char *p, size_t len) {
    fhp_str s;
    s.ptr = len ? p : fhp_empty_string;
    s.len = len;
    return s;
}

FHP_INLINE fhp_str fhp_cs(const char *z) {
    return z ? fhp_s(z, strlen(z)) : fhp_s(NULL, 0);
}

#define FHP_EMPTY fhp_s(NULL, 0)

FHP_INLINE int fhp_str_eq(fhp_str a, fhp_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

FHP_INLINE int fhp_starts_with(fhp_str s, fhp_str prefix) {
    return s.len >= prefix.len && (prefix.len == 0 || memcmp(s.ptr, prefix.ptr, prefix.len) == 0);
}

FHP_INLINE int fhp_ends_with(fhp_str s, fhp_str suffix) {
    return s.len >= suffix.len && (suffix.len == 0 || memcmp(s.ptr + s.len - suffix.len, suffix.ptr, suffix.len) == 0);
}

/* A growable byte buffer in an arena. */
typedef struct fhp_buf {
    char *p;
    size_t len, cap;
    fhp_arena *a;
} fhp_buf;

void fhp_buf_init(fhp_buf *b, fhp_arena *a, size_t cap);
void fhp_buf_grow(fhp_buf *b, size_t extra);
FHP_INLINE void fhp_buf_add(fhp_buf *b, const char *s, size_t n) {
    if (n == 0) return;
    if (b->len + n > b->cap) fhp_buf_grow(b, n);
    if (b->cap >= b->len + n) {
        memcpy(b->p + b->len, s, n);
        b->len += n;
    }
}
FHP_INLINE void fhp_buf_addc(fhp_buf *b, char c) {
    if (b->len + 1 > b->cap) fhp_buf_grow(b, 1);
    if (b->cap > b->len) b->p[b->len++] = c;
}
FHP_INLINE void fhp_buf_adds(fhp_buf *b, fhp_str s) { fhp_buf_add(b, s.ptr, s.len); }
void fhp_buf_add_cp(fhp_buf *b, uint32_t cp);
FHP_INLINE fhp_str fhp_buf_str(const fhp_buf *b) { return fhp_s(b->p, b->len); }

/* A growable array of int32 in an arena. */
typedef struct fhp_ivec {
    int32_t *p;
    size_t len, cap;
    fhp_arena *a;
} fhp_ivec;

void fhp_ivec_init(fhp_ivec *v, fhp_arena *a, size_t cap);
void fhp_ivec_grow(fhp_ivec *v);
FHP_INLINE void fhp_ivec_push(fhp_ivec *v, int32_t x) {
    if (v->len == v->cap) fhp_ivec_grow(v);
    if (v->len < v->cap) v->p[v->len++] = x;
}

/* ----- characters ----- */

FHP_INLINE int fhp_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0c || c == 0x0b;
}
FHP_INLINE int fhp_is_alpha_ascii(unsigned char c) { return (unsigned)((c | 0x20) - 'a') < 26; }
FHP_INLINE int fhp_is_digit(unsigned char c) { return (unsigned)(c - '0') < 10; }
FHP_INLINE int fhp_is_hex(unsigned char c) { return fhp_is_digit(c) || (unsigned)((c | 0x20) - 'a') < 6; }
FHP_INLINE unsigned char fhp_lower_ascii(unsigned char c) { return (unsigned)(c - 'A') < 26 ? (unsigned char)(c | 0x20) : c; }
FHP_INLINE unsigned char fhp_upper_ascii(unsigned char c) { return (unsigned)(c - 'a') < 26 ? (unsigned char)(c & ~0x20) : c; }
FHP_INLINE int fhp_is_char_boundary(const char *s, size_t len, size_t i) {
    return i == 0 || i >= len || ((unsigned char)s[i] & 0xC0) != 0x80;
}

#if !defined(FHP_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64))
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
FHP_INLINE unsigned fhp_ctz32(unsigned x) {
    unsigned long i;
    _BitScanForward(&i, x);
    return (unsigned)i;
}
#else
#define fhp_ctz32(x) ((unsigned)__builtin_ctz(x))
#endif
/* memchr for the many short searches of the parser (the C library's has a high cost per call on some platforms). */
FHP_INLINE const void *fhp_memchr(const void *p, int c, size_t n) {
    const char *s = (const char *)p;
    size_t i = 0;
    __m128i v = _mm_set1_epi8((char)c);
    for (; i + 16 <= n; i += 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(s + i)), v));
        if (m) return s + i + fhp_ctz32(m);
    }
    for (; i < n; i++)
        if (s[i] == (char)c) return s + i;
    return NULL;
}
/* The first of three bytes (the parser inside a tag: '<', '=', '>'). */
FHP_INLINE const char *fhp_memchr3(const char *s, size_t n, char a, char b, char c) {
    size_t i = 0;
    __m128i va = _mm_set1_epi8(a), vb = _mm_set1_epi8(b), vc = _mm_set1_epi8(c);
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i e = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, va), _mm_cmpeq_epi8(x, vb)), _mm_cmpeq_epi8(x, vc));
        unsigned m = (unsigned)_mm_movemask_epi8(e);
        if (m) return s + i + fhp_ctz32(m);
    }
    for (; i < n; i++)
        if (s[i] == a || s[i] == b || s[i] == c) return s + i;
    return NULL;
}
#else
#define fhp_memchr memchr
const char *fhp_memchr3(const char *s, size_t n, char a, char b, char c);
#endif

const char *fhp_memchr2(const char *s, size_t n, char a, char b);
size_t fhp_count_byte(const char *s, size_t n, char c);
/* strings.Index with ASCII case folding; sub must be ASCII. -1 when not found. */
ptrdiff_t fhp_index_fold(const char *s, size_t n, const char *sub, size_t m);
ptrdiff_t fhp_index(const char *s, size_t n, const char *sub, size_t m);
int fhp_eq_ignore_ascii_case(const char *a, const char *b, size_t n);
int fhp_is_lower_ascii(fhp_str s);

/* UTF-8: the code point at s (0xFFFD for an invalid sequence, which counts 1 byte) and its size. */
uint32_t fhp_utf8_decode(const char *s, size_t len, size_t *size);
size_t fhp_utf8_encode(uint32_t cp, char *out);
size_t fhp_utf8_valid_prefix(const char *s, size_t len);
size_t fhp_utf16_len(fhp_str s);
size_t fhp_char_count(fhp_str s);

int fhp_is_white_space(uint32_t cp); /* Unicode White_Space, as Rust char::is_whitespace / .NET IsWhiteSpace */
int fhp_is_alphabetic(uint32_t cp);
int fhp_is_numeric(uint32_t cp);
/* s[i...] starts with whitespace (ASCII or Unicode); size: its length. */
int fhp_space_at(const char *s, size_t len, size_t i, size_t *size);
fhp_str fhp_trim(fhp_str s);       /* Unicode whitespace at both ends */
fhp_str fhp_trim_start(fhp_str s);
fhp_str fhp_trim_end(fhp_str s);
fhp_str fhp_trim_ascii_end(fhp_str s);
/* Case-insensitive equality as Go's strings.EqualFold: ASCII fast path, simple case folding otherwise. */
int fhp_equal_fold(fhp_str a, fhp_str b);
/* The lower-case string (Unicode, full mapping); s itself when it is lower-case ASCII already. */
fhp_str fhp_lower_key(fhp_arena *a, fhp_str s);
fhp_str fhp_to_lower(fhp_arena *a, fhp_str s);
fhp_str fhp_to_upper(fhp_arena *a, fhp_str s);

/* ----- the document ----- */

#define FHP_NO_CUSTOM 0xFFFFFFFFu

typedef struct fhp_nodedata {
    uint32_t custom;     /* index into custom names for tags not in the tables; FHP_NO_CUSTOM otherwise */
    int32_t parent;      /* index of the enclosing tag, -1 at the top level */
    int32_t start;       /* the opening '<' */
    int32_t end;         /* the last '>': of the end tag when there is one, otherwise of the opening tag */
    int32_t inner_start; /* right after the opening tag; 0 when the tag is not closed */
    int32_t inner_end;   /* the last byte before the end tag; 0 when the tag is not closed */
    int32_t depth;
    int16_t key; /* tag key, or -1 */
    int16_t pad;
} fhp_nodedata;

typedef struct fhp_url fhp_url;

typedef struct fhp_css_tree {
    int32_t *previous; /* previous element sibling, -1 for none */
    int32_t *next;
    int32_t *position; /* 1-based position among the element siblings; 0: not an element */
    int32_t *from_end;
    int32_t *type_position; /* the same among the siblings with the same name */
    int32_t *type_from_end;
} fhp_css_tree;

struct fhp_doc {
    const char *src;
    size_t len;
    fhp_nodedata *nodes;
    size_t count;
    fhp_str *custom_names;
    size_t custom_count;
    fhp_str url;
    fhp_url *base; /* the page URL parsed once; NULL when there is none */
    fhp_arena mem;      /* the document's own memory: source copy, nodes, names, lazy data */
    fhp_arena *results; /* the arena NULL stands for, created on first use */
    fhp_css_tree *css;
    int css_types; /* type positions built */
    int has_pre;   /* 0: not looked up, 1: no pre, listing or plaintext in the page, 2: there is one (fhp_doc_has_pre) */
};

FHP_INLINE const fhp_nodedata *fhp_nd(const fhp_doc *d, fhp_node n) { return &d->nodes[n]; }
FHP_INLINE int fhp_valid(const fhp_doc *d, fhp_node n) { return d && n >= 0 && (size_t)n < d->count; }
FHP_INLINE fhp_str fhp_src_slice(const fhp_doc *d, size_t from, size_t to) { return fhp_s(d->src + from, to - from); }
fhp_arena *fhp_result_arena(const fhp_doc *d, fhp_arena *a);

/* parse.c */
int fhp_parse_into(fhp_doc *doc, size_t max_tags);
int fhp_lookup_key(const char *name, size_t len); /* tag key, or -1 */
int fhp_is_markup_start(const char *s, size_t len, size_t i);
size_t fhp_find_raw_text_end(const char *s, size_t len, size_t from, const char *end_tag, size_t end_len);
size_t fhp_find_comment_end(const char *s, size_t len, size_t content_start);
FHP_INLINE int fhp_has_raw_text(int key) { return key >= 0 && (fhp_tag_flags[key] & FHP_F_RAW_TEXT); }

/* decode.c */
int fhp_needs_decoding(fhp_str s);
fhp_str fhp_normalize_newlines(fhp_arena *a, fhp_str s);
fhp_str fhp_decode(fhp_arena *a, fhp_str s, int in_attribute);
/* s starts with '#': bytes consumed (0: not a reference) and the code point. */
size_t fhp_match_numeric(const char *s, size_t len, uint32_t *cp);
/* s is the text right after '&': bytes consumed (0: not a reference) and the value. */
size_t fhp_match_named(const char *s, size_t len, int in_attribute, fhp_str *value);

/* attributes.c */
typedef struct fhp_scanned_attr {
    size_t name_start, name_len;
    int has_value;
    size_t value_start, value_len;
} fhp_scanned_attr;
size_t fhp_open_tag_end(const fhp_doc *doc, fhp_node n);
void fhp_attr_scanner(const fhp_doc *doc, fhp_node n, fhp_attr_iter *it);
int fhp_next_attr(fhp_attr_iter *it, fhp_scanned_attr *a);
int fhp_attr_equals(fhp_str raw, fhp_str value);
int fhp_has_attribute(const fhp_doc *doc, fhp_node n, fhp_str name);
fhp_str fhp_attr_raw_s(const fhp_doc *doc, fhp_node n, fhp_str name);
int fhp_has_class_s(const fhp_doc *doc, fhp_node n, fhp_str cls);

/* text.c */
fhp_str fhp_clear_tags(fhp_arena *a, fhp_str s);
fhp_str fhp_clear_tags_line_breaks(fhp_arena *a, fhp_str s);
int fhp_is_text(const fhp_doc *doc, fhp_node n, fhp_str expected);
fhp_str fhp_clear_tags_in(fhp_arena *a, fhp_str s, int in_pre);
int fhp_needs_collapse(fhp_str s);
int fhp_doc_has_pre(const fhp_doc *doc);
int fhp_in_pre(const fhp_doc *doc, fhp_node n);

/* document.c */
int fhp_is_element(const fhp_doc *doc, size_t i);
fhp_str fhp_body_s(const fhp_doc *doc, fhp_node n);
fhp_str fhp_name_s(const fhp_doc *doc, fhp_node n);

/* links.c */
fhp_url *fhp_absolute_url(fhp_arena *a, fhp_str s);
fhp_str fhp_correct_link(fhp_arena *out, fhp_str raw, fhp_str page_url, const fhp_url *base);

/* css.c */
const fhp_css_tree *fhp_doc_tree(const fhp_doc *doc);
void fhp_doc_type_positions(const fhp_doc *doc);

/* numbers */
void fhp_format_number(double d, fhp_buf *out);

#endif

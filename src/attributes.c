/* Attributes: read in place from the opening tag. */
#include "internal.h"

#define K_COMMENT KEY(FHP_TAG_COMMENT)

/* The position of the '>' that ends the opening tag. */
size_t fhp_open_tag_end(const fhp_doc *doc, fhp_node n) {
    const fhp_nodedata *d = fhp_nd(doc, n);
    size_t end = (size_t)(d->inner_start > 0 && d->inner_end > 0 ? d->inner_start - 1 : d->end);
    return end < doc->len ? end : doc->len;
}

/* Reads the value that follows an attribute name at pos; returns the position after it. */
static size_t read_value_at(const char *s, size_t pos, size_t end, int *has, size_t *vs, size_t *vl) {
    size_t p = pos;
    while (p < end && fhp_is_space((unsigned char)s[p])) p++;
    if (p >= end || s[p] != '=') {
        *has = 0;
        *vs = *vl = 0;
        return pos;
    }
    p++;
    while (p < end && fhp_is_space((unsigned char)s[p])) p++;
    *has = 1;
    if (p < end && (s[p] == '"' || s[p] == '\'')) {
        const char *close;
        *vs = p + 1;
        close = (const char *)fhp_memchr(s + p + 1, s[p], end - p - 1);
        if (!close) {
            *vl = end - *vs;
            return end;
        }
        *vl = (size_t)(close - s) - *vs;
        return (size_t)(close - s) + 1;
    }
    *vs = p;
    while (p < end && !fhp_is_space((unsigned char)s[p])) p++;
    *vl = p - *vs;
    return p;
}

void fhp_attr_scanner(const fhp_doc *doc, fhp_node n, fhp_attr_iter *it) {
    const char *b = doc->src;
    size_t end = fhp_open_tag_end(doc, n);
    size_t i = (size_t)fhp_nd(doc, n)->start + 1; /* skip '<' and the name */
    while (i < end) {
        char c = b[i];
        if (fhp_is_space((unsigned char)c) || c == '/' || c == '>') break;
        i++;
    }
    it->src = b;
    it->end = end;
    it->pos = i;
}

int fhp_next_attr(fhp_attr_iter *it, fhp_scanned_attr *a) {
    const char *b = it->src;
    for (;;) {
        size_t name_start;
        while (it->pos < it->end && (fhp_is_space((unsigned char)b[it->pos]) || b[it->pos] == '/')) it->pos++;
        if (it->pos >= it->end) return 0;
        name_start = it->pos;
        while (it->pos < it->end) {
            char c = b[it->pos];
            if (fhp_is_space((unsigned char)c) || c == '=' || c == '/' || c == '>') break;
            it->pos++;
        }
        if (it->pos == name_start) {
            it->pos++; /* a stray '=': skip it */
            continue;
        }
        a->name_start = name_start;
        a->name_len = it->pos - name_start;
        it->pos = read_value_at(b, it->pos, it->end, &a->has_value, &a->value_start, &a->value_len);
        return 1;
    }
}

void fhp_attrs_begin(const fhp_doc *doc, fhp_node n, fhp_attr_iter *it) {
    if (!fhp_valid(doc, n) || fhp_nd(doc, n)->key == K_COMMENT) {
        it->src = fhp_empty_string;
        it->pos = it->end = 0;
        return;
    }
    fhp_attr_scanner(doc, n, it);
}

int fhp_attrs_next(fhp_attr_iter *it, fhp_str *name, fhp_str *raw_value) {
    fhp_scanned_attr a;
    if (!fhp_next_attr(it, &a)) return 0;
    if (name) *name = fhp_s(it->src + a.name_start, a.name_len);
    if (raw_value) *raw_value = a.has_value ? fhp_s(it->src + a.value_start, a.value_len) : FHP_EMPTY;
    return 1;
}

FHP_INLINE int starts_name(char c) {
    return fhp_is_space((unsigned char)c) || c == '"' || c == '\'' || c == '/' || c == '=';
}

FHP_INLINE int ends_name(char c) { return fhp_is_space((unsigned char)c) || c == '=' || c == '/' || c == '>'; }

/* The next place at or after from where the attribute name stands as a word of its own, or -1. */
static ptrdiff_t next_word(const char *open, size_t len, fhp_str attribute, size_t from) {
    while (from < len) {
        ptrdiff_t found = fhp_index_fold(open + from, len - from, attribute.ptr, attribute.len);
        size_t after;
        if (found < 0) return -1;
        from += (size_t)found;
        after = from + attribute.len;
        if (starts_name(open[from - 1]) && (after == len || ends_name(open[after]))) return (ptrdiff_t)from;
        from++;
    }
    return -1;
}

/* Whether the quote at quote (-1: none) stands right after an '='. */
static int opens_value(const char *before, ptrdiff_t quote) {
    size_t k;
    if (quote <= 0) return 0;
    k = (size_t)quote - 1; /* the tag starts with '<' */
    while (k > 0 && fhp_is_space((unsigned char)before[k])) k--;
    return before[k] == '=';
}

static ptrdiff_t memrchr_(const char *s, size_t n, char c) {
    while (n > 0) {
        n--;
        if (s[n] == c) return (ptrdiff_t)n;
    }
    return -1;
}

/* Whether the word at pos of an opening tag is an attribute name, judged by the bytes before it alone. 0 means
   "not sure". */
static int is_name_for_sure(const char *open, size_t pos) {
    size_t last = pos - 1;
    if (!fhp_is_space((unsigned char)open[last])) return 0;
    while (last > 0 && fhp_is_space((unsigned char)open[last])) last--;
    if (open[last] == '=') return 0;
    /* Not inside a quoted value: it is enough that the last quote of each kind before the word does not open a value. */
    return !opens_value(open, memrchr_(open, last + 1, '"')) && !opens_value(open, memrchr_(open, last + 1, '\''));
}

fhp_str fhp_attr_raw_s(const fhp_doc *doc, fhp_node n, fhp_str attribute) {
    const fhp_nodedata *d = fhp_nd(doc, n);
    const char *src = doc->src;
    size_t start, end, len;
    const char *open;
    ptrdiff_t pos;
    int first_word;
    /* a comment has no attributes, whatever its text looks like */
    if (attribute.len == 0 || d->key == K_COMMENT) return FHP_EMPTY;
    start = (size_t)d->start;
    end = fhp_open_tag_end(doc, n);
    if (end <= start) return FHP_EMPTY;
    open = src + start; /* "<name attributes", without the closing '>' */
    len = end - start;

    /* The first place where the name stands as a word of its own. Most tags have none. */
    pos = next_word(open, len, attribute, 1);
    if (pos < 0) return FHP_EMPTY;

    /* That word is the attribute unless it is inside the value of another one (title="see id=5", x= id). Nearly
       always that is ruled out at once, and the value is read in place. */
    first_word = d->key >= 0 && (size_t)pos == (size_t)fhp_tag_name_lens[d->key] + 2;
    if (first_word || is_name_for_sure(open, (size_t)pos)) {
        int has;
        size_t vs, vl;
        read_value_at(src, start + (size_t)pos + attribute.len, end, &has, &vs, &vl);
        return has ? fhp_s(src + vs, vl) : FHP_EMPTY;
    }

    /* Otherwise the attributes are read one by one, up to the one that starts at that word. */
    {
        fhp_attr_iter it;
        fhp_scanned_attr a;
        fhp_attr_scanner(doc, n, &it);
        while (fhp_next_attr(&it, &a)) {
            size_t name = a.name_start - start;
            /* a word the scanner has passed was inside a value: the next one takes its place */
            while ((size_t)pos < name) {
                pos = next_word(open, len, attribute, (size_t)pos + 1);
                if (pos < 0) return FHP_EMPTY;
            }
            if ((size_t)pos == name && a.name_len == attribute.len)
                return a.has_value ? fhp_s(src + a.value_start, a.value_len) : FHP_EMPTY;
        }
    }
    return FHP_EMPTY;
}

fhp_str fhp_attr_raw(const fhp_doc *doc, fhp_node n, const char *name) {
    if (!fhp_valid(doc, n) || !name) return FHP_EMPTY;
    return fhp_attr_raw_s(doc, n, fhp_cs(name));
}

fhp_str fhp_attr(const fhp_doc *doc, fhp_node n, const char *name, fhp_arena *a) {
    if (!fhp_valid(doc, n) || !name) return FHP_EMPTY;
    return fhp_decode(fhp_result_arena(doc, a), fhp_attr_raw_s(doc, n, fhp_cs(name)), 1);
}

/* Compares the decoded value; it allocates (scratch) only when the raw value has '&' or CR. */
int fhp_attr_equals(fhp_str raw, fhp_str value) {
    if (fhp_needs_decoding(raw)) {
        fhp_arena *s = fhp_scratch();
        fhp_mark m = fhp_arena_mark(s);
        int eq = fhp_str_eq(fhp_decode(s, raw, 1), value);
        fhp_arena_release(s, m);
        return eq;
    }
    return fhp_str_eq(raw, value);
}

int fhp_has_class_s(const fhp_doc *doc, fhp_node n, fhp_str cls) {
    fhp_str raw, value;
    fhp_arena *s = NULL;
    fhp_mark m = {NULL, 0, 0};
    int result = 0;
    size_t from = 0;
    if (cls.len == 0) return 0;
    raw = fhp_attr_raw_s(doc, n, fhp_s("class", 5));
    value = raw;
    if (fhp_needs_decoding(raw)) {
        s = fhp_scratch();
        m = fhp_arena_mark(s);
        value = fhp_decode(s, raw, 1);
    }
    if (value.len == cls.len) {
        result = fhp_equal_fold(value, cls);
    } else if (cls.len < value.len) {
        while (from + cls.len <= value.len) {
            ptrdiff_t f = fhp_index_fold(value.ptr + from, value.len - from, cls.ptr, cls.len);
            size_t i, after;
            if (f < 0) break;
            i = from + (size_t)f;
            after = i + cls.len;
            if ((i == 0 || fhp_is_space((unsigned char)value.ptr[i - 1])) &&
                (after == value.len || fhp_is_space((unsigned char)value.ptr[after]))) {
                result = 1;
                break;
            }
            from = i + 1;
        }
    }
    if (s) fhp_arena_release(s, m);
    return result;
}

int fhp_has_class(const fhp_doc *doc, fhp_node n, const char *cls) {
    if (!fhp_valid(doc, n) || !cls) return 0;
    return fhp_has_class_s(doc, n, fhp_cs(cls));
}

/* Whether the element has the attribute, with or without a value. */
int fhp_has_attribute(const fhp_doc *doc, fhp_node n, fhp_str name) {
    fhp_attr_iter it;
    fhp_scanned_attr a;
    if (fhp_attr_raw_s(doc, n, name).len) return 1;
    if (fhp_nd(doc, n)->key == K_COMMENT) return 0;
    fhp_attr_scanner(doc, n, &it);
    while (fhp_next_attr(&it, &a))
        if (fhp_equal_fold(fhp_s(doc->src + a.name_start, a.name_len), name)) return 1;
    return 0;
}

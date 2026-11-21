/* Documents, reading tags, navigation and search. */
#include <stdlib.h>

#include "internal.h"

#define K_COMMENT KEY(FHP_TAG_COMMENT)

/* ----- creating a document ----- */

/* The length of the maximal invalid subpart at s (Unicode "U+FFFD substitution of maximal subparts"). */
static size_t invalid_subpart(const unsigned char *s, size_t len) {
    unsigned char c = s[0];
    size_t need, i;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF)
        need = 1;
    else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else
        return 1;
    for (i = 1; i <= need && i < len; i++) {
        unsigned char b = s[i];
        if (i == 1 ? (b < lo || b > hi) : (b & 0xC0) != 0x80) return i;
    }
    return i;
}

/* Copies src into dst replacing invalid UTF-8 with U+FFFD; returns the length written (dst must hold 3 * len). */
static size_t copy_lossy(char *dst, const char *src, size_t len) {
    size_t i = 0, o = 0;
    while (i < len) {
        size_t valid = fhp_utf8_valid_prefix(src + i, len - i);
        memcpy(dst + o, src + i, valid);
        o += valid;
        i += valid;
        if (i < len) {
            i += invalid_subpart((const unsigned char *)src + i, len - i);
            memcpy(dst + o, "\xEF\xBF\xBD", 3);
            o += 3;
        }
    }
    return o;
}

static fhp_doc *create(const char *src, size_t len, const char *page_url, int borrow) {
    fhp_arena mem;
    fhp_doc *doc;
    size_t max_tags, valid = len, copy_len = len;
    size_t url_len = page_url ? strlen(page_url) : 0;
    if (!src) len = 0;
    if (!borrow && len) {
        valid = fhp_utf8_valid_prefix(src, len);
        if (valid < len) copy_len = len * 3; /* worst case of the replacements */
    }
    max_tags = len ? fhp_count_byte(src, len, '<') : 0;
    fhp_arena_init(&mem, sizeof(fhp_doc) + (borrow ? 0 : copy_len + 1) + max_tags * sizeof(fhp_nodedata) + url_len +
                             4096);
    doc = (fhp_doc *)fhp_alloc(&mem, sizeof(fhp_doc));
    if (!doc) {
        fhp_arena_destroy(&mem);
        return NULL;
    }
    memset(doc, 0, sizeof *doc);
    mem.chunk_size = 4096; /* the first chunk holds the document; later ones are small */
    if (borrow) {
        doc->src = len ? src : fhp_empty_string;
        doc->len = len;
    } else {
        char *copy = (char *)fhp_alloc(&mem, copy_len + 1);
        if (!copy) {
            fhp_arena_destroy(&mem);
            return NULL;
        }
        if (valid == len) {
            if (len) memcpy(copy, src, len);
            doc->len = len;
        } else {
            doc->len = copy_lossy(copy, src, len);
            max_tags = fhp_count_byte(copy, doc->len, '<');
        }
        copy[doc->len] = 0;
        doc->src = copy;
    }
    doc->mem = mem;
    if (fhp_parse_into(doc, max_tags) != 0) {
        fhp_arena_destroy(&doc->mem);
        return NULL;
    }
    doc->url = FHP_EMPTY;
    if (url_len) {
        char *u = fhp_strndup(&doc->mem, page_url, url_len);
        if (u) {
            doc->url = fhp_s(u, url_len);
            doc->base = fhp_absolute_url(&doc->mem, doc->url);
        }
    }
    return doc;
}

fhp_doc *fhp_parse(const char *src, size_t len) { return create(src, len, NULL, 0); }

fhp_doc *fhp_parse_url(const char *src, size_t len, const char *page_url) { return create(src, len, page_url, 0); }

fhp_doc *fhp_parse_borrowed(const char *src, size_t len, const char *page_url) {
    return create(src, len, page_url, 1);
}

void fhp_free(fhp_doc *doc) {
    fhp_arena mem;
    if (!doc) return;
    fhp_arena_free(doc->results);
    mem = doc->mem; /* the document lives in its own arena */
    fhp_arena_destroy(&mem);
}

fhp_str fhp_doc_source(const fhp_doc *doc) { return doc ? fhp_s(doc->src, doc->len) : FHP_EMPTY; }
fhp_str fhp_doc_url(const fhp_doc *doc) { return doc ? doc->url : FHP_EMPTY; }
size_t fhp_doc_len(const fhp_doc *doc) { return doc ? doc->count : 0; }

fhp_arena *fhp_result_arena(const fhp_doc *doc, fhp_arena *a) {
    fhp_doc *d = (fhp_doc *)doc;
    if (a) return a;
    if (!d->results) {
        d->results = fhp_arena_new(0);
        if (!d->results) return fhp_scratch(); /* out of memory: the value lives until the next call */
    }
    return d->results;
}

fhp_arena *fhp_doc_arena(const fhp_doc *doc) { return doc ? fhp_result_arena(doc, NULL) : NULL; }

void fhp_doc_prepare(const fhp_doc *doc) {
    if (!doc) return;
    fhp_doc_tree(doc);
    fhp_doc_type_positions(doc);
    fhp_result_arena(doc, NULL);
}

/* ----- reading a tag ----- */

int fhp_tag(const fhp_doc *doc, fhp_node n) {
    if (!fhp_valid(doc, n)) return FHP_TAG_NONE;
    return fhp_nd(doc, n)->key + 1;
}

fhp_str fhp_name_s(const fhp_doc *doc, fhp_node n) {
    const fhp_nodedata *d = fhp_nd(doc, n);
    if (d->key >= 0) return fhp_s(fhp_tag_names[d->key], fhp_tag_name_lens[d->key]);
    if (d->custom != FHP_NO_CUSTOM) return doc->custom_names[d->custom];
    return FHP_EMPTY;
}

fhp_str fhp_name(const fhp_doc *doc, fhp_node n) { return fhp_valid(doc, n) ? fhp_name_s(doc, n) : FHP_EMPTY; }

int fhp_depth(const fhp_doc *doc, fhp_node n) { return fhp_valid(doc, n) ? fhp_nd(doc, n)->depth : -1; }

void fhp_pos(const fhp_doc *doc, fhp_node n, size_t *start, size_t *end) {
    const fhp_nodedata *d;
    if (!fhp_valid(doc, n)) {
        if (start) *start = 0;
        if (end) *end = 0;
        return;
    }
    d = fhp_nd(doc, n);
    if (start) *start = (size_t)d->start;
    if (end) *end = (size_t)d->end;
}

fhp_str fhp_body_s(const fhp_doc *doc, fhp_node n) {
    const fhp_nodedata *d = fhp_nd(doc, n);
    if (d->inner_start < 0 || d->inner_end <= 0 || d->inner_start > d->inner_end + 1) return FHP_EMPTY;
    return fhp_trim(fhp_src_slice(doc, (size_t)d->inner_start, (size_t)d->inner_end + 1));
}

fhp_str fhp_body(const fhp_doc *doc, fhp_node n) { return fhp_valid(doc, n) ? fhp_body_s(doc, n) : FHP_EMPTY; }

fhp_str fhp_outer_html(const fhp_doc *doc, fhp_node n) {
    const fhp_nodedata *d;
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    d = fhp_nd(doc, n);
    if (d->inner_start <= 0 || d->inner_end <= 0) return FHP_EMPTY;
    return fhp_trim(fhp_src_slice(doc, (size_t)d->start, (size_t)d->end + 1));
}

static int has_children(const fhp_doc *doc, fhp_node n) {
    return (size_t)n + 1 < doc->count && doc->nodes[n + 1].start < doc->nodes[n].end;
}

int fhp_has_children(const fhp_doc *doc, fhp_node n) { return fhp_valid(doc, n) && has_children(doc, n); }

/* Whether the page has a pre, listing or plaintext, looked up once: most pages have none, and then the text does not
   look for one around each tag. */
int fhp_doc_has_pre(const fhp_doc *cdoc) {
    fhp_doc *doc = (fhp_doc *)cdoc; /* a lazy field, as the CSS tree */
    if (doc->has_pre == 0) {
        size_t i;
        doc->has_pre = 1;
        for (i = 0; i < doc->count; i++) {
            int k = doc->nodes[i].key;
            if (k == KEY(FHP_TAG_PRE) || k == KEY(FHP_TAG_LISTING) || k == KEY(FHP_TAG_PLAINTEXT)) {
                doc->has_pre = 2;
                break;
            }
        }
    }
    return doc->has_pre == 2;
}

/* Whether the tag is pre, listing or plaintext, or inside one. */
int fhp_in_pre(const fhp_doc *doc, fhp_node n) {
    if (!fhp_doc_has_pre(doc)) return 0;
    while (n >= 0) {
        int k = doc->nodes[n].key;
        if (k == KEY(FHP_TAG_PRE) || k == KEY(FHP_TAG_LISTING) || k == KEY(FHP_TAG_PLAINTEXT)) return 1;
        n = doc->nodes[n].parent;
    }
    return 0;
}

/* The visible text in one line, as jsoup's text(): <br>, block elements and table cells separate words, inline tags
   do not, whitespace and line breaks collapse to one space; inside <pre> whitespace is kept; script, style, textarea,
   title, ... are their content as written. */
fhp_str fhp_text(const fhp_doc *doc, fhp_node n, fhp_arena *a) {
    fhp_str body;
    int key, leaf, plain, in_pre;
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    body = fhp_body_s(doc, n);
    key = fhp_nd(doc, n)->key;
    a = fhp_result_arena(doc, a);
    leaf = !has_children(doc, n);
    /* script, style, textarea, title, ... without child tags: their content is text as written */
    if (leaf && fhp_has_raw_text(key)) return fhp_decode(a, body, 0);
    /* no child tags, no '<' and no whitespace to collapse: the body is the text */
    plain = leaf && !fhp_memchr(body.ptr, '<', body.len);
    if (plain && !fhp_needs_collapse(body)) return fhp_decode(a, body, 0);
    /* <pre> and what is in it keep their whitespace (looked up only when there is whitespace to keep) */
    in_pre = fhp_in_pre(doc, n);
    if (plain && in_pre) return fhp_decode(a, body, 0);
    return fhp_clear_tags_in(a, body, in_pre);
}

fhp_str fhp_text_line_breaks(const fhp_doc *doc, fhp_node n, fhp_arena *a) {
    const fhp_nodedata *d;
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    d = fhp_nd(doc, n);
    if (d->inner_start < 0 || d->inner_end <= 0) return FHP_EMPTY;
    return fhp_clear_tags_line_breaks(fhp_result_arena(doc, a), fhp_body_s(doc, n));
}

/* s[from..to] with both ends moved back to a char boundary: a position from the parser may stand inside the last char
   of a cut-off tag (the .NET version, which counts UTF-16 chars, leaves that char out as well). */
static fhp_str slice(const char *s, size_t len, size_t from, size_t to) {
    if (to > len) to = len;
    while (!fhp_is_char_boundary(s, len, to)) to--;
    if (from > to) from = to;
    while (!fhp_is_char_boundary(s, len, from)) from--;
    return fhp_s(s + from, to - from);
}

fhp_str fhp_source(const fhp_doc *doc, fhp_node n) {
    const fhp_nodedata *d;
    size_t start, end, length;
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    d = fhp_nd(doc, n);
    start = (size_t)d->start;
    end = (size_t)d->end;
    if (d->inner_start > 0 && d->inner_end > 0)
        length = (size_t)d->inner_start - start - 2;
    else if (end > 0 && doc->src[end - 1] == '/')
        length = end - start - 2;
    else
        length = end - start - 1;
    return fhp_trim(slice(doc->src, doc->len, start + 1, start + 1 + length));
}

/* ----- navigation ----- */

fhp_node fhp_parent(const fhp_doc *doc, fhp_node n) { return fhp_valid(doc, n) ? fhp_nd(doc, n)->parent : FHP_NONE; }

fhp_nodes fhp_children(const fhp_doc *doc, fhp_node n, fhp_arena *a) {
    fhp_ivec v;
    fhp_nodes out;
    size_t i;
    const fhp_nodedata *d;
    out.items = NULL;
    out.len = 0;
    if (!fhp_valid(doc, n)) return out;
    d = fhp_nd(doc, n);
    fhp_ivec_init(&v, fhp_result_arena(doc, a), 0);
    for (i = (size_t)n + 1; i < doc->count && doc->nodes[i].end <= d->end; i++)
        if (doc->nodes[i].depth == d->depth + 1) fhp_ivec_push(&v, (int32_t)i);
    out.items = v.p;
    out.len = v.len;
    return out;
}

fhp_node fhp_next(const fhp_doc *doc, fhp_node n) {
    return fhp_valid(doc, n) && (size_t)n + 1 < doc->count ? n + 1 : FHP_NONE;
}

fhp_node fhp_previous(const fhp_doc *doc, fhp_node n) { return fhp_valid(doc, n) && n >= 1 ? n - 1 : FHP_NONE; }

fhp_node fhp_next_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q) {
    size_t i;
    int32_t end;
    if (!fhp_valid(doc, n)) return FHP_NONE;
    end = fhp_nd(doc, n)->end;
    for (i = (size_t)n + 1; i < doc->count; i++)
        if (doc->nodes[i].start > end && fhp_matches(doc, (fhp_node)i, q)) return (fhp_node)i;
    return FHP_NONE;
}

fhp_node fhp_next_sibling_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q) {
    int32_t depth;
    size_t i;
    if (!fhp_valid(doc, n)) return FHP_NONE;
    /* deeper tags are inside the following siblings; a shallower one ends the parent */
    depth = fhp_nd(doc, n)->depth;
    for (i = (size_t)n + 1; i < doc->count; i++) {
        if (doc->nodes[i].depth < depth) break;
        if (doc->nodes[i].depth == depth && fhp_matches(doc, (fhp_node)i, q)) return (fhp_node)i;
    }
    return FHP_NONE;
}

fhp_node fhp_previous_sibling_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q) {
    int32_t depth;
    fhp_node p;
    if (!fhp_valid(doc, n)) return FHP_NONE;
    /* deeper tags are inside the previous siblings; a shallower one is the parent */
    depth = fhp_nd(doc, n)->depth;
    for (p = n - 1; p >= 0; p--) {
        if (doc->nodes[p].depth < depth) break;
        if (doc->nodes[p].depth == depth && fhp_matches(doc, p, q)) return p;
    }
    return FHP_NONE;
}

fhp_node fhp_next_sibling(const fhp_doc *doc, fhp_node n) { return fhp_next_sibling_matching(doc, n, NULL); }
fhp_node fhp_previous_sibling(const fhp_doc *doc, fhp_node n) { return fhp_previous_sibling_matching(doc, n, NULL); }

fhp_node fhp_parent_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q) {
    fhp_node p;
    if (!fhp_valid(doc, n)) return FHP_NONE;
    for (p = fhp_nd(doc, n)->parent; p >= 0; p = doc->nodes[p].parent)
        if (fhp_matches(doc, p, q)) return p;
    return FHP_NONE;
}

fhp_str fhp_text_after_tag_until_next_one(const fhp_doc *doc, fhp_node n) {
    size_t from;
    const char *next;
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    from = (size_t)fhp_nd(doc, n)->end + 1;
    if (from >= doc->len) return FHP_EMPTY;
    next = (const char *)fhp_memchr(doc->src + from, '<', doc->len - from);
    return fhp_src_slice(doc, from, next ? (size_t)(next - doc->src) : doc->len);
}

fhp_str fhp_text_no_tags_after_tag_until_next_one(const fhp_doc *doc, fhp_node n, fhp_arena *a) {
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    return fhp_clear_tags(fhp_result_arena(doc, a), fhp_text_after_tag_until_next_one(doc, n));
}

fhp_str fhp_text_after_until(const fhp_doc *doc, fhp_node n, const fhp_query *q) {
    fhp_node until = fhp_next_matching(doc, n, q);
    if (until < 0) return FHP_EMPTY;
    return fhp_src_slice(doc, (size_t)fhp_nd(doc, n)->end + 1, (size_t)fhp_nd(doc, until)->start);
}

/* ----- search ----- */

fhp_query fhp_by_name(const char *name) {
    fhp_query q;
    memset(&q, 0, sizeof q);
    if (name) {
        q.tag = fhp_lookup_tag(name);
        q.name = name;
    }
    return q;
}

static int attr_starts_with(fhp_str raw, fhp_str prefix) {
    if (fhp_needs_decoding(raw)) {
        fhp_arena *s = fhp_scratch();
        fhp_mark m = fhp_arena_mark(s);
        int r = fhp_starts_with(fhp_decode(s, raw, 1), prefix);
        fhp_arena_release(s, m);
        return r;
    }
    return fhp_starts_with(raw, prefix);
}

static int matches(const fhp_doc *doc, fhp_node n, const fhp_query *q) {
    const fhp_nodedata *d = fhp_nd(doc, n);
    if (!q) return 1;
    if (q->tag > 0) {
        if (d->key != q->tag - 1 &&
            (d->key >= 0 || !q->name || !q->name[0] || !fhp_equal_fold(fhp_name_s(doc, n), fhp_cs(q->name))))
            return 0;
    } else if (q->name && q->name[0] && !fhp_equal_fold(fhp_name_s(doc, n), fhp_cs(q->name))) {
        return 0;
    }
    if (q->id && (!q->id[0] || !fhp_attr_equals(fhp_attr_raw_s(doc, n, fhp_s("id", 2)), fhp_cs(q->id)))) return 0;
    if (q->attr && !fhp_attr_equals(fhp_attr_raw_s(doc, n, fhp_cs(q->attr)), fhp_cs(q->attr_value))) return 0;
    if (q->cls && !fhp_has_class_s(doc, n, fhp_cs(q->cls))) return 0;
    if (q->id_starts_with && !attr_starts_with(fhp_attr_raw_s(doc, n, fhp_s("id", 2)), fhp_cs(q->id_starts_with)))
        return 0;
    if (q->source && !fhp_str_eq(fhp_source(doc, n), fhp_cs(q->source))) return 0;
    if (q->body && !fhp_str_eq(fhp_body_s(doc, n), fhp_cs(q->body))) return 0;
    if (q->body_starts_with && !fhp_starts_with(fhp_body_s(doc, n), fhp_cs(q->body_starts_with))) return 0;
    /* the text last: it is the most expensive check */
    if (q->text && !fhp_is_text(doc, n, fhp_cs(q->text))) return 0;
    if (q->text_starts_with) {
        fhp_arena *s = fhp_scratch();
        fhp_mark m = fhp_arena_mark(s);
        int r = fhp_starts_with(fhp_text(doc, n, s), fhp_cs(q->text_starts_with));
        fhp_arena_release(s, m);
        if (!r) return 0;
    }
    return 1;
}

int fhp_matches(const fhp_doc *doc, fhp_node n, const fhp_query *q) { return fhp_valid(doc, n) && matches(doc, n, q); }

/* The range of tags inside scope: [first, ...) up to the tag that ends after end. */
static void scope_range(const fhp_doc *doc, fhp_node scope, size_t *first, int32_t *end) {
    if (scope >= 0) {
        *first = (size_t)scope + 1;
        *end = fhp_nd(doc, scope)->end;
    } else {
        *first = 0;
        *end = (int32_t)doc->len;
    }
}

fhp_node fhp_find(const fhp_doc *doc, fhp_node scope, const fhp_query *q) {
    size_t i;
    int32_t end;
    if (!doc || (scope != FHP_NONE && !fhp_valid(doc, scope))) return FHP_NONE;
    scope_range(doc, scope, &i, &end);
    for (; i < doc->count; i++) {
        if (doc->nodes[i].end > end) break;
        if (matches(doc, (fhp_node)i, q)) return (fhp_node)i;
    }
    return FHP_NONE;
}

fhp_nodes fhp_find_all(const fhp_doc *doc, fhp_node scope, const fhp_query *q, fhp_arena *a) {
    fhp_ivec v;
    fhp_nodes out;
    size_t i;
    int32_t end;
    out.items = NULL;
    out.len = 0;
    if (!doc || (scope != FHP_NONE && !fhp_valid(doc, scope))) return out;
    fhp_ivec_init(&v, fhp_result_arena(doc, a), 0);
    scope_range(doc, scope, &i, &end);
    for (; i < doc->count; i++) {
        if (doc->nodes[i].end > end) break;
        if (matches(doc, (fhp_node)i, q)) fhp_ivec_push(&v, (int32_t)i);
    }
    out.items = v.p;
    out.len = v.len;
    return out;
}

/* Not a comment, the doctype or the XML declaration. */
int fhp_is_element(const fhp_doc *doc, size_t i) {
    const fhp_nodedata *d = &doc->nodes[i];
    fhp_str name;
    if (d->key >= 0)
        return d->key != K_COMMENT && d->key != KEY(FHP_TAG_DOCTYPE) && d->key != KEY(FHP_TAG_XML_DECLARATION);
    name = fhp_name_s(doc, (fhp_node)i);
    return name.len && name.ptr[0] != '!' && name.ptr[0] != '?';
}

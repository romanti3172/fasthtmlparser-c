/* CSS selectors over the flat tag list. */
#include <stdio.h>
#include <stdlib.h>

#include "internal.h"

#define K_COMMENT KEY(FHP_TAG_COMMENT)

/* ----- the tree: sibling links and positions, built once per document ----- */

const fhp_css_tree *fhp_doc_tree(const fhp_doc *cdoc) {
    fhp_doc *doc = (fhp_doc *)cdoc;
    size_t count = doc->count, i;
    fhp_css_tree *t;
    int32_t *last_child, *children;
    fhp_arena *s;
    fhp_mark m;
    if (doc->css) return doc->css;
    t = (fhp_css_tree *)fhp_alloc(&doc->mem, sizeof(fhp_css_tree));
    if (!t) return NULL;
    t->previous = (int32_t *)fhp_alloc(&doc->mem, (count + 1) * sizeof(int32_t) * 4);
    if (!t->previous) return NULL;
    t->next = t->previous + count + 1;
    t->position = t->next + count + 1;
    t->from_end = t->position + count + 1;
    t->type_position = t->type_from_end = NULL;
    for (i = 0; i < count; i++) {
        t->previous[i] = t->next[i] = -1;
        t->position[i] = t->from_end[i] = 0;
    }
    s = fhp_scratch();
    m = fhp_arena_mark(s);
    /* per parent (slot = parent index + 1, slot 0: the top level): its last element child so far and their number */
    last_child = (int32_t *)fhp_alloc(s, (count + 1) * sizeof(int32_t));
    children = (int32_t *)fhp_alloc(s, (count + 1) * sizeof(int32_t));
    if (!last_child || !children) {
        fhp_arena_release(s, m);
        return NULL;
    }
    for (i = 0; i <= count; i++) {
        last_child[i] = -1;
        children[i] = 0;
    }
    for (i = 0; i < count; i++) {
        size_t slot;
        int32_t prev;
        if (!fhp_is_element(doc, i)) continue;
        slot = (size_t)(doc->nodes[i].parent + 1);
        prev = last_child[slot];
        t->previous[i] = prev;
        if (prev >= 0) t->next[prev] = (int32_t)i;
        last_child[slot] = (int32_t)i;
        children[slot]++;
        t->position[i] = children[slot];
    }
    for (i = 0; i < count; i++)
        if (t->position[i] > 0) t->from_end[i] = children[doc->nodes[i].parent + 1] - t->position[i] + 1;
    fhp_arena_release(s, m);
    doc->css = t;
    return t;
}

/* Positions among the siblings with the same name (:nth-of-type). */
void fhp_doc_type_positions(const fhp_doc *cdoc) {
    fhp_doc *doc = (fhp_doc *)cdoc;
    fhp_css_tree *t = (fhp_css_tree *)fhp_doc_tree(doc);
    size_t count = doc->count, cap = 16, i;
    uint64_t *keys;
    int32_t *counts;
    fhp_arena *s;
    fhp_mark m;
    if (!t || doc->css_types) return;
    t->type_position = (int32_t *)fhp_alloc(&doc->mem, (count + 1) * sizeof(int32_t) * 2);
    if (!t->type_position) return;
    t->type_from_end = t->type_position + count + 1;
    while (cap < count * 2) cap *= 2;
    s = fhp_scratch();
    m = fhp_arena_mark(s);
    keys = (uint64_t *)fhp_alloc(s, cap * sizeof(uint64_t));
    counts = (int32_t *)fhp_alloc(s, cap * sizeof(int32_t));
    if (!keys || !counts) {
        fhp_arena_release(s, m);
        return;
    }
    for (i = 0; i < cap; i++) keys[i] = 0;
#define TYPE_KEY(i)                                                                                         \
    (((uint64_t)(uint32_t)(doc->nodes[i].parent + 1) << 32 | (uint64_t)(doc->nodes[i].key >= 0             \
                                                                          ? (uint32_t)doc->nodes[i].key   \
                                                                          : 0x10000u + doc->nodes[i].custom)) + 1)
    for (i = 0; i < count; i++) {
        uint64_t k;
        size_t h;
        t->type_position[i] = t->type_from_end[i] = 0;
        if (t->position[i] == 0) continue; /* not an element */
        k = TYPE_KEY(i);
        h = (size_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
        while (keys[h] && keys[h] != k) h = (h + 1) & (cap - 1);
        if (!keys[h]) {
            keys[h] = k;
            counts[h] = 0;
        }
        t->type_position[i] = ++counts[h];
    }
    for (i = 0; i < count; i++) {
        uint64_t k;
        size_t h;
        if (t->type_position[i] == 0) continue;
        k = TYPE_KEY(i);
        h = (size_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
        while (keys[h] != k) h = (h + 1) & (cap - 1);
        t->type_from_end[i] = counts[h] - t->type_position[i] + 1;
    }
#undef TYPE_KEY
    fhp_arena_release(s, m);
    doc->css_types = 1;
}

/* ----- the compiled selector ----- */

enum comb { C_NONE, C_DESCENDANT, C_CHILD, C_ADJACENT, C_SIBLING };
enum attr_op { A_EXISTS, A_EQUALS, A_INCLUDES, A_DASH, A_PREFIX, A_SUFFIX, A_SUBSTRING };
enum pseudo { P_ROOT, P_SCOPE, P_EMPTY, P_LINK, P_CHECKED, P_DISABLED, P_ENABLED, P_REQUIRED, P_OPTIONAL };
enum simple_kind { S_ID, S_CLASS, S_ATTR, S_NTH, S_NOT, S_IS, S_HAS, S_CONTAINS, S_PSEUDO };

typedef struct css_list css_list;

typedef struct simple {
    int kind;
    fhp_str value; /* id, class, attribute value, :contains text */
    fhp_str name;  /* attribute name */
    int op;
    int fold;
    int pseudo;
    int64_t a, b; /* An+B */
    int from_end, of_type;
    css_list *list; /* :not / :is / :has, or the "of S" of :nth-child */
} simple;

typedef struct compound {
    int key;      /* tag key, or -1 */
    fhp_str name; /* lower-case name not in the tables; len 0 with key -1: any element */
    int has_name;
    simple *simples;
    size_t n, cap;
} compound;

typedef struct complex {
    int leading;
    compound *compounds;
    int *combinators; /* combinators[i] stands between compounds[i] and [i+1] */
    size_t n, cap;
} complex;

struct css_list {
    complex *items;
    size_t n, cap;
};

struct fhp_css {
    fhp_arena mem;
    css_list *list;
};

/* The document, the tag the query was called on (:scope, -1 for the document) and the tag a relative selector starts
   from (:has(> a), or node.css("> li"); -1: the document). */
typedef struct ctx {
    const fhp_doc *doc;
    const fhp_css_tree *tree;
    fhp_node scope;
    fhp_node anchor;
} ctx;

static int list_matches(const css_list *l, fhp_node n, const ctx *c);

FHP_INLINE fhp_node parent_of(const ctx *c, fhp_node i) { return c->doc->nodes[i].parent; }

static const fhp_css_tree *tree(const ctx *c) {
    return c->tree ? c->tree : fhp_doc_tree(c->doc);
}

static int compound_matches(const compound *cp, fhp_node n, const ctx *c);

/* n matches compound i and the part to the left of it matches the tags the combinators lead to, right to left as
   browsers do. */
static int is_related(int comb, fhp_node anchor, fhp_node n, const ctx *c);

static int complex_matches(const complex *cx, size_t i, fhp_node n, const ctx *c) {
    if (!compound_matches(&cx->compounds[i], n, c)) return 0;
    if (i == 0) return cx->leading == C_NONE || is_related(cx->leading, c->anchor, n, c);
    switch (cx->combinators[i - 1]) {
    case C_CHILD: {
        fhp_node p = parent_of(c, n);
        return p >= 0 && complex_matches(cx, i - 1, p, c);
    }
    case C_DESCENDANT: {
        fhp_node p;
        for (p = parent_of(c, n); p >= 0; p = parent_of(c, p))
            if (complex_matches(cx, i - 1, p, c)) return 1;
        return 0;
    }
    case C_ADJACENT: {
        const fhp_css_tree *t = tree(c);
        fhp_node p = t ? t->previous[n] : -1;
        return p >= 0 && complex_matches(cx, i - 1, p, c);
    }
    case C_SIBLING: {
        const fhp_css_tree *t = tree(c);
        fhp_node p;
        if (!t) return 0;
        for (p = t->previous[n]; p >= 0; p = t->previous[p])
            if (complex_matches(cx, i - 1, p, c)) return 1;
        return 0;
    }
    default:
        return 0;
    }
}

/* Whether n stands in the combinator relation to anchor (-1: the document). */
static int is_related(int comb, fhp_node anchor, fhp_node n, const ctx *c) {
    const fhp_nodedata *nodes = c->doc->nodes;
    switch (comb) {
    case C_CHILD:
        return anchor < 0 ? nodes[n].parent < 0 : nodes[n].parent == anchor;
    case C_DESCENDANT: {
        fhp_node p;
        if (anchor < 0) return 1;
        for (p = parent_of(c, n); p >= 0; p = parent_of(c, p))
            if (p == anchor) return 1;
        return 0;
    }
    case C_ADJACENT: {
        const fhp_css_tree *t = tree(c);
        return anchor >= 0 && t && t->previous[n] == anchor;
    }
    case C_SIBLING:
        return anchor >= 0 && anchor < n && nodes[anchor].parent == nodes[n].parent && fhp_is_element(c->doc, (size_t)anchor);
    default:
        return 0;
    }
}

/* :has(): whether a tag in the anchor's scope matches the relative selector. */
static int has_match(const css_list *relative, fhp_node anchor, const ctx *c) {
    const fhp_doc *doc = c->doc;
    const fhp_nodedata *nodes = doc->nodes;
    ctx inner = *c;
    int32_t anchor_end = nodes[anchor].end;
    size_t k;
    inner.anchor = anchor;
    for (k = 0; k < relative->n; k++) {
        const complex *item = &relative->items[k];
        size_t last = item->n - 1;
        size_t i = (size_t)anchor + 1;
        if (item->leading == C_DESCENDANT || item->leading == C_CHILD) {
            /* the tags inside the anchor */
            for (; i < doc->count && nodes[i].end <= anchor_end; i++)
                if (fhp_is_element(doc, i) && complex_matches(item, last, (fhp_node)i, &inner)) return 1;
        } else {
            /* the following siblings and the tags inside them, up to the end of the parent */
            int64_t end;
            fhp_node p;
            while (i < doc->count && nodes[i].end <= anchor_end) i++;
            p = parent_of(c, anchor);
            end = p >= 0 ? nodes[p].end : INT32_MAX;
            for (; i < doc->count && nodes[i].end <= end; i++)
                if (fhp_is_element(doc, i) && complex_matches(item, last, (fhp_node)i, &inner)) return 1;
        }
    }
    return 0;
}

FHP_INLINE int is_css_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == 0x0c || c == '\r'; }

static int equal(fhp_str a, fhp_str b, int fold) { return fold ? fhp_equal_fold(a, b) : fhp_str_eq(a, b); }

/* Whether a whitespace-separated list contains word. */
static int contains_word(fhp_str list, fhp_str word, int fold) {
    size_t i = 0;
    if (word.len == 0 || list.len < word.len) return 0;
    while (i <= list.len) {
        size_t start = i;
        while (i < list.len && !is_css_space((unsigned char)list.ptr[i])) i++;
        if (i > start && equal(fhp_s(list.ptr + start, i - start), word, fold)) return 1;
        i++;
    }
    return 0;
}

static int attribute_matches(const fhp_doc *doc, fhp_node n, const simple *s) {
    fhp_str raw = fhp_attr_raw_s(doc, n, s->name), a, v = s->value;
    fhp_arena *sc;
    fhp_mark m;
    int r = 0;
    if (raw.len == 0) {
        /* missing or empty: only these can match an empty value */
        switch (s->op) {
        case A_EXISTS:
            return fhp_has_attribute(doc, n, s->name);
        case A_EQUALS:
        case A_DASH:
            return v.len == 0 && fhp_has_attribute(doc, n, s->name);
        default:
            return 0;
        }
    }
    if (s->op == A_EXISTS) return 1;
    sc = fhp_scratch();
    m = fhp_arena_mark(sc);
    a = fhp_decode(sc, raw, 1);
#define SLICE_EQ(from, to) \
    (fhp_is_char_boundary(a.ptr, a.len, from) && fhp_is_char_boundary(a.ptr, a.len, to) && \
     equal(fhp_s(a.ptr + (from), (to) - (from)), v, s->fold))
    switch (s->op) {
    case A_EQUALS:
        r = equal(a, v, s->fold);
        break;
    case A_INCLUDES: {
        size_t k;
        r = 1;
        for (k = 0; k < v.len; k++)
            if (is_css_space((unsigned char)v.ptr[k])) r = 0;
        r = r && contains_word(a, v, s->fold);
        break;
    }
    case A_DASH:
        r = equal(a, v, s->fold) || (a.len > v.len && a.ptr[v.len] == '-' && SLICE_EQ(0, v.len));
        break;
    case A_PREFIX:
        r = v.len && a.len >= v.len && SLICE_EQ(0, v.len);
        break;
    case A_SUFFIX:
        r = v.len && a.len >= v.len && SLICE_EQ(a.len - v.len, a.len);
        break;
    case A_SUBSTRING:
        if (v.len == 0) {
            r = 0;
        } else if (!s->fold) {
            r = fhp_index(a.ptr, a.len, v.ptr, v.len) >= 0;
        } else {
            size_t i;
            r = 0;
            for (i = 0; i + v.len <= a.len && !r; i++) r = SLICE_EQ(i, i + v.len);
        }
        break;
    default:
        r = 1;
    }
#undef SLICE_EQ
    fhp_arena_release(sc, m);
    return r;
}

static int nth_matches(const simple *s, fhp_node i, const ctx *c) {
    const fhp_css_tree *t = tree(c);
    int64_t position;
    if (!t) return 0;
    if (s->list) {
        fhp_node sib;
        if (!list_matches(s->list, i, c)) return 0;
        position = 1;
        for (sib = s->from_end ? t->next[i] : t->previous[i]; sib >= 0;
             sib = s->from_end ? t->next[sib] : t->previous[sib])
            if (list_matches(s->list, sib, c)) position++;
    } else if (s->of_type) {
        if (!c->doc->css_types) fhp_doc_type_positions(c->doc);
        if (!t->type_position) return 0;
        position = s->from_end ? t->type_from_end[i] : t->type_position[i];
    } else {
        position = s->from_end ? t->from_end[i] : t->position[i];
    }
    if (position <= 0) return 0; /* not an element */
    /* position = a*n + b for some n >= 0 */
    if (s->a == 0) return position == s->b;
    {
        int64_t diff = position - s->b;
        return diff % s->a == 0 && diff / s->a >= 0;
    }
}

/* The first top-level element: <html> on a page. */
static int is_root(fhp_node i, const ctx *c) {
    const fhp_css_tree *t = tree(c);
    return c->doc->nodes[i].parent < 0 && t && t->previous[i] < 0;
}

static int name_is(fhp_str name, const char *w) { return fhp_str_eq(name, fhp_cs(w)); }

static int is_form_control(fhp_str name) {
    return name_is(name, "button") || name_is(name, "input") || name_is(name, "select") || name_is(name, "textarea") ||
           name_is(name, "optgroup") || name_is(name, "option") || name_is(name, "fieldset");
}

static int is_input(fhp_str name) { return name_is(name, "input") || name_is(name, "select") || name_is(name, "textarea"); }

/* No child elements and no text; comments are allowed. */
static int is_empty(fhp_node i, const fhp_doc *doc) {
    const fhp_nodedata *n = &doc->nodes[i];
    int32_t position;
    size_t k;
    if (n->inner_start <= 0 || n->inner_end <= 0) return 1; /* a void or unclosed tag has no content */
    position = n->inner_start;
    for (k = (size_t)i + 1; k < doc->count && doc->nodes[k].end <= n->end; k++) {
        const fhp_nodedata *child = &doc->nodes[k];
        if (fhp_is_element(doc, k) || child->start > position) return 0; /* an element, or text before a comment */
        if (child->end + 1 > position) position = child->end + 1;
    }
    return position > n->inner_end;
}

static int pseudo_matches(int kind, fhp_node i, const ctx *c) {
    const fhp_doc *doc = c->doc;
    fhp_str name = fhp_name_s(doc, i);
    switch (kind) {
    case P_ROOT:
        return is_root(i, c);
    case P_SCOPE:
        return c->scope >= 0 ? i == c->scope : is_root(i, c);
    case P_EMPTY:
        return is_empty(i, doc);
    case P_LINK:
        return (name_is(name, "a") || name_is(name, "area")) && fhp_has_attribute(doc, i, fhp_s("href", 4));
    case P_CHECKED:
        return (name_is(name, "input") && fhp_has_attribute(doc, i, fhp_s("checked", 7))) ||
               (name_is(name, "option") && fhp_has_attribute(doc, i, fhp_s("selected", 8)));
    case P_DISABLED:
        return is_form_control(name) && fhp_has_attribute(doc, i, fhp_s("disabled", 8));
    case P_ENABLED:
        return is_form_control(name) && !fhp_has_attribute(doc, i, fhp_s("disabled", 8));
    case P_REQUIRED:
        return is_input(name) && fhp_has_attribute(doc, i, fhp_s("required", 8));
    case P_OPTIONAL:
        return is_input(name) && !fhp_has_attribute(doc, i, fhp_s("required", 8));
    default:
        return 0;
    }
}

static int simple_matches(const simple *s, fhp_node i, const ctx *c) {
    const fhp_doc *doc = c->doc;
    switch (s->kind) {
    case S_ID:
        return fhp_attr_equals(fhp_attr_raw_s(doc, i, fhp_s("id", 2)), s->value);
    case S_CLASS: {
        fhp_str raw = fhp_attr_raw_s(doc, i, fhp_s("class", 5));
        int r;
        fhp_arena *sc;
        fhp_mark m;
        if (raw.len == 0) return 0;
        if (!fhp_needs_decoding(raw)) return contains_word(raw, s->value, 0);
        sc = fhp_scratch();
        m = fhp_arena_mark(sc);
        r = contains_word(fhp_decode(sc, raw, 1), s->value, 0);
        fhp_arena_release(sc, m);
        return r;
    }
    case S_ATTR:
        return attribute_matches(doc, i, s);
    case S_NTH:
        return nth_matches(s, i, c);
    case S_NOT:
        return !list_matches(s->list, i, c);
    case S_IS:
        return list_matches(s->list, i, c);
    case S_HAS:
        return has_match(s->list, i, c);
    case S_CONTAINS: {
        fhp_arena *sc = fhp_scratch();
        fhp_mark m = fhp_arena_mark(sc);
        fhp_str text = fhp_text(doc, i, sc);
        int r = fhp_index(text.ptr, text.len, s->value.ptr, s->value.len) >= 0;
        fhp_arena_release(sc, m);
        return r;
    }
    case S_PSEUDO:
        return pseudo_matches(s->pseudo, i, c);
    default:
        return 0;
    }
}

static int compound_matches(const compound *cp, fhp_node n, const ctx *c) {
    const fhp_nodedata *d = &c->doc->nodes[n];
    size_t k;
    if (cp->key >= 0) {
        if (d->key != cp->key) return 0;
    } else if (cp->has_name) {
        if (d->key >= 0 || !fhp_str_eq(fhp_name_s(c->doc, n), cp->name)) return 0;
    } else if (!fhp_is_element(c->doc, (size_t)n)) {
        return 0;
    }
    for (k = 0; k < cp->n; k++)
        if (!simple_matches(&cp->simples[k], n, c)) return 0;
    return 1;
}

static int list_matches(const css_list *l, fhp_node n, const ctx *c) {
    size_t k;
    for (k = 0; k < l->n; k++)
        if (complex_matches(&l->items[k], l->items[k].n - 1, n, c)) return 1;
    return 0;
}

/* ----- running a selector ----- */

static void scan(const fhp_css *q, const fhp_doc *doc, fhp_node scope, int first_only, fhp_ivec *out, fhp_node *first) {
    ctx c;
    size_t i, from;
    int32_t end;
    c.doc = doc;
    c.tree = NULL;
    c.scope = scope;
    c.anchor = scope;
    if (scope >= 0) {
        from = (size_t)scope + 1;
        end = doc->nodes[scope].end;
    } else {
        from = 0;
        end = (int32_t)doc->len;
    }
    if (first) *first = FHP_NONE;
    for (i = from; i < doc->count; i++) {
        if (doc->nodes[i].end > end) break;
        if (list_matches(q->list, (fhp_node)i, &c)) {
            if (first_only) {
                *first = (fhp_node)i;
                return;
            }
            fhp_ivec_push(out, (int32_t)i);
        }
    }
}

fhp_nodes fhp_css_all(const fhp_css *q, const fhp_doc *doc, fhp_node scope, fhp_arena *a) {
    fhp_ivec v;
    fhp_nodes out;
    out.items = NULL;
    out.len = 0;
    if (!q || !doc || (scope != FHP_NONE && !fhp_valid(doc, scope))) return out;
    fhp_ivec_init(&v, fhp_result_arena(doc, a), 0);
    scan(q, doc, scope, 0, &v, NULL);
    out.items = v.p;
    out.len = v.len;
    return out;
}

fhp_node fhp_css_first(const fhp_css *q, const fhp_doc *doc, fhp_node scope) {
    fhp_node first;
    if (!q || !doc || (scope != FHP_NONE && !fhp_valid(doc, scope))) return FHP_NONE;
    scan(q, doc, scope, 1, NULL, &first);
    return first;
}

int fhp_css_matches(const fhp_css *q, const fhp_doc *doc, fhp_node n) {
    ctx c;
    if (!q || !fhp_valid(doc, n) || !fhp_is_element(doc, (size_t)n)) return 0;
    c.doc = doc;
    c.tree = NULL;
    c.scope = n;
    c.anchor = n;
    return list_matches(q->list, n, &c);
}

/* ----- compiler ----- */

/* Parses a selector list (Selectors Level 3 and the common Level 4 pseudo-classes). Identifiers and strings follow the
   CSS syntax, escapes included (#a\:b, .\31 0, [title="a\"b"]). */
typedef struct compiler {
    const char *s;
    size_t len, p;
    fhp_arena *a;
    char *err;
    size_t err_size;
    int failed;
} compiler;

static void fail(compiler *c, const char *fmt, const char *arg) {
    char msg[256];
    if (c->failed) return;
    c->failed = 1;
    if (!c->err || !c->err_size) return;
    snprintf(msg, sizeof msg, fmt, arg ? arg : "");
    snprintf(c->err, c->err_size, "invalid CSS selector \"%s\" at position %u: %s", c->s, (unsigned)c->p, msg);
}

/* The character at byte position p (for messages). */
static const char *char_at(compiler *c, size_t p, char *buf) {
    size_t size;
    while (p > 0 && !fhp_is_char_boundary(c->s, c->len, p)) p--;
    if (p >= c->len) {
        buf[0] = 0;
        return buf;
    }
    fhp_utf8_decode(c->s + p, c->len - p, &size);
    memcpy(buf, c->s + p, size);
    buf[size] = 0;
    return buf;
}

static unsigned char peek(const compiler *c, size_t offset) {
    return c->p + offset < c->len ? (unsigned char)c->s[c->p + offset] : 0;
}

static int skip_space(compiler *c) {
    size_t start = c->p;
    while (c->p < c->len && is_css_space((unsigned char)c->s[c->p])) c->p++;
    return c->p > start;
}

static int is_name_start_byte(unsigned char ch) { return fhp_is_alpha_ascii(ch) || ch == '_' || ch >= 0x80; }

static int is_escape(const compiler *c, size_t p) {
    if (p >= c->len || c->s[p] != '\\') return 0;
    if (p + 1 >= c->len) return 1;
    return !(c->s[p + 1] == '\n' || c->s[p + 1] == '\r' || c->s[p + 1] == 0x0c);
}

static int is_name_char(const compiler *c, size_t p) {
    unsigned char ch;
    if (p >= c->len) return 0;
    ch = (unsigned char)c->s[p];
    return is_name_start_byte(ch) || fhp_is_digit(ch) || ch == '-' || is_escape(c, p);
}

static int is_ident_start(const compiler *c, size_t p) {
    if (p >= c->len) return 0;
    if (c->s[p] == '-') {
        p++;
        return p < c->len && (is_name_start_byte((unsigned char)c->s[p]) || c->s[p] == '-' || is_escape(c, p));
    }
    return is_name_start_byte((unsigned char)c->s[p]) || is_escape(c, p);
}

/* Whether the identifier at the position is word (case-insensitive) and ends there. */
static int is_word(const compiler *c, const char *word) {
    size_t n = strlen(word);
    return c->p + n <= c->len && fhp_eq_ignore_ascii_case(c->s + c->p, word, n) && !is_name_char(c, c->p + n);
}

/* '\' then up to 6 hex digits and one optional whitespace, or any other char as itself. */
static void read_escape(compiler *c, fhp_buf *b) {
    uint32_t code = 0;
    int digits = 0;
    c->p++; /* '\' */
    if (c->p >= c->len) {
        fhp_buf_add_cp(b, 0xFFFD);
        return;
    }
    if (!fhp_is_hex((unsigned char)c->s[c->p])) {
        size_t size = 1;
        if (fhp_is_char_boundary(c->s, c->len, c->p)) fhp_utf8_decode(c->s + c->p, c->len - c->p, &size);
        fhp_buf_add(b, c->s + c->p, size);
        c->p += size;
        return;
    }
    while (digits < 6 && c->p < c->len && fhp_is_hex((unsigned char)c->s[c->p])) {
        unsigned char h = (unsigned char)c->s[c->p];
        code = code * 16 + (fhp_is_digit(h) ? (uint32_t)(h - '0') : (uint32_t)((h | 0x20) - 'a' + 10));
        c->p++;
        digits++;
    }
    if (c->p < c->len && is_css_space((unsigned char)c->s[c->p])) {
        if (c->s[c->p] == '\r' && peek(c, 1) == '\n')
            c->p += 2;
        else
            c->p++;
    }
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
    fhp_buf_add_cp(b, code);
}

static fhp_str read_name(compiler *c) {
    fhp_buf b;
    fhp_buf_init(&b, c->a, 16);
    while (is_name_char(c, c->p)) {
        if (c->s[c->p] == '\\')
            read_escape(c, &b);
        else
            fhp_buf_addc(&b, c->s[c->p++]);
    }
    return fhp_buf_str(&b);
}

static fhp_str read_string(compiler *c) {
    char quote = c->s[c->p];
    size_t start = c->p;
    fhp_buf b;
    fhp_buf_init(&b, c->a, 16);
    c->p++;
    for (;;) {
        char ch;
        if (c->p >= c->len) {
            c->p = start;
            fail(c, "unterminated string", NULL);
            return FHP_EMPTY;
        }
        ch = c->s[c->p];
        if (ch == quote) {
            c->p++;
            return fhp_buf_str(&b);
        }
        if (ch == '\\') {
            if (c->p + 1 < c->len && (c->s[c->p + 1] == '\n' || c->s[c->p + 1] == 0x0c))
                c->p += 2; /* an escaped line break continues the string */
            else if (c->p + 2 < c->len && c->s[c->p + 1] == '\r')
                c->p += c->s[c->p + 2] == '\n' ? 3 : 2;
            else
                read_escape(c, &b);
        } else {
            fhp_buf_addc(&b, ch);
            c->p++;
        }
    }
}

static int expect(compiler *c, char ch) {
    char buf[8], want[2];
    if (peek(c, 0) != (unsigned char)ch) {
        want[0] = ch;
        want[1] = 0;
        if (c->p >= c->len) {
            char m[32];
            snprintf(m, sizeof m, "'%s' expected", want);
            fail(c, "%s", m);
        } else {
            char m[64];
            snprintf(m, sizeof m, "'%s' expected instead of '%s'", want, char_at(c, c->p, buf));
            fail(c, "%s", m);
        }
        return 0;
    }
    c->p++;
    return 1;
}

static int read_combinator(compiler *c) {
    int comb;
    switch (peek(c, 0)) {
    case '>':
        comb = C_CHILD;
        break;
    case '+':
        comb = C_ADJACENT;
        break;
    case '~':
        comb = C_SIBLING;
        break;
    default:
        return C_NONE;
    }
    c->p++;
    skip_space(c);
    return comb;
}

static css_list *parse_list(compiler *c, int relative, int top_level);

static simple *add_simple(compiler *c, compound *cp) {
    simple *s;
    if (cp->n == cp->cap) {
        size_t cap = cp->cap ? cp->cap * 2 : 4;
        simple *p = (simple *)fhp_realloc(c->a, cp->simples, cp->cap * sizeof(simple), cap * sizeof(simple));
        if (!p) {
            fail(c, "out of memory", NULL);
            return NULL;
        }
        cp->simples = p;
        cp->cap = cap;
    }
    s = &cp->simples[cp->n++];
    memset(s, 0, sizeof *s);
    s->value = s->name = FHP_EMPTY;
    return s;
}

/* HTML: the values of these attributes are compared ignoring ASCII case unless the selector has the 's' flag. */
static const char *const case_insensitive_values[] = {
    "accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset", "checked", "clear", "codetype",
    "color", "compact", "declare", "defer", "dir", "direction", "disabled", "enctype", "face", "frame", "hreflang",
    "http-equiv", "lang", "language", "link", "media", "method", "multiple", "nohref", "noresize", "noshade",
    "nowrap", "readonly", "rel", "rev", "rules", "scope", "scrolling", "selected", "shape", "target", "text", "type",
    "valign", "valuetype", "vlink"};

/* '[' name [op value [i|s]] ']' */
static void parse_attribute(compiler *c, compound *cp) {
    simple *s;
    fhp_str name, value, lower;
    unsigned char ch;
    int op, fold = 0;
    size_t k;
    c->p++; /* '[' */
    skip_space(c);
    if (!is_ident_start(c, c->p)) {
        fail(c, "an attribute name expected", NULL);
        return;
    }
    name = read_name(c);
    skip_space(c);
    if (peek(c, 0) == ']') {
        c->p++;
        s = add_simple(c, cp);
        if (!s) return;
        s->kind = S_ATTR;
        s->name = name;
        s->op = A_EXISTS;
        return;
    }
    ch = peek(c, 0);
    switch (ch) {
    case '=':
        c->p++;
        op = A_EQUALS;
        break;
    case '~':
    case '|':
    case '^':
    case '$':
    case '*': {
        op = ch == '~' ? A_INCLUDES : ch == '|' ? A_DASH : ch == '^' ? A_PREFIX : ch == '$' ? A_SUFFIX : A_SUBSTRING;
        if (peek(c, 1) != '=') {
            char m[8];
            m[0] = (char)ch;
            m[1] = 0;
            fail(c, "'=' expected after '%s'", m);
            return;
        }
        c->p += 2;
        break;
    }
    default:
        fail(c, "'=', '~=', '|=', '^=', '$=', '*=' or ']' expected", NULL);
        return;
    }
    skip_space(c);
    if (peek(c, 0) == '"' || peek(c, 0) == '\'') {
        value = read_string(c);
        if (c->failed) return;
    } else if (is_ident_start(c, c->p)) {
        value = read_name(c);
    } else {
        fail(c, "an attribute value expected (quote it if it is not a plain word)", NULL);
        return;
    }
    skip_space(c);
    lower = fhp_to_lower(c->a, name);
    for (k = 0; k < sizeof case_insensitive_values / sizeof case_insensitive_values[0]; k++)
        if (fhp_str_eq(lower, fhp_cs(case_insensitive_values[k]))) fold = 1;
    if (is_ident_start(c, c->p)) {
        fhp_str flag = read_name(c);
        if (fhp_equal_fold(flag, fhp_s("i", 1))) {
            fold = 1;
        } else if (fhp_equal_fold(flag, fhp_s("s", 1))) {
            fold = 0;
        } else {
            char *z = fhp_strndup(c->a, flag.ptr, flag.len);
            fail(c, "unknown attribute flag '%s'", z);
            return;
        }
        skip_space(c);
    }
    if (!expect(c, ']')) return;
    s = add_simple(c, cp);
    if (!s) return;
    s->kind = S_ATTR;
    s->name = name;
    s->op = op;
    s->value = value;
    s->fold = fold;
}

static int parse_int(const char *s, size_t len, int64_t *out) {
    int64_t v = 0;
    size_t i = 0;
    int neg = 0;
    if (len == 0) return 0;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i >= len) return 0;
    for (; i < len; i++) {
        if (!fhp_is_digit((unsigned char)s[i])) return 0;
        v = v * 10 + (s[i] - '0');
        if (v > (int64_t)1 << 40) return 0;
    }
    if (neg) v = -v;
    if (v < INT32_MIN || v > INT32_MAX) return 0;
    *out = v;
    return 1;
}

FHP_INLINE int is_nth_char(unsigned char ch) {
    return ch == '+' || ch == '-' || ch == 'n' || ch == 'N' || fhp_is_digit(ch) || is_css_space(ch);
}

/* An+B: odd, even, 3, n, -n+3, 2n+1, +2n - 1 */
static int parse_nth(compiler *c, int64_t *a, int64_t *b) {
    char text[64];
    size_t tl = 0, start = c->p, k, npos = (size_t)-1, ncount = 0;
    if (is_word(c, "odd")) {
        c->p += 3;
        *a = 2;
        *b = 1;
        return 1;
    }
    if (is_word(c, "even")) {
        c->p += 4;
        *a = 2;
        *b = 0;
        return 1;
    }
    while (c->p < c->len) {
        unsigned char ch = (unsigned char)c->s[c->p];
        if (!is_nth_char(ch)) break;
        if (ch == 'N') ch = 'n';
        if (!is_css_space(ch)) {
            if (tl < sizeof text - 1) text[tl] = (char)ch;
            tl++;
        }
        c->p++;
    }
    if (tl < sizeof text) {
        for (k = 0; k < tl; k++)
            if (text[k] == 'n') {
                if (npos == (size_t)-1) npos = k;
                ncount++;
            }
        if (ncount == 0) {
            if (parse_int(text, tl, b)) {
                *a = 0;
                return 1;
            }
        } else if (ncount == 1) {
            const char *at = text, *bt = text + npos + 1;
            size_t al = npos, bl = tl - npos - 1;
            int ok_a, ok_b;
            if (al == 0 || (al == 1 && at[0] == '+')) {
                *a = 1;
                ok_a = 1;
            } else if (al == 1 && at[0] == '-') {
                *a = -1;
                ok_a = 1;
            } else {
                ok_a = parse_int(at, al, a);
            }
            if (bl == 0) {
                *b = 0;
                ok_b = 1;
            } else {
                ok_b = (bt[0] == '+' || bt[0] == '-') && bl > 1 && fhp_is_digit((unsigned char)bt[1]) && parse_int(bt, bl, b);
            }
            if (ok_a && ok_b) return 1;
        }
    }
    c->p = start;
    fail(c, "An+B expected (odd, even, 3, 2n+1, -n+3)", NULL);
    return 0;
}

static void parse_pseudo(compiler *c, compound *cp) {
    size_t start;
    fhp_str name;
    char *zname;
    simple *s;
    c->p++; /* ':' */
    if (peek(c, 0) == ':') {
        fail(c, "pseudo-elements (::before, ::text) select no tags", NULL);
        return;
    }
    if (!is_ident_start(c, c->p)) {
        fail(c, "a pseudo-class name expected after ':'", NULL);
        return;
    }
    start = c->p;
    name = fhp_to_lower(c->a, read_name(c));
    zname = fhp_strndup(c->a, name.ptr, name.len);
    if (!zname) {
        fail(c, "out of memory", NULL);
        return;
    }
#define NAME(w) (strcmp(zname, w) == 0)
    if (peek(c, 0) == '(') {
        c->p++;
        skip_space(c);
        if (NAME("not") || NAME("is") || NAME("where") || NAME("matches") || NAME("any") || NAME("has")) {
            int has = NAME("has");
            css_list *l = parse_list(c, has, 0);
            if (c->failed) return;
            s = add_simple(c, cp);
            if (!s) return;
            s->kind = NAME("not") ? S_NOT : has ? S_HAS : S_IS;
            s->list = l;
        } else if (NAME("nth-child") || NAME("nth-last-child") || NAME("nth-of-type") || NAME("nth-last-of-type")) {
            int64_t a, b;
            int of_type = name.len >= 7 && memcmp(zname + name.len - 7, "of-type", 7) == 0;
            css_list *of = NULL;
            if (!parse_nth(c, &a, &b)) return;
            skip_space(c);
            if (!of_type && is_word(c, "of")) {
                c->p += 2;
                of = parse_list(c, 0, 0);
                if (c->failed) return;
            }
            s = add_simple(c, cp);
            if (!s) return;
            s->kind = S_NTH;
            s->a = a;
            s->b = b;
            s->from_end = strstr(zname, "last") != NULL;
            s->of_type = of_type;
            s->list = of;
        } else if (NAME("contains")) {
            fhp_str text;
            if (peek(c, 0) == '"' || peek(c, 0) == '\'') {
                text = read_string(c);
                if (c->failed) return;
            } else if (is_ident_start(c, c->p)) {
                text = read_name(c);
            } else {
                fail(c, "a string expected in :contains()", NULL);
                return;
            }
            s = add_simple(c, cp);
            if (!s) return;
            s->kind = S_CONTAINS;
            s->value = text;
        } else {
            c->p = start;
            fail(c, "unknown pseudo-class ':%s()'", zname);
            return;
        }
        skip_space(c);
        expect(c, ')');
        return;
    }
    {
        int64_t b = 1;
        int kind = -1, from_end = 0, of_type = 0, twice = 0, pseudo = -1;
        if (NAME("first-child")) kind = S_NTH;
        else if (NAME("last-child")) kind = S_NTH, from_end = 1;
        else if (NAME("only-child")) kind = S_NTH, twice = 1;
        else if (NAME("first-of-type")) kind = S_NTH, of_type = 1;
        else if (NAME("last-of-type")) kind = S_NTH, from_end = 1, of_type = 1;
        else if (NAME("only-of-type")) kind = S_NTH, of_type = 1, twice = 1;
        else if (NAME("root")) pseudo = P_ROOT;
        else if (NAME("scope")) pseudo = P_SCOPE;
        else if (NAME("empty")) pseudo = P_EMPTY;
        else if (NAME("link") || NAME("any-link")) pseudo = P_LINK;
        else if (NAME("checked")) pseudo = P_CHECKED;
        else if (NAME("disabled")) pseudo = P_DISABLED;
        else if (NAME("enabled")) pseudo = P_ENABLED;
        else if (NAME("required")) pseudo = P_REQUIRED;
        else if (NAME("optional")) pseudo = P_OPTIONAL;
        else {
            c->p = start;
            fail(c, "unknown pseudo-class ':%s'", zname);
            return;
        }
        if (pseudo >= 0) {
            s = add_simple(c, cp);
            if (!s) return;
            s->kind = S_PSEUDO;
            s->pseudo = pseudo;
            return;
        }
        (void)kind;
        s = add_simple(c, cp);
        if (!s) return;
        s->kind = S_NTH;
        s->b = b;
        s->from_end = twice ? 0 : from_end;
        s->of_type = of_type;
        if (twice) {
            s = add_simple(c, cp);
            if (!s) return;
            s->kind = S_NTH;
            s->b = 1;
            s->from_end = 1;
            s->of_type = of_type;
        }
    }
#undef NAME
}

/* compound: [type | '*'] (#id | .class | [attribute] | :pseudo)* */
static int parse_compound(compiler *c, compound *cp) {
    int has_type = 0;
    char buf[8];
    memset(cp, 0, sizeof *cp);
    cp->key = -1;
    cp->name = FHP_EMPTY;
    if (peek(c, 0) == '*') {
        c->p++;
        has_type = 1;
    } else if (is_ident_start(c, c->p)) {
        fhp_str name = fhp_to_lower(c->a, read_name(c));
        has_type = 1;
        cp->key = fhp_lookup_key(name.ptr, name.len);
        if (cp->key < 0) {
            cp->name = name;
            cp->has_name = 1;
        }
    }
    if (peek(c, 0) == '|') {
        fail(c, "namespaces are not supported; escape the colon of a prefixed name: dc\\:creator", NULL);
        return 0;
    }
    while (!c->failed) {
        unsigned char ch = peek(c, 0);
        if (ch == '#') {
            simple *s;
            fhp_str id;
            c->p++;
            if (!is_name_char(c, c->p)) {
                fail(c, "an id expected after '#'", NULL);
                return 0;
            }
            id = read_name(c);
            s = add_simple(c, cp);
            if (!s) return 0;
            s->kind = S_ID;
            s->value = id;
        } else if (ch == '.') {
            simple *s;
            fhp_str cls;
            c->p++;
            if (!is_ident_start(c, c->p)) {
                fail(c, "a class name expected after '.'", NULL);
                return 0;
            }
            cls = read_name(c);
            s = add_simple(c, cp);
            if (!s) return 0;
            s->kind = S_CLASS;
            s->value = cls;
        } else if (ch == '[') {
            parse_attribute(c, cp);
        } else if (ch == ':') {
            parse_pseudo(c, cp);
        } else {
            break;
        }
    }
    if (c->failed) return 0;
    if (!has_type && cp->n == 0) {
        if (c->p >= c->len)
            fail(c, "a selector expected", NULL);
        else
            fail(c, "unexpected '%s'", char_at(c, c->p, buf));
        return 0;
    }
    return 1;
}

static int add_compound(compiler *c, complex *cx, int comb) {
    if (cx->n == cx->cap) {
        size_t cap = cx->cap ? cx->cap * 2 : 4;
        compound *p = (compound *)fhp_realloc(c->a, cx->compounds, cx->cap * sizeof(compound), cap * sizeof(compound));
        int *q = (int *)fhp_alloc(c->a, cap * sizeof(int));
        if (!p || !q) {
            fail(c, "out of memory", NULL);
            return 0;
        }
        if (cx->n) memcpy(q, cx->combinators, (cx->n - 1) * sizeof(int));
        cx->compounds = p;
        cx->combinators = q;
        cx->cap = cap;
    }
    if (cx->n > 0) cx->combinators[cx->n - 1] = comb;
    if (!parse_compound(c, &cx->compounds[cx->n])) return 0;
    cx->n++;
    return 1;
}

/* complex: [combinator] compound (combinator compound)* */
static int parse_complex(compiler *c, complex *cx, int relative, int top_level) {
    memset(cx, 0, sizeof *cx);
    skip_space(c);
    cx->leading = C_NONE;
    if (relative || top_level) {
        int comb = read_combinator(c);
        if (comb != C_NONE) {
            if (top_level && comb != C_CHILD) {
                fail(c, "a selector can start only with '>' (the children of the tag it is called on)", NULL);
                return 0;
            }
            cx->leading = comb;
        } else if (relative) {
            cx->leading = C_DESCENDANT;
        }
    }
    if (!add_compound(c, cx, C_NONE)) return 0;
    for (;;) {
        int ws = skip_space(c), comb;
        unsigned char ch = peek(c, 0);
        char buf[8];
        if (ch == 0 || ch == ',' || ch == ')') return 1;
        comb = read_combinator(c);
        if (comb == C_NONE) {
            if (!ws) {
                fail(c, "unexpected '%s'", char_at(c, c->p, buf));
                return 0;
            }
            comb = C_DESCENDANT;
        }
        if (!add_compound(c, cx, comb)) return 0;
    }
}

/* selector-list: complex (',' complex)* */
static css_list *parse_list(compiler *c, int relative, int top_level) {
    css_list *l = (css_list *)fhp_alloc(c->a, sizeof(css_list));
    if (!l) {
        fail(c, "out of memory", NULL);
        return NULL;
    }
    memset(l, 0, sizeof *l);
    for (;;) {
        if (l->n == l->cap) {
            size_t cap = l->cap ? l->cap * 2 : 2;
            complex *p = (complex *)fhp_realloc(c->a, l->items, l->cap * sizeof(complex), cap * sizeof(complex));
            if (!p) {
                fail(c, "out of memory", NULL);
                return NULL;
            }
            l->items = p;
            l->cap = cap;
        }
        if (!parse_complex(c, &l->items[l->n], relative, top_level)) return NULL;
        l->n++;
        skip_space(c);
        if (peek(c, 0) != ',') return l;
        c->p++;
    }
}

fhp_css *fhp_css_compile(const char *selector, char *err, size_t err_size) {
    compiler c;
    fhp_css *q;
    fhp_arena mem;
    char buf[8];
    if (err && err_size) err[0] = 0;
    if (!selector) selector = "";
    fhp_arena_init(&mem, 1024);
    q = (fhp_css *)fhp_alloc(&mem, sizeof(fhp_css));
    if (!q) return NULL;
    c.s = selector;
    c.len = strlen(selector);
    c.p = 0;
    c.a = &mem;
    c.err = err;
    c.err_size = err_size;
    c.failed = 0;
    q->list = parse_list(&c, 0, 1);
    if (!c.failed && c.p < c.len) fail(&c, "unexpected '%s'", char_at(&c, c.p, buf));
    if (c.failed || !q->list) {
        fhp_arena_destroy(&mem);
        return NULL;
    }
    q->mem = mem;
    return q;
}

void fhp_css_free(fhp_css *q) {
    fhp_arena mem;
    if (!q) return;
    mem = q->mem;
    fhp_arena_destroy(&mem);
}

int fhp_select(const fhp_doc *doc, fhp_node scope, const char *selector, fhp_arena *a, fhp_nodes *out) {
    fhp_css *q = fhp_css_compile(selector, NULL, 0);
    if (out) {
        out->items = NULL;
        out->len = 0;
    }
    if (!q) return -1;
    if (out) *out = fhp_css_all(q, doc, scope, a);
    fhp_css_free(q);
    return 0;
}

fhp_node fhp_select_first(const fhp_doc *doc, fhp_node scope, const char *selector) {
    fhp_css *q = fhp_css_compile(selector, NULL, 0);
    fhp_node n;
    if (!q) return FHP_NONE;
    n = fhp_css_first(q, doc, scope);
    fhp_css_free(q);
    return n;
}

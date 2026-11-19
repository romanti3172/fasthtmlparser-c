/* XPath 1.0 over the flat tag list: a tokenizer, a recursive-descent compiler and an evaluator. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "internal.h"

#define K_COMMENT KEY(FHP_TAG_COMMENT)

/* ----- the data model over the flat tag list ----- */

enum xkind { X_ROOT, X_ELEMENT, X_COMMENT, X_TEXT, X_ATTRIBUTE };

/* A node of the XPath data model: the document root, a tag (element or comment), a text node (a run of source text
   between tags) or an attribute (positions in the opening tag). */
typedef struct xnode {
    int kind;
    int32_t tag;   /* element / comment: the tag; text / attribute: the parent element (-1: the root) */
    size_t start;  /* text: first byte; attribute: name start */
    size_t length; /* text: length; attribute: name length */
    int has_value; /* attribute: value start and length */
    size_t value_start, value_length;
} xnode;

typedef struct xset {
    xnode *p;
    size_t len, cap;
} xset;

typedef struct estate {
    const fhp_doc *doc;
    fhp_arena *a; /* scratch for the evaluation */
    int has_all;
    xset all; /* every element, comment and text node in document order (following / preceding with text()) */
} estate;

static xnode root_node(void) {
    xnode x;
    memset(&x, 0, sizeof x);
    x.kind = X_ROOT;
    x.tag = -1;
    return x;
}

static xnode of_tag(const fhp_doc *doc, size_t i) {
    xnode x;
    memset(&x, 0, sizeof x);
    x.kind = doc->nodes[i].key == K_COMMENT ? X_COMMENT : X_ELEMENT;
    x.tag = (int32_t)i;
    return x;
}

static void xpush(estate *st, xset *s, xnode x) {
    if (s->len == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        xnode *p = (xnode *)fhp_realloc(st->a, s->p, s->cap * sizeof(xnode), cap * sizeof(xnode));
        if (!p) return;
        s->p = p;
        s->cap = cap;
    }
    s->p[s->len++] = x;
}

static xset xempty(void) {
    xset s;
    s.p = NULL;
    s.len = s.cap = 0;
    return s;
}

/* Document order and identity: tags and text nodes by their first byte; attributes right after their element, before
   its children. */
static int64_t xkey(const fhp_doc *doc, const xnode *x) {
    switch (x->kind) {
    case X_ROOT:
        return -1;
    case X_ELEMENT:
    case X_COMMENT:
        return (int64_t)doc->nodes[x->tag].start << 32;
    case X_ATTRIBUTE:
        return (int64_t)doc->nodes[x->tag].start << 32 | (int64_t)(uint32_t)x->start;
    default:
        return (int64_t)x->start << 32;
    }
}

/* The source positions the node covers (for following / preceding). */
static int64_t range_start(const fhp_doc *doc, const xnode *x) {
    switch (x->kind) {
    case X_ROOT:
        return -1;
    case X_TEXT:
        return (int64_t)x->start;
    default:
        return doc->nodes[x->tag].start;
    }
}

static int64_t range_end(const fhp_doc *doc, const xnode *x) {
    switch (x->kind) {
    case X_ROOT:
        return INT32_MAX;
    case X_ELEMENT:
    case X_COMMENT:
        return doc->nodes[x->tag].end;
    case X_TEXT:
        return (int64_t)x->start + (int64_t)x->length - 1;
    default:
        return doc->nodes[x->tag].start; /* the children of its element follow it */
    }
}

FHP_INLINE fhp_str attr_name_of(const fhp_doc *doc, const xnode *x) { return fhp_s(doc->src + x->start, x->length); }

/* ----- node tests ----- */

enum test_kind { T_NAME, T_PREFIX, T_WILDCARD, T_NODE, T_TEXT, T_COMMENT, T_PI, T_CONTAINER };

typedef struct node_test {
    int kind;
    fhp_str name;
    int key; /* the name as a known tag, or -1 */
} node_test;

static node_test make_test(int kind, fhp_str name) {
    node_test t;
    t.kind = kind;
    t.name = name;
    t.key = kind == T_NAME ? fhp_lookup_key(name.ptr, name.len) : -1;
    return t;
}

FHP_INLINE int matches_text(const node_test *t) { return t->kind == T_NODE || t->kind == T_TEXT; }
FHP_INLINE int matches_tags(const node_test *t) { return t->kind != T_TEXT && t->kind != T_PI; }

static int prefix_fold(fhp_str name, fhp_str prefix) {
    return name.len >= prefix.len && fhp_is_char_boundary(name.ptr, name.len, prefix.len) &&
           fhp_equal_fold(fhp_s(name.ptr, prefix.len), prefix);
}

static int matches_tag(const node_test *t, const fhp_doc *doc, size_t i) {
    const fhp_nodedata *d = &doc->nodes[i];
    switch (t->kind) {
    case T_NAME:
        /* as by_name(name).matches(n) */
        if (t->key >= 0) return d->key == t->key || (d->key < 0 && fhp_equal_fold(fhp_name_s(doc, (fhp_node)i), t->name));
        return fhp_equal_fold(fhp_name_s(doc, (fhp_node)i), t->name);
    case T_PREFIX:
        return fhp_is_element(doc, i) && prefix_fold(fhp_name_s(doc, (fhp_node)i), t->name);
    case T_WILDCARD:
    case T_CONTAINER:
        return fhp_is_element(doc, i);
    case T_NODE:
        return fhp_is_element(doc, i) || d->key == K_COMMENT;
    case T_COMMENT:
        return d->key == K_COMMENT;
    default:
        return 0;
    }
}

static int matches_attribute(const node_test *t, fhp_str name) {
    switch (t->kind) {
    case T_NAME:
        return fhp_equal_fold(name, t->name);
    case T_PREFIX:
        return prefix_fold(name, t->name);
    case T_WILDCARD:
    case T_NODE:
        return 1;
    default:
        return 0;
    }
}

static int test_matches(const node_test *t, const fhp_doc *doc, const xnode *x, int attribute_axis) {
    switch (x->kind) {
    case X_ROOT:
        return t->kind == T_NODE || t->kind == T_CONTAINER;
    case X_ELEMENT:
    case X_COMMENT:
        return matches_tag(t, doc, (size_t)x->tag);
    case X_TEXT:
        return matches_text(t);
    default:
        return attribute_axis ? matches_attribute(t, attr_name_of(doc, x)) : t->kind == T_NODE;
    }
}

/* ----- navigation ----- */

/* The content of a container: the tags (nodes[first..], up to end, at child_depth) and the text range. */
static int content(const fhp_doc *doc, const xnode *x, size_t *first, int64_t *end, int32_t *child_depth, size_t *p,
                   size_t *text_end) {
    if (x->kind == X_ROOT) {
        *first = 0;
        *end = INT32_MAX;
        *child_depth = 0;
        *p = 0;
        *text_end = doc->len;
        return 1;
    }
    if (x->kind == X_ELEMENT) {
        const fhp_nodedata *t = &doc->nodes[x->tag];
        if (t->inner_start > 0 && t->inner_end > 0) {
            *first = (size_t)x->tag + 1;
            *end = t->end;
            *child_depth = t->depth + 1;
            *p = (size_t)t->inner_start;
            *text_end = (size_t)t->inner_end + 1;
            return 1;
        }
    }
    return 0;
}

static void add_text(estate *st, int32_t parent, size_t start, size_t end, xset *out) {
    if (end > start) {
        xnode x;
        memset(&x, 0, sizeof x);
        x.kind = X_TEXT;
        x.tag = parent;
        x.start = start;
        x.length = end - start;
        xpush(st, out, x);
    }
}

/* Appends the children of the root or an element that match test, text nodes included, in document order. */
static void add_children(estate *st, const xnode *x, const node_test *test, xset *out) {
    const fhp_doc *doc = st->doc;
    int want_text = matches_text(test), want_tags = matches_tags(test);
    size_t first, p, text_end, i;
    int64_t end;
    int32_t child_depth, parent;
    if (!want_text && !want_tags) return;
    if (!content(doc, x, &first, &end, &child_depth, &p, &text_end)) return;
    parent = x->kind == X_ELEMENT ? x->tag : -1;
    for (i = first; i < doc->count; i++) {
        const fhp_nodedata *t = &doc->nodes[i];
        if (t->end > end) break;
        if (t->depth != child_depth) continue;
        if (want_text && (size_t)t->start > p) add_text(st, parent, p, (size_t)t->start < text_end ? (size_t)t->start : text_end, out);
        if (want_tags && matches_tag(test, doc, i)) xpush(st, out, of_tag(doc, i));
        p = (size_t)t->end + 1;
    }
    if (want_text && text_end > p) add_text(st, parent, p, text_end, out);
}

static void add_descendants_with_text(estate *st, const xnode *x, const node_test *test, xset *out) {
    xset children = xempty();
    node_test any = make_test(T_NODE, FHP_EMPTY);
    size_t k;
    add_children(st, x, &any, &children);
    for (k = 0; k < children.len; k++) {
        xnode child = children.p[k];
        if (test_matches(test, st->doc, &child, 0)) xpush(st, out, child);
        if (child.kind == X_ELEMENT) add_descendants_with_text(st, &child, test, out);
    }
}

/* Appends the descendants of the root or an element that match test, in document order. */
static void add_descendants(estate *st, const xnode *x, const node_test *test, xset *out) {
    const fhp_doc *doc = st->doc;
    size_t first, p, text_end, i;
    int64_t end;
    int32_t child_depth;
    if (matches_text(test)) {
        add_descendants_with_text(st, x, test, out);
        return;
    }
    if (!matches_tags(test)) return;
    if (!content(doc, x, &first, &end, &child_depth, &p, &text_end)) return;
    /* tags only: one linear scan of the flat list */
    for (i = first; i < doc->count; i++) {
        if (doc->nodes[i].end > end) break;
        if (matches_tag(test, doc, i)) xpush(st, out, of_tag(doc, i));
    }
}

static int parent_node(const fhp_doc *doc, const xnode *x, xnode *out) {
    switch (x->kind) {
    case X_ROOT:
        return 0;
    case X_TEXT:
    case X_ATTRIBUTE:
        *out = x->tag >= 0 ? of_tag(doc, (size_t)x->tag) : root_node();
        return 1;
    default: {
        const fhp_nodedata *t = &doc->nodes[x->tag];
        *out = t->depth > 0 && t->parent >= 0 ? of_tag(doc, (size_t)t->parent) : root_node();
        return 1;
    }
    }
}

static void add_attributes(estate *st, const xnode *x, const node_test *test, xset *out) {
    fhp_attr_iter it;
    fhp_scanned_attr a;
    if (x->kind != X_ELEMENT) return;
    fhp_attr_scanner(st->doc, x->tag, &it);
    while (fhp_next_attr(&it, &a)) {
        if (matches_attribute(test, fhp_s(st->doc->src + a.name_start, a.name_len))) {
            xnode n;
            n.kind = X_ATTRIBUTE;
            n.tag = x->tag;
            n.start = a.name_start;
            n.length = a.name_len;
            n.has_value = a.has_value;
            n.value_start = a.value_start;
            n.value_length = a.value_len;
            xpush(st, out, n);
        }
    }
}

/* The raw value of the first attribute with the name; 0 when the element has none. */
static int attribute_of(const fhp_doc *doc, const xnode *x, fhp_str name, fhp_str *value) {
    fhp_attr_iter it;
    fhp_scanned_attr a;
    if (x->kind != X_ELEMENT) return 0;
    fhp_attr_scanner(doc, x->tag, &it);
    while (fhp_next_attr(&it, &a)) {
        if (fhp_equal_fold(fhp_s(doc->src + a.name_start, a.name_len), name)) {
            *value = a.has_value ? fhp_s(doc->src + a.value_start, a.value_len) : FHP_EMPTY;
            return 1;
        }
    }
    return 0;
}

/* The root and elements give their visible text (as text()); text nodes and attributes their decoded value; comments
   their content. */
static fhp_str string_value(estate *st, const xnode *x) {
    const fhp_doc *doc = st->doc;
    switch (x->kind) {
    case X_ROOT:
        return fhp_clear_tags(st->a, fhp_s(doc->src, doc->len));
    case X_ELEMENT:
        return fhp_text(doc, x->tag, st->a);
    case X_ATTRIBUTE:
        if (!x->has_value) return FHP_EMPTY;
        return fhp_decode(st->a, fhp_s(doc->src + x->value_start, x->value_length), 1);
    case X_TEXT: {
        fhp_str text = fhp_s(doc->src + x->start, x->length);
        if (x->tag >= 0 && fhp_has_raw_text(doc->nodes[x->tag].key)) return fhp_normalize_newlines(st->a, text);
        return fhp_decode(st->a, text, 0);
    }
    default: {
        const fhp_nodedata *t = &doc->nodes[x->tag];
        fhp_str s = fhp_s(doc->src + t->start, (size_t)(t->end - t->start) + 1);
        if (fhp_starts_with(s, fhp_s("<!--", 4))) s = fhp_s(s.ptr + 4, s.len - 4);
        if (fhp_ends_with(s, fhp_s("-->", 3))) s.len -= 3;
        return s;
    }
    }
}

/* For name() / local-name(): the lower-case tag or attribute name; "" for other nodes. */
static fhp_str node_name(estate *st, const xnode *x) {
    if (x->kind == X_ELEMENT) return fhp_name_s(st->doc, x->tag);
    if (x->kind == X_ATTRIBUTE) return fhp_to_lower(st->a, attr_name_of(st->doc, x));
    return FHP_EMPTY;
}

/* ----- values ----- */

enum xtype { Y_NODES, Y_NUMBER, Y_STRING, Y_BOOLEAN };

typedef struct value {
    int type;
    xset nodes;
    double num;
    fhp_str str;
    int b;
} value;

static value v_num(double d) {
    value v;
    memset(&v, 0, sizeof v);
    v.type = Y_NUMBER;
    v.num = d;
    return v;
}

static value v_str(fhp_str s) {
    value v;
    memset(&v, 0, sizeof v);
    v.type = Y_STRING;
    v.str = s;
    return v;
}

static value v_bool(int b) {
    value v;
    memset(&v, 0, sizeof v);
    v.type = Y_BOOLEAN;
    v.b = b != 0;
    return v;
}

static value v_nodes(xset s) {
    value v;
    memset(&v, 0, sizeof v);
    v.type = Y_NODES;
    v.nodes = s;
    return v;
}

static int to_boolean(const value *v) {
    switch (v->type) {
    case Y_BOOLEAN:
        return v->b;
    case Y_NUMBER:
        return v->num != 0 && v->num == v->num;
    case Y_STRING:
        return v->str.len != 0;
    default:
        return v->nodes.len != 0;
    }
}

static double xnan(void) {
    volatile double z = 0.0;
    return z / z;
}

/* An optional '-', digits with an optional decimal point; anything else is NaN. */
static double string_to_number(fhp_str value) {
    fhp_str s = fhp_trim(value);
    size_t i = 0, digits = 0, point = 0;
    char buf[64];
    char *tmp;
    double d;
    if (s.len && s.ptr[0] == '-') i = 1;
    for (; i < s.len; i++) {
        char c = s.ptr[i];
        if (fhp_is_digit((unsigned char)c))
            digits++;
        else if (c == '.')
            point++;
        else
            return xnan();
    }
    if (digits == 0 || point > 1) return xnan();
    if (s.len < sizeof buf) {
        memcpy(buf, s.ptr, s.len);
        buf[s.len] = 0;
        return strtod(buf, NULL);
    }
    tmp = (char *)malloc(s.len + 1);
    if (!tmp) return xnan();
    memcpy(tmp, s.ptr, s.len);
    tmp[s.len] = 0;
    d = strtod(tmp, NULL);
    free(tmp);
    return d;
}

static double to_number(estate *st, const value *v) {
    switch (v->type) {
    case Y_NUMBER:
        return v->num;
    case Y_BOOLEAN:
        return v->b ? 1.0 : 0.0;
    case Y_STRING:
        return string_to_number(v->str);
    default:
        if (v->nodes.len == 0) return xnan();
        return string_to_number(string_value(st, &v->nodes.p[0]));
    }
}

static fhp_str number_string(fhp_arena *a, double d) {
    fhp_buf b;
    fhp_buf_init(&b, a, 32);
    fhp_format_number(d, &b);
    return fhp_buf_str(&b);
}

static fhp_str to_string(estate *st, const value *v) {
    switch (v->type) {
    case Y_STRING:
        return v->str;
    case Y_BOOLEAN:
        return v->b ? fhp_s("true", 4) : fhp_s("false", 5);
    case Y_NUMBER:
        return number_string(st->a, v->num);
    default:
        if (v->nodes.len == 0) return FHP_EMPTY;
        return string_value(st, &v->nodes.p[0]);
    }
}

typedef struct keyed {
    int64_t key;
    xnode n;
} keyed;

static int cmp_keyed(const void *a, const void *b) {
    int64_t x = ((const keyed *)a)->key, y = ((const keyed *)b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int in_document_order(const fhp_doc *doc, const xset *s) {
    size_t i;
    for (i = 1; i < s->len; i++)
        if (!(xkey(doc, &s->p[i - 1]) < xkey(doc, &s->p[i]))) return 0;
    return 1;
}

/* Puts the nodes in document order without duplicates. */
static xset sort_unique(estate *st, xset s) {
    keyed *k;
    size_t i, n = 0;
    if (s.len < 2 || in_document_order(st->doc, &s)) return s;
    k = (keyed *)fhp_alloc(st->a, s.len * sizeof(keyed));
    if (!k) return s;
    for (i = 0; i < s.len; i++) {
        k[i].key = xkey(st->doc, &s.p[i]);
        k[i].n = s.p[i];
    }
    qsort(k, s.len, sizeof(keyed), cmp_keyed);
    for (i = 0; i < s.len; i++)
        if (n == 0 || k[i].key != k[n - 1].key) k[n++] = k[i];
    for (i = 0; i < n; i++) s.p[i] = k[i].n;
    s.len = n;
    return s;
}

/* ----- expressions ----- */

enum compare_op { OP_EQ, OP_NEQ, OP_LT, OP_LE, OP_GT, OP_GE };
enum arith_op { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD };
enum axis {
    AX_CHILD, AX_DESCENDANT, AX_DESCENDANT_OR_SELF, AX_SELF, AX_PARENT, AX_ANCESTOR, AX_ANCESTOR_OR_SELF,
    AX_FOLLOWING_SIBLING, AX_PRECEDING_SIBLING, AX_FOLLOWING, AX_PRECEDING, AX_ATTRIBUTE
};
enum func {
    F_LAST, F_POSITION, F_COUNT, F_NAME, F_LOCAL_NAME, F_STRING, F_CONCAT, F_STARTS_WITH, F_ENDS_WITH, F_CONTAINS,
    F_SUBSTRING_BEFORE, F_SUBSTRING_AFTER, F_SUBSTRING, F_STRING_LENGTH, F_NORMALIZE_SPACE, F_TRANSLATE, F_LOWER_CASE,
    F_UPPER_CASE, F_NOT, F_TRUE, F_FALSE, F_BOOLEAN, F_NUMBER, F_SUM, F_FLOOR, F_CEILING, F_ROUND
};
enum ekind {
    E_LITERAL, E_NUMBER, E_LOGIC, E_COMPARE, E_ARITH, E_NEGATE, E_UNION, E_ATTR_EQUALS, E_ATTR_EXISTS, E_PATH,
    E_FILTER, E_FUNCTION
};

typedef struct expr expr;

typedef struct step {
    int axis;
    node_test test;
    expr **predicates;
    size_t npred;
} step;

struct expr {
    int kind;
    fhp_str str;   /* literal; attribute name (E_ATTR_*) */
    fhp_str value; /* E_ATTR_EQUALS */
    double num;
    int op;   /* and (1) / or; compare op; arith op; negate (E_ATTR_EQUALS); func */
    int type; /* the result type of a function */
    expr *left, *right; /* E_PATH: left is the start (or NULL); E_FILTER: left is the primary */
    int absolute;
    step *steps;
    size_t nsteps;
    expr **list; /* predicates of E_FILTER, arguments of E_FUNCTION */
    size_t nlist;
};

struct fhp_xpath {
    fhp_arena mem;
    expr *e;
};

static int expr_type(const expr *e) {
    switch (e->kind) {
    case E_LITERAL:
        return Y_STRING;
    case E_NUMBER:
    case E_ARITH:
    case E_NEGATE:
        return Y_NUMBER;
    case E_LOGIC:
    case E_COMPARE:
    case E_ATTR_EQUALS:
    case E_ATTR_EXISTS:
        return Y_BOOLEAN;
    case E_UNION:
    case E_PATH:
    case E_FILTER:
        return Y_NODES;
    default:
        return e->type;
    }
}

/* Whether position() or last() is used in the current context (not inside nested predicates). */
static int uses_position(const expr *e) {
    size_t i;
    switch (e->kind) {
    case E_LOGIC:
    case E_COMPARE:
    case E_ARITH:
    case E_UNION:
        return uses_position(e->left) || uses_position(e->right);
    case E_NEGATE:
        return uses_position(e->left);
    case E_PATH:
        return e->left && uses_position(e->left);
    case E_FILTER:
        return uses_position(e->left);
    case E_FUNCTION:
        if (e->op == F_POSITION || e->op == F_LAST) return 1;
        for (i = 0; i < e->nlist; i++)
            if (uses_position(e->list[i])) return 1;
        return 0;
    default:
        return 0;
    }
}

/* Whether the predicate depends on the position of the node (a number, position(), last()). */
static int is_positional(const expr *e) { return expr_type(e) == Y_NUMBER || uses_position(e); }

typedef struct ectx {
    xnode node;
    size_t position, size;
    estate *st;
} ectx;

static value eval(const expr *e, const ectx *c);
static int eval_bool(const expr *e, const ectx *c);
static xset filter(estate *st, xset nodes, expr *const *predicates, size_t n);
static xset step_apply(const step *s, xset input, estate *st);

static xset eval_nodes(const expr *e, const ectx *c) {
    value v = eval(e, c);
    return v.type == Y_NODES ? v.nodes : xempty(); /* the compiler makes sure it is a node-set */
}

static fhp_str eval_string(const expr *e, const ectx *c) {
    value v = eval(e, c);
    return to_string(c->st, &v);
}

static double eval_number(const expr *e, const ectx *c) {
    value v;
    if (e->kind == E_NUMBER) return e->num;
    v = eval(e, c);
    return to_number(c->st, &v);
}

static int compare_atoms(estate *st, int op, const value *a, const value *b) {
    double x, y;
    if (op == OP_EQ || op == OP_NEQ) {
        int eq;
        if (a->type == Y_BOOLEAN || b->type == Y_BOOLEAN)
            eq = to_boolean(a) == to_boolean(b);
        else if (a->type == Y_NUMBER || b->type == Y_NUMBER)
            eq = to_number(st, a) == to_number(st, b);
        else
            eq = fhp_str_eq(to_string(st, a), to_string(st, b));
        return eq == (op == OP_EQ);
    }
    x = to_number(st, a);
    y = to_number(st, b);
    switch (op) {
    case OP_LT:
        return x < y;
    case OP_LE:
        return x <= y;
    case OP_GT:
        return x > y;
    default:
        return x >= y;
    }
}

static int compare_with_nodes(estate *st, int op, const xset *nodes, const value *other, int nodes_on_left) {
    size_t i;
    int is_number;
    if (other->type == Y_BOOLEAN) {
        value as_bool = v_bool(nodes->len != 0);
        return nodes_on_left ? compare_atoms(st, op, &as_bool, other) : compare_atoms(st, op, other, &as_bool);
    }
    is_number = other->type == Y_NUMBER;
    for (i = 0; i < nodes->len; i++) {
        fhp_str s = string_value(st, &nodes->p[i]);
        value v = is_number ? v_num(string_to_number(s)) : v_str(s);
        if (nodes_on_left ? compare_atoms(st, op, &v, other) : compare_atoms(st, op, other, &v)) return 1;
    }
    return 0;
}

static int compare(estate *st, int op, const value *a, const value *b) {
    if (a->type == Y_NODES && b->type == Y_NODES) {
        size_t i, j;
        fhp_str *values_b;
        if (a->nodes.len == 0 || b->nodes.len == 0) return 0;
        values_b = (fhp_str *)fhp_alloc(st->a, b->nodes.len * sizeof(fhp_str));
        if (!values_b) return 0;
        for (j = 0; j < b->nodes.len; j++) values_b[j] = string_value(st, &b->nodes.p[j]);
        for (i = 0; i < a->nodes.len; i++) {
            value x = v_str(string_value(st, &a->nodes.p[i]));
            for (j = 0; j < b->nodes.len; j++) {
                value y = v_str(values_b[j]);
                if (compare_atoms(st, op, &x, &y)) return 1;
            }
        }
        return 0;
    }
    if (a->type == Y_NODES) return compare_with_nodes(st, op, &a->nodes, b, 1);
    if (b->type == Y_NODES) return compare_with_nodes(st, op, &b->nodes, a, 0);
    return compare_atoms(st, op, a, b);
}

/* A number compares with the position, anything else is converted to boolean. */
static int eval_predicate(const expr *e, const ectx *c) {
    value v;
    if (expr_type(e) == Y_BOOLEAN) return eval_bool(e, c);
    v = eval(e, c);
    if (v.type == Y_NUMBER) return v.num == (double)c->position;
    return to_boolean(&v);
}

static double xround(double v) {
    if (v != v || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) return v;
    return floor(v + 0.5);
}

/* XPath 1.0: the chars at 1-based positions p with round(start) <= p < round(start) + round(length). */
static fhp_str substring(const expr *e, const ectx *c) {
    fhp_str s = eval_string(e->list[0], c);
    double from = xround(eval_number(e->list[1], c));
    double to = e->nlist > 2 ? from + xround(eval_number(e->list[2], c)) : HUGE_VAL;
    double n;
    size_t count, first, end, i, k, bstart = s.len, bend = s.len;
    if (from != from || to != to) return FHP_EMPTY;
    count = fhp_char_count(s);
    n = (double)count;
    first = from <= 1.0 ? 0 : from > n ? count : (size_t)from - 1;
    end = to >= n + 1.0 ? count : to <= 1.0 ? 0 : (size_t)to - 1;
    if (end <= first) return FHP_EMPTY;
    /* char indexes -> byte offsets */
    for (i = 0, k = 0; i <= s.len; k++) {
        size_t size;
        if (k == first) bstart = i;
        if (k == end) {
            bend = i;
            break;
        }
        if (i == s.len) break;
        fhp_utf8_decode(s.ptr + i, s.len - i, &size);
        i += size;
    }
    return fhp_s(s.ptr + bstart, bend - bstart);
}

static fhp_str normalize_space(estate *st, fhp_str s) {
    fhp_buf b;
    size_t i = 0;
    int pending = 0;
    fhp_buf_init(&b, st->a, s.len);
    while (i < s.len) {
        size_t size;
        if (fhp_space_at(s.ptr, s.len, i, &size)) {
            pending = b.len > 0;
            i += size;
            continue;
        }
        if (pending) {
            fhp_buf_addc(&b, ' ');
            pending = 0;
        }
        fhp_buf_add(&b, s.ptr + i, size);
        i += size;
    }
    return fhp_buf_str(&b);
}

static fhp_str translate(estate *st, fhp_str s, fhp_str from, fhp_str to) {
    fhp_buf b;
    size_t i = 0;
    fhp_buf_init(&b, st->a, s.len);
    while (i < s.len) {
        size_t size, j = 0, index = 0, k;
        uint32_t ch = fhp_utf8_decode(s.ptr + i, s.len - i, &size);
        int found = 0;
        while (j < from.len) {
            size_t fs;
            uint32_t f = fhp_utf8_decode(from.ptr + j, from.len - j, &fs);
            if (f == ch) {
                found = 1;
                break;
            }
            j += fs;
            index++;
        }
        if (!found) {
            fhp_buf_add(&b, s.ptr + i, size);
        } else {
            /* the char at index in to, if there is one */
            size_t t = 0;
            for (k = 0; t < to.len; k++) {
                size_t ts;
                uint32_t tc = fhp_utf8_decode(to.ptr + t, to.len - t, &ts);
                if (k == index) {
                    fhp_buf_add_cp(&b, tc);
                    break;
                }
                t += ts;
            }
        }
        i += size;
    }
    return fhp_buf_str(&b);
}

static value call(const expr *e, const ectx *c) {
    estate *st = c->st;
    const fhp_doc *doc = st->doc;
#define ARG(i) eval_string(e->list[i], c)
    switch (e->op) {
    case F_LAST:
        return v_num((double)c->size);
    case F_POSITION:
        return v_num((double)c->position);
    case F_COUNT:
        return v_num((double)eval_nodes(e->list[0], c).len);
    case F_NAME:
    case F_LOCAL_NAME: {
        fhp_str name = FHP_EMPTY;
        if (e->nlist == 0) {
            name = node_name(st, &c->node);
        } else {
            xset s = eval_nodes(e->list[0], c);
            if (s.len) name = node_name(st, &s.p[0]);
        }
        if (e->op == F_LOCAL_NAME) {
            size_t i = name.len;
            while (i > 0 && name.ptr[i - 1] != ':') i--;
            return v_str(fhp_s(name.ptr + i, name.len - i));
        }
        return v_str(name);
    }
    case F_STRING:
        return v_str(e->nlist == 0 ? string_value(st, &c->node) : ARG(0));
    case F_CONCAT: {
        fhp_buf b;
        size_t i;
        fhp_buf_init(&b, st->a, 64);
        for (i = 0; i < e->nlist; i++) fhp_buf_adds(&b, ARG(i));
        return v_str(fhp_buf_str(&b));
    }
    case F_STARTS_WITH: {
        fhp_str a = ARG(0), b = ARG(1);
        return v_bool(fhp_starts_with(a, b));
    }
    case F_ENDS_WITH: {
        fhp_str a = ARG(0), b = ARG(1);
        return v_bool(fhp_ends_with(a, b));
    }
    case F_CONTAINS: {
        fhp_str a = ARG(0), b = ARG(1);
        return v_bool(fhp_index(a.ptr, a.len, b.ptr, b.len) >= 0);
    }
    case F_SUBSTRING_BEFORE: {
        fhp_str s = ARG(0), t = ARG(1);
        ptrdiff_t i = fhp_index(s.ptr, s.len, t.ptr, t.len);
        return v_str(i < 0 ? FHP_EMPTY : fhp_s(s.ptr, (size_t)i));
    }
    case F_SUBSTRING_AFTER: {
        fhp_str s = ARG(0), t = ARG(1);
        ptrdiff_t i = fhp_index(s.ptr, s.len, t.ptr, t.len);
        return v_str(i < 0 ? FHP_EMPTY : fhp_s(s.ptr + i + t.len, s.len - (size_t)i - t.len));
    }
    case F_SUBSTRING:
        return v_str(substring(e, c));
    case F_STRING_LENGTH:
        return v_num((double)fhp_utf16_len(e->nlist == 0 ? string_value(st, &c->node) : ARG(0)));
    case F_NORMALIZE_SPACE:
        return v_str(normalize_space(st, e->nlist == 0 ? string_value(st, &c->node) : ARG(0)));
    case F_TRANSLATE: {
        fhp_str s = ARG(0), from = ARG(1), to = ARG(2);
        return v_str(translate(st, s, from, to));
    }
    case F_LOWER_CASE:
        return v_str(fhp_to_lower(st->a, ARG(0)));
    case F_UPPER_CASE:
        return v_str(fhp_to_upper(st->a, ARG(0)));
    case F_NOT:
        return v_bool(!eval_bool(e->list[0], c));
    case F_TRUE:
        return v_bool(1);
    case F_FALSE:
        return v_bool(0);
    case F_BOOLEAN:
        return v_bool(eval_bool(e->list[0], c));
    case F_NUMBER:
        if (e->nlist == 0) return v_num(string_to_number(string_value(st, &c->node)));
        return v_num(eval_number(e->list[0], c));
    case F_SUM: {
        xset s = eval_nodes(e->list[0], c);
        double sum = 0;
        size_t i;
        for (i = 0; i < s.len; i++) sum += string_to_number(string_value(st, &s.p[i]));
        return v_num(sum);
    }
    case F_FLOOR:
        return v_num(floor(eval_number(e->list[0], c)));
    case F_CEILING:
        return v_num(ceil(eval_number(e->list[0], c)));
    case F_ROUND:
        return v_num(xround(eval_number(e->list[0], c)));
    default:
        (void)doc;
        return v_str(FHP_EMPTY);
    }
#undef ARG
}

static value eval(const expr *e, const ectx *c) {
    estate *st = c->st;
    switch (e->kind) {
    case E_LITERAL:
        return v_str(e->str);
    case E_NUMBER:
        return v_num(e->num);
    case E_LOGIC:
    case E_COMPARE:
    case E_ATTR_EQUALS:
    case E_ATTR_EXISTS:
        return v_bool(eval_bool(e, c));
    case E_ARITH: {
        double x = eval_number(e->left, c), y = eval_number(e->right, c);
        switch (e->op) {
        case OP_ADD:
            return v_num(x + y);
        case OP_SUB:
            return v_num(x - y);
        case OP_MUL:
            return v_num(x * y);
        case OP_DIV:
            return v_num(x / y);
        default:
            return v_num(fmod(x, y));
        }
    }
    case E_NEGATE:
        return v_num(-eval_number(e->left, c));
    case E_UNION: {
        xset l = eval_nodes(e->left, c), r = eval_nodes(e->right, c), u = xempty();
        size_t i;
        for (i = 0; i < l.len; i++) xpush(st, &u, l.p[i]);
        for (i = 0; i < r.len; i++) xpush(st, &u, r.p[i]);
        return v_nodes(sort_unique(st, u));
    }
    case E_PATH: {
        xset current = xempty();
        size_t i;
        if (e->left)
            current = eval_nodes(e->left, c);
        else
            xpush(st, &current, e->absolute ? root_node() : c->node);
        for (i = 0; i < e->nsteps && current.len; i++) current = step_apply(&e->steps[i], current, st);
        return v_nodes(current);
    }
    case E_FILTER:
        return v_nodes(filter(st, eval_nodes(e->left, c), e->list, e->nlist));
    case E_FUNCTION:
        return call(e, c);
    default:
        return v_str(FHP_EMPTY);
    }
}

static int eval_bool(const expr *e, const ectx *c) {
    switch (e->kind) {
    case E_LITERAL:
        return e->str.len != 0;
    case E_LOGIC:
        if (e->op) return eval_bool(e->left, c) && eval_bool(e->right, c);
        return eval_bool(e->left, c) || eval_bool(e->right, c);
    case E_COMPARE: {
        value a = eval(e->left, c), b = eval(e->right, c);
        return compare(c->st, e->op, &a, &b);
    }
    case E_ATTR_EQUALS: {
        /* '!=' is true when the attribute exists and differs (a missing attribute is an empty node-set) */
        fhp_str raw;
        if (!attribute_of(c->st->doc, &c->node, e->str, &raw)) return 0;
        return fhp_attr_equals(raw, e->value) != e->op;
    }
    case E_ATTR_EXISTS: {
        fhp_str raw;
        return attribute_of(c->st->doc, &c->node, e->str, &raw);
    }
    default: {
        value v = eval(e, c);
        return to_boolean(&v);
    }
    }
}

/* Applies the predicates one after another; positions are 1-based in the order of the list. */
static xset filter(estate *st, xset nodes, expr *const *predicates, size_t n) {
    size_t p;
    for (p = 0; p < n; p++) {
        xset kept = xempty();
        size_t i;
        ectx c;
        c.st = st;
        c.size = nodes.len;
        for (i = 0; i < nodes.len; i++) {
            c.node = nodes.p[i];
            c.position = i + 1;
            if (eval_predicate(predicates[p], &c)) xpush(st, &kept, nodes.p[i]);
        }
        nodes = kept;
    }
    return nodes;
}

static int is_reverse(const step *s) {
    return s->axis == AX_ANCESTOR || s->axis == AX_ANCESTOR_OR_SELF || s->axis == AX_PRECEDING ||
           s->axis == AX_PRECEDING_SIBLING;
}

static int has_positional_predicates(const step *s) {
    size_t i;
    for (i = 0; i < s->npred; i++)
        if (is_positional(s->predicates[i])) return 1;
    return 0;
}

static void collect_siblings(estate *st, const xnode *x, const node_test *test, xset *out, int following) {
    const fhp_doc *doc = st->doc;
    xnode parent;
    xset siblings;
    node_test any;
    int64_t key;
    size_t k, index;
    int found = 0;
    if (x->kind == X_ROOT || x->kind == X_ATTRIBUTE) return;
    if (!matches_text(test) && x->kind != X_TEXT) {
        /* tags only: walk the flat list; deeper tags are inside the siblings, a shallower one ends the parent */
        size_t idx = (size_t)x->tag;
        int32_t depth = doc->nodes[idx].depth;
        if (following) {
            for (k = idx + 1; k < doc->count; k++) {
                if (doc->nodes[k].depth < depth) break;
                if (doc->nodes[k].depth == depth && matches_tag(test, doc, k)) xpush(st, out, of_tag(doc, k));
            }
        } else {
            for (k = idx; k-- > 0;) {
                if (doc->nodes[k].depth < depth) break;
                if (doc->nodes[k].depth == depth && matches_tag(test, doc, k)) xpush(st, out, of_tag(doc, k));
            }
        }
        return;
    }
    if (!parent_node(doc, x, &parent)) return;
    siblings = xempty();
    any = make_test(T_NODE, FHP_EMPTY);
    add_children(st, &parent, &any, &siblings);
    key = xkey(doc, x);
    for (index = 0; index < siblings.len; index++)
        if (xkey(doc, &siblings.p[index]) == key) {
            found = 1;
            break;
        }
    if (!found) return;
    if (following) {
        for (k = index + 1; k < siblings.len; k++)
            if (test_matches(test, doc, &siblings.p[k], 0)) xpush(st, out, siblings.p[k]);
    } else {
        for (k = index; k-- > 0;)
            if (test_matches(test, doc, &siblings.p[k], 0)) xpush(st, out, siblings.p[k]);
    }
}

static const xset *all_nodes(estate *st) {
    if (!st->has_all) {
        xnode r = root_node();
        node_test any = make_test(T_NODE, FHP_EMPTY);
        st->all = xempty();
        add_descendants(st, &r, &any, &st->all);
        st->has_all = 1;
    }
    return &st->all;
}

static void collect_following(estate *st, const xnode *x, const node_test *test, xset *out) {
    const fhp_doc *doc = st->doc;
    int64_t after;
    size_t i, first;
    if (x->kind == X_ROOT) return;
    after = range_end(doc, x);
    if (matches_text(test)) {
        const xset *all = all_nodes(st);
        for (i = 0; i < all->len; i++)
            if (range_start(doc, &all->p[i]) > after && test_matches(test, doc, &all->p[i], 0)) xpush(st, out, all->p[i]);
        return;
    }
    first = x->kind == X_ELEMENT || x->kind == X_COMMENT ? (size_t)x->tag + 1 : 0;
    for (i = first; i < doc->count; i++)
        if ((int64_t)doc->nodes[i].start > after && matches_tag(test, doc, i)) xpush(st, out, of_tag(doc, i));
}

static void collect_preceding(estate *st, const xnode *x, const node_test *test, xset *out) {
    const fhp_doc *doc = st->doc;
    int64_t before;
    size_t i, last;
    if (x->kind == X_ROOT) return;
    before = range_start(doc, x);
    if (matches_text(test)) {
        const xset *all = all_nodes(st);
        for (i = all->len; i-- > 0;)
            if (range_end(doc, &all->p[i]) < before && test_matches(test, doc, &all->p[i], 0)) xpush(st, out, all->p[i]);
        return;
    }
    last = x->kind == X_ELEMENT || x->kind == X_COMMENT ? (size_t)x->tag : doc->count;
    for (i = last; i-- > 0;)
        if ((int64_t)doc->nodes[i].end < before && matches_tag(test, doc, i)) xpush(st, out, of_tag(doc, i));
}

/* Appends the nodes of the axis that pass the node test, in axis order (reverse axes: nearest first). */
static void collect(const step *s, estate *st, const xnode *x, xset *out) {
    const fhp_doc *doc = st->doc;
    const node_test *test = &s->test;
    switch (s->axis) {
    case AX_CHILD:
        add_children(st, x, test, out);
        break;
    case AX_DESCENDANT:
        add_descendants(st, x, test, out);
        break;
    case AX_DESCENDANT_OR_SELF:
        if (test_matches(test, doc, x, 0)) xpush(st, out, *x);
        add_descendants(st, x, test, out);
        break;
    case AX_SELF:
        if (test_matches(test, doc, x, 0)) xpush(st, out, *x);
        break;
    case AX_PARENT: {
        xnode p;
        if (parent_node(doc, x, &p) && test_matches(test, doc, &p, 0)) xpush(st, out, p);
        break;
    }
    case AX_ANCESTOR:
    case AX_ANCESTOR_OR_SELF: {
        xnode current = *x, p;
        if (s->axis == AX_ANCESTOR_OR_SELF && test_matches(test, doc, &current, 0)) xpush(st, out, current);
        while (parent_node(doc, &current, &p)) {
            current = p;
            if (test_matches(test, doc, &current, 0)) xpush(st, out, current);
        }
        break;
    }
    case AX_FOLLOWING_SIBLING:
    case AX_PRECEDING_SIBLING:
        collect_siblings(st, x, test, out, s->axis == AX_FOLLOWING_SIBLING);
        break;
    case AX_FOLLOWING:
        collect_following(st, x, test, out);
        break;
    case AX_PRECEDING:
        collect_preceding(st, x, test, out);
        break;
    default:
        add_attributes(st, x, test, out);
    }
}

/* Appends the nodes of the axis that pass the test and the predicates, in document order. */
static void select_from(const step *s, estate *st, const xnode *x, xset *out) {
    size_t start = out->len;
    collect(s, st, x, out);
    if (s->npred || is_reverse(s)) {
        xset tail = xempty(), selected;
        size_t i;
        for (i = start; i < out->len; i++) xpush(st, &tail, out->p[i]);
        out->len = start;
        selected = filter(st, tail, s->predicates, s->npred);
        if (is_reverse(s)) {
            for (i = selected.len; i-- > 0;) xpush(st, out, selected.p[i]);
        } else {
            for (i = 0; i < selected.len; i++) xpush(st, out, selected.p[i]);
        }
    }
}

/* The descendant axes (tags only) on input nodes in document order. A tag that lies in the part of the tag list
   already scanned for an earlier input node is skipped: that scan has tested it and every tag its own scan would
   test. */
static xset select_descendants_of_nested(const step *s, const xset *input, estate *st) {
    const fhp_doc *doc = st->doc;
    xset out = xempty();
    size_t scanned_to = 0, k;
    int scanned_any = 0;
    for (k = 0; k < input->len; k++) {
        const xnode *x = &input->p[k];
        if (x->kind == X_ELEMENT || x->kind == X_COMMENT) {
            size_t index = (size_t)x->tag;
            const fhp_nodedata *t = &doc->nodes[index];
            if (scanned_any && index < scanned_to) continue;
            select_from(s, st, x, &out);
            /* where add_descendants stops for this tag */
            if (x->kind == X_ELEMENT && t->inner_start > 0 && t->inner_end > 0) {
                size_t j = index + 1;
                while (j < doc->count && doc->nodes[j].end <= t->end) j++;
                scanned_to = scanned_any && scanned_to > j ? scanned_to : j;
                scanned_any = 1;
            }
        } else {
            select_from(s, st, x, &out);
            if (x->kind == X_ROOT) {
                scanned_to = (size_t)-1;
                scanned_any = 1;
            }
        }
    }
    return out;
}

/* A set of node keys (open addressing). */
typedef struct keyset {
    int64_t *slots; /* key + 2; 0: empty */
    size_t cap, len;
} keyset;

static int keyset_insert(estate *st, keyset *ks, int64_t key) {
    size_t h;
    int64_t k = key + 2;
    if ((ks->len + 1) * 2 > ks->cap) {
        size_t cap = ks->cap ? ks->cap * 2 : 64, i;
        int64_t *slots = (int64_t *)fhp_alloc(st->a, cap * sizeof(int64_t));
        if (!slots) return 1;
        memset(slots, 0, cap * sizeof(int64_t));
        for (i = 0; i < ks->cap; i++) {
            if (ks->slots[i]) {
                size_t j = (size_t)(((uint64_t)ks->slots[i] * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
                while (slots[j]) j = (j + 1) & (cap - 1);
                slots[j] = ks->slots[i];
            }
        }
        ks->slots = slots;
        ks->cap = cap;
    }
    h = (size_t)(((uint64_t)k * 0x9E3779B97F4A7C15ull) >> 32) & (ks->cap - 1);
    while (ks->slots[h]) {
        if (ks->slots[h] == k) return 0;
        h = (h + 1) & (ks->cap - 1);
    }
    ks->slots[h] = k;
    ks->len++;
    return 1;
}

/* The walk up from an input node stops at an ancestor already reached from an earlier one, because that walk has gone
   on to the root. Only the root and elements are marked: they are the only parents. */
static xset select_shared_ancestors(const step *s, const xset *input, estate *st) {
    const fhp_doc *doc = st->doc;
    keyset visited;
    xset out = xempty();
    size_t k;
    visited.slots = NULL;
    visited.cap = visited.len = 0;
    for (k = 0; k < input->len; k++) {
        size_t start = out.len;
        xnode current = input->p[k], p;
        if (s->axis == AX_ANCESTOR_OR_SELF) {
            if ((current.kind == X_ROOT || current.kind == X_ELEMENT) && !keyset_insert(st, &visited, xkey(doc, &current)))
                continue;
            if (test_matches(&s->test, doc, &current, 0)) xpush(st, &out, current);
        }
        while (parent_node(doc, &current, &p)) {
            if (!keyset_insert(st, &visited, xkey(doc, &p))) break;
            current = p;
            if (test_matches(&s->test, doc, &current, 0)) xpush(st, &out, current);
        }
        if (s->npred && out.len > start) {
            xset tail = xempty(), kept;
            size_t i;
            for (i = start; i < out.len; i++) xpush(st, &tail, out.p[i]);
            out.len = start;
            kept = filter(st, tail, s->predicates, s->npred);
            for (i = 0; i < kept.len; i++) xpush(st, &out, kept.p[i]);
        }
    }
    return out;
}

static xset step_apply(const step *s, xset input, estate *st) {
    xset result = xempty();
    size_t k;
    if (input.len == 1) {
        select_from(s, st, &input.p[0], &result);
        return result;
    }
    /* Nested input nodes (//div//a on divs inside divs, //a/ancestor::* on links in one list) give the same nodes again
       and again: n nested nodes would collect about n^2 nodes before the duplicates are removed. When the predicates
       do not depend on the position, a node passes them or not whichever input node it comes from, so the input nodes
       whose results are already collected are skipped: the work stays linear. */
    if (!has_positional_predicates(s)) {
        if (s->axis == AX_DESCENDANT || s->axis == AX_DESCENDANT_OR_SELF) {
            if (!matches_text(&s->test) && in_document_order(st->doc, &input))
                return sort_unique(st, select_descendants_of_nested(s, &input, st));
        } else if (s->axis == AX_ANCESTOR || s->axis == AX_ANCESTOR_OR_SELF) {
            return sort_unique(st, select_shared_ancestors(s, &input, st));
        }
    }
    for (k = 0; k < input.len; k++) select_from(s, st, &input.p[k], &result);
    return sort_unique(st, result);
}

/* ----- running an expression ----- */

int fhp_xpath_is_node_set(const fhp_xpath *q) { return q && expr_type(q->e) == Y_NODES; }

static value run(const fhp_xpath *q, const fhp_doc *doc, fhp_node context, estate *st) {
    ectx c;
    st->doc = doc;
    st->a = fhp_scratch();
    st->has_all = 0;
    c.st = st;
    c.position = 1;
    c.size = 1;
    c.node = context >= 0 ? of_tag(doc, (size_t)context) : root_node();
    return eval(q->e, &c);
}

static int bad_context(const fhp_doc *doc, fhp_node context) {
    return !doc || (context != FHP_NONE && !fhp_valid(doc, context));
}

int fhp_xpath_nodes(const fhp_xpath *q, const fhp_doc *doc, fhp_node context, fhp_arena *a, fhp_nodes *out) {
    estate st;
    fhp_mark m;
    value v;
    fhp_ivec res;
    size_t i;
    if (out) {
        out->items = NULL;
        out->len = 0;
    }
    if (!q || expr_type(q->e) != Y_NODES) return -1;
    if (bad_context(doc, context)) return 0;
    a = fhp_result_arena(doc, a);
    fhp_ivec_init(&res, a, 0);
    m = fhp_arena_mark(fhp_scratch());
    v = run(q, doc, context, &st);
    for (i = 0; i < v.nodes.len; i++)
        if (v.nodes.p[i].kind == X_ELEMENT || v.nodes.p[i].kind == X_COMMENT) fhp_ivec_push(&res, v.nodes.p[i].tag);
    fhp_arena_release(st.a, m);
    if (out) {
        out->items = res.p;
        out->len = res.len;
    }
    return 0;
}

static fhp_str copy_str(fhp_arena *a, fhp_str s) { return fhp_s(fhp_strndup(a, s.ptr, s.len), s.len); }

/* Strings that are slices of the page stay where they are; built ones are copied out of the scratch arena. */
static fhp_str keep(const fhp_doc *doc, fhp_arena *a, fhp_str s) {
    if (s.len == 0) return FHP_EMPTY;
    if (s.ptr >= doc->src && s.ptr + s.len <= doc->src + doc->len) return s;
    return copy_str(a, s);
}

fhp_strs fhp_xpath_values(const fhp_xpath *q, const fhp_doc *doc, fhp_node context, fhp_arena *a) {
    estate st;
    fhp_mark m;
    value v;
    fhp_strs out;
    fhp_str *items;
    size_t i, n;
    out.items = NULL;
    out.len = 0;
    if (!q || bad_context(doc, context)) return out;
    a = fhp_result_arena(doc, a);
    m = fhp_arena_mark(fhp_scratch());
    v = run(q, doc, context, &st);
    n = v.type == Y_NODES ? v.nodes.len : 1;
    items = (fhp_str *)fhp_alloc(a, (n ? n : 1) * sizeof(fhp_str));
    if (items) {
        if (v.type == Y_NODES) {
            for (i = 0; i < n; i++) {
                fhp_mark m2 = fhp_arena_mark(st.a);
                items[i] = keep(doc, a, string_value(&st, &v.nodes.p[i]));
                fhp_arena_release(st.a, m2);
            }
        } else {
            items[0] = keep(doc, a, to_string(&st, &v));
        }
        out.items = items;
        out.len = n;
    }
    fhp_arena_release(st.a, m);
    return out;
}

int fhp_xpath_value(const fhp_xpath *q, const fhp_doc *doc, fhp_node context, fhp_arena *a, fhp_str *out) {
    estate st;
    fhp_mark m;
    value v;
    int found = 1;
    fhp_str s = FHP_EMPTY;
    if (out) *out = FHP_EMPTY;
    if (!q || bad_context(doc, context)) return 0;
    a = fhp_result_arena(doc, a);
    m = fhp_arena_mark(fhp_scratch());
    v = run(q, doc, context, &st);
    if (v.type == Y_NODES) {
        if (v.nodes.len)
            s = keep(doc, a, string_value(&st, &v.nodes.p[0]));
        else
            found = 0;
    } else {
        s = keep(doc, a, to_string(&st, &v));
    }
    fhp_arena_release(st.a, m);
    if (out) *out = s;
    return found;
}

int fhp_xpath_select(const fhp_doc *doc, fhp_node context, const char *expression, fhp_arena *a, fhp_nodes *out) {
    fhp_xpath *q = fhp_xpath_compile(expression, NULL, 0);
    int r;
    if (out) {
        out->items = NULL;
        out->len = 0;
    }
    if (!q) return -1;
    r = fhp_xpath_nodes(q, doc, context, a, out);
    fhp_xpath_free(q);
    return r;
}

fhp_str fhp_xpath_string(const fhp_doc *doc, fhp_node context, const char *expression, fhp_arena *a) {
    fhp_xpath *q = fhp_xpath_compile(expression, NULL, 0);
    fhp_str s = FHP_EMPTY;
    if (!q) return s;
    fhp_xpath_value(q, doc, context, a, &s);
    fhp_xpath_free(q);
    return s;
}

/* ----- tokenizer ----- */

enum tok {
    TK_END, TK_SLASH, TK_DOUBLE_SLASH, TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET, TK_DOT, TK_DOTDOT, TK_AT,
    TK_COMMA, TK_COLONCOLON, TK_PIPE, TK_PLUS, TK_MINUS, TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_STAR,     /* name test * */
    TK_MULTIPLY, /* operator * */
    TK_AND, TK_OR, TK_DIV, TK_MOD, TK_NAME, TK_LITERAL, TK_NUMBER, TK_DOLLAR
};

typedef struct token {
    int kind;
    fhp_str text;
    double number;
    size_t position; /* byte offset */
} token;

typedef struct xcompiler {
    const char *s;
    size_t len;
    token *tokens;
    size_t ntokens, cap, index;
    fhp_arena *a;
    char *err;
    size_t err_size;
    int failed;
} xcompiler;

static void xfail_at(xcompiler *c, size_t position, const char *msg) {
    if (c->failed) return;
    c->failed = 1;
    if (c->err && c->err_size)
        snprintf(c->err, c->err_size, "invalid XPath \"%s\" at position %u: %s", c->s, (unsigned)position, msg);
}

static int is_name_start(uint32_t c) { return fhp_is_alphabetic(c) || c == '_'; }
static int is_name_char_x(uint32_t c) {
    return fhp_is_alphabetic(c) || fhp_is_numeric(c) || c == '_' || c == '-' || c == '.';
}

static uint32_t char_at_x(const xcompiler *c, size_t i, size_t *size) {
    if (i >= c->len) {
        *size = 0;
        return 0;
    }
    return fhp_utf8_decode(c->s + i, c->len - i, size);
}

static int push_token(xcompiler *c, token t) {
    if (c->ntokens == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        token *p = (token *)fhp_realloc(c->a, c->tokens, c->cap * sizeof(token), cap * sizeof(token));
        if (!p) {
            xfail_at(c, t.position, "out of memory");
            return 0;
        }
        c->tokens = p;
        c->cap = cap;
    }
    c->tokens[c->ntokens++] = t;
    return 1;
}

static int tokenize(xcompiler *c) {
    size_t i = 0;
    const char *s = c->s;
    for (;;) {
        size_t size, start, nsize;
        uint32_t ch, next;
        token tok;
        int operator_expected = 0;
        while (i < c->len) {
            uint32_t w = char_at_x(c, i, &size);
            if (!fhp_is_white_space(w)) break;
            i += size;
        }
        memset(&tok, 0, sizeof tok);
        tok.text = FHP_EMPTY;
        tok.position = i;
        if (i >= c->len) {
            tok.kind = TK_END;
            return push_token(c, tok);
        }
        start = i;
        ch = char_at_x(c, i, &size);
        next = char_at_x(c, i + size, &nsize);
        /* XPath 1.0 lexical rule: after a token that can end an operand, '*' and and/or/div/mod are operators */
        if (c->ntokens) {
            int k = c->tokens[c->ntokens - 1].kind;
            operator_expected = k == TK_RPAREN || k == TK_RBRACKET || k == TK_DOT || k == TK_DOTDOT || k == TK_STAR ||
                                k == TK_NAME || k == TK_LITERAL || k == TK_NUMBER;
        }
#define ONE(kind_, n)    \
    do {                 \
        tok.kind = kind_; \
        i += n;          \
    } while (0)
        switch (ch) {
        case '/':
            if (next == '/') ONE(TK_DOUBLE_SLASH, 2); else ONE(TK_SLASH, 1);
            break;
        case '(': ONE(TK_LPAREN, 1); break;
        case ')': ONE(TK_RPAREN, 1); break;
        case '[': ONE(TK_LBRACKET, 1); break;
        case ']': ONE(TK_RBRACKET, 1); break;
        case '@': ONE(TK_AT, 1); break;
        case ',': ONE(TK_COMMA, 1); break;
        case '|': ONE(TK_PIPE, 1); break;
        case '+': ONE(TK_PLUS, 1); break;
        case '-': ONE(TK_MINUS, 1); break;
        case '=': ONE(TK_EQ, 1); break;
        case '$': ONE(TK_DOLLAR, 1); break;
        case '*': ONE(operator_expected ? TK_MULTIPLY : TK_STAR, 1); break;
        case '!':
            if (next != '=') {
                xfail_at(c, i, "'!' must be followed by '='");
                return 0;
            }
            ONE(TK_NEQ, 2);
            break;
        case '<':
            if (next == '=') ONE(TK_LE, 2); else ONE(TK_LT, 1);
            break;
        case '>':
            if (next == '=') ONE(TK_GE, 2); else ONE(TK_GT, 1);
            break;
        case ':':
            if (next != ':') {
                xfail_at(c, i, "unexpected ':'");
                return 0;
            }
            ONE(TK_COLONCOLON, 2);
            break;
        case '"':
        case '\'': {
            const char *end = (const char *)fhp_memchr(s + i + 1, (int)ch, c->len - i - 1);
            if (!end) {
                xfail_at(c, i, "unterminated string literal");
                return 0;
            }
            tok.kind = TK_LITERAL;
            tok.text = fhp_s(s + i + 1, (size_t)(end - s) - i - 1);
            i = (size_t)(end - s) + 1;
            break;
        }
        default:
            if (ch == '.' && next == '.') {
                ONE(TK_DOTDOT, 2);
            } else if (ch == '.' && !(next < 0x80 && fhp_is_digit((unsigned char)next))) {
                ONE(TK_DOT, 1);
            } else if ((ch < 0x80 && fhp_is_digit((unsigned char)ch)) || ch == '.') {
                char buf[64];
                size_t n;
                while (i < c->len && fhp_is_digit((unsigned char)s[i])) i++;
                if (i < c->len && s[i] == '.') {
                    i++;
                    while (i < c->len && fhp_is_digit((unsigned char)s[i])) i++;
                }
                tok.kind = TK_NUMBER;
                n = i - start < sizeof buf - 1 ? i - start : sizeof buf - 1;
                memcpy(buf, s + start, n);
                buf[n] = 0;
                if (i - start < sizeof buf - 1) {
                    tok.number = strtod(buf, NULL);
                } else {
                    char *big = fhp_strndup(c->a, s + start, i - start);
                    tok.number = big ? strtod(big, NULL) : 0;
                }
            } else if (is_name_start(ch)) {
                i += size;
                while (i < c->len) {
                    size_t cs, ns;
                    uint32_t cc = char_at_x(c, i, &cs);
                    if (is_name_char_x(cc)) {
                        i += cs;
                    } else if (cc == ':') {
                        uint32_t nc = char_at_x(c, i + 1, &ns);
                        if (i + 1 < c->len && nc != ':' && (is_name_start(nc) || nc == '*')) {
                            /* a prefixed name (dc:creator) or prefix:* */
                            i += 1 + ns;
                            if (nc == '*') break;
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
                tok.kind = TK_NAME;
                tok.text = fhp_s(s + start, i - start);
                if (operator_expected) {
                    if (fhp_str_eq(tok.text, fhp_s("and", 3)))
                        tok.kind = TK_AND;
                    else if (fhp_str_eq(tok.text, fhp_s("or", 2)))
                        tok.kind = TK_OR;
                    else if (fhp_str_eq(tok.text, fhp_s("div", 3)))
                        tok.kind = TK_DIV;
                    else if (fhp_str_eq(tok.text, fhp_s("mod", 3)))
                        tok.kind = TK_MOD;
                    else {
                        char msg[128];
                        snprintf(msg, sizeof msg, "expected an operator, found '%.*s'", (int)(tok.text.len > 64 ? 64 : tok.text.len), tok.text.ptr);
                        xfail_at(c, start, msg);
                        return 0;
                    }
                }
            } else {
                char msg[64], ch8[5];
                size_t n = fhp_utf8_encode(ch, ch8);
                ch8[n] = 0;
                snprintf(msg, sizeof msg, "unexpected character '%s'", ch8);
                xfail_at(c, i, msg);
                return 0;
            }
        }
#undef ONE
        if (!push_token(c, tok)) return 0;
    }
}

/* ----- parser ----- */

static const token *xpeek(const xcompiler *c, size_t offset) {
    size_t k = c->index + offset;
    return &c->tokens[k < c->ntokens ? k : c->ntokens - 1];
}

static token advance(xcompiler *c) {
    token t = *xpeek(c, 0);
    if (c->index < c->ntokens) c->index++;
    return t;
}

static int accept(xcompiler *c, int kind) {
    if (xpeek(c, 0)->kind != kind) return 0;
    c->index++;
    return 1;
}

static void describe(const xcompiler *c, const token *t, char *buf, size_t size) {
    switch (t->kind) {
    case TK_END:
        snprintf(buf, size, "the end of the expression");
        break;
    case TK_NAME:
    case TK_LITERAL:
        snprintf(buf, size, "'%.*s'", (int)(t->text.len > 64 ? 64 : t->text.len), t->text.ptr);
        break;
    case TK_NUMBER:
        snprintf(buf, size, "%g", t->number);
        break;
    default: {
        size_t n = c->len - t->position < 2 ? c->len - t->position : 2;
        while (n > 0 && fhp_is_space((unsigned char)c->s[t->position + n - 1])) n--;
        snprintf(buf, size, "'%.*s'", (int)n, c->s + t->position);
    }
    }
}

static void xfail(xcompiler *c, const char *msg) { xfail_at(c, xpeek(c, 0)->position, msg); }

static void xfail_unexpected(xcompiler *c, const char *prefix) {
    char d[96], msg[160];
    describe(c, xpeek(c, 0), d, sizeof d);
    snprintf(msg, sizeof msg, "%s%s", prefix, d);
    xfail(c, msg);
}

static int expect_tok(xcompiler *c, int kind, const char *what) {
    if (!accept(c, kind)) {
        char p[64];
        snprintf(p, sizeof p, "expected %s, found ", what);
        xfail_unexpected(c, p);
        return 0;
    }
    return 1;
}

static expr *new_expr(xcompiler *c, int kind) {
    expr *e = (expr *)fhp_alloc(c->a, sizeof(expr));
    if (!e) {
        xfail(c, "out of memory");
        return NULL;
    }
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->str = e->value = FHP_EMPTY;
    return e;
}

static expr *binary(xcompiler *c, int kind, int op, expr *left, expr *right) {
    expr *e;
    if (!left || !right) return NULL;
    e = new_expr(c, kind);
    if (!e) return NULL;
    e->op = op;
    e->left = left;
    e->right = right;
    return e;
}

/* Appends p to a growable array of expression pointers. */
static int push_expr(xcompiler *c, expr ***list, size_t *n, size_t *cap, expr *p) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 4;
        expr **l = (expr **)fhp_realloc(c->a, *list, *cap * sizeof(expr *), nc * sizeof(expr *));
        if (!l) {
            xfail(c, "out of memory");
            return 0;
        }
        *list = l;
        *cap = nc;
    }
    (*list)[(*n)++] = p;
    return 1;
}

static expr *parse_expr(xcompiler *c);

static const char *const node_types[] = {"node", "text", "comment", "processing-instruction"};

static int is_node_type(fhp_str s) {
    size_t i;
    for (i = 0; i < 4; i++)
        if (fhp_str_eq(s, fhp_cs(node_types[i]))) return 1;
    return 0;
}

/* Whether the expression is a bare @name (one attribute step, no predicates); the name. */
static int attribute_name(const expr *e, fhp_str *name) {
    const step *s;
    if (!e || e->kind != E_PATH || e->left || e->absolute || e->nsteps != 1) return 0;
    s = &e->steps[0];
    if (s->axis != AX_ATTRIBUTE || s->npred || s->test.kind != T_NAME) return 0;
    *name = s->test.name;
    return 1;
}

static int parse_predicates(xcompiler *c, expr ***list, size_t *n) {
    size_t cap = 0;
    *list = NULL;
    *n = 0;
    while (accept(c, TK_LBRACKET)) {
        expr *p = parse_expr(c);
        fhp_str name;
        if (!p) return 0;
        if (!expect_tok(c, TK_RBRACKET, "']'")) return 0;
        /* [@name]: an existence check instead of a node-set */
        if (attribute_name(p, &name)) {
            expr *x = new_expr(c, E_ATTR_EXISTS);
            if (!x) return 0;
            x->str = name;
            p = x;
        }
        if (!push_expr(c, list, n, &cap, p)) return 0;
    }
    return 1;
}

static int can_start_step(const token *t) {
    return t->kind == TK_DOT || t->kind == TK_DOTDOT || t->kind == TK_AT || t->kind == TK_STAR || t->kind == TK_NAME;
}

static int parse_node_test(xcompiler *c, node_test *out) {
    token t;
    fhp_str name;
    if (accept(c, TK_STAR)) {
        *out = make_test(T_WILDCARD, FHP_EMPTY);
        return 1;
    }
    t = *xpeek(c, 0);
    if (t.kind != TK_NAME) {
        xfail_unexpected(c, "expected a node test, found ");
        return 0;
    }
    advance(c);
    name = t.text;
    if (xpeek(c, 0)->kind == TK_LPAREN && is_node_type(name)) {
        advance(c);
        if (fhp_str_eq(name, fhp_s("processing-instruction", 22))) accept(c, TK_LITERAL);
        if (!expect_tok(c, TK_RPAREN, "')'")) return 0;
        if (fhp_str_eq(name, fhp_s("node", 4)))
            *out = make_test(T_NODE, FHP_EMPTY);
        else if (fhp_str_eq(name, fhp_s("text", 4)))
            *out = make_test(T_TEXT, FHP_EMPTY);
        else if (fhp_str_eq(name, fhp_s("comment", 7)))
            *out = make_test(T_COMMENT, FHP_EMPTY);
        else
            *out = make_test(T_PI, FHP_EMPTY);
        return 1;
    }
    if (name.len >= 2 && name.ptr[name.len - 1] == '*' && name.ptr[name.len - 2] == ':') {
        *out = make_test(T_PREFIX, fhp_s(name.ptr, name.len - 1));
        return 1;
    }
    *out = make_test(T_NAME, name);
    return 1;
}

static int parse_step(xcompiler *c, step *s) {
    memset(s, 0, sizeof *s);
    if (accept(c, TK_DOT)) {
        s->axis = AX_SELF;
        s->test = make_test(T_NODE, FHP_EMPTY);
        return 1;
    }
    if (accept(c, TK_DOTDOT)) {
        s->axis = AX_PARENT;
        s->test = make_test(T_NODE, FHP_EMPTY);
        return 1;
    }
    s->axis = AX_CHILD;
    if (accept(c, TK_AT)) {
        s->axis = AX_ATTRIBUTE;
    } else if (xpeek(c, 0)->kind == TK_NAME && xpeek(c, 1)->kind == TK_COLONCOLON) {
        static const char *const axes[] = {"child", "descendant", "descendant-or-self", "self", "parent", "ancestor",
                                           "ancestor-or-self", "following-sibling", "preceding-sibling", "following",
                                           "preceding", "attribute"};
        token name = advance(c);
        size_t k;
        int found = -1;
        for (k = 0; k < sizeof axes / sizeof axes[0]; k++)
            if (fhp_str_eq(name.text, fhp_cs(axes[k]))) found = (int)k;
        if (found < 0) {
            char msg[128];
            if (fhp_str_eq(name.text, fhp_s("namespace", 9))) {
                xfail_at(c, name.position, "the namespace axis is not supported");
            } else {
                snprintf(msg, sizeof msg, "unknown axis '%.*s'", (int)(name.text.len > 64 ? 64 : name.text.len), name.text.ptr);
                xfail_at(c, name.position, msg);
            }
            return 0;
        }
        s->axis = found;
        advance(c);
    }
    if (!parse_node_test(c, &s->test)) return 0;
    return parse_predicates(c, &s->predicates, &s->npred);
}

typedef struct steps {
    step *p;
    size_t n, cap;
} steps;

static step *add_step(xcompiler *c, steps *v) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 4;
        step *p = (step *)fhp_realloc(c->a, v->p, v->cap * sizeof(step), cap * sizeof(step));
        if (!p) {
            xfail(c, "out of memory");
            return NULL;
        }
        v->p = p;
        v->cap = cap;
    }
    return &v->p[v->n++];
}

/* Steps separated by '/' or '//'; after_slash: the next token is a '/' or '//' to read first. */
static int parse_relative_steps(xcompiler *c, steps *v, int after_slash) {
    step *s;
    if (!after_slash) {
        s = add_step(c, v);
        if (!s || !parse_step(c, s)) return 0;
    }
    for (;;) {
        if (accept(c, TK_SLASH)) {
            s = add_step(c, v);
            if (!s || !parse_step(c, s)) return 0;
        } else if (accept(c, TK_DOUBLE_SLASH)) {
            s = add_step(c, v);
            if (!s) return 0;
            memset(s, 0, sizeof *s);
            s->axis = AX_DESCENDANT_OR_SELF;
            s->test = make_test(T_NODE, FHP_EMPTY);
            s = add_step(c, v);
            if (!s || !parse_step(c, s)) return 0;
        } else {
            return 1;
        }
    }
}

/* //x is descendant-or-self::node()/child::x. Without positional predicates on x it is the same as descendant::x: one
   linear scan of the tag list instead of the children of every node. */
static void optimize(steps *v) {
    size_t i = 0;
    while (i + 1 < v->n) {
        step *s = &v->p[i], *next = &v->p[i + 1];
        if (s->axis != AX_DESCENDANT_OR_SELF || s->test.kind != T_NODE || s->npred) {
            i++;
            continue;
        }
        if (next->axis == AX_CHILD && !has_positional_predicates(next)) {
            step merged = *next;
            merged.axis = AX_DESCENDANT;
            *s = merged;
            memmove(&v->p[i + 1], &v->p[i + 2], (v->n - i - 2) * sizeof(step));
            v->n--;
        } else if (next->axis == AX_CHILD || next->axis == AX_ATTRIBUTE) {
            /* only the root and elements have children or attributes: skip the text nodes */
            s->axis = AX_DESCENDANT_OR_SELF;
            s->test = make_test(T_CONTAINER, FHP_EMPTY);
            s->npred = 0;
        }
        i++;
    }
}

static expr *make_path(xcompiler *c, expr *start, int absolute, steps *v) {
    expr *e = new_expr(c, E_PATH);
    if (!e) return NULL;
    optimize(v);
    e->left = start;
    e->absolute = absolute;
    e->steps = v->p;
    e->nsteps = v->n;
    return e;
}

static expr *parse_location_path(xcompiler *c) {
    steps v;
    int absolute = 0;
    memset(&v, 0, sizeof v);
    if (accept(c, TK_SLASH)) {
        absolute = 1;
        if (can_start_step(xpeek(c, 0)) && !parse_relative_steps(c, &v, 0)) return NULL;
    } else if (xpeek(c, 0)->kind == TK_DOUBLE_SLASH) {
        absolute = 1;
        if (!parse_relative_steps(c, &v, 1)) return NULL;
    } else {
        if (!can_start_step(xpeek(c, 0))) {
            xfail_unexpected(c, "unexpected ");
            return NULL;
        }
        if (!parse_relative_steps(c, &v, 0)) return NULL;
    }
    return make_path(c, NULL, absolute, &v);
}

/* name -> function, min and max args, result type */
static const struct {
    const char *name;
    int func, min, max, type;
} functions[] = {
    {"last", F_LAST, 0, 0, Y_NUMBER},
    {"position", F_POSITION, 0, 0, Y_NUMBER},
    {"count", F_COUNT, 1, 1, Y_NUMBER},
    {"name", F_NAME, 0, 1, Y_STRING},
    {"local-name", F_LOCAL_NAME, 0, 1, Y_STRING},
    {"string", F_STRING, 0, 1, Y_STRING},
    {"concat", F_CONCAT, 2, 1 << 30, Y_STRING},
    {"starts-with", F_STARTS_WITH, 2, 2, Y_BOOLEAN},
    {"ends-with", F_ENDS_WITH, 2, 2, Y_BOOLEAN},
    {"contains", F_CONTAINS, 2, 2, Y_BOOLEAN},
    {"substring-before", F_SUBSTRING_BEFORE, 2, 2, Y_STRING},
    {"substring-after", F_SUBSTRING_AFTER, 2, 2, Y_STRING},
    {"substring", F_SUBSTRING, 2, 3, Y_STRING},
    {"string-length", F_STRING_LENGTH, 0, 1, Y_NUMBER},
    {"normalize-space", F_NORMALIZE_SPACE, 0, 1, Y_STRING},
    {"translate", F_TRANSLATE, 3, 3, Y_STRING},
    {"lower-case", F_LOWER_CASE, 1, 1, Y_STRING},
    {"upper-case", F_UPPER_CASE, 1, 1, Y_STRING},
    {"not", F_NOT, 1, 1, Y_BOOLEAN},
    {"true", F_TRUE, 0, 0, Y_BOOLEAN},
    {"false", F_FALSE, 0, 0, Y_BOOLEAN},
    {"boolean", F_BOOLEAN, 1, 1, Y_BOOLEAN},
    {"number", F_NUMBER, 0, 1, Y_NUMBER},
    {"sum", F_SUM, 1, 1, Y_NUMBER},
    {"floor", F_FLOOR, 1, 1, Y_NUMBER},
    {"ceiling", F_CEILING, 1, 1, Y_NUMBER},
    {"round", F_ROUND, 1, 1, Y_NUMBER},
};

static expr *parse_primary(xcompiler *c) {
    token t = advance(c);
    size_t k, cap = 0;
    int found = -1;
    expr *e;
    char msg[160];
    switch (t.kind) {
    case TK_LITERAL:
        e = new_expr(c, E_LITERAL);
        if (e) e->str = t.text;
        return e;
    case TK_NUMBER:
        e = new_expr(c, E_NUMBER);
        if (e) e->num = t.number;
        return e;
    case TK_LPAREN:
        e = parse_expr(c);
        if (!e || !expect_tok(c, TK_RPAREN, "')'")) return NULL;
        return e;
    case TK_DOLLAR:
        xfail_at(c, t.position, "variables are not supported");
        return NULL;
    default:
        break;
    }
    /* function call */
    for (k = 0; k < sizeof functions / sizeof functions[0]; k++)
        if (fhp_str_eq(t.text, fhp_cs(functions[k].name))) found = (int)k;
    if (found < 0) {
        snprintf(msg, sizeof msg, "unknown function '%.*s()'", (int)(t.text.len > 64 ? 64 : t.text.len), t.text.ptr);
        xfail_at(c, t.position, msg);
        return NULL;
    }
    if (!expect_tok(c, TK_LPAREN, "'('")) return NULL;
    e = new_expr(c, E_FUNCTION);
    if (!e) return NULL;
    e->op = functions[found].func;
    e->type = functions[found].type;
    if (!accept(c, TK_RPAREN)) {
        for (;;) {
            expr *arg = parse_expr(c);
            if (!arg || !push_expr(c, &e->list, &e->nlist, &cap, arg)) return NULL;
            if (!accept(c, TK_COMMA)) break;
        }
        if (!expect_tok(c, TK_RPAREN, "')'")) return NULL;
    }
    if ((int)e->nlist < functions[found].min || (int)e->nlist > functions[found].max) {
        snprintf(msg, sizeof msg, "wrong number of arguments for %s()", functions[found].name);
        xfail_at(c, t.position, msg);
        return NULL;
    }
    if ((e->op == F_COUNT || e->op == F_SUM || e->op == F_NAME || e->op == F_LOCAL_NAME) && e->nlist &&
        expr_type(e->list[0]) != Y_NODES) {
        snprintf(msg, sizeof msg, "%s() needs a node-set argument", functions[found].name);
        xfail_at(c, t.position, msg);
        return NULL;
    }
    return e;
}

static expr *parse_path(xcompiler *c) {
    token t = *xpeek(c, 0);
    int is_filter = t.kind == TK_LITERAL || t.kind == TK_NUMBER || t.kind == TK_LPAREN || t.kind == TK_DOLLAR ||
                    (t.kind == TK_NAME && xpeek(c, 1)->kind == TK_LPAREN && !is_node_type(t.text));
    size_t position = t.position;
    expr *primary;
    expr **preds;
    size_t npred;
    int k;
    steps v;
    if (!is_filter) return parse_location_path(c);
    primary = parse_primary(c);
    if (!primary || !parse_predicates(c, &preds, &npred)) return NULL;
    if (npred) {
        expr *f;
        if (expr_type(primary) != Y_NODES) {
            xfail_at(c, position, "a predicate needs a node-set");
            return NULL;
        }
        f = new_expr(c, E_FILTER);
        if (!f) return NULL;
        f->left = primary;
        f->list = preds;
        f->nlist = npred;
        primary = f;
    }
    k = xpeek(c, 0)->kind;
    if (k != TK_SLASH && k != TK_DOUBLE_SLASH) return primary;
    if (expr_type(primary) != Y_NODES) {
        xfail_at(c, position, "'/' needs a node-set on the left");
        return NULL;
    }
    memset(&v, 0, sizeof v);
    if (!parse_relative_steps(c, &v, 1)) return NULL;
    return make_path(c, primary, 0, &v);
}

static expr *parse_union(xcompiler *c) {
    expr *left = parse_path(c);
    while (left && xpeek(c, 0)->kind == TK_PIPE) {
        size_t position = advance(c).position;
        expr *right = parse_path(c);
        if (!right) return NULL;
        if (expr_type(left) != Y_NODES || expr_type(right) != Y_NODES) {
            xfail_at(c, position, "'|' needs node-sets on both sides");
            return NULL;
        }
        left = binary(c, E_UNION, 0, left, right);
    }
    return left;
}

static expr *parse_unary(xcompiler *c) {
    if (accept(c, TK_MINUS)) {
        expr *inner = parse_unary(c), *e;
        if (!inner) return NULL;
        e = new_expr(c, E_NEGATE);
        if (e) e->left = inner;
        return e;
    }
    return parse_union(c);
}

static expr *parse_multiplicative(xcompiler *c) {
    expr *left = parse_unary(c);
    while (left) {
        int op;
        if (accept(c, TK_MULTIPLY))
            op = OP_MUL;
        else if (accept(c, TK_DIV))
            op = OP_DIV;
        else if (accept(c, TK_MOD))
            op = OP_MOD;
        else
            break;
        left = binary(c, E_ARITH, op, left, parse_unary(c));
    }
    return left;
}

static expr *parse_additive(xcompiler *c) {
    expr *left = parse_multiplicative(c);
    while (left) {
        int op;
        if (accept(c, TK_PLUS))
            op = OP_ADD;
        else if (accept(c, TK_MINUS))
            op = OP_SUB;
        else
            break;
        left = binary(c, E_ARITH, op, left, parse_multiplicative(c));
    }
    return left;
}

static expr *parse_relational(xcompiler *c) {
    expr *left = parse_additive(c);
    while (left) {
        int op;
        if (accept(c, TK_LT))
            op = OP_LT;
        else if (accept(c, TK_LE))
            op = OP_LE;
        else if (accept(c, TK_GT))
            op = OP_GT;
        else if (accept(c, TK_GE))
            op = OP_GE;
        else
            break;
        left = binary(c, E_COMPARE, op, left, parse_additive(c));
    }
    return left;
}

/* @name = 'literal' is compared in place; everything else by the general rules. */
static expr *make_equality(xcompiler *c, int op, expr *left, expr *right) {
    fhp_str name;
    if (!left || !right) return NULL;
    if ((attribute_name(left, &name) && right->kind == E_LITERAL) ||
        (attribute_name(right, &name) && left->kind == E_LITERAL)) {
        expr *e = new_expr(c, E_ATTR_EQUALS);
        if (!e) return NULL;
        e->str = name;
        e->value = right->kind == E_LITERAL && attribute_name(left, &name) ? right->str : left->str;
        e->op = op == OP_NEQ;
        return e;
    }
    return binary(c, E_COMPARE, op, left, right);
}

static expr *parse_equality(xcompiler *c) {
    expr *left = parse_relational(c);
    while (left) {
        int op;
        if (accept(c, TK_EQ))
            op = OP_EQ;
        else if (accept(c, TK_NEQ))
            op = OP_NEQ;
        else
            break;
        left = make_equality(c, op, left, parse_relational(c));
    }
    return left;
}

static expr *parse_and(xcompiler *c) {
    expr *left = parse_equality(c);
    while (left && accept(c, TK_AND)) left = binary(c, E_LOGIC, 1, left, parse_equality(c));
    return left;
}

static expr *parse_expr(xcompiler *c) {
    expr *left = parse_and(c);
    while (left && accept(c, TK_OR)) left = binary(c, E_LOGIC, 0, left, parse_and(c));
    return left;
}

fhp_xpath *fhp_xpath_compile(const char *expression, char *err, size_t err_size) {
    xcompiler c;
    fhp_arena mem;
    fhp_xpath *q;
    char *copy;
    size_t len;
    if (err && err_size) err[0] = 0;
    if (!expression) expression = "";
    len = strlen(expression);
    fhp_arena_init(&mem, 2048);
    q = (fhp_xpath *)fhp_alloc(&mem, sizeof(fhp_xpath));
    copy = fhp_strndup(&mem, expression, len); /* literals and names point into it */
    if (!q || !copy) {
        fhp_arena_destroy(&mem);
        return NULL;
    }
    memset(&c, 0, sizeof c);
    c.s = copy;
    c.len = len;
    c.a = &mem;
    c.err = err;
    c.err_size = err_size;
    q->e = NULL;
    if (tokenize(&c)) {
        q->e = parse_expr(&c);
        if (q->e && xpeek(&c, 0)->kind != TK_END) xfail_unexpected(&c, "unexpected ");
    }
    if (c.failed || !q->e) {
        if (!c.failed) xfail_at(&c, 0, "invalid expression");
        fhp_arena_destroy(&mem);
        return NULL;
    }
    q->mem = mem;
    return q;
}

void fhp_xpath_free(fhp_xpath *q) {
    fhp_arena mem;
    if (!q) return;
    mem = q->mem;
    fhp_arena_destroy(&mem);
}

/* Benchmarks of fasthtmlparser against lexbor (HTML5 parser with CSS selectors) and Gumbo (HTML5 parser, no selectors:
 * the tree is walked by hand) on the same pages as the .NET, Go and Rust benchmarks. Each library must extract the
 * same records first.
 *
 *     cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build bench/build
 *     bench/build/fhp_bench [--check]
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime, dirent */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fasthtmlparser.h"

#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>

#include "gumbo.h"

#ifdef _WIN32
#include <windows.h>
static double now(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
#endif

static volatile size_t sink; /* keeps the work from being optimized away */

static char *read_page(const char *name, size_t *len) {
    char path[1024];
    FILE *f;
    long n;
    char *buf;
    snprintf(path, sizeof path, "%s/%s", FHP_TESTDATA, name);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) exit(1);
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/* ----- records ----- */

typedef struct book {
    char title[256], price[32], rating[32];
} book;

typedef struct quote {
    char text[1024], author[128], tags[512];
} quote;

typedef struct books {
    book items[64];
    int n;
} books;

typedef struct quotes {
    quote items[32];
    int n;
} quotes;

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* Copies s (trimmed of ASCII whitespace) into a zero-terminated field. */
static void set(char *dst, size_t size, const char *s, size_t len) {
    while (len && is_ws(*s)) s++, len--;
    while (len && is_ws(s[len - 1])) len--;
    if (len >= size) len = size - 1;
    memcpy(dst, s, len);
    dst[len] = 0;
}

static void set_last_word(char *dst, size_t size, const char *s, size_t len) {
    size_t i = len;
    while (i > 0 && s[i - 1] != ' ') i--;
    set(dst, size, s + i, len - i);
}

static void append(char *dst, size_t size, const char *s, size_t len) {
    size_t n = strlen(dst);
    if (n && n + 1 < size) dst[n++] = ',', dst[n] = 0;
    if (n + len >= size) len = size - n - 1;
    memcpy(dst + n, s, len);
    dst[n + len] = 0;
}

/* ----- fasthtmlparser ----- */

static fhp_arena *arena;

static void books_fast(const char *page, size_t len, books *out) {
    fhp_doc *doc = fhp_parse(page, len);
    fhp_query pods = {FHP_TAG_ARTICLE}, h3 = {FHP_TAG_H3}, a = {FHP_TAG_A}, price = {FHP_TAG_P}, rating = {FHP_TAG_P};
    fhp_nodes list;
    size_t i;
    fhp_arena_reset(arena);
    pods.cls = "product_pod";
    price.cls = "price_color";
    rating.cls = "star-rating";
    list = fhp_find_all(doc, FHP_NONE, &pods, arena);
    out->n = 0;
    for (i = 0; i < list.len; i++) {
        book *b = &out->items[out->n++];
        fhp_node pod = list.items[i], t = fhp_find(doc, fhp_find(doc, pod, &h3), &a);
        fhp_str s = fhp_attr(doc, t, "title", arena);
        set(b->title, sizeof b->title, s.ptr, s.len);
        s = fhp_text(doc, fhp_find(doc, pod, &price), arena);
        set(b->price, sizeof b->price, s.ptr, s.len);
        s = fhp_attr(doc, fhp_find(doc, pod, &rating), "class", arena);
        set_last_word(b->rating, sizeof b->rating, s.ptr, s.len);
    }
    fhp_free(doc);
}

typedef struct fast_queries {
    fhp_css *pods, *title, *price, *rating, *quote, *text, *author, *tag;
    fhp_xpath *pods_x, *title_x, *price_x, *rating_x;
} fast_queries;

static fast_queries fq;

static void books_fast_css(const char *page, size_t len, books *out) {
    fhp_doc *doc = fhp_parse(page, len);
    fhp_nodes list;
    size_t i;
    fhp_arena_reset(arena);
    list = fhp_css_all(fq.pods, doc, FHP_NONE, arena);
    out->n = 0;
    for (i = 0; i < list.len; i++) {
        book *b = &out->items[out->n++];
        fhp_node pod = list.items[i];
        fhp_str s = fhp_attr(doc, fhp_css_first(fq.title, doc, pod), "title", arena);
        set(b->title, sizeof b->title, s.ptr, s.len);
        s = fhp_text(doc, fhp_css_first(fq.price, doc, pod), arena);
        set(b->price, sizeof b->price, s.ptr, s.len);
        s = fhp_attr(doc, fhp_css_first(fq.rating, doc, pod), "class", arena);
        set_last_word(b->rating, sizeof b->rating, s.ptr, s.len);
    }
    fhp_free(doc);
}

static void books_fast_xpath(const char *page, size_t len, books *out) {
    fhp_doc *doc = fhp_parse(page, len);
    fhp_nodes list;
    size_t i;
    fhp_arena_reset(arena);
    fhp_xpath_nodes(fq.pods_x, doc, FHP_NONE, arena, &list);
    out->n = 0;
    for (i = 0; i < list.len; i++) {
        book *b = &out->items[out->n++];
        fhp_node pod = list.items[i];
        fhp_str s;
        fhp_xpath_value(fq.title_x, doc, pod, arena, &s);
        set(b->title, sizeof b->title, s.ptr, s.len);
        fhp_xpath_value(fq.price_x, doc, pod, arena, &s);
        set(b->price, sizeof b->price, s.ptr, s.len);
        fhp_xpath_value(fq.rating_x, doc, pod, arena, &s);
        set_last_word(b->rating, sizeof b->rating, s.ptr, s.len);
    }
    fhp_free(doc);
}

static void quotes_fast(const char *page, size_t len, quotes *out) {
    fhp_doc *doc = fhp_parse(page, len);
    fhp_query qd = {FHP_TAG_DIV}, qt = {FHP_TAG_SPAN}, qa = {FHP_TAG_SMALL}, qtag = {FHP_TAG_A};
    fhp_nodes list;
    size_t i, k;
    fhp_arena_reset(arena);
    qd.cls = "quote";
    qt.cls = "text";
    qa.cls = "author";
    qtag.cls = "tag";
    list = fhp_find_all(doc, FHP_NONE, &qd, arena);
    out->n = 0;
    for (i = 0; i < list.len; i++) {
        quote *q = &out->items[out->n++];
        fhp_node n = list.items[i];
        fhp_str s = fhp_text(doc, fhp_find(doc, n, &qt), arena);
        fhp_nodes tags;
        set(q->text, sizeof q->text, s.ptr, s.len);
        s = fhp_text(doc, fhp_find(doc, n, &qa), arena);
        set(q->author, sizeof q->author, s.ptr, s.len);
        q->tags[0] = 0;
        tags = fhp_find_all(doc, n, &qtag, arena);
        for (k = 0; k < tags.len; k++) {
            s = fhp_text(doc, tags.items[k], arena);
            append(q->tags, sizeof q->tags, s.ptr, s.len);
        }
    }
    fhp_free(doc);
}

static void quotes_fast_css(const char *page, size_t len, quotes *out) {
    fhp_doc *doc = fhp_parse(page, len);
    fhp_nodes list;
    size_t i, k;
    fhp_arena_reset(arena);
    list = fhp_css_all(fq.quote, doc, FHP_NONE, arena);
    out->n = 0;
    for (i = 0; i < list.len; i++) {
        quote *q = &out->items[out->n++];
        fhp_node n = list.items[i];
        fhp_str s = fhp_text(doc, fhp_css_first(fq.text, doc, n), arena);
        fhp_nodes tags;
        set(q->text, sizeof q->text, s.ptr, s.len);
        s = fhp_text(doc, fhp_css_first(fq.author, doc, n), arena);
        set(q->author, sizeof q->author, s.ptr, s.len);
        q->tags[0] = 0;
        tags = fhp_css_all(fq.tag, doc, n, arena);
        for (k = 0; k < tags.len; k++) {
            s = fhp_text(doc, tags.items[k], arena);
            append(q->tags, sizeof q->tags, s.ptr, s.len);
        }
    }
    fhp_free(doc);
}

/* ----- lexbor (CSS selectors) ----- */

typedef struct lexbor_queries {
    lxb_css_parser_t *parser;
    lxb_css_memory_t *memory;
    lxb_css_selectors_t *css_selectors;
    lxb_selectors_t *selectors;
    lxb_css_selector_list_t *pods, *title, *price, *rating, *quote, *text, *author, *tag;
} lexbor_queries;

static lexbor_queries lq;

static lxb_css_selector_list_t *lexbor_selector(const char *s) {
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(lq.parser, (const lxb_char_t *)s, strlen(s));
    if (!list) {
        fprintf(stderr, "lexbor: cannot parse %s\n", s);
        exit(1);
    }
    return list;
}

static void lexbor_init(void) {
    lq.memory = lxb_css_memory_create();
    lxb_css_memory_init(lq.memory, 128);
    lq.parser = lxb_css_parser_create();
    lxb_css_parser_init(lq.parser, NULL);
    lxb_css_parser_memory_set(lq.parser, lq.memory);
    lq.css_selectors = lxb_css_selectors_create();
    lxb_css_selectors_init(lq.css_selectors);
    lxb_css_parser_selectors_set(lq.parser, lq.css_selectors);
    lq.selectors = lxb_selectors_create();
    lxb_selectors_init(lq.selectors);
    lq.pods = lexbor_selector("article.product_pod");
    lq.title = lexbor_selector("h3 a");
    lq.price = lexbor_selector("p.price_color");
    lq.rating = lexbor_selector("p.star-rating");
    lq.quote = lexbor_selector("div.quote");
    lq.text = lexbor_selector("span.text");
    lq.author = lexbor_selector("small.author");
    lq.tag = lexbor_selector("a.tag");
}

typedef struct node_list {
    lxb_dom_node_t *items[256];
    int n;
} node_list;

static lxb_status_t collect_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx) {
    node_list *l = (node_list *)ctx;
    (void)spec;
    if (l->n < 256) l->items[l->n++] = node;
    return LXB_STATUS_OK;
}

static lxb_status_t first_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx) {
    (void)spec;
    *(lxb_dom_node_t **)ctx = node;
    return LXB_STATUS_STOP;
}

static lxb_dom_node_t *lexbor_first(lxb_dom_node_t *root, lxb_css_selector_list_t *list) {
    lxb_dom_node_t *found = NULL;
    lxb_selectors_find(lq.selectors, root, list, first_cb, &found);
    return found;
}

static void lexbor_text(lxb_dom_node_t *node, char *dst, size_t size) {
    size_t len = 0;
    lxb_char_t *t;
    dst[0] = 0;
    if (!node) return;
    t = lxb_dom_node_text_content(node, &len);
    if (t) {
        set(dst, size, (const char *)t, len);
        lxb_dom_document_destroy_text(node->owner_document, t);
    }
}

static const char *lexbor_attr(lxb_dom_node_t *node, const char *name, size_t *len) {
    const lxb_char_t *v;
    *len = 0;
    if (!node) return "";
    v = lxb_dom_element_get_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)name, strlen(name), len);
    return v ? (const char *)v : "";
}

static void books_lexbor(const char *page, size_t len, books *out) {
    lxb_html_document_t *document = lxb_html_document_create();
    lxb_dom_node_t *root;
    node_list pods;
    int i;
    lxb_html_document_parse(document, (const lxb_char_t *)page, len);
    root = lxb_dom_interface_node(document);
    pods.n = 0;
    lxb_selectors_find(lq.selectors, root, lq.pods, collect_cb, &pods);
    out->n = 0;
    for (i = 0; i < pods.n; i++) {
        book *b = &out->items[out->n++];
        size_t vl;
        const char *v = lexbor_attr(lexbor_first(pods.items[i], lq.title), "title", &vl);
        set(b->title, sizeof b->title, v, vl);
        lexbor_text(lexbor_first(pods.items[i], lq.price), b->price, sizeof b->price);
        v = lexbor_attr(lexbor_first(pods.items[i], lq.rating), "class", &vl);
        set_last_word(b->rating, sizeof b->rating, v, vl);
    }
    lxb_html_document_destroy(document);
}

static void quotes_lexbor(const char *page, size_t len, quotes *out) {
    lxb_html_document_t *document = lxb_html_document_create();
    lxb_dom_node_t *root;
    node_list list;
    int i, k;
    lxb_html_document_parse(document, (const lxb_char_t *)page, len);
    root = lxb_dom_interface_node(document);
    list.n = 0;
    lxb_selectors_find(lq.selectors, root, lq.quote, collect_cb, &list);
    out->n = 0;
    for (i = 0; i < list.n; i++) {
        quote *q = &out->items[out->n++];
        node_list tags;
        lexbor_text(lexbor_first(list.items[i], lq.text), q->text, sizeof q->text);
        lexbor_text(lexbor_first(list.items[i], lq.author), q->author, sizeof q->author);
        q->tags[0] = 0;
        tags.n = 0;
        lxb_selectors_find(lq.selectors, list.items[i], lq.tag, collect_cb, &tags);
        for (k = 0; k < tags.n; k++) {
            char t[128];
            lexbor_text(tags.items[k], t, sizeof t);
            append(q->tags, sizeof q->tags, t, strlen(t));
        }
    }
    lxb_html_document_destroy(document);
}

/* ----- Gumbo (no selectors: the tree is walked) ----- */

static int has_class(const GumboNode *n, const char *cls) {
    const GumboAttribute *a = gumbo_get_attribute(&n->v.element.attributes, "class");
    size_t cl = strlen(cls);
    const char *s;
    if (!a) return 0;
    for (s = a->value; *s;) {
        const char *e;
        while (*s && is_ws(*s)) s++;
        e = s;
        while (*e && !is_ws(*e)) e++;
        if ((size_t)(e - s) == cl && memcmp(s, cls, cl) == 0) return 1;
        s = e;
    }
    return 0;
}

static int matches(const GumboNode *n, GumboTag tag, const char *cls) {
    return n->type == GUMBO_NODE_ELEMENT && n->v.element.tag == tag && (!cls || has_class(n, cls));
}

/* The first descendant (in document order) with the tag and class. */
static const GumboNode *gumbo_first(const GumboNode *n, GumboTag tag, const char *cls) {
    unsigned i;
    if (n->type != GUMBO_NODE_ELEMENT && n->type != GUMBO_NODE_DOCUMENT) return NULL;
    {
        const GumboVector *children = n->type == GUMBO_NODE_ELEMENT ? &n->v.element.children : &n->v.document.children;
        for (i = 0; i < children->length; i++) {
            const GumboNode *c = (const GumboNode *)children->data[i];
            const GumboNode *found;
            if (matches(c, tag, cls)) return c;
            found = gumbo_first(c, tag, cls);
            if (found) return found;
        }
    }
    return NULL;
}

typedef struct gnode_list {
    const GumboNode *items[256];
    int n;
} gnode_list;

static void gumbo_all(const GumboNode *n, GumboTag tag, const char *cls, gnode_list *out) {
    unsigned i;
    const GumboVector *children;
    if (n->type == GUMBO_NODE_ELEMENT)
        children = &n->v.element.children;
    else if (n->type == GUMBO_NODE_DOCUMENT)
        children = &n->v.document.children;
    else
        return;
    for (i = 0; i < children->length; i++) {
        const GumboNode *c = (const GumboNode *)children->data[i];
        if (matches(c, tag, cls) && out->n < 256) out->items[out->n++] = c;
        gumbo_all(c, tag, cls, out);
    }
}

static void gumbo_text_rec(const GumboNode *n, char *dst, size_t size, size_t *len) {
    unsigned i;
    if (n->type == GUMBO_NODE_TEXT || n->type == GUMBO_NODE_WHITESPACE || n->type == GUMBO_NODE_CDATA) {
        size_t l = strlen(n->v.text.text);
        if (*len + l >= size) l = size - *len - 1;
        memcpy(dst + *len, n->v.text.text, l);
        *len += l;
        dst[*len] = 0;
        return;
    }
    if (n->type != GUMBO_NODE_ELEMENT) return;
    for (i = 0; i < n->v.element.children.length; i++)
        gumbo_text_rec((const GumboNode *)n->v.element.children.data[i], dst, size, len);
}

static void gumbo_text(const GumboNode *n, char *dst, size_t size) {
    char buf[2048];
    size_t len = 0;
    buf[0] = 0;
    dst[0] = 0;
    if (!n) return;
    gumbo_text_rec(n, buf, sizeof buf, &len);
    set(dst, size, buf, len);
}

static const char *gumbo_attr(const GumboNode *n, const char *name) {
    const GumboAttribute *a;
    if (!n) return "";
    a = gumbo_get_attribute(&n->v.element.attributes, name);
    return a ? a->value : "";
}

static void books_gumbo(const char *page, size_t len, books *out) {
    GumboOutput *g = gumbo_parse_with_options(&kGumboDefaultOptions, page, len);
    gnode_list pods;
    int i;
    pods.n = 0;
    gumbo_all(g->document, GUMBO_TAG_ARTICLE, "product_pod", &pods);
    out->n = 0;
    for (i = 0; i < pods.n; i++) {
        book *b = &out->items[out->n++];
        const GumboNode *h3 = gumbo_first(pods.items[i], GUMBO_TAG_H3, NULL);
        const char *v = gumbo_attr(h3 ? gumbo_first(h3, GUMBO_TAG_A, NULL) : NULL, "title");
        set(b->title, sizeof b->title, v, strlen(v));
        gumbo_text(gumbo_first(pods.items[i], GUMBO_TAG_P, "price_color"), b->price, sizeof b->price);
        v = gumbo_attr(gumbo_first(pods.items[i], GUMBO_TAG_P, "star-rating"), "class");
        set_last_word(b->rating, sizeof b->rating, v, strlen(v));
    }
    gumbo_destroy_output(&kGumboDefaultOptions, g);
}

static void quotes_gumbo(const char *page, size_t len, quotes *out) {
    GumboOutput *g = gumbo_parse_with_options(&kGumboDefaultOptions, page, len);
    gnode_list list;
    int i, k;
    list.n = 0;
    gumbo_all(g->document, GUMBO_TAG_DIV, "quote", &list);
    out->n = 0;
    for (i = 0; i < list.n; i++) {
        quote *q = &out->items[out->n++];
        gnode_list tags;
        gumbo_text(gumbo_first(list.items[i], GUMBO_TAG_SPAN, "text"), q->text, sizeof q->text);
        gumbo_text(gumbo_first(list.items[i], GUMBO_TAG_SMALL, "author"), q->author, sizeof q->author);
        q->tags[0] = 0;
        tags.n = 0;
        gumbo_all(list.items[i], GUMBO_TAG_A, "tag", &tags);
        for (k = 0; k < tags.n; k++) {
            char t[128];
            gumbo_text(tags.items[k], t, sizeof t);
            append(q->tags, sizeof q->tags, t, strlen(t));
        }
    }
    gumbo_destroy_output(&kGumboDefaultOptions, g);
}

/* ----- checks ----- */

static void expect_books(const char *who, const books *got, const books *want) {
    int i;
    if (got->n != want->n) {
        fprintf(stderr, "%s: %d books, expected %d\n", who, got->n, want->n);
        exit(1);
    }
    for (i = 0; i < want->n; i++) {
        const book *g = &got->items[i], *w = &want->items[i];
        if (strcmp(g->title, w->title) || strcmp(g->price, w->price) || strcmp(g->rating, w->rating)) {
            fprintf(stderr, "%s: book %d is {%s, %s, %s}, expected {%s, %s, %s}\n", who, i, g->title, g->price,
                    g->rating, w->title, w->price, w->rating);
            exit(1);
        }
    }
}

static void expect_quotes(const char *who, const quotes *got, const quotes *want) {
    int i;
    if (got->n != want->n) {
        fprintf(stderr, "%s: %d quotes, expected %d\n", who, got->n, want->n);
        exit(1);
    }
    for (i = 0; i < want->n; i++) {
        const quote *g = &got->items[i], *w = &want->items[i];
        if (strcmp(g->text, w->text) || strcmp(g->author, w->author) || strcmp(g->tags, w->tags)) {
            fprintf(stderr, "%s: quote %d is {%s, %s, %s}, expected {%s, %s, %s}\n", who, i, g->text, g->author,
                    g->tags, w->text, w->author, w->tags);
            exit(1);
        }
    }
}

/* ----- timing ----- */

typedef void (*task_fn)(const char *, size_t, void *);

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The median time per call of five batches, in seconds. */
static double measure(void (*f)(const void *), const void *arg) {
    unsigned long iterations = 1, i;
    double samples[5];
    int s;
    for (;;) {
        double start = now();
        for (i = 0; i < iterations; i++) f(arg);
        if (now() - start > 0.1) break;
        iterations *= 2;
    }
    for (s = 0; s < 5; s++) {
        double start = now();
        for (i = 0; i < iterations * 5; i++) f(arg);
        samples[s] = (now() - start) / (double)(iterations * 5);
    }
    qsort(samples, 5, sizeof(double), cmp_double);
    return samples[2];
}

static void row(const char *task, const char *lib, size_t bytes, void (*f)(const void *), const void *arg) {
    double t = measure(f, arg);
    if (bytes)
        printf("| %s | %s | %.1f \xC2\xB5s | %.0f MB/s |\n", task, lib, t * 1e6, (double)bytes / t / 1e6);
    else
        printf("| %s | %s | %.1f \xC2\xB5s |  |\n", task, lib, t * 1e6);
    fflush(stdout);
}

typedef struct page {
    const char *src;
    size_t len;
} page;

static void parse_fast(const void *p) {
    const page *pg = (const page *)p;
    fhp_doc *doc = fhp_parse(pg->src, pg->len);
    sink += fhp_doc_len(doc);
    fhp_free(doc);
}

static void parse_fast_borrowed(const void *p) {
    const page *pg = (const page *)p;
    fhp_doc *doc = fhp_parse_borrowed(pg->src, pg->len, NULL);
    sink += fhp_doc_len(doc);
    fhp_free(doc);
}

static void parse_lexbor(const void *p) {
    const page *pg = (const page *)p;
    lxb_html_document_t *document = lxb_html_document_create();
    lxb_html_document_parse(document, (const lxb_char_t *)pg->src, pg->len);
    sink += (size_t)document->dom_document.node.first_child;
    lxb_html_document_destroy(document);
}

/* lexbor at its best: one document reused (parse cleans it), so its memory pools are not allocated again. */
static lxb_html_document_t *reused_document;

static void parse_lexbor_reused(const void *p) {
    const page *pg = (const page *)p;
    if (!reused_document) reused_document = lxb_html_document_create();
    lxb_html_document_parse(reused_document, (const lxb_char_t *)pg->src, pg->len);
    sink += (size_t)reused_document->dom_document.node.first_child;
}

static void parse_gumbo(const void *p) {
    const page *pg = (const page *)p;
    GumboOutput *g = gumbo_parse_with_options(&kGumboDefaultOptions, pg->src, pg->len);
    sink += g->document->v.document.children.length;
    gumbo_destroy_output(&kGumboDefaultOptions, g);
}

static books book_out;
static quotes quote_out;

#define SCRAPE(name, fn, out)                         \
    static void name(const void *p) {                 \
        const page *pg = (const page *)p;             \
        fn(pg->src, pg->len, &out);                   \
        sink += (size_t)out.n;                        \
    }
SCRAPE(scrape_books_fast, books_fast, book_out)
SCRAPE(scrape_books_fast_css, books_fast_css, book_out)
SCRAPE(scrape_books_fast_xpath, books_fast_xpath, book_out)
SCRAPE(scrape_books_lexbor, books_lexbor, book_out)
SCRAPE(scrape_books_gumbo, books_gumbo, book_out)
SCRAPE(scrape_quotes_fast, quotes_fast, quote_out)
SCRAPE(scrape_quotes_fast_css, quotes_fast_css, quote_out)
SCRAPE(scrape_quotes_lexbor, quotes_lexbor, quote_out)
SCRAPE(scrape_quotes_gumbo, quotes_gumbo, quote_out)

static char *replace_all(const char *s, size_t len, const char *what, size_t *out_len) {
    size_t wl = strlen(what), n = 0, i = 0;
    char *out = (char *)malloc(len + 1);
    while (i < len) {
        if (i + wl <= len && memcmp(s + i, what, wl) == 0) {
            i += wl;
            continue;
        }
        out[n++] = s[i++];
    }
    out[n] = 0;
    *out_len = n;
    return out;
}

static const char *compiler_name(void) {
    static char buf[128];
#if defined(__clang__)
    snprintf(buf, sizeof buf, "clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    snprintf(buf, sizeof buf, "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    snprintf(buf, sizeof buf, "MSVC %d", _MSC_FULL_VER);
#else
    snprintf(buf, sizeof buf, "unknown compiler");
#endif
    return buf;
}

int main(int argc, char **argv) {
    page quotes_page, laptops, books_page, books_x10, books_no_end;
    books expected, got;
    quotes expected_q, got_q;
    size_t tmp_len, i;
    char *tmp;
    char err[256];
    int check_only = argc > 1 && strcmp(argv[1], "--check") == 0;
    struct {
        const char *name;
        const page *pg;
    } parses[5];

    quotes_page.src = read_page("quotes/page1.html", &quotes_page.len);
    laptops.src = read_page("webscraper/laptops.html", &laptops.len);
    books_page.src = read_page("books/index.html", &books_page.len);
    {
        char *x10 = (char *)malloc(books_page.len * 10 + 1);
        for (i = 0; i < 10; i++) memcpy(x10 + i * books_page.len, books_page.src, books_page.len);
        x10[books_page.len * 10] = 0;
        books_x10.src = x10;
        books_x10.len = books_page.len * 10;
    }
    tmp = replace_all(books_page.src, books_page.len, "</li>", &tmp_len);
    books_no_end.src = replace_all(tmp, tmp_len, "</p>", &books_no_end.len);
    free(tmp);

    arena = fhp_arena_new(0);
    fq.pods = fhp_css_compile("article.product_pod", err, sizeof err);
    fq.title = fhp_css_compile("h3 a", err, sizeof err);
    fq.price = fhp_css_compile("p.price_color", err, sizeof err);
    fq.rating = fhp_css_compile("p.star-rating", err, sizeof err);
    fq.quote = fhp_css_compile("div.quote", err, sizeof err);
    fq.text = fhp_css_compile("span.text", err, sizeof err);
    fq.author = fhp_css_compile("small.author", err, sizeof err);
    fq.tag = fhp_css_compile("a.tag", err, sizeof err);
    fq.pods_x = fhp_xpath_compile("//article[contains(concat(' ', @class, ' '), ' product_pod ')]", err, sizeof err);
    fq.title_x = fhp_xpath_compile(".//h3/a/@title", err, sizeof err);
    fq.price_x = fhp_xpath_compile(".//p[contains(concat(' ', @class, ' '), ' price_color ')]", err, sizeof err);
    fq.rating_x = fhp_xpath_compile(".//p[contains(concat(' ', @class, ' '), ' star-rating ')]/@class", err, sizeof err);
    lexbor_init();

    /* every library must extract the same records */
    books_fast(books_page.src, books_page.len, &expected);
    if (expected.n != 20) {
        fprintf(stderr, "fasthtmlparser: %d books, expected 20\n", expected.n);
        return 1;
    }
    books_fast_css(books_page.src, books_page.len, &got);
    expect_books("fasthtmlparser CSS", &got, &expected);
    books_fast_xpath(books_page.src, books_page.len, &got);
    expect_books("fasthtmlparser XPath", &got, &expected);
    books_lexbor(books_page.src, books_page.len, &got);
    expect_books("lexbor", &got, &expected);
    books_gumbo(books_page.src, books_page.len, &got);
    expect_books("gumbo", &got, &expected);
    quotes_fast(quotes_page.src, quotes_page.len, &expected_q);
    if (expected_q.n != 10) {
        fprintf(stderr, "fasthtmlparser: %d quotes, expected 10\n", expected_q.n);
        return 1;
    }
    quotes_fast_css(quotes_page.src, quotes_page.len, &got_q);
    expect_quotes("fasthtmlparser CSS", &got_q, &expected_q);
    quotes_lexbor(quotes_page.src, quotes_page.len, &got_q);
    expect_quotes("lexbor", &got_q, &expected_q);
    quotes_gumbo(quotes_page.src, quotes_page.len, &got_q);
    expect_quotes("gumbo", &got_q, &expected_q);
    printf("{%s, %s, %s}\n%s\n\n", expected.items[0].title, expected.items[0].price, expected.items[0].rating,
           expected_q.items[0].author);
    if (check_only) return 0;

    printf("%s, fasthtmlparser %s, lexbor 3.0.1, gumbo 0.14.1\n\n", compiler_name(), FHP_VERSION);
    printf("| task | library | time | throughput |\n|---|---|---:|---:|\n");
    parses[0].name = "parse quotes 11 KB", parses[0].pg = &quotes_page;
    parses[1].name = "parse laptops 38 KB", parses[1].pg = &laptops;
    parses[2].name = "parse books 51 KB", parses[2].pg = &books_page;
    parses[3].name = "parse books x10 510 KB", parses[3].pg = &books_x10;
    parses[4].name = "parse books, no </li> </p>", parses[4].pg = &books_no_end;
    for (i = 0; i < 5; i++) {
        size_t n = parses[i].pg->len;
        row(parses[i].name, "fasthtmlparser", n, parse_fast, parses[i].pg);
        row(parses[i].name, "fasthtmlparser (borrowed)", n, parse_fast_borrowed, parses[i].pg);
        row(parses[i].name, "lexbor", n, parse_lexbor, parses[i].pg);
        row(parses[i].name, "lexbor (document reused)", n, parse_lexbor_reused, parses[i].pg);
        row(parses[i].name, "gumbo", n, parse_gumbo, parses[i].pg);
    }
    row("scrape 20 books", "fasthtmlparser find", 0, scrape_books_fast, &books_page);
    row("scrape 20 books", "fasthtmlparser CSS", 0, scrape_books_fast_css, &books_page);
    row("scrape 20 books", "fasthtmlparser XPath", 0, scrape_books_fast_xpath, &books_page);
    row("scrape 20 books", "lexbor CSS", 0, scrape_books_lexbor, &books_page);
    row("scrape 20 books", "gumbo (tree walk)", 0, scrape_books_gumbo, &books_page);
    row("scrape 10 quotes", "fasthtmlparser find", 0, scrape_quotes_fast, &quotes_page);
    row("scrape 10 quotes", "fasthtmlparser CSS", 0, scrape_quotes_fast_css, &quotes_page);
    row("scrape 10 quotes", "lexbor CSS", 0, scrape_quotes_lexbor, &quotes_page);
    row("scrape 10 quotes", "gumbo (tree walk)", 0, scrape_quotes_gumbo, &quotes_page);
    return 0;
}

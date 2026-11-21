/* Tests: the API examples of the README, and conformance with z3l.FastHtmlParser (.NET): the expected output in
 * testdata/conformance comes from the .NET version for the same pages (see the Rust port's tools/conformance). Tag
 * names, parents, depths, positions (UTF-8 byte offsets), texts, attributes, navigation and links must be exactly the
 * same, and so must the results of every XPath and CSS query of testdata/conformance/queries.json. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime, dirent */
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h" /* the positions the parser keeps */
#include "json.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

typedef struct sha256 {
    unsigned h[8];
    unsigned char block[64];
    size_t used;
    unsigned long long total;
} sha256;
void sha256_init(sha256 *s);
void sha256_update(sha256 *s, const void *data, size_t len);
void sha256_final(sha256 *s, unsigned char out[32]);

#ifndef FHP_TESTDATA
#define FHP_TESTDATA "testdata"
#endif

/* The page URL the .NET dump uses. */
static const char *PAGE_URL = "https://Example.com:443/catalog/page-1.html?sort=asc#top";

static int failures;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    failures++;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = (size_t)n;
    }
    return buf;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The sorted stems of the files with the extension in dir. */
static char **list_dir(const char *dir, const char *ext, size_t *count) {
    char **names = NULL;
    size_t n = 0, cap = 0, el = strlen(ext);
#ifdef _WIN32
    char pattern[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*%s", dir, ext);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char *name = fd.cFileName;
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
#endif
            size_t nl = strlen(name);
            if (nl <= el || strcmp(name + nl - el, ext) != 0) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                names = (char **)realloc(names, cap * sizeof(char *));
            }
            names[n] = (char *)malloc(nl - el + 1);
            memcpy(names[n], name, nl - el);
            names[n][nl - el] = 0;
            n++;
#ifdef _WIN32
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
        }
        closedir(d);
    }
#endif
    if (n) qsort(names, n, sizeof(char *), cmp_str);
    *count = n;
    return names;
}

static int jstr_eq(const jvalue *v, fhp_str s) {
    return v && v->type == J_STRING && v->len == s.len && (s.len == 0 || memcmp(v->str, s.ptr, s.len) == 0);
}

static int jint_eq(const jvalue *v, long long x) { return v && v->type == J_NUMBER && v->num == (double)x; }

static void short_str(fhp_str s, char *buf, size_t size) {
    size_t n = s.len < size - 1 ? s.len : size - 1;
    if (n > 300) n = 300;
    memcpy(buf, s.ptr, n);
    buf[n] = 0;
}

/* ----- API examples (the README) ----- */

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) fail("%s:%d: check failed: %s", __FILE__, __LINE__, #cond); \
    } while (0)

static int eq(fhp_str s, const char *z) { return s.len == strlen(z) && memcmp(s.ptr, z, s.len) == 0; }

static void test_readme(void) {
    static const char page[] =
        "<html><body>\n"
        "  <article class=\"product_pod\"><h3><a href=\"b/1\" title=\"A Light in the Attic\">A Light...</a></h3>\n"
        "    <p class=\"price_color\">\xC2\xA3" "51.77</p><p class=\"star-rating Three\"></p></article>\n"
        "  <dl><dt>Price</dt><dd>\xC2\xA3" "51.77</dd></dl>\n"
        "</body></html>";
    fhp_doc *doc = fhp_parse_url(page, sizeof page - 1, "https://example.com/catalog/");
    fhp_arena *a = fhp_arena_new(0);
    fhp_css *pods, *prices;
    fhp_xpath *titles;
    fhp_nodes list;
    fhp_strs values;
    fhp_node pod, a_tag, dt, body;
    fhp_query q;
    size_t i;
    char err[256];
    CHECK(doc != NULL);

    /* CSS selectors */
    pods = fhp_css_compile("article.product_pod", err, sizeof err);
    CHECK(pods != NULL);
    list = fhp_css_all(pods, doc, FHP_NONE, a);
    CHECK(list.len == 1);
    for (i = 0; i < list.len; i++) {
        fhp_node t = fhp_select_first(doc, list.items[i], "h3 a");
        fhp_node p = fhp_select_first(doc, list.items[i], "p.price_color");
        CHECK(eq(fhp_attr(doc, t, "title", a), "A Light in the Attic"));
        CHECK(eq(fhp_text(doc, p, a), "\xC2\xA3" "51.77"));
    }
    pod = fhp_select_first(doc, FHP_NONE, "article");
    CHECK(fhp_select(doc, pod, "> h3", a, &list) == 0 && list.len == 1);
    CHECK(fhp_select(doc, pod, "h3 >", a, &list) == -1);

    /* XPath */
    CHECK(eq(fhp_xpath_string(doc, FHP_NONE, "//dt[.='Price']/following-sibling::dd[1]", a), "\xC2\xA3" "51.77"));
    CHECK(eq(fhp_xpath_string(doc, FHP_NONE, "count(//p)", a), "2"));
    titles = fhp_xpath_compile("//article//h3/a/@title", err, sizeof err);
    CHECK(titles != NULL);
    values = fhp_xpath_values(titles, doc, FHP_NONE, a);
    CHECK(values.len == 1 && eq(values.items[0], "A Light in the Attic"));
    prices = fhp_css_compile("p.price_color", NULL, 0);
    CHECK(fhp_css_all(prices, doc, FHP_NONE, a).len == 1);
    CHECK(fhp_xpath_compile("//a[", err, sizeof err) == NULL && strstr(err, "invalid XPath") != NULL);
    CHECK(strstr(err, "unexpected end of the expression") != NULL);
    CHECK(fhp_css_compile("a[", err, sizeof err) == NULL && strstr(err, "invalid CSS selector") != NULL);

    /* search */
    memset(&q, 0, sizeof q);
    q.tag = FHP_TAG_ARTICLE;
    q.cls = "product_pod";
    pod = fhp_find(doc, FHP_NONE, &q);
    CHECK(pod >= 0);
    memset(&q, 0, sizeof q);
    q.tag = FHP_TAG_P;
    q.cls = "price_color";
    CHECK(eq(fhp_text(doc, fhp_find(doc, pod, &q), a), "\xC2\xA3" "51.77"));
    q = fhp_by_name("my-widget");
    q.cls = "x";
    CHECK(fhp_find(doc, FHP_NONE, &q) == FHP_NONE);

    /* shortcuts */
    dt = fhp_find_by_name_text(doc, FHP_NONE, "dt", "Price");
    CHECK(dt >= 0);
    CHECK(eq(fhp_text(doc, fhp_next_sibling_by_name(doc, dt, "dd"), a), "\xC2\xA3" "51.77"));
    CHECK(fhp_find_by_name_class(doc, FHP_NONE, "P", "STAR-RATING") >= 0);

    /* reading a tag */
    a_tag = fhp_select_first(doc, FHP_NONE, "h3 a");
    CHECK(eq(fhp_text(doc, a_tag, a), "A Light..."));
    CHECK(eq(fhp_attr(doc, a_tag, FHP_ATTR_TITLE, a), "A Light in the Attic"));
    CHECK(eq(fhp_href(doc, a_tag, a), "https://example.com/catalog/b/1"));
    CHECK(eq(fhp_source(doc, a_tag), "a href=\"b/1\" title=\"A Light in the Attic\""));
    CHECK(eq(fhp_name(doc, a_tag), "a") && fhp_depth(doc, a_tag) == 4 && fhp_tag(doc, a_tag) == FHP_TAG_A);
    memset(&q, 0, sizeof q);
    q.tag = FHP_TAG_BODY;
    body = fhp_find(doc, FHP_NONE, &q);
    {
        fhp_str t = fhp_text_line_breaks(doc, body, a);
        CHECK(t.len > 20 && memcmp(t.ptr, "A Light...\n\n\xC2\xA3" "51.77", 18) == 0);
    }

    /* navigation */
    CHECK(eq(fhp_name(doc, fhp_parent(doc, a_tag)), "h3"));
    CHECK(fhp_parent_by_name(doc, a_tag, "article") == pod);
    CHECK(fhp_children(doc, pod, a).len == 3);
    CHECK(eq(fhp_name(doc, fhp_next_sibling(doc, dt)), "dd"));

    /* attributes */
    {
        fhp_attr_iter it;
        fhp_str name, value;
        int n = 0;
        fhp_attrs_begin(doc, a_tag, &it);
        while (fhp_attrs_next(&it, &name, &value)) n++;
        CHECK(n == 2);
    }

    /* entities and text helpers */
    CHECK(eq(fhp_decode_html("a &amp; b &lt; &#x41;&copy2024", 30, a), "a & b < A\xC2\xA9" "2024"));
    CHECK(eq(fhp_html_to_text("<p>a <b>b</b></p><script>x</script> c", 37, a), "a b c"));
    CHECK(fhp_lookup_tag("DIV") == FHP_TAG_DIV && strcmp(fhp_tag_name(FHP_TAG_DIV), "div") == 0);

    fhp_css_free(pods);
    fhp_css_free(prices);
    fhp_xpath_free(titles);
    fhp_arena_free(a);
    fhp_free(doc);

    /* a borrowed source, the document arena, invalid UTF-8 */
    {
        static const char src[] = "<ul><li>one<li>two</ul><p id=x>\xFF\xFEz</p>";
        fhp_doc *d = fhp_parse_borrowed(src, sizeof src - 1, NULL);
        fhp_doc *c = fhp_parse(src, sizeof src - 1);
        CHECK(fhp_doc_len(d) == 4 && fhp_doc_len(c) == 4);
        CHECK(eq(fhp_text(d, 2, NULL), "two"));
        CHECK(eq(fhp_text(c, 3, NULL), "\xEF\xBF\xBD\xEF\xBF\xBDz"));
        CHECK(fhp_find_by_id(c, FHP_NONE, "x") == 3);
        fhp_doc_prepare(c);
        fhp_free(d);
        fhp_free(c);
    }
}

/* ----- conformance: tags ----- */

static void test_tags_as_dotnet(void) {
    char dir[1024], path[1024];
    size_t count, f, pages_failed = 0;
    char **names;
    fhp_arena *a = fhp_arena_new(0);
    snprintf(dir, sizeof dir, "%s/conformance/input", FHP_TESTDATA);
    names = list_dir(dir, ".html", &count);
    if (count == 0) fail("no inputs in %s", dir);
    for (f = 0; f < count; f++) {
        size_t len, elen, i;
        char *src, *exp;
        jvalue *expected;
        fhp_doc *doc;
        int problems = 0;
        snprintf(path, sizeof path, "%s/%s.html", dir, names[f]);
        src = read_file(path, &len);
        snprintf(path, sizeof path, "%s/conformance/expected/%s.json", FHP_TESTDATA, names[f]);
        exp = read_file(path, &elen);
        expected = exp ? json_parse(exp, elen) : NULL;
        if (!src || !expected || expected->type != J_ARRAY) {
            fail("%s: cannot read the page or the expected data", names[f]);
            continue;
        }
        doc = fhp_parse_url(src, len, PAGE_URL);
        if (fhp_doc_len(doc) != expected->count) {
            fail("%s: tags: %u, .NET: %u", names[f], (unsigned)fhp_doc_len(doc), (unsigned)expected->count);
            problems++;
        }
        for (i = 0; i < expected->count && i < fhp_doc_len(doc) && problems < 10; i++) {
            const jvalue *e = expected->items[i];
            fhp_node n = (fhp_node)i;
            size_t start, end;
            const jvalue *attrs;
            char buf[400];
            fhp_str text;
            fhp_nodes children;
            char kids[4096];
            size_t k, kl = 0;
            fhp_arena_reset(a);
            fhp_pos(doc, n, &start, &end);
            text = fhp_text(doc, n, a);
            {
                /* the internal positions: innerStart / innerEnd as the parser keeps them */
                long long is = doc->nodes[i].inner_start, ie = doc->nodes[i].inner_end, parent = doc->nodes[i].parent;
                if (!jstr_eq(json_get(e, "name"), fhp_name(doc, n)) || !jint_eq(json_get(e, "parent"), parent) ||
                    !jint_eq(json_get(e, "depth"), fhp_depth(doc, n)) || !jint_eq(json_get(e, "start"), (long long)start) ||
                    !jint_eq(json_get(e, "end"), (long long)end) || !jint_eq(json_get(e, "innerStart"), is) ||
                    !jint_eq(json_get(e, "innerEnd"), ie) || !jstr_eq(json_get(e, "text"), text)) {
                    char want[400];
                    json_print(e, want, sizeof want);
                    short_str(text, buf, sizeof buf);
                    fail("%s [%u] <%.*s> parent %lld depth %d start %u end %u inner %lld..%lld text \"%s\"\n  .net: %s",
                         names[f], (unsigned)i, (int)fhp_name(doc, n).len, fhp_name(doc, n).ptr, parent,
                         fhp_depth(doc, n), (unsigned)start, (unsigned)end, is, ie, buf, want);
                    problems++;
                    continue;
                }
            }
            attrs = json_get(e, "attrs");
            for (k = 0; attrs && k < attrs->count; k++) {
                fhp_str v = fhp_attr(doc, n, attrs->keys[k], a);
                if (!jstr_eq(attrs->items[k], v)) {
                    short_str(v, buf, sizeof buf);
                    fail("%s [%u] @%s: \"%s\"", names[f], (unsigned)i, attrs->keys[k], buf);
                    problems++;
                }
            }
            children = fhp_children(doc, n, a);
            kids[0] = 0;
            for (k = 0; k < children.len && kl + 16 < sizeof kids; k++)
                kl += (size_t)snprintf(kids + kl, sizeof kids - kl, k ? ",%d" : "%d", children.items[k]);
#define CHECK_STR(key, value)                                                                    \
    do {                                                                                         \
        fhp_str v_ = (value);                                                                    \
        if (!jstr_eq(json_get(e, key), v_)) {                                                    \
            char want_[400];                                                                     \
            json_print(json_get(e, key), want_, sizeof want_);                                   \
            short_str(v_, buf, sizeof buf);                                                      \
            fail("%s [%u] <%.*s> %s\n  c:    \"%s\"\n  .net: %s", names[f], (unsigned)i,          \
                 (int)fhp_name(doc, n).len, fhp_name(doc, n).ptr, key, buf, want_);              \
            problems++;                                                                          \
        }                                                                                        \
    } while (0)
#define CHECK_INT(key, value)                                                                          \
    do {                                                                                               \
        long long v_ = (value);                                                                        \
        if (!jint_eq(json_get(e, key), v_)) {                                                          \
            fail("%s [%u] %s: %lld", names[f], (unsigned)i, key, v_);                                  \
            problems++;                                                                                \
        }                                                                                              \
    } while (0)
            CHECK_STR("textLineBreaks", fhp_text_line_breaks(doc, n, a));
            CHECK_STR("source", fhp_source(doc, n));
            CHECK_INT("next", fhp_next(doc, n));
            CHECK_INT("previous", fhp_previous(doc, n));
            CHECK_INT("nextSibling", fhp_next_sibling(doc, n));
            CHECK_INT("previousSibling", fhp_previous_sibling(doc, n));
            {
                fhp_str ks;
                ks.ptr = kids;
                ks.len = strlen(kids);
                CHECK_STR("children", ks);
            }
            CHECK_STR("href", fhp_href(doc, n, a));
            CHECK_STR("src", fhp_src(doc, n, a));
            CHECK_STR("mailto", fhp_href_mail_to(doc, n, a));
            CHECK_STR("tel", fhp_href_telephone(doc, n, a));
            CHECK_STR("textAfter", fhp_text_after_tag_until_next_one(doc, n));
            CHECK_STR("textNoTagsAfter", fhp_text_no_tags_after_tag_until_next_one(doc, n, a));
#undef CHECK_STR
#undef CHECK_INT
        }
        if (problems) pages_failed++;
        fhp_free(doc);
        json_free(expected);
        free(src);
        free(exp);
        free(names[f]);
    }
    free(names);
    fhp_arena_free(a);
    printf("tags_as_dotnet: %u pages, %u differ\n", (unsigned)count, (unsigned)pages_failed);
}

/* ----- conformance: queries ----- */

static void test_compile_as_dotnet(void) {
    char path[1024];
    size_t len, k, n = 0, bad = 0;
    char *s;
    jvalue *expected;
    const jvalue *xpath, *css;
    snprintf(path, sizeof path, "%s/conformance/expected/queries/compile.json", FHP_TESTDATA);
    s = read_file(path, &len);
    expected = s ? json_parse(s, len) : NULL;
    if (!expected) {
        fail("cannot read %s", path);
        return;
    }
    xpath = json_get(expected, "xpath");
    css = json_get(expected, "css");
    for (k = 0; xpath && k < xpath->count; k++, n++) {
        char err[512];
        fhp_xpath *q = fhp_xpath_compile(xpath->keys[k], err, sizeof err);
        int ok = q != NULL, want = strcmp(xpath->items[k]->str, "ok") == 0;
        if (ok != want) {
            fail("XPath \"%s\": %s, .NET: %s (%s)", xpath->keys[k], ok ? "ok" : "error", xpath->items[k]->str, err);
            bad++;
        }
        fhp_xpath_free(q);
    }
    for (k = 0; css && k < css->count; k++, n++) {
        char err[512];
        fhp_css *q = fhp_css_compile(css->keys[k], err, sizeof err);
        int ok = q != NULL, want = strcmp(css->items[k]->str, "ok") == 0;
        if (ok != want) {
            fail("CSS \"%s\": %s, .NET: %s (%s)", css->keys[k], ok ? "ok" : "error", css->items[k]->str, err);
            bad++;
        }
        fhp_css_free(q);
    }
    printf("compile_as_dotnet: %u queries, %u differ\n", (unsigned)n, (unsigned)bad);
    json_free(expected);
    free(s);
}

/* [count, hash] of the values (sha256 of the values joined with \x01, the first 16 hex digits), or "throws". */
typedef struct result {
    int throws;
    size_t count;
    char hash[17];
} result;

typedef struct hasher {
    sha256 s;
    size_t count;
} hasher;

static void h_init(hasher *h) {
    sha256_init(&h->s);
    h->count = 0;
}

static void h_add(hasher *h, const char *s, size_t len) {
    if (h->count) sha256_update(&h->s, "\x01", 1);
    sha256_update(&h->s, s, len);
    h->count++;
}

static void h_add_int(hasher *h, int v) {
    char buf[16];
    h_add(h, buf, (size_t)snprintf(buf, sizeof buf, "%d", v));
}

static result h_result(hasher *h) {
    result r;
    unsigned char d[32];
    int i;
    sha256_final(&h->s, d);
    r.throws = 0;
    r.count = h->count;
    for (i = 0; i < 8; i++) snprintf(r.hash + 2 * i, 3, "%02x", d[i]);
    return r;
}

static result throws(void) {
    result r;
    memset(&r, 0, sizeof r);
    r.throws = 1;
    return r;
}

static int result_eq(const jvalue *want, result got) {
    char count[32];
    if (!want) return 0;
    if (want->type == J_STRING) return got.throws && strcmp(want->str, "throws") == 0;
    if (got.throws || want->type != J_ARRAY || want->count != 2) return 0;
    snprintf(count, sizeof count, "%u", (unsigned)got.count);
    return strcmp(want->items[0]->str, count) == 0 && strcmp(want->items[1]->str, got.hash) == 0;
}

static void describe(result r, char *buf, size_t size) {
    if (r.throws)
        snprintf(buf, size, "\"throws\"");
    else
        snprintf(buf, size, "[\"%u\",\"%s\"]", (unsigned)r.count, r.hash);
}

static void test_queries_as_dotnet(void) {
    char dir[1024], path[1024];
    size_t count, f, pages_failed = 0, checks = 0;
    char **names;
    fhp_arena *a = fhp_arena_new(0);
    snprintf(dir, sizeof dir, "%s/conformance/expected/queries", FHP_TESTDATA);
    names = list_dir(dir, ".json", &count);
    if (count < 100) fail("no query results in %s", dir);
    for (f = 0; f < count; f++) {
        size_t len, elen, k, s, nscopes;
        char *src, *exp;
        jvalue *expected;
        const jvalue *scopes, *xpath, *css;
        fhp_doc *doc;
        int problems = 0;
        if (strcmp(names[f], "compile") == 0) {
            free(names[f]);
            continue;
        }
        snprintf(path, sizeof path, "%s/%s.json", dir, names[f]);
        exp = read_file(path, &elen);
        snprintf(path, sizeof path, "%s/conformance/input/%s.html", FHP_TESTDATA, names[f]);
        src = read_file(path, &len);
        expected = exp ? json_parse(exp, elen) : NULL;
        if (!src || !expected) {
            fail("%s: cannot read the page or the expected data", names[f]);
            continue;
        }
        doc = fhp_parse(src, len);
        scopes = json_get(expected, "scopes");
        nscopes = scopes ? scopes->count : 0;
        xpath = json_get(expected, "xpath");
        css = json_get(expected, "css");

#define CHECK_RESULT(what, query, want, got)                                       \
    do {                                                                           \
        result g_ = (got);                                                         \
        checks++;                                                                  \
        if (problems < 10 && !result_eq(want, g_)) {                               \
            char gb_[64], wb_[256];                                                \
            describe(g_, gb_, sizeof gb_);                                         \
            json_print(want, wb_, sizeof wb_);                                     \
            fail("%s: %s %s\n  c:    %s\n  .net: %s", names[f], what, query, gb_, wb_); \
            problems++;                                                            \
        }                                                                          \
    } while (0)

        for (k = 0; xpath && k < xpath->count; k++) {
            const char *x = xpath->keys[k];
            const jvalue *want = xpath->items[k];
            fhp_xpath *q;
            hasher h;
            size_t i;
            if (strcmp(names[f], "snip039") == 0 && strcmp(x, "//div/node()") == 0) continue; /* known difference */
            q = fhp_xpath_compile(x, NULL, 0);
            if (!q) {
                fail("%s: XPath %s does not compile", names[f], x);
                problems++;
                continue;
            }
            fhp_arena_reset(a);
            {
                fhp_nodes nodes;
                if (fhp_xpath_nodes(q, doc, FHP_NONE, a, &nodes) != 0) {
                    CHECK_RESULT("XPath tags", x, json_get(want, "tags"), throws());
                } else {
                    h_init(&h);
                    for (i = 0; i < nodes.len; i++) h_add_int(&h, nodes.items[i]);
                    CHECK_RESULT("XPath tags", x, json_get(want, "tags"), h_result(&h));
                }
            }
            {
                fhp_strs v = fhp_xpath_values(q, doc, FHP_NONE, a);
                h_init(&h);
                for (i = 0; i < v.len; i++) h_add(&h, v.items[i].ptr, v.items[i].len);
                CHECK_RESULT("XPath values", x, json_get(want, "values"), h_result(&h));
            }
            {
                fhp_str v;
                h_init(&h);
                if (fhp_xpath_value(q, doc, FHP_NONE, a, &v))
                    h_add(&h, v.ptr, v.len);
                else
                    h_add(&h, "<null>", 6);
                CHECK_RESULT("XPath value", x, json_get(want, "value"), h_result(&h));
            }
            {
                const jvalue *ws = json_get(want, "scoped");
                for (s = 0; s < nscopes; s++) {
                    fhp_node n = (fhp_node)scopes->items[s]->num;
                    fhp_nodes nodes;
                    result r;
                    if (fhp_xpath_nodes(q, doc, n, a, &nodes) != 0) {
                        r = throws();
                    } else {
                        fhp_strs v = fhp_xpath_values(q, doc, n, a);
                        h_init(&h);
                        for (i = 0; i < nodes.len; i++) h_add_int(&h, nodes.items[i]);
                        h_add(&h, "|", 1);
                        for (i = 0; i < v.len; i++) h_add(&h, v.items[i].ptr, v.items[i].len);
                        r = h_result(&h);
                    }
                    CHECK_RESULT("XPath scoped", x, ws && s < ws->count ? ws->items[s] : NULL, r);
                }
            }
            fhp_xpath_free(q);
        }

        for (k = 0; css && k < css->count; k++) {
            const char *c = css->keys[k];
            const jvalue *want = css->items[k];
            fhp_css *q = fhp_css_compile(c, NULL, 0);
            hasher h;
            size_t i;
            if (!q) {
                fail("%s: CSS %s does not compile", names[f], c);
                problems++;
                continue;
            }
            fhp_arena_reset(a);
            {
                fhp_nodes all = fhp_css_all(q, doc, FHP_NONE, a);
                h_init(&h);
                for (i = 0; i < all.len; i++) h_add_int(&h, all.items[i]);
                CHECK_RESULT("CSS all", c, json_get(want, "all"), h_result(&h));
            }
            {
                fhp_node first = fhp_css_first(q, doc, FHP_NONE);
                h_init(&h);
                if (first >= 0)
                    h_add_int(&h, first);
                else
                    h_add(&h, "<null>", 6);
                CHECK_RESULT("CSS first", c, json_get(want, "first"), h_result(&h));
            }
            {
                h_init(&h);
                for (i = 0; i < fhp_doc_len(doc); i++)
                    if (fhp_css_matches(q, doc, (fhp_node)i)) h_add_int(&h, (int)i);
                CHECK_RESULT("CSS matches", c, json_get(want, "matches"), h_result(&h));
            }
            {
                const jvalue *ws = json_get(want, "scoped");
                for (s = 0; s < nscopes; s++) {
                    fhp_nodes all = fhp_css_all(q, doc, (fhp_node)scopes->items[s]->num, a);
                    h_init(&h);
                    for (i = 0; i < all.len; i++) h_add_int(&h, all.items[i]);
                    CHECK_RESULT("CSS scoped", c, ws && s < ws->count ? ws->items[s] : NULL, h_result(&h));
                }
            }
            fhp_css_free(q);
        }
#undef CHECK_RESULT
        if (problems) pages_failed++;
        fhp_free(doc);
        json_free(expected);
        free(src);
        free(exp);
        free(names[f]);
    }
    free(names);
    fhp_arena_free(a);
    printf("queries_as_dotnet: %u pages, %u checks, %u pages differ\n", (unsigned)(count - 1), (unsigned)checks,
           (unsigned)pages_failed);
}

/* ----- robustness ----- */

static unsigned long long seed = 0x2545F4914F6CDD1DULL;

static size_t rnd(size_t n) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (size_t)(seed % (n ? n : 1));
}

/* Pages cut, spliced and sprinkled with markup characters and multi-byte text at random: every function on every tag
   must work (run it under a sanitizer for the full value of this test). */
static void test_mangled_pages(void) {
    static const char *const pieces[] = {"<",        ">",     "</",          "<!--",  "-->",    "=",       "\"",
                                         "'",        "&",     "&amp",        "&#x",   "<p>",    "<li>",    "<td>",
                                         "<script>", "</div>", "\xD0\xB6",   "\xF0\x9F\x98\x80", "\r", " ",
                                         "<a href=", "/>",    "<?xml",       "<![CDATA[", "\xC2\xA0", "<option>",
                                         "<table>"};
    const size_t npieces = sizeof pieces / sizeof pieces[0];
    char dir[1024], path[1024];
    size_t count, f, rounds = 4, docs = 0;
    char **names;
    const char *env = getenv("FASTHTML_FUZZ_ROUNDS");
    fhp_arena *a = fhp_arena_new(0);
    fhp_css *css = fhp_css_compile("div > p:nth-child(2n+1), a[href^='/'], li:has(+ li), :empty, :root", NULL, 0);
    fhp_xpath *x1 = fhp_xpath_compile("//text() | //@* | //comment()", NULL, 0);
    fhp_xpath *x2 = fhp_xpath_compile("//*[contains(., 'a')]/following::text()[1]", NULL, 0);
    if (env && atoi(env) > 0) rounds = (size_t)atoi(env);
    snprintf(dir, sizeof dir, "%s/conformance/input", FHP_TESTDATA);
    names = list_dir(dir, ".html", &count);
    for (f = 0; f < count; f++) {
        size_t len, r;
        char *page;
        snprintf(path, sizeof path, "%s/%s.html", dir, names[f]);
        page = read_file(path, &len);
        for (r = 0; page && r < rounds; r++) {
            size_t cap = len * 2 + 4096, n = 0, i = 0;
            char *s = (char *)malloc(cap);
            fhp_doc *doc;
            size_t t;
            while (i < len) {
                size_t run = 1 + rnd(200), k;
                const char *piece;
                /* whole code points, as the Rust test cuts chars */
                for (k = 0; k < run && i < len; k++) {
                    do {
                        if (n + 8 >= cap) {
                            cap *= 2;
                            s = (char *)realloc(s, cap);
                        }
                        s[n++] = page[i++];
                    } while (i < len && ((unsigned char)page[i] & 0xC0) == 0x80);
                }
                if (rnd(4) == 0) {
                    size_t skip = rnd(50);
                    for (k = 0; k < skip && i < len; k++) {
                        i++;
                        while (i < len && ((unsigned char)page[i] & 0xC0) == 0x80) i++;
                    }
                }
                piece = pieces[rnd(npieces)];
                if (n + strlen(piece) + 8 >= cap) {
                    cap = cap * 2 + strlen(piece);
                    s = (char *)realloc(s, cap);
                }
                memcpy(s + n, piece, strlen(piece));
                n += strlen(piece);
            }
            doc = fhp_parse_url(s, n, "http://h.example/a/b?c#d");
            docs++;
            for (t = 0; t < fhp_doc_len(doc); t++) {
                fhp_node nd = (fhp_node)t;
                fhp_attr_iter it;
                fhp_query q;
                fhp_arena_reset(a);
                fhp_text(doc, nd, a);
                fhp_text_line_breaks(doc, nd, a);
                fhp_body(doc, nd);
                fhp_outer_html(doc, nd);
                fhp_source(doc, nd);
                fhp_href(doc, nd, a);
                fhp_src(doc, nd, a);
                fhp_attr(doc, nd, "href", a);
                fhp_attr(doc, nd, "class", a);
                fhp_has_class(doc, nd, "x");
                fhp_attrs_begin(doc, nd, &it);
                while (fhp_attrs_next(&it, NULL, NULL)) {
                }
                fhp_children(doc, nd, a);
                fhp_text_after_tag_until_next_one(doc, nd);
                fhp_text_no_tags_after_tag_until_next_one(doc, nd, a);
                fhp_href_mail_to(doc, nd, a);
                memset(&q, 0, sizeof q);
                q.text = "x";
                fhp_matches(doc, nd, &q);
                fhp_next_sibling(doc, nd);
                fhp_previous_sibling(doc, nd);
            }
            fhp_arena_reset(a);
            fhp_xpath_values(x1, doc, FHP_NONE, a);
            fhp_xpath_values(x2, doc, FHP_NONE, a);
            fhp_css_all(css, doc, FHP_NONE, a);
            fhp_free(doc);
            free(s);
        }
        free(page);
        free(names[f]);
    }
    free(names);
    fhp_css_free(css);
    fhp_xpath_free(x1);
    fhp_xpath_free(x2);
    fhp_arena_free(a);
    printf("mangled_pages: %u documents\n", (unsigned)docs);
}

/* The text is the visible text in one line, as jsoup's text() (the cases of TextOneLineTests in the .NET version). */
static void test_text_one_line(void) {
    static const char *const cases[][2] = {
        {"line1<br>line2", "line1 line2"}, {"line1<br><br><br>line2", "line1 line2"}, {"line1 <br> line2", "line1 line2"},
        {"line1<BR/>line2", "line1 line2"}, {"<p>a</p><p>b</p>", "a b"}, {"<ul><li>a</li><li>b</li></ul>", "a b"},
        {"<table><tr><td>Price</td><td>100</td></tr></table>", "Price 100"}, {"<h1>Title</h1>text", "Title text"},
        {"a<span>b</span>c", "abc"}, {"a<my-widget>b</my-widget>", "ab"}, {"a\nb", "a b"}, {"a\r\n\t  b", "a b"},
        {"a<!-- c -->b", "ab"}, {"<p>a</p><script>x()</script><p>b</p>", "a b"}, {"a &amp; b", "a & b"},
        {"x<pre>a\n  b</pre>y", "x a\n  b y"},
    };
    static const char *const leaves[][2] = {{"span", "a b"}, {"pre", "c\n  d"}, {"textarea", "e\n  f"}};
    const char *leaf_page = "<span>a\n   b</span><pre>c\n  d</pre><textarea>e\n  f</textarea>";
    fhp_arena *a = fhp_arena_new(0);
    char page[256];
    size_t k;
    fhp_doc *doc;
    for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        fhp_query q = {FHP_TAG_DIV};
        fhp_node div;
        fhp_str text;
        snprintf(page, sizeof page, "<div>%s</div>", cases[k][0]);
        doc = fhp_parse(page, strlen(page));
        div = fhp_find(doc, FHP_NONE, &q);
        text = fhp_text(doc, div, a);
        if (text.len != strlen(cases[k][1]) || memcmp(text.ptr, cases[k][1], text.len) != 0)
            fail("text of \"%s\": \"%.*s\", want \"%s\"", cases[k][0], (int)text.len, text.ptr, cases[k][1]);
        q.text = cases[k][1];
        CHECK(fhp_find(doc, FHP_NONE, &q) == div);
        fhp_free(doc);
    }
    doc = fhp_parse(leaf_page, strlen(leaf_page));
    for (k = 0; k < sizeof leaves / sizeof leaves[0]; k++) {
        fhp_query q = fhp_by_name(leaves[k][0]);
        fhp_str text = fhp_text(doc, fhp_find(doc, FHP_NONE, &q), a);
        if (text.len != strlen(leaves[k][1]) || memcmp(text.ptr, leaves[k][1], text.len) != 0)
            fail("text of <%s>: \"%.*s\", want \"%s\"", leaves[k][0], (int)text.len, text.ptr, leaves[k][1]);
    }
    fhp_free(doc);
    fhp_arena_free(a);
}

int main(int argc, char **argv) {
    int i;
    const char *only = NULL;
    for (i = 1; i < argc; i++) only = argv[i];
    if (!only || strcmp(only, "readme") == 0) test_readme();
    if (!only || strcmp(only, "text") == 0) test_text_one_line();
    if (!only || strcmp(only, "tags") == 0) test_tags_as_dotnet();
    if (!only || strcmp(only, "compile") == 0) test_compile_as_dotnet();
    if (!only || strcmp(only, "queries") == 0) test_queries_as_dotnet();
    if (!only || strcmp(only, "mangled") == 0) test_mangled_pages();
    fhp_thread_cleanup();
    if (failures) {
        printf("FAILED: %d problems\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}

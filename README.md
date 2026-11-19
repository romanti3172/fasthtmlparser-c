# fasthtmlparser (C)

C99 port of [z3l.FastHtmlParser](https://github.com/romanti3172/z3l.FastHtmlParser): a fast HTML / XML parser for
scraping, with **XPath 1.0** and **CSS selectors**. It is built for extracting data, not for building a DOM: a page
becomes a flat list of tags that hold only positions in the source. Text, attributes and entities are read when you
ask for them, and a value that needs no decoding is a slice of the page.

The whole API is ported: the parser, attributes, HTML5 entities, visible text and text with line breaks, `find` /
`find_all`, navigation, links, XPath 1.0, CSS selectors and the one-call shortcuts. Plain C99, no dependencies, one
header; memory comes from arenas (no allocation per tag or per value), and the API is made for FFI: tags are integer
indexes, strings are pointer + length, nothing has to be freed one by one. There are also ports to
[Go](https://github.com/romanti3172/fasthtmlparser), [Rust](https://github.com/romanti3172/fasthtmlparser-rs),
[Java](https://github.com/romanti3172/fasthtmlparser-java),
[JavaScript / TypeScript](https://github.com/romanti3172/fasthtmlparser-js),
[Python](https://github.com/romanti3172/fasthtmlparser-py), [PHP](https://github.com/romanti3172/fasthtmlparser-php)
and [Ruby](https://github.com/romanti3172/fasthtmlparser-rb).

## Build

CMake 3.16+, any C99 compiler: gcc, clang, MSVC (Visual Studio 2015 and later).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build        # the conformance tests
```

This builds the static library `fasthtmlparser` and the shared one (`fasthtmlparser.dll` / `libfasthtmlparser.so`,
`-DFHP_BUILD_SHARED=OFF` to skip it). In your own CMake project:

```cmake
include(FetchContent)
FetchContent_Declare(fasthtmlparser GIT_REPOSITORY https://github.com/romanti3172/fasthtmlparser-c.git GIT_TAG main)
set(FHP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fasthtmlparser)
target_link_libraries(your_app PRIVATE fasthtmlparser)   # or fasthtmlparser_shared
```

Or just add the files of `src/` to your build and `include/` to the include path. On x86-64 the byte searches use
SSE2; other CPUs get the portable code (`-DFHP_NO_SIMD` forces it).

## Example

```c
#include <stdio.h>
#include "fasthtmlparser.h"

static const char page[] =
    "<html><body>\n"
    "  <article class=\"product_pod\"><h3><a href=\"b/1\" title=\"A Light in the Attic\">A Light...</a></h3>\n"
    "    <p class=\"price_color\">£51.77</p><p class=\"star-rating Three\"></p></article>\n"
    "  <dl><dt>Price</dt><dd>£51.77</dd></dl>\n"
    "</body></html>";

int main(void) {
    fhp_doc *doc = fhp_parse_url(page, sizeof page - 1, "https://example.com/catalog/"); /* or fhp_parse(page, len) */
    fhp_arena *a = fhp_arena_new(0); /* results: decoded text, lists of tags, links */
    fhp_nodes pods;
    size_t i;

    /* CSS selectors, as querySelectorAll / querySelector / matches; compile once, reuse on any document */
    fhp_css *pod_css = fhp_css_compile("article.product_pod", NULL, 0);
    pods = fhp_css_all(pod_css, doc, FHP_NONE, a);
    for (i = 0; i < pods.len; i++) {
        fhp_str title = fhp_attr(doc, fhp_select_first(doc, pods.items[i], "h3 a"), "title", a);
        fhp_str price = fhp_text(doc, fhp_select_first(doc, pods.items[i], "p.price_color"), a);
        printf("%.*s: %.*s\n", (int)title.len, title.ptr, (int)price.len, price.ptr); /* A Light in the Attic: £51.77 */
    }

    /* XPath 1.0 */
    fhp_str price = fhp_xpath_string(doc, FHP_NONE, "//dt[.='Price']/following-sibling::dd[1]", a); /* "£51.77" */
    fhp_str count = fhp_xpath_string(doc, FHP_NONE, "count(//p)", a);                              /* "2" */
    fhp_xpath *titles = fhp_xpath_compile("//article//h3/a/@title", NULL, 0);
    fhp_strs values = fhp_xpath_values(titles, doc, FHP_NONE, a);                                   /* 1 value */

    /* search by name, class, id, attribute, text: a query struct, zero = any tag */
    fhp_query q = {FHP_TAG_ARTICLE};
    q.cls = "product_pod";
    fhp_node pod = fhp_find(doc, FHP_NONE, &q);
    fhp_query custom = fhp_by_name("my-widget"); /* any name works as a string */

    /* one-call shortcuts (find_by_name_*, find_all_by_name_*, is_name_*, parent_by_name, next_sibling_by_name, ...) */
    fhp_node dt = fhp_find_by_name_text(doc, FHP_NONE, "dt", "Price");
    fhp_node dd = fhp_next_sibling_by_name(doc, dt, "dd");

    /* reading a tag */
    fhp_node link = fhp_select_first(doc, FHP_NONE, "h3 a");
    fhp_text(doc, link, a);            /* "A Light...": visible text, entities decoded */
    fhp_attr(doc, link, FHP_ATTR_TITLE, a); /* decoded value; fhp_attr_raw: as written, a slice of the page */
    fhp_href(doc, link, a);            /* "https://example.com/catalog/b/1"; fhp_src for src */
    fhp_source(doc, link);             /* the opening tag without '<' and '>' */
    fhp_name(doc, link);               /* "a"; fhp_tag(doc, link) == FHP_TAG_A, fhp_depth(doc, link) == 4 */

    /* navigation */
    fhp_parent(doc, link);                      /* the <h3> */
    fhp_parent_by_name(doc, link, "article");   /* == pod */
    fhp_children(doc, pod, a);                  /* 3 tags */

    (void)price, (void)count, (void)values, (void)custom, (void)dd;
    fhp_css_free(pod_css);
    fhp_xpath_free(titles);
    fhp_arena_free(a);
    fhp_free(doc);
    return 0;
}
```

The example is built and run by the tests (`tests/example.c`); `test_readme` in `tests/main.c` checks the values.

## Conventions

- A tag (`fhp_node`) is its index in document order; `FHP_NONE` (-1) is "no tag". Search functions take the tag to
  search inside, or `FHP_NONE` for the whole document.
- Strings are `fhp_str` (pointer + length, **not** zero-terminated). A missing value is `""` (never a NULL pointer),
  a missing tag is `FHP_NONE`. Input strings (names, selectors, URLs) are zero-terminated UTF-8.
- Slices of the page (`fhp_body`, `fhp_source`, `fhp_attr_raw`, ...) live as long as the document. Built values
  (decoded text, lists, links) go to the `fhp_arena` you pass; `NULL` means the document's own arena, freed with the
  document. `fhp_arena_reset` makes an arena reusable, so a scraping loop allocates nothing in steady state.
- `fhp_parse` copies the page (invalid UTF-8 becomes U+FFFD, as Rust's `from_utf8_lossy`); `fhp_parse_borrowed`
  parses in place. Positions are byte offsets.
- A document is read-only: to read one document from several threads, call `fhp_doc_prepare(doc)` once and give each
  thread its own arena. Compiled `fhp_css` / `fhp_xpath` queries are immutable and shared freely. Per-thread scratch
  buffers are reused between calls; `fhp_thread_cleanup()` releases them.
- Errors: `fhp_css_compile` / `fhp_xpath_compile` return NULL and write a message; `fhp_xpath_nodes` returns -1 for an
  expression that gives a number or string (use `fhp_xpath_values` / `fhp_xpath_value`). The compile-and-run helpers
  (`fhp_select`, `fhp_select_first`, `fhp_xpath_select`, `fhp_xpath_string`) do the same in one call.

Queries (`fhp_query`): `tag` or `name`, then `cls`, `id`, `attr` + `attr_value`, `text`, `id_starts_with`,
`text_starts_with`, `body`, `body_starts_with`, `source`. `FHP_TAG_*` and `FHP_ATTR_*` cover about 250 tag and 230
attribute names (HTML, SVG, MathML, RSS, Atom, sitemap); any other name works as a string.

CSS: type, `*`, `#id`, `.class`, attribute selectors with all operators and the `i` / `s` flags, the four combinators,
selector lists, `:first-child`, `:last-child`, `:only-child`, `:*-of-type`, `:nth-child(An+B [of S])` and its relatives,
`:not()`, `:is()`, `:where()`, `:has()`, `:empty`, `:root`, `:scope`, `:link`, `:checked`, `:disabled`, `:enabled`,
`:required`, `:optional`, `:contains("text")`. XPath: all axes except namespace, predicates, `|`, the operators and all
XPath 1.0 functions, plus `ends-with()`, `lower-case()`, `upper-case()`. HTML rules apply: names are case-insensitive,
the string-value of an element is its visible text, attribute values and text are decoded.

## Same results as the .NET version

The conformance tests (`tests/main.c`) compare the C port with z3l.FastHtmlParser 1.4.1 (.NET) on the 117 pages of the
Go and Rust ports: the test fixtures, broken-markup snippets, edge cases (entities, raw text, comments, XML, custom
elements, 600-deep nesting, Unicode whitespace, links of every kind, line breaks), every fixture with `</li> </p> </td>
</tr>` left out, copies with random end tags dropped, names upper-cased and the page cut off, and copies saved with CR
LF and with a lone CR.

- `tags`: every tag (over 15,000): names, parents, depths, positions (UTF-8 byte offsets), visible text, text with line
  breaks, 10 attribute values, the opening tag source, next / previous tag and sibling, children, the resolved href and
  src, mailto: and tel: values, and the text after the tag must be identical.
- `compile` and `queries`: 569 queries. Whether each one compiles must be the same (119 are invalid); the 228 XPath and
  222 CSS queries that compile are run on every page, on the document and from three tags of the page, and every CSS
  selector is also matched against every tag: 252,447 checks of selected tags and values.
- `mangled`: the pages cut, spliced and sprinkled with markup and multi-byte text; every function on every tag and a
  set of queries must run cleanly (`FASTHTML_FUZZ_ROUNDS=50` for a longer run; it passes under AddressSanitizer).

All pass with MSVC and gcc, with one documented exception, as in the other ports: on the page nested 600 levels deep,
where the .NET result depends on the order of an unstable sort (`snip039 //div/node()`).

Intended differences, as in the Go and Rust ports: `fhp_text_line_breaks` joins lines with `"\n"` on every OS, and a
missing value is `""` (or `FHP_NONE` for a tag) where the .NET version returns null. Links resolved against the page
are written as .NET's `System.Uri` writes them (host in lower case, no default port, readable escapes); absolute links
are returned as written.

## Benchmarks

`bench/` builds against [lexbor](https://github.com/lexbor/lexbor) 3.0.1 (a fast HTML5 parser with CSS selectors) and
[Gumbo](https://codeberg.org/gumbo-parser/gumbo-parser) 0.14.1 (Google's HTML5 parser, no selectors: the tree is
walked by hand), fetched by CMake; the library itself does not depend on them. Same pages and tasks as the other ports;
every library must extract the same records before anything is timed.

```sh
cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build bench/build
bench/build/fhp_bench
```

AMD Ryzen 7 3800X, Windows, MSVC 19.52 (full tables in [`bench-results.md`](bench-results.md): Windows with MSVC and
gcc 16, Linux with gcc 14 and clang 19 from `docker build -f bench/Dockerfile -t fhp-bench-c .`). Parse a page:

| Page | fasthtmlparser | lexbor | lexbor, document reused | Gumbo |
|---|---:|---:|---:|---:|
| quotes, 11 KB | 15 μs | 122 μs | 94 μs | 653 μs |
| laptops, 38 KB | 46 μs | 364 μs | 302 μs | 1,956 μs |
| books, 51 KB | 50 μs | 354 μs | 314 μs | 2,696 μs |
| books x10, 510 KB | 554 μs | 4,581 μs | 4,070 μs | 28,161 μs |
| books without `</li>` `</p>` | 50 μs | 305 μs | 295 μs | 2,726 μs |

Scrape a page: parse and extract the records. Books: 20 × title, price, rating; quotes: 10 × text, author, tags.

| Task | fasthtmlparser | Other |
|---|---:|---:|
| books, `find` | 70 μs | |
| books, CSS | 71 μs | lexbor 356 μs |
| books, XPath | 105 μs | |
| books, tree walk | | Gumbo 2,722 μs |
| quotes, `find` | 25 μs | |
| quotes, CSS | 24 μs | lexbor 123 μs |
| quotes, tree walk | | Gumbo 686 μs |

On Linux (same machine, gcc 14) the gaps are the same or wider: books 51 KB in 44 μs (fasthtmlparser) against 484 μs
(lexbor; 282 μs with the document reused) and 2,085 μs (Gumbo); the books scrape with CSS in 56 μs against 292 μs
(lexbor) and 2,020 μs (Gumbo).

These are different tools: lexbor and Gumbo build a full HTML5 DOM by the spec's tree-construction rules (implied
`<html>` / `<head>` / `<body>`, the adoption agency, foster parenting), which a scraper rarely needs and which this
parser does not do; it applies the HTML5 optional-end-tag rules and keeps the tags as written. The comparison is the
one a scraper sees: the same records out of the same pages.

## Layout

- `include/fasthtmlparser.h`: the whole API; `include/fasthtmlparser_names.h`: `FHP_TAG_*`, `FHP_ATTR_*` (generated)
- `src/parse.c`: the scanner and the HTML5 optional-end-tag rules (port of `ParsingCore.cs`)
- `src/attributes.c`, `src/decode.c`, `src/text.c`: attributes, character references, visible text, text with line
  breaks
- `src/document.c`: documents, reading tags, navigation, queries; `src/links.c`: href / src, mailto:, tel:
- `src/css.c`: CSS selectors; `src/xpath.c`: XPath 1.0; `src/shortcuts.c`: the one-call shortcuts
- `src/util.c`: arenas, byte search (SSE2), UTF-8 and Unicode helpers
- `src/tables_gen.c`, `src/unicode_gen.c`: generated by `python tools/gen.py ../fasthtmlparser-rs` from the tables of
  the Rust port (tag names, attribute names, entities) and Python's `unicodedata` (case mappings, letters, whitespace)
- `tests/`: the conformance tests (with a small JSON reader and SHA-256 of their own)
- `bench/`: benchmarks in their own CMake project, so the library has no dependencies

## License

Same terms as z3l.FastHtmlParser (see LICENSE.md): free for companies under USD 1M annual revenue and under 100
people, a commercial license above that. This is source-available, not an OSI open-source license.

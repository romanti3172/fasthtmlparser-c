/*
 * fasthtmlparser: a fast HTML / XML parser for scraping, with XPath 1.0 and CSS selectors.
 * A C99 port of z3l.FastHtmlParser (https://github.com/romanti3172/z3l.FastHtmlParser).
 *
 * A document is a flat list of tags in document order; each tag holds only positions in the source. Text, attributes
 * and entities are read lazily, when they are asked for, and a value that needs no decoding is a slice of the page.
 *
 * Conventions
 * - A tag is an index (fhp_node) into the document's tag list; FHP_NONE (-1) is "no tag". Functions that search
 *   take a tag to search inside, or FHP_NONE for the whole document.
 * - Strings are fhp_str: a pointer and a length, NOT zero-terminated. A missing value is an empty string
 *   (len 0, ptr never NULL). Input strings (names, selectors, URLs) are zero-terminated UTF-8.
 * - Values that are slices of the page (fhp_body, fhp_attr_raw, ...) live as long as the document. Values that are
 *   built (decoded text, lists of tags, links) are allocated in an fhp_arena the caller passes; NULL means the
 *   document's own arena (freed with the document, or emptied by fhp_arena_reset(fhp_doc_arena(doc))).
 * - A document is read-only after parsing. To read one document from several threads at once, give each thread
 *   its own arena (not NULL) and call fhp_doc_prepare() once before sharing it. Compiled CSS / XPath queries are
 *   immutable and can be shared by any number of threads and documents.
 * - The library keeps per-thread scratch buffers (reused between calls, no allocation in steady state);
 *   fhp_thread_cleanup() releases them when a thread is done with the library.
 */
#ifndef FASTHTMLPARSER_H
#define FASTHTMLPARSER_H

#include <stddef.h>
#include <stdint.h>

#include "fasthtmlparser_names.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(FHP_SHARED) && defined(_WIN32)
#if defined(FHP_BUILDING)
#define FHP_API __declspec(dllexport)
#else
#define FHP_API __declspec(dllimport)
#endif
#elif defined(FHP_SHARED) && defined(__GNUC__)
#define FHP_API __attribute__((visibility("default")))
#else
#define FHP_API
#endif

#define FHP_VERSION "0.1.3"

typedef struct fhp_doc fhp_doc;
typedef struct fhp_arena fhp_arena;
typedef struct fhp_css fhp_css;
typedef struct fhp_xpath fhp_xpath;

/* A tag: its index in document order. */
typedef int32_t fhp_node;
#define FHP_NONE (-1)

typedef struct fhp_str {
    const char *ptr;
    size_t len;
} fhp_str;

typedef struct fhp_nodes {
    const fhp_node *items;
    size_t len;
} fhp_nodes;

typedef struct fhp_strs {
    const fhp_str *items;
    size_t len;
} fhp_strs;

/* ----- arenas ----- */

/* A bump allocator for results; chunk_size 0 picks a default (16 KB). */
FHP_API fhp_arena *fhp_arena_new(size_t chunk_size);
/* Makes all the memory reusable (everything allocated in it becomes invalid); keeps the chunks. */
FHP_API void fhp_arena_reset(fhp_arena *a);
FHP_API void fhp_arena_free(fhp_arena *a);
/* Bytes handed out since the last reset. */
FHP_API size_t fhp_arena_used(const fhp_arena *a);

/* ----- documents ----- */

/* Parses an HTML or XML page. The source is copied; invalid UTF-8 sequences become U+FFFD. NULL when out of memory. */
FHP_API fhp_doc *fhp_parse(const char *src, size_t len);
/* fhp_parse and records the page URL: relative links (fhp_href, fhp_src) are resolved against it. */
FHP_API fhp_doc *fhp_parse_url(const char *src, size_t len, const char *page_url);
/* Parses without copying: src must stay valid and unchanged while the document lives, and should be valid UTF-8
   (other bytes are safe, but are read as they are). page_url may be NULL. */
FHP_API fhp_doc *fhp_parse_borrowed(const char *src, size_t len, const char *page_url);
FHP_API void fhp_free(fhp_doc *doc);

FHP_API fhp_str fhp_doc_source(const fhp_doc *doc);
/* The page URL given to fhp_parse_url, or "". */
FHP_API fhp_str fhp_doc_url(const fhp_doc *doc);
/* The number of tags (comments included). */
FHP_API size_t fhp_doc_len(const fhp_doc *doc);
/* The document's own result arena (what NULL stands for). */
FHP_API fhp_arena *fhp_doc_arena(const fhp_doc *doc);
/* Builds the lazy per-document data of the CSS selectors now, so that the document is safe to share between threads. */
FHP_API void fhp_doc_prepare(const fhp_doc *doc);
/* Releases the scratch buffers of the calling thread. */
FHP_API void fhp_thread_cleanup(void);

/* ----- tag names ----- */

/* The lower-case name of a known tag ("div", "!--" for comments); "" for FHP_TAG_NONE. */
FHP_API const char *fhp_tag_name(int tag);
/* The FHP_TAG_* of a name (ASCII case-insensitive), or FHP_TAG_NONE. */
FHP_API int fhp_lookup_tag(const char *name);

/* ----- reading a tag ----- */

/* FHP_TAG_* of the tag, or FHP_TAG_NONE for a name that is not a known one. */
FHP_API int fhp_tag(const fhp_doc *doc, fhp_node n);
/* The lower-case tag name ("div", "!--" for comments, "custom-tag", ...). */
FHP_API fhp_str fhp_name(const fhp_doc *doc, fhp_node n);
/* The nesting depth: 0 for top-level tags. */
FHP_API int fhp_depth(const fhp_doc *doc, fhp_node n);
/* Byte offsets of the opening '<' and of the last '>' (of the end tag when there is one). */
FHP_API void fhp_pos(const fhp_doc *doc, fhp_node n, size_t *start, size_t *end);
/* The inner HTML (trimmed); "" for tags that are not closed. */
FHP_API fhp_str fhp_body(const fhp_doc *doc, fhp_node n);
/* The source of the whole element; "" for tags that are not closed. */
FHP_API fhp_str fhp_outer_html(const fhp_doc *doc, fhp_node n);
/* The opening tag without '<' and '>': the name and the attributes. */
FHP_API fhp_str fhp_source(const fhp_doc *doc, fhp_node n);
/* The visible text with entities decoded: tags, comments and the content of script, style, noscript, template,
   noframes, noembed removed, whitespace collapsed; without child tags the inner text as is (trimmed). */
FHP_API fhp_str fhp_text(const fhp_doc *doc, fhp_node n, fhp_arena *a);
/* The text with the visible line structure kept (<br>, block elements and paragraphs give line breaks). */
FHP_API fhp_str fhp_text_line_breaks(const fhp_doc *doc, fhp_node n, fhp_arena *a);
/* Whether another tag starts inside this one. */
FHP_API int fhp_has_children(const fhp_doc *doc, fhp_node n);

/* The decoded value of the first attribute with the name (case-insensitive); "" when it is missing or empty. */
FHP_API fhp_str fhp_attr(const fhp_doc *doc, fhp_node n, const char *name, fhp_arena *a);
/* The value as written in the source (without quotes); "" for a missing attribute or one without a value. */
FHP_API fhp_str fhp_attr_raw(const fhp_doc *doc, fhp_node n, const char *name);
/* Whether one of the space-separated classes equals cls (case-insensitive). */
FHP_API int fhp_has_class(const fhp_doc *doc, fhp_node n, const char *cls);

/* The attributes of the opening tag in source order: (name, raw value) pairs.
       fhp_attr_iter it; fhp_str name, value;
       fhp_attrs_begin(doc, n, &it);
       while (fhp_attrs_next(&it, &name, &value)) ...                                                              */
typedef struct fhp_attr_iter {
    const char *src;
    size_t pos, end;
} fhp_attr_iter;
FHP_API void fhp_attrs_begin(const fhp_doc *doc, fhp_node n, fhp_attr_iter *it);
FHP_API int fhp_attrs_next(fhp_attr_iter *it, fhp_str *name, fhp_str *raw_value);

/* The link a browser follows: href decoded and made absolute against the page URL; an empty or missing href gives
   the page URL. */
FHP_API fhp_str fhp_href(const fhp_doc *doc, fhp_node n, fhp_arena *a);
FHP_API fhp_str fhp_href_from(const fhp_doc *doc, fhp_node n, const char *base_url, fhp_arena *a);
FHP_API fhp_str fhp_src(const fhp_doc *doc, fhp_node n, fhp_arena *a);
FHP_API fhp_str fhp_src_from(const fhp_doc *doc, fhp_node n, const char *base_url, fhp_arena *a);
/* The address of a mailto: link / the number of a tel: link (after the first ':'), or "". */
FHP_API fhp_str fhp_href_mail_to(const fhp_doc *doc, fhp_node n, fhp_arena *a);
FHP_API fhp_str fhp_href_telephone(const fhp_doc *doc, fhp_node n, fhp_arena *a);

/* ----- navigation ----- */

FHP_API fhp_node fhp_parent(const fhp_doc *doc, fhp_node n);
/* The direct children: the tags inside this one, one level deeper. */
FHP_API fhp_nodes fhp_children(const fhp_doc *doc, fhp_node n, fhp_arena *a);
/* The tag that follows / precedes in document order. */
FHP_API fhp_node fhp_next(const fhp_doc *doc, fhp_node n);
FHP_API fhp_node fhp_previous(const fhp_doc *doc, fhp_node n);
/* The next / previous tag at the same level inside the same parent. */
FHP_API fhp_node fhp_next_sibling(const fhp_doc *doc, fhp_node n);
FHP_API fhp_node fhp_previous_sibling(const fhp_doc *doc, fhp_node n);
/* The source right after the tag up to the next '<' (or the end of the page). */
FHP_API fhp_str fhp_text_after_tag_until_next_one(const fhp_doc *doc, fhp_node n);
/* The same as visible text: entities decoded, whitespace collapsed. */
FHP_API fhp_str fhp_text_no_tags_after_tag_until_next_one(const fhp_doc *doc, fhp_node n, fhp_arena *a);

/* ----- search -----
   A query selects tags by name and, optionally, class, id, attribute value, visible text, inner HTML and the source
   of the opening tag. A NULL field is no criterion. Zero-initialized it selects every tag:
       fhp_query q = {FHP_TAG_ARTICLE};  q.cls = "product_pod";
       fhp_node pod = fhp_find(doc, FHP_NONE, &q);                                                                */
typedef struct fhp_query {
    int tag;                      /* FHP_TAG_*; FHP_TAG_NONE: any tag, or the one named by name */
    const char *name;             /* any name, case-insensitive (for names that are not in FHP_TAG_*) */
    const char *cls;              /* one of the classes, case-insensitive */
    const char *id;               /* the decoded id */
    const char *attr;             /* the decoded value of attribute attr equals attr_value */
    const char *attr_value;
    const char *text;             /* the visible text (fhp_text) */
    const char *id_starts_with;
    const char *text_starts_with;
    const char *body;             /* the inner HTML (trimmed) */
    const char *body_starts_with;
    const char *source;           /* the opening tag without '<' and '>' (fhp_source) */
} fhp_query;

/* A query by name: the FHP_TAG_* when the name is a known one, the name otherwise. */
FHP_API fhp_query fhp_by_name(const char *name);
FHP_API int fhp_matches(const fhp_doc *doc, fhp_node n, const fhp_query *q);
/* The first / all tags inside scope (FHP_NONE: the document) that match q. */
FHP_API fhp_node fhp_find(const fhp_doc *doc, fhp_node scope, const fhp_query *q);
FHP_API fhp_nodes fhp_find_all(const fhp_doc *doc, fhp_node scope, const fhp_query *q, fhp_arena *a);
/* The nearest enclosing tag / next sibling / previous sibling that matches q. */
FHP_API fhp_node fhp_parent_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q);
FHP_API fhp_node fhp_next_sibling_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q);
FHP_API fhp_node fhp_previous_sibling_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q);
/* The first tag after the end of this one that matches q (tags inside it are skipped). */
FHP_API fhp_node fhp_next_matching(const fhp_doc *doc, fhp_node n, const fhp_query *q);
/* The source between the end of the tag and the next tag that matches q; "" when there is none. */
FHP_API fhp_str fhp_text_after_until(const fhp_doc *doc, fhp_node n, const fhp_query *q);

/* ----- one-call shortcuts of z3l.FastHtmlParser (name: any tag name; scope: a tag, or FHP_NONE) ----- */

FHP_API fhp_node fhp_find_by_id(const fhp_doc *doc, fhp_node scope, const char *id);
FHP_API fhp_node fhp_find_by_name(const fhp_doc *doc, fhp_node scope, const char *name);
FHP_API fhp_node fhp_find_by_name_class(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls);
FHP_API fhp_node fhp_find_by_name_id(const fhp_doc *doc, fhp_node scope, const char *name, const char *id);
FHP_API fhp_node fhp_find_by_name_text(const fhp_doc *doc, fhp_node scope, const char *name, const char *text);
FHP_API fhp_node fhp_find_by_name_body(const fhp_doc *doc, fhp_node scope, const char *name, const char *body);
FHP_API fhp_node fhp_find_by_name_attribute(const fhp_doc *doc, fhp_node scope, const char *name,
                                            const char *attribute, const char *value);
FHP_API fhp_node fhp_find_by_name_itemprop(const fhp_doc *doc, fhp_node scope, const char *name, const char *itemprop);
/* class / id / text are skipped when NULL or empty */
FHP_API fhp_node fhp_find_by_name_class_text(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls,
                                             const char *text);
FHP_API fhp_node fhp_find_by_name_id_text(const fhp_doc *doc, fhp_node scope, const char *name, const char *id,
                                          const char *text);
FHP_API fhp_node fhp_find_by_name_class_body(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls,
                                             const char *body);
FHP_API fhp_node fhp_find_by_name_id_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                 const char *prefix);
FHP_API fhp_node fhp_find_by_name_body_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                   const char *prefix);
/* FHP_NONE for an empty prefix */
FHP_API fhp_node fhp_find_by_name_text_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                   const char *prefix);
FHP_API fhp_node fhp_find_by_name_class_text_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                         const char *cls, const char *prefix);
FHP_API fhp_nodes fhp_find_all_by_name(const fhp_doc *doc, fhp_node scope, const char *name, fhp_arena *a);
FHP_API fhp_nodes fhp_find_all_by_name_class(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls,
                                             fhp_arena *a);
FHP_API fhp_nodes fhp_find_all_by_name_id(const fhp_doc *doc, fhp_node scope, const char *name, const char *id,
                                          fhp_arena *a);
FHP_API fhp_nodes fhp_find_all_by_name_attribute(const fhp_doc *doc, fhp_node scope, const char *name,
                                                 const char *attribute, const char *value, fhp_arena *a);
FHP_API fhp_nodes fhp_find_all_by_name_itemprop(const fhp_doc *doc, fhp_node scope, const char *name,
                                                const char *itemprop, fhp_arena *a);
FHP_API fhp_nodes fhp_find_all_by_name_id_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                      const char *prefix, fhp_arena *a);
FHP_API fhp_nodes fhp_find_all_by_name_body_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                        const char *prefix, fhp_arena *a);

FHP_API int fhp_is_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls);
FHP_API int fhp_is_name_id(const fhp_doc *doc, fhp_node n, const char *name, const char *id);
FHP_API int fhp_is_name_text(const fhp_doc *doc, fhp_node n, const char *name, const char *text);
FHP_API int fhp_is_name_body(const fhp_doc *doc, fhp_node n, const char *name, const char *body);
FHP_API int fhp_is_name_attribute(const fhp_doc *doc, fhp_node n, const char *name, const char *attribute,
                                  const char *value);
FHP_API int fhp_is_name_itemprop(const fhp_doc *doc, fhp_node n, const char *name, const char *itemprop);
FHP_API int fhp_is_name_source(const fhp_doc *doc, fhp_node n, const char *name, const char *source);
FHP_API int fhp_is_name_class_body(const fhp_doc *doc, fhp_node n, const char *name, const char *cls,
                                   const char *body);
/* class / id / text / body are skipped when NULL or empty */
FHP_API int fhp_is_name_class_text(const fhp_doc *doc, fhp_node n, const char *name, const char *cls,
                                   const char *text);
FHP_API int fhp_is_name_id_text(const fhp_doc *doc, fhp_node n, const char *name, const char *id, const char *text);
FHP_API int fhp_is_name_class_id_body(const fhp_doc *doc, fhp_node n, const char *name, const char *cls,
                                      const char *id, const char *body);
FHP_API int fhp_is_name_text_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *prefix);
FHP_API int fhp_is_name_body_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *prefix);
FHP_API int fhp_is_name_id_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *prefix);
FHP_API int fhp_is_name_class_text_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *cls,
                                               const char *prefix);

FHP_API fhp_node fhp_parent_by_name(const fhp_doc *doc, fhp_node n, const char *name);
FHP_API fhp_node fhp_parent_by_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls);
FHP_API fhp_node fhp_next_by_name(const fhp_doc *doc, fhp_node n, const char *name);
FHP_API fhp_node fhp_next_by_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls);
FHP_API fhp_node fhp_next_sibling_by_name(const fhp_doc *doc, fhp_node n, const char *name);
FHP_API fhp_node fhp_next_sibling_by_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls);
FHP_API fhp_node fhp_previous_sibling_by_name(const fhp_doc *doc, fhp_node n, const char *name);
FHP_API fhp_str fhp_text_after_until_tag_by_name(const fhp_doc *doc, fhp_node n, const char *name);
FHP_API fhp_str fhp_text_after_until_tag_by_name_class(const fhp_doc *doc, fhp_node n, const char *name,
                                                       const char *cls);

/* The tags of the list at the level of the first one (the outermost tags of a find_all result), optionally with
   the name (and the class). */
FHP_API fhp_nodes fhp_first_level(const fhp_doc *doc, fhp_nodes list, fhp_arena *a);
FHP_API fhp_nodes fhp_first_level_by_name(const fhp_doc *doc, fhp_nodes list, const char *name, fhp_arena *a);
FHP_API fhp_nodes fhp_first_level_by_name_class(const fhp_doc *doc, fhp_nodes list, const char *name, const char *cls,
                                                fhp_arena *a);

/* ----- CSS selectors -----
   Type, *, #id, .class, attribute selectors ([a], =, ~=, |=, ^=, $=, *=, with the i / s flags), the four
   combinators, selector lists, :first-child, :last-child, :only-child, :*-of-type, :nth-child(An+B [of S]) and its
   relatives, :not(), :is(), :where(), :has(), :empty, :root, :scope, :link, :checked, :disabled, :enabled,
   :required, :optional, :contains("text"). As querySelectorAll: a selector run on a tag returns the tags inside it
   while the whole selector is matched against the document; :scope is the tag, and a selector may start with '>'
   for its children. */

/* NULL on an invalid selector; the message goes to err (when not NULL). */
FHP_API fhp_css *fhp_css_compile(const char *selector, char *err, size_t err_size);
FHP_API void fhp_css_free(fhp_css *q);
FHP_API fhp_nodes fhp_css_all(const fhp_css *q, const fhp_doc *doc, fhp_node scope, fhp_arena *a);
FHP_API fhp_node fhp_css_first(const fhp_css *q, const fhp_doc *doc, fhp_node scope);
/* Whether the tag matches (as element.matches()); false for comments. */
FHP_API int fhp_css_matches(const fhp_css *q, const fhp_doc *doc, fhp_node n);
/* Compile-and-run in one call: 0, or -1 for an invalid selector (out is then empty). */
FHP_API int fhp_select(const fhp_doc *doc, fhp_node scope, const char *selector, fhp_arena *a, fhp_nodes *out);
/* The first match, or FHP_NONE (also for an invalid selector). */
FHP_API fhp_node fhp_select_first(const fhp_doc *doc, fhp_node scope, const char *selector);

/* ----- XPath 1.0 -----
   All axes except namespace, predicates, '|', the operators and the XPath 1.0 functions, plus ends-with(),
   lower-case() and upper-case(). HTML rules apply: names are case-insensitive, the string-value of an element is
   its visible text, attribute values and text are decoded. On a tag, relative paths start at it. */

FHP_API fhp_xpath *fhp_xpath_compile(const char *expression, char *err, size_t err_size);
FHP_API void fhp_xpath_free(fhp_xpath *q);
/* Whether the expression gives a node-set (and so fhp_xpath_nodes can be used). */
FHP_API int fhp_xpath_is_node_set(const fhp_xpath *q);
/* The selected tags (elements and comments) in document order: 0, or -1 when the expression is no node-set. */
FHP_API int fhp_xpath_nodes(const fhp_xpath *q, const fhp_doc *doc, fhp_node context, fhp_arena *a, fhp_nodes *out);
/* The string-values of the selected nodes, or the one value of an expression that gives a number, string or
   boolean. */
FHP_API fhp_strs fhp_xpath_values(const fhp_xpath *q, const fhp_doc *doc, fhp_node context, fhp_arena *a);
/* The first value: 1, or 0 when the expression selects no node (out is then ""). */
FHP_API int fhp_xpath_value(const fhp_xpath *q, const fhp_doc *doc, fhp_node context, fhp_arena *a, fhp_str *out);
/* Compile-and-run in one call: 0, or -1 for an invalid expression or one that gives no node-set. */
FHP_API int fhp_xpath_select(const fhp_doc *doc, fhp_node context, const char *expression, fhp_arena *a,
                             fhp_nodes *out);
/* The first value, or "" (also for an invalid expression). */
FHP_API fhp_str fhp_xpath_string(const fhp_doc *doc, fhp_node context, const char *expression, fhp_arena *a);

/* ----- text helpers ----- */

/* Decodes the character references of a text; CR LF and a lone CR become LF. a may not be NULL. */
FHP_API fhp_str fhp_decode_html(const char *s, size_t len, fhp_arena *a);
/* The visible text of an HTML fragment, as fhp_text gives it for a tag's content. */
FHP_API fhp_str fhp_html_to_text(const char *html, size_t len, fhp_arena *a);
/* The text of an HTML fragment with the line structure kept, as fhp_text_line_breaks. */
FHP_API fhp_str fhp_html_to_text_line_breaks(const char *html, size_t len, fhp_arena *a);

#ifdef __cplusplus
}
#endif

#endif /* FASTHTMLPARSER_H */

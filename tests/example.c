/* The example of the README, kept compiling and running. */
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

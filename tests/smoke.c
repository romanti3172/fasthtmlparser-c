/* Links against the shared library: the exported API works across the DLL / .so boundary. */
#include <stdio.h>
#include <string.h>

#include "fasthtmlparser.h"

int main(void) {
    static const char page[] = "<div class=a><p>one</p><p>two &amp; three</p></div>";
    fhp_doc *doc = fhp_parse(page, sizeof page - 1);
    fhp_arena *a = fhp_arena_new(0);
    fhp_nodes ps;
    fhp_str t;
    int ok = doc != NULL;
    ok = ok && fhp_select(doc, FHP_NONE, "div.a > p", a, &ps) == 0 && ps.len == 2;
    t = ok ? fhp_text(doc, ps.items[1], a) : fhp_xpath_string(doc, FHP_NONE, "x", a);
    ok = ok && t.len == 11 && memcmp(t.ptr, "two & three", 11) == 0;
    t = fhp_xpath_string(doc, FHP_NONE, "count(//p)", a);
    ok = ok && t.len == 1 && t.ptr[0] == '2';
    fhp_arena_free(a);
    fhp_free(doc);
    fhp_thread_cleanup();
    printf(ok ? "shared library: ok\n" : "shared library: FAILED\n");
    return ok ? 0 : 1;
}

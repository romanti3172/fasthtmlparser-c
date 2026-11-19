/* The one-call shortcuts of z3l.FastHtmlParser (find_by_name_class, is_name_itemprop, parent_by_name, ...). */
#include "internal.h"

/* The older "optional" shortcuts skip an empty criterion. */
FHP_INLINE const char *opt(const char *s) { return s && s[0] ? s : NULL; }

#define Q(name) fhp_query q = fhp_by_name(name)

fhp_node fhp_find_by_id(const fhp_doc *doc, fhp_node scope, const char *id) {
    fhp_query q;
    if (!id || !id[0]) return FHP_NONE;
    memset(&q, 0, sizeof q);
    q.id = id;
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name(const fhp_doc *doc, fhp_node scope, const char *name) {
    Q(name);
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_class(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls) {
    Q(name);
    q.cls = cls ? cls : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_id(const fhp_doc *doc, fhp_node scope, const char *name, const char *id) {
    Q(name);
    q.id = id ? id : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_text(const fhp_doc *doc, fhp_node scope, const char *name, const char *text) {
    Q(name);
    q.text = text ? text : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_body(const fhp_doc *doc, fhp_node scope, const char *name, const char *body) {
    Q(name);
    q.body = body ? body : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_attribute(const fhp_doc *doc, fhp_node scope, const char *name, const char *attribute,
                                    const char *value) {
    Q(name);
    q.attr = attribute ? attribute : "";
    q.attr_value = value ? value : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_itemprop(const fhp_doc *doc, fhp_node scope, const char *name, const char *itemprop) {
    return fhp_find_by_name_attribute(doc, scope, name, FHP_ATTR_ITEMPROP, itemprop);
}

fhp_node fhp_find_by_name_class_text(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls,
                                     const char *text) {
    Q(name);
    q.cls = opt(cls);
    q.text = opt(text);
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_id_text(const fhp_doc *doc, fhp_node scope, const char *name, const char *id,
                                  const char *text) {
    Q(name);
    q.id = opt(id);
    q.text = opt(text);
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_class_body(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls,
                                     const char *body) {
    Q(name);
    q.cls = cls ? cls : "";
    q.body = body ? body : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_id_starts_with(const fhp_doc *doc, fhp_node scope, const char *name, const char *prefix) {
    Q(name);
    q.id_starts_with = prefix ? prefix : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_body_starts_with(const fhp_doc *doc, fhp_node scope, const char *name, const char *prefix) {
    Q(name);
    q.body_starts_with = prefix ? prefix : "";
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_text_starts_with(const fhp_doc *doc, fhp_node scope, const char *name, const char *prefix) {
    Q(name);
    if (!prefix || !prefix[0]) return FHP_NONE;
    q.text_starts_with = prefix;
    return fhp_find(doc, scope, &q);
}

fhp_node fhp_find_by_name_class_text_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                 const char *cls, const char *prefix) {
    Q(name);
    if (!prefix || !prefix[0]) return FHP_NONE;
    q.cls = cls ? cls : "";
    q.text_starts_with = prefix;
    return fhp_find(doc, scope, &q);
}

fhp_nodes fhp_find_all_by_name(const fhp_doc *doc, fhp_node scope, const char *name, fhp_arena *a) {
    Q(name);
    return fhp_find_all(doc, scope, &q, a);
}

fhp_nodes fhp_find_all_by_name_class(const fhp_doc *doc, fhp_node scope, const char *name, const char *cls,
                                     fhp_arena *a) {
    Q(name);
    q.cls = cls ? cls : "";
    return fhp_find_all(doc, scope, &q, a);
}

fhp_nodes fhp_find_all_by_name_id(const fhp_doc *doc, fhp_node scope, const char *name, const char *id, fhp_arena *a) {
    Q(name);
    q.id = id ? id : "";
    return fhp_find_all(doc, scope, &q, a);
}

fhp_nodes fhp_find_all_by_name_attribute(const fhp_doc *doc, fhp_node scope, const char *name, const char *attribute,
                                         const char *value, fhp_arena *a) {
    Q(name);
    q.attr = attribute ? attribute : "";
    q.attr_value = value ? value : "";
    return fhp_find_all(doc, scope, &q, a);
}

fhp_nodes fhp_find_all_by_name_itemprop(const fhp_doc *doc, fhp_node scope, const char *name, const char *itemprop,
                                        fhp_arena *a) {
    return fhp_find_all_by_name_attribute(doc, scope, name, FHP_ATTR_ITEMPROP, itemprop, a);
}

fhp_nodes fhp_find_all_by_name_id_starts_with(const fhp_doc *doc, fhp_node scope, const char *name, const char *prefix,
                                              fhp_arena *a) {
    Q(name);
    q.id_starts_with = prefix ? prefix : "";
    return fhp_find_all(doc, scope, &q, a);
}

fhp_nodes fhp_find_all_by_name_body_starts_with(const fhp_doc *doc, fhp_node scope, const char *name,
                                                const char *prefix, fhp_arena *a) {
    Q(name);
    q.body_starts_with = prefix ? prefix : "";
    return fhp_find_all(doc, scope, &q, a);
}

int fhp_is_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls) {
    Q(name);
    q.cls = cls ? cls : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_id(const fhp_doc *doc, fhp_node n, const char *name, const char *id) {
    Q(name);
    q.id = id ? id : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_text(const fhp_doc *doc, fhp_node n, const char *name, const char *text) {
    Q(name);
    q.text = text ? text : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_body(const fhp_doc *doc, fhp_node n, const char *name, const char *body) {
    Q(name);
    q.body = body ? body : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_attribute(const fhp_doc *doc, fhp_node n, const char *name, const char *attribute, const char *value) {
    Q(name);
    q.attr = attribute ? attribute : "";
    q.attr_value = value ? value : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_itemprop(const fhp_doc *doc, fhp_node n, const char *name, const char *itemprop) {
    return fhp_is_name_attribute(doc, n, name, FHP_ATTR_ITEMPROP, itemprop);
}

int fhp_is_name_source(const fhp_doc *doc, fhp_node n, const char *name, const char *source) {
    Q(name);
    q.source = source ? source : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_class_body(const fhp_doc *doc, fhp_node n, const char *name, const char *cls, const char *body) {
    Q(name);
    q.cls = cls ? cls : "";
    q.body = body ? body : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_class_text(const fhp_doc *doc, fhp_node n, const char *name, const char *cls, const char *text) {
    Q(name);
    q.cls = opt(cls);
    q.text = opt(text);
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_id_text(const fhp_doc *doc, fhp_node n, const char *name, const char *id, const char *text) {
    Q(name);
    q.id = opt(id);
    q.text = opt(text);
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_class_id_body(const fhp_doc *doc, fhp_node n, const char *name, const char *cls, const char *id,
                              const char *body) {
    Q(name);
    q.cls = opt(cls);
    q.id = opt(id);
    q.body = opt(body);
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_text_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *prefix) {
    Q(name);
    q.text_starts_with = prefix ? prefix : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_body_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *prefix) {
    Q(name);
    q.body_starts_with = prefix ? prefix : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_id_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *prefix) {
    Q(name);
    q.id_starts_with = prefix ? prefix : "";
    return fhp_matches(doc, n, &q);
}

int fhp_is_name_class_text_starts_with(const fhp_doc *doc, fhp_node n, const char *name, const char *cls,
                                       const char *prefix) {
    Q(name);
    q.cls = cls ? cls : "";
    q.text_starts_with = prefix ? prefix : "";
    return fhp_matches(doc, n, &q);
}

fhp_node fhp_parent_by_name(const fhp_doc *doc, fhp_node n, const char *name) {
    Q(name);
    return fhp_parent_matching(doc, n, &q);
}

fhp_node fhp_parent_by_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls) {
    Q(name);
    q.cls = cls ? cls : "";
    return fhp_parent_matching(doc, n, &q);
}

fhp_node fhp_next_by_name(const fhp_doc *doc, fhp_node n, const char *name) {
    Q(name);
    return fhp_next_matching(doc, n, &q);
}

fhp_node fhp_next_by_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls) {
    Q(name);
    q.cls = cls ? cls : "";
    return fhp_next_matching(doc, n, &q);
}

fhp_node fhp_next_sibling_by_name(const fhp_doc *doc, fhp_node n, const char *name) {
    Q(name);
    return fhp_next_sibling_matching(doc, n, &q);
}

fhp_node fhp_next_sibling_by_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls) {
    Q(name);
    q.cls = cls ? cls : "";
    return fhp_next_sibling_matching(doc, n, &q);
}

fhp_node fhp_previous_sibling_by_name(const fhp_doc *doc, fhp_node n, const char *name) {
    Q(name);
    return fhp_previous_sibling_matching(doc, n, &q);
}

fhp_str fhp_text_after_until_tag_by_name(const fhp_doc *doc, fhp_node n, const char *name) {
    Q(name);
    return fhp_text_after_until(doc, n, &q);
}

fhp_str fhp_text_after_until_tag_by_name_class(const fhp_doc *doc, fhp_node n, const char *name, const char *cls) {
    Q(name);
    q.cls = cls ? cls : "";
    return fhp_text_after_until(doc, n, &q);
}

static fhp_nodes first_level(const fhp_doc *doc, fhp_nodes list, const fhp_query *q, fhp_arena *a) {
    fhp_ivec v;
    fhp_nodes out;
    size_t i;
    int level;
    out.items = NULL;
    out.len = 0;
    if (!doc || list.len == 0) return out;
    level = fhp_depth(doc, list.items[0]);
    fhp_ivec_init(&v, fhp_result_arena(doc, a), 0);
    for (i = 0; i < list.len; i++)
        if (fhp_depth(doc, list.items[i]) == level && (!q || fhp_matches(doc, list.items[i], q)))
            fhp_ivec_push(&v, list.items[i]);
    out.items = v.p;
    out.len = v.len;
    return out;
}

fhp_nodes fhp_first_level(const fhp_doc *doc, fhp_nodes list, fhp_arena *a) { return first_level(doc, list, NULL, a); }

fhp_nodes fhp_first_level_by_name(const fhp_doc *doc, fhp_nodes list, const char *name, fhp_arena *a) {
    Q(name);
    return first_level(doc, list, &q, a);
}

fhp_nodes fhp_first_level_by_name_class(const fhp_doc *doc, fhp_nodes list, const char *name, const char *cls,
                                        fhp_arena *a) {
    Q(name);
    q.cls = cls ? cls : "";
    return first_level(doc, list, &q, a);
}

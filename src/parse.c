/* A page is scanned once in fast mode: tags are matched to their end tags and nothing else. Well-formed markup needs
 * nothing more. Where the HTML5 optional-end-tag rules matter (<li>one<li>two</ul>), tags are left open inside an
 * element: when its end tag (or the end of the document) finds them on the stack and one of them is a rule target
 * (li, p, td, tr, option, ...), only that part is scanned again in rules mode, from the outermost such tag up to that
 * end tag. Everything before it was parsed right and is kept.
 *
 * Port of ParsingCore.cs from z3l.FastHtmlParser (by way of the Rust port); positions are byte offsets. */
#include <stdlib.h>

#include "internal.h"

/* Nesting deeper than this is flattened, as browsers do (Blink: 512). */
#define MAX_DEPTH 512
/* The parts scanned again with the optional-end-tag rules may add up to this many times the source length. */
#define REPAIR_BUDGET_FACTOR 8

#define K_P KEY(FHP_TAG_P)
#define K_COMMENT KEY(FHP_TAG_COMMENT)

/* Per-tag data needed while parsing: matching an end tag scans these without touching the nodes. */
typedef struct tag_state {
    int32_t key; /* tag key, or -1 for a custom name */
    int32_t depth;
    int32_t prev_open;   /* the open tags as a linked list in document order */
    int32_t self_closed; /* <x/>: stays in the open list (a later </x> ends it), but does not nest anything */
} tag_state;

/* Scratch reused between parses on a thread: only the result is allocated per document. */
static FHP_TLS tag_state *tls_states;
static FHP_TLS size_t tls_states_cap;

void fhp_parse_cleanup(void) {
    free(tls_states);
    tls_states = NULL;
    tls_states_cap = 0;
}

typedef struct pstate {
    const char *src;
    size_t len;
    int is_xml;
    fhp_doc *doc;
    fhp_nodedata *nodes;
    tag_state *states;
    int32_t stack[MAX_DEPTH];        /* indexes of the open tags that contain the following ones */
    int32_t open_by_key[FHP_TAG_COUNT]; /* per tag, how many are on the stack: kept only in rules mode */
    int32_t open_in_list[FHP_TAG_COUNT]; /* per tag, how many are in the open list: an end tag with none is skipped */
    /* custom names: lower-case name -> index */
    fhp_arena *scratch;
    int32_t *map;
    size_t map_cap;
    fhp_str *names;
    size_t names_len, names_cap;
    fhp_ivec custom_open; /* per custom name, how many tags with it are in the open list */
    size_t count;
    size_t stack_top;
    int32_t last_open;
    int32_t open_p;   /* <p> tags on the stack, kept in both modes */
    size_t budget;    /* bytes that may still be scanned again by repair */
} pstate;

/* ----- tag names ----- */

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

/* Finds a tag name case-insensitively (ASCII) without allocating: its key, or -1. */
int fhp_lookup_key(const char *name, size_t len) {
    char buf[32];
    size_t i;
    uint32_t h;
    if (len == 0 || len > 32) return -1;
    for (i = 0; i < len; i++) buf[i] = (char)fhp_lower_ascii((unsigned char)name[i]);
    h = fnv(buf, len) & (FHP_TAG_HASH_SIZE - 1);
    for (;;) {
        int k = fhp_tag_hash[h];
        if (k < 0) return -1;
        if (fhp_tag_name_lens[k] == len && memcmp(fhp_tag_names[k], buf, len) == 0) return k;
        h = (h + 1) & (FHP_TAG_HASH_SIZE - 1);
    }
}

const char *fhp_tag_name(int tag) {
    if (tag < 1 || tag > FHP_TAG_COUNT) return "";
    return fhp_tag_names[KEY(tag)];
}

int fhp_lookup_tag(const char *name) {
    if (!name) return FHP_TAG_NONE;
    return fhp_lookup_key(name, strlen(name)) + 1;
}

/* ----- helpers ----- */

/* As in HTML5, '<' starts markup only before an ASCII letter, '/', '!' or '?'. */
int fhp_is_markup_start(const char *s, size_t len, size_t i) {
    unsigned char c;
    if (i + 1 >= len) return 0;
    c = (unsigned char)s[i + 1];
    return fhp_is_alpha_ascii(c) || c == '/' || c == '!' || c == '?';
}

/* The position of the '<' of the end tag (e.g. "</script"), or len if there is none. */
size_t fhp_find_raw_text_end(const char *s, size_t len, size_t from, const char *end_tag, size_t end_len) {
    while (from < len) {
        ptrdiff_t found = fhp_index_fold(s + from, len - from, end_tag, end_len);
        size_t pos, after;
        if (found < 0) break;
        pos = from + (size_t)found;
        after = pos + end_len;
        /* "</script>", "</script >", "</script/>" end it; "</scripts" does not. */
        if (after >= len || s[after] == '>' || s[after] == '/' || fhp_is_space((unsigned char)s[after])) return pos;
        from = after;
    }
    return len;
}

/* The position of the '>' that ends a comment whose content starts at content_start. As in HTML5: "<!-->" and
   "<!--->" are empty comments, "-->" or "--!>" end a comment, otherwise it runs to the end. */
size_t fhp_find_comment_end(const char *s, size_t len, size_t content_start) {
    size_t cs = content_start < len ? content_start : len;
    const char *content = s + cs;
    size_t clen = len - cs, from = 0;
    if (clen >= 1 && content[0] == '>') return content_start;
    if (clen >= 2 && content[0] == '-' && content[1] == '>') return content_start + 1;
    while (from < clen) {
        ptrdiff_t dashes = fhp_index(content + from, clen - from, "--", 2);
        size_t pos;
        if (dashes < 0) break;
        pos = from + (size_t)dashes + 2;
        if (pos < clen && content[pos] == '>') return cs + pos;
        if (pos + 1 < clen && content[pos] == '!' && content[pos + 1] == '>') return cs + pos + 1;
        from = from + (size_t)dashes + 1;
    }
    return len - 1;
}

static fhp_str extract_tag_name(fhp_str tag_source) {
    fhp_str t = fhp_trim(tag_source);
    size_t i = 0;
    if (t.len >= 3 && memcmp(t.ptr, "!--", 3) == 0) return fhp_s(t.ptr, 3);
    /* The name ends at whitespace or '/': <br/> is "br", as <br /> is. */
    while (i < t.len && !fhp_is_space((unsigned char)t.ptr[i]) && t.ptr[i] != '/') i++;
    return fhp_s(t.ptr, i);
}

/* An XML document (RSS, Atom, sitemap, XHTML) is parsed by XML rules: elements are closed only by an end tag or "/>". */
static int is_xml(const char *s, size_t len) {
    size_t i = 0;
    for (;;) {
        if (i < len && fhp_is_space((unsigned char)s[i])) {
            i++;
        } else if (i + 3 <= len && (unsigned char)s[i] == 0xEF && (unsigned char)s[i + 1] == 0xBB &&
                   (unsigned char)s[i + 2] == 0xBF) {
            i += 3; /* U+FEFF */
        } else {
            break;
        }
    }
    return len - i >= 5 && fhp_eq_ignore_ascii_case(s + i, "<?xml", 5);
}

/* ----- custom names ----- */

static int32_t custom_get(pstate *st, fhp_str lower) {
    size_t h;
    if (!st->map_cap) return -1;
    h = fnv(lower.ptr, lower.len) & (st->map_cap - 1);
    for (;;) {
        int32_t c = st->map[h];
        if (c < 0) return -1;
        if (fhp_str_eq(st->names[c], lower)) return c;
        h = (h + 1) & (st->map_cap - 1);
    }
}

static void custom_put(pstate *st, fhp_str lower, int32_t index) {
    size_t h = fnv(lower.ptr, lower.len) & (st->map_cap - 1);
    while (st->map[h] >= 0) h = (h + 1) & (st->map_cap - 1);
    st->map[h] = index;
}

/* The lower-case name as one entry per document. */
static uint32_t intern_name(pstate *st, fhp_str name) {
    fhp_mark m;
    fhp_str lower;
    int32_t c;
    char *copy;
    if (name.len == 0) return FHP_NO_CUSTOM;
    m = fhp_arena_mark(st->scratch);
    lower = fhp_lower_key(st->scratch, name);
    c = custom_get(st, lower);
    if (c >= 0) {
        fhp_arena_release(st->scratch, m);
        return (uint32_t)c;
    }
    copy = fhp_strndup(&st->doc->mem, lower.ptr, lower.len);
    fhp_arena_release(st->scratch, m);
    if (!copy) return FHP_NO_CUSTOM;
    if (st->names_len == st->names_cap) {
        size_t cap = st->names_cap ? st->names_cap * 2 : 16;
        fhp_str *n = (fhp_str *)fhp_realloc(st->scratch, st->names, st->names_cap * sizeof(fhp_str), cap * sizeof(fhp_str));
        if (!n) return FHP_NO_CUSTOM;
        st->names = n;
        st->names_cap = cap;
    }
    if ((st->names_len + 1) * 2 > st->map_cap) {
        size_t cap = st->map_cap ? st->map_cap * 2 : 64, i;
        int32_t *map = (int32_t *)fhp_alloc(st->scratch, cap * sizeof(int32_t));
        if (!map) return FHP_NO_CUSTOM;
        for (i = 0; i < cap; i++) map[i] = -1;
        st->map = map;
        st->map_cap = cap;
        for (i = 0; i < st->names_len; i++) custom_put(st, st->names[i], (int32_t)i);
    }
    st->names[st->names_len] = fhp_s(copy, lower.len);
    custom_put(st, st->names[st->names_len], (int32_t)st->names_len);
    fhp_ivec_push(&st->custom_open, 0);
    return (uint32_t)st->names_len++;
}

static void adjust_custom(pstate *st, uint32_t custom, int delta) {
    if (custom != FHP_NO_CUSTOM && custom < st->custom_open.len) st->custom_open.p[custom] += delta;
}

/* ----- the open list and the stack ----- */

/* Removes the tags from index first on from the open list (they are its tail).
   With end >= 0 the ones that have no end tag of their own end there. */
static void truncate_open_list(pstate *st, int32_t first, int64_t end) {
    int32_t k = st->last_open;
    while (k >= first && k >= 0) {
        tag_state state = st->states[k];
        fhp_nodedata *node = &st->nodes[k];
        if (state.key >= 0)
            st->open_in_list[state.key]--;
        else
            adjust_custom(st, node->custom, -1);
        if (end >= 0 && !state.self_closed && node->inner_end == 0) {
            node->inner_end = (int32_t)end;
            node->end = (int32_t)end;
        }
        k = state.prev_open;
    }
    st->last_open = k;
}

static void parent_and_depth(const pstate *st, int32_t *parent, int32_t *depth) {
    if (st->stack_top > 0) {
        *parent = st->stack[st->stack_top - 1];
        *depth = st->states[*parent].depth + 1;
    } else {
        *parent = -1;
        *depth = 0;
    }
}

/* Among the tags left open at stack positions [from, to), the outermost one that a rule should have ended: its stack
   position, or -1. */
static ptrdiff_t first_misnested(const pstate *st, size_t from, size_t to) {
    size_t s;
    for (s = from; s + 1 < to; s++) {
        int32_t key = st->states[st->stack[s]].key;
        size_t inner;
        if (key < 0 || !(fhp_tag_flags[key] & FHP_F_RULE_TARGET)) continue;
        for (inner = s + 1; inner < to; inner++) {
            int32_t inner_key = st->states[st->stack[inner]].key;
            if (inner_key >= 0 && fhp_tag_rule[inner_key] >= 0 && fhp_rules[fhp_tag_rule[inner_key]].targets[key])
                return (ptrdiff_t)s;
        }
    }
    return -1;
}

/* The stack position of the outermost target of rule below the first stop tag, or -1. */
static ptrdiff_t scope_start(const pstate *st, const fhp_rule *rule) {
    ptrdiff_t from = -1;
    size_t s = st->stack_top;
    while (s > 0) {
        int32_t k;
        s--;
        k = st->states[st->stack[s]].key;
        if (k < 0) continue;
        if (rule->targets[k])
            from = (ptrdiff_t)s;
        else if (rule->stops[k])
            break;
    }
    return from;
}

/* Ends the outermost target found below the first stop tag together with everything opened inside it. */
static void close_in_scope(pstate *st, const fhp_rule *rule, int64_t end, int by_key) {
    ptrdiff_t from = scope_start(st, rule);
    int32_t first;
    if (from < 0) return;
    first = st->stack[from];
    while (st->stack_top > (size_t)from) {
        int32_t key;
        st->stack_top--;
        key = st->states[st->stack[st->stack_top]].key;
        if (key == K_P) st->open_p--;
        if (by_key && key >= 0) st->open_by_key[key]--;
    }
    truncate_open_list(st, first, end);
}

static void scan(pstate *st, int rules, size_t from, size_t to, int *tag_started_out, size_t *tag_start_out);

/* Scans again with the optional-end-tag rules from the tag at stack position stack_pos up to region_end: the tags from
   it on are replaced, the ones before it are kept. */
static void repair(pstate *st, size_t stack_pos, size_t region_end) {
    int32_t first = st->stack[stack_pos];
    size_t from = (size_t)st->nodes[first].start;
    size_t cost = region_end > from ? region_end - from : 0;
    size_t s;
    int ts;
    size_t tstart;
    /* Past the budget the part is left as it is, so that parsing stays linear on any markup. */
    if (cost > st->budget) return;
    st->budget -= cost;

    truncate_open_list(st, first, -1);
    st->stack_top = stack_pos;
    st->count = (size_t)first;

    memset(st->open_by_key, 0, sizeof st->open_by_key);
    st->open_p = 0;
    for (s = 0; s < stack_pos; s++) {
        int32_t key = st->states[st->stack[s]].key;
        if (key < 0) continue;
        st->open_by_key[key]++;
        if (key == K_P) st->open_p++;
    }
    scan(st, 1, from, region_end, &ts, &tstart);
}

/* Applies rule for the tag that starts at tag_start. If a tag it ends was itself misnested, that part is scanned again
   with the rules first. */
static void apply_rule_in_fast_mode(pstate *st, const fhp_rule *rule, size_t tag_start) {
    ptrdiff_t from = scope_start(st, rule), m;
    if (from < 0) return;
    m = first_misnested(st, (size_t)from, st->stack_top);
    if (m >= 0) repair(st, (size_t)m, tag_start - 1);
    close_in_scope(st, rule, (int64_t)tag_start - 2, 0);
}

static int rule_any_open(const pstate *st, const fhp_rule *rule) {
    int i;
    for (i = 0; i < rule->target_count; i++)
        if (st->open_by_key[rule->target_keys[i]] > 0) return 1;
    return 0;
}

/* ----- tags ----- */

/* Ends the innermost open tag with the end tag's name. Returns 1 when it scanned a part again with the
   optional-end-tag rules instead, and the end tag is to be matched once more. */
static int match_end(pstate *st, int rules, int key, fhp_str name, size_t tag_start, size_t gt, int can_repair) {
    uint32_t closing = FHP_NO_CUSTOM;
    int32_t j;
    if (key >= 0) {
        if (st->open_in_list[key] <= 0) return 0;
    } else {
        fhp_mark m;
        int32_t c;
        if (st->names_len == 0) return 0;
        m = fhp_arena_mark(st->scratch);
        c = custom_get(st, fhp_lower_key(st->scratch, name));
        fhp_arena_release(st->scratch, m);
        if (c < 0 || st->custom_open.p[c] <= 0) return 0;
        closing = (uint32_t)c;
    }

    j = st->last_open;
    while (j >= 0) {
        int32_t next = st->states[j].prev_open;
        int matched = key >= 0 ? st->states[j].key == key : st->states[j].key < 0 && st->nodes[j].custom == closing;
        int64_t inner_end;
        if (!matched) {
            j = next;
            continue;
        }
        inner_end = (int64_t)tag_start - 2;

        /* Tags left open inside it: if the optional-end-tag rules apply to one of them, scan its part again with the
           rules (fast mode only; rules mode has applied them). */
        if (can_repair && !rules && !st->is_xml && st->stack_top > 0 && st->stack[st->stack_top - 1] > j) {
            size_t above = st->stack_top - 1;
            ptrdiff_t target;
            while (above > 0 && st->stack[above - 1] > j) above--;
            target = first_misnested(st, above, st->stack_top);
            if (target >= 0) {
                repair(st, (size_t)target, tag_start - 1);
                return 1;
            }
        }

        st->nodes[j].inner_end = (int32_t)inner_end;
        st->nodes[j].end = (int32_t)gt;

        while (st->stack_top > 0 && st->stack[st->stack_top - 1] >= j) {
            int32_t popped;
            st->stack_top--;
            popped = st->states[st->stack[st->stack_top]].key;
            if (popped == K_P) st->open_p--;
            if (rules && popped >= 0) st->open_by_key[popped]--;
        }

        /* It leaves the open list with everything opened after it; the tags opened inside it and never closed end
           right before this end tag, as in a browser. */
        truncate_open_list(st, j, inner_end);
        return 0;
    }
    return 0;
}

/* Matches the end tag src[tag_start-1 ..= gt] to the innermost open tag with that name. */
static void end_tag(pstate *st, int rules, size_t tag_start, size_t gt) {
    fhp_str name = fhp_trim(fhp_s(st->src + tag_start + 1, gt - tag_start - 1));
    int key;
    if (name.len == 0) return;
    key = fhp_lookup_key(name.ptr, name.len);
    /* The rules may have ended the matched tag itself (<li><div><li>x</div>: the second <li> ends the first one together
       with its <div>, as in a browser): the end tag is matched again, to what is open now. Once: the part is not
       scanned a second time. */
    if (match_end(st, rules, key, name, tag_start, gt, 1)) match_end(st, rules, key, name, tag_start, gt, 0);
}

/* Adds the tag src[tag_start-1 ..= gt]; returns the position to continue from (gt, or before a raw-text end tag). */
static size_t start_tag(pstate *st, int rules, size_t to, size_t tag_start, size_t gt) {
    fhp_str tag_source = fhp_s(st->src + tag_start, gt - tag_start);
    fhp_str name = extract_tag_name(tag_source);
    int key = fhp_lookup_key(name.ptr, name.len);
    uint32_t custom = FHP_NO_CUSTOM;
    int32_t parent, depth;
    size_t index;
    int is_void, self_closing;
    fhp_str trimmed;

    /* Custom names (svg elements, web components, ...): one string per distinct name per document. */
    if (key < 0) custom = intern_name(st, name);

    if (!st->is_xml && key >= 0) {
        /* Optional end tags (HTML5): <li> ends an open <li>, <td> an open <td>, ... */
        int r = fhp_tag_rule[key];
        if (r >= 0) {
            const fhp_rule *rule = &fhp_rules[r];
            if (rules) {
                if (rule_any_open(st, rule)) close_in_scope(st, rule, (int64_t)tag_start - 2, 1);
            } else if (st->stack_top > 0) {
                /* The common case of a left-out end tag is caught right here: the innermost open tag is a target. */
                int32_t top = st->states[st->stack[st->stack_top - 1]].key;
                if (top >= 0 && rule->targets[top]) apply_rule_in_fast_mode(st, rule, tag_start);
            }
        }
        /* A block element ends an open <p>. Usually no <p> is open and this is one compare. */
        if (st->open_p > 0 && (fhp_tag_flags[key] & FHP_F_CLOSES_P)) {
            const fhp_rule *p_rule = &fhp_rules[FHP_P_RULE];
            if (rules)
                close_in_scope(st, p_rule, (int64_t)tag_start - 2, 1);
            else
                apply_rule_in_fast_mode(st, p_rule, tag_start);
        }
    }

    /* the innermost open tag is the parent */
    parent_and_depth(st, &parent, &depth);

    index = st->count;
    {
        fhp_nodedata *nd = &st->nodes[index];
        nd->custom = custom;
        nd->parent = parent;
        nd->start = (int32_t)tag_start - 1;
        nd->end = (int32_t)gt;
        nd->inner_start = (int32_t)gt + 1;
        nd->inner_end = 0;
        nd->depth = depth;
        nd->key = (int16_t)key;
        nd->pad = 0;
    }

    /* Void elements (<br>, <img>, <meta>, ...) and declarations never contain anything. */
    is_void = !st->is_xml && key >= 0 && (fhp_tag_flags[key] & FHP_F_VOID);
    trimmed = fhp_trim_ascii_end(tag_source);
    self_closing = is_void || (trimmed.len > 0 && trimmed.ptr[trimmed.len - 1] == '/');

    {
        tag_state *s = &st->states[index];
        s->key = key;
        s->depth = depth;
        if (is_void) {
            s->prev_open = -1;
            s->self_closed = 0;
        } else {
            s->self_closed = self_closing;
            s->prev_open = st->last_open;
            st->last_open = (int32_t)index;
            if (key >= 0)
                st->open_in_list[key]++;
            else
                adjust_custom(st, custom, 1);
        }
    }
    st->count++;

    if (!self_closing) {
        /* Deeper than MAX_DEPTH the tag is not nested: it stays in the open list for its end tag. */
        if (st->stack_top < MAX_DEPTH) {
            st->stack[st->stack_top++] = (int32_t)index;
            if (key == K_P) st->open_p++;
            if (rules && key >= 0) st->open_by_key[key]++;
        }
        /* <script>, <style>, <textarea>, <title>, ...: the content is text, not markup; continue right before the
           matching end tag. */
        if (!st->is_xml && fhp_has_raw_text(key)) {
            char end[16];
            size_t n = fhp_tag_name_lens[key];
            end[0] = '<';
            end[1] = '/';
            memcpy(end + 2, fhp_tag_names[key], n);
            return fhp_find_raw_text_end(st->src, to, gt + 1, end, n + 2) - 1;
        }
    }
    return gt;
}

/* Parses src[from..to] (from is outside a tag), appending tags at count. Tags still open at to stay on the stack. */
static void scan(pstate *st, int rules, size_t from, size_t to, int *tag_started_out, size_t *tag_start_out) {
    const char *src = st->src;
    int tag_started = 0;
    size_t tag_start = 0;
    size_t i = from;
    while (i < to) {
        if (!tag_started) {
            /* Outside a tag only '<' matters: jump to it. */
            const char *lt = (const char *)fhp_memchr(src + i, '<', to - i);
            if (!lt) break;
            i = (size_t)(lt - src);

            if (to - (i + 1) >= 3 && src[i + 1] == '!' && src[i + 2] == '-' && src[i + 3] == '-') {
                /* <!-- ... -->: one "!--" tag; its content is not markup, it does not nest anything. */
                size_t comment_end = fhp_find_comment_end(src, to, i + 4);
                int32_t parent, depth;
                fhp_nodedata *nd = &st->nodes[st->count];
                tag_state *s = &st->states[st->count];
                parent_and_depth(st, &parent, &depth);
                nd->custom = FHP_NO_CUSTOM;
                nd->parent = parent;
                nd->start = (int32_t)i;
                nd->end = (int32_t)comment_end;
                nd->inner_start = (int32_t)comment_end + 1;
                nd->inner_end = 0;
                nd->depth = depth;
                nd->key = (int16_t)K_COMMENT;
                nd->pad = 0;
                s->key = K_COMMENT;
                s->depth = depth;
                s->prev_open = -1;
                s->self_closed = 0;
                st->count++;
                i = comment_end + 1;
                continue;
            }

            /* As in HTML5, '<' starts markup only before a letter, '/', '!' or '?': "a < b" is text. */
            if (!fhp_is_markup_start(src, to, i)) {
                i++;
                continue;
            }
            tag_started = 1;
            tag_start = i + 1;
            i++;
            continue;
        }

        /* Inside a tag only '<', '=' and '>' matter. */
        {
            const char *next = fhp_memchr3(src + i, to - i, '<', '=', '>');
            if (!next) break;
            i = (size_t)(next - src);
        }
        if (src[i] == '<') {
            /* An unfinished tag: start over at this '<' (it may also start a comment). */
            tag_started = 0;
            continue;
        }
        if (src[i] == '=') {
            /* A quoted value may contain '<' and '>' (data-x="<p>...</p>"): jump over it. */
            size_t q = i + 1;
            while (q < to && fhp_is_space((unsigned char)src[q])) q++;
            if (q < to && (src[q] == '"' || src[q] == '\'')) {
                const char *close = (const char *)fhp_memchr(src + q + 1, src[q], to - q - 1);
                if (close) i = (size_t)(close - src);
            }
        } else {
            /* '>' */
            if (src[tag_start] == '/')
                end_tag(st, rules, tag_start, i);
            else
                i = start_tag(st, rules, to, tag_start, i);
            tag_started = 0;
        }
        i++;
    }
    *tag_started_out = tag_started;
    *tag_start_out = tag_start;
}

/* Parses doc->src into doc->nodes and the custom names. 0, or -1 when out of memory. */
int fhp_parse_into(fhp_doc *doc, size_t max_tags) {
    pstate *st;
    size_t k;
    int tag_started;
    size_t tag_start;
    int64_t region_end;
    fhp_mark mark;
    fhp_arena *scratch;

    /* max_tags: the number of '<'. Every tag starts with its own '<', so this is an exact upper bound. */
    doc->count = 0;
    doc->nodes = NULL;
    doc->custom_names = NULL;
    doc->custom_count = 0;
    if (max_tags == 0) return 0;

    doc->nodes = (fhp_nodedata *)fhp_alloc(&doc->mem, max_tags * sizeof(fhp_nodedata));
    if (!doc->nodes) return -1;
    if (tls_states_cap < max_tags) {
        size_t cap = max_tags < 1024 ? 1024 : max_tags;
        tag_state *s = (tag_state *)realloc(tls_states, cap * sizeof(tag_state));
        if (!s) return -1;
        tls_states = s;
        tls_states_cap = cap;
    }

    scratch = fhp_scratch();
    mark = fhp_arena_mark(scratch);
    st = (pstate *)fhp_alloc(scratch, sizeof(pstate));
    if (!st) return -1;
    st->src = doc->src;
    st->len = doc->len;
    st->is_xml = is_xml(doc->src, doc->len);
    st->doc = doc;
    st->nodes = doc->nodes;
    st->states = tls_states;
    memset(st->open_by_key, 0, sizeof st->open_by_key);
    memset(st->open_in_list, 0, sizeof st->open_in_list);
    st->scratch = scratch;
    st->map = NULL;
    st->map_cap = 0;
    st->names = NULL;
    st->names_len = st->names_cap = 0;
    fhp_ivec_init(&st->custom_open, scratch, 0);
    st->count = 0;
    st->stack_top = 0;
    st->last_open = -1;
    st->open_p = 0;
    st->budget = REPAIR_BUDGET_FACTOR * doc->len;

    scan(st, 0, 0, doc->len, &tag_started, &tag_start);

    /* The end of the document ends every tag still open (a truncated page keeps its text), right before an
       unfinished tag if the source stops inside one. */
    region_end = tag_started ? (int64_t)tag_start - 1 : (int64_t)doc->len;
    if (!st->is_xml) {
        ptrdiff_t s = first_misnested(st, 0, st->stack_top);
        if (s >= 0) repair(st, (size_t)s, (size_t)region_end);
    }
    truncate_open_list(st, 0, region_end - 1);

    doc->count = st->count;
    for (k = 0; k < st->count; k++)
        if (doc->nodes[k].inner_end == 0) doc->nodes[k].inner_start = 0; /* not closed: no body */
    if (st->names_len) {
        doc->custom_names = (fhp_str *)fhp_alloc(&doc->mem, st->names_len * sizeof(fhp_str));
        if (!doc->custom_names) {
            fhp_arena_release(scratch, mark);
            return -1;
        }
        memcpy(doc->custom_names, st->names, st->names_len * sizeof(fhp_str));
        doc->custom_count = st->names_len;
    }
    fhp_arena_release(scratch, mark);
    return 0;
}

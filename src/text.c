/* Visible text, and text with the line structure kept. */
#include "internal.h"

/* For the inside of an opening tag, the end tag ("</script") when the element's content is never visible text
   (script, style, noscript, template, noframes, noembed); NULL otherwise. */
static const char *invisible_content_end(const char *tag, size_t len, size_t *end_len) {
    unsigned char first;
    size_t n = 1;
    const char *end;
    fhp_str t;
    if (len == 0) return NULL;
    first = (unsigned char)(tag[0] | 0x20);
    if (first != 's' && first != 't' && first != 'n') return NULL;
    while (n < len && n <= 8 && fhp_is_alpha_ascii((unsigned char)tag[n])) n++;
    if (n < len && tag[n] != '/' && !fhp_is_space((unsigned char)tag[n])) return NULL;
#define IS(word) (n == sizeof(word) - 1 && fhp_eq_ignore_ascii_case(tag, word, n))
    if (IS("style"))
        end = "</style";
    else if (IS("script"))
        end = "</script";
    else if (IS("noembed"))
        end = "</noembed";
    else if (IS("noscript"))
        end = "</noscript";
    else if (IS("template"))
        end = "</template";
    else if (IS("noframes"))
        end = "</noframes";
    else
        return NULL;
#undef IS
    /* <script src="x" />: no content */
    t = fhp_trim_ascii_end(fhp_s(tag, len));
    if (t.len && t.ptr[t.len - 1] == '/') return NULL;
    *end_len = strlen(end);
    return end;
}

/* s[i] is '<'. The index of the last byte of what is not text there: a comment, a tag, or a tag together with
   invisible content (script, style, ...) up to its end tag. 0: an unterminated tag (*ok = 0). The result may be
   (size_t)-1 when the content runs to... never: find_raw_text_end returns at least tag_end + 1. */
static size_t skip_markup(const char *s, size_t len, size_t i, int *ok) {
    const char *rest = s + i + 1;
    size_t rest_len = len - i - 1;
    const char *close;
    size_t tag_end, end_len;
    const char *end_tag;
    *ok = 1;
    if (rest_len >= 3 && rest[0] == '!' && rest[1] == '-' && rest[2] == '-') return fhp_find_comment_end(s, len, i + 4);
    close = (const char *)fhp_memchr(rest, '>', rest_len);
    if (!close) {
        *ok = 0;
        return 0;
    }
    tag_end = (size_t)(close - s);
    end_tag = invisible_content_end(rest, (size_t)(close - rest), &end_len);
    if (end_tag) return fhp_find_raw_text_end(s, len, tag_end + 1, end_tag, end_len) - 1;
    return tag_end;
}

/* Whether the markup at s[i] ('<') is a tag that separates words in the one-line text, as in jsoup's text(): <br>,
   a block element or a table cell, opening or closing (the line-break tags of the text with line breaks). Comments,
   declarations, inline and unknown tags do not. */
static int separates_words(const char *s, size_t len, size_t i) {
    size_t start = i + 1, end;
    int key, kind;
    if (start < len && s[start] == '/') start++;
    end = start;
    while (end < len && (fhp_is_alpha_ascii((unsigned char)s[end]) || (s[end] >= '0' && s[end] <= '9'))) end++;
    if (end == start) return 0;
    key = fhp_lookup_key(s + start, end - start);
    if (key < 0) return 0;
    kind = fhp_tag_line_break[key];
    return kind == LB_LINE || kind == LB_PARAGRAPH || kind == LB_SPACE;
}

/* +1 for an opening pre, listing or plaintext at s[i], -1 for a closing one, 0 otherwise: their text keeps its
   whitespace, as in jsoup. */
static int pre_tag(const char *s, size_t len, size_t i) {
    size_t start = i + 1, end;
    int sign = 1, key;
    if (start < len && s[start] == '/') {
        start++;
        sign = -1;
    }
    if (start >= len || ((s[start] | 0x20) != 'p' && (s[start] | 0x20) != 'l')) return 0; /* the usual tag: one compare */
    end = start;
    while (end < len && fhp_is_alpha_ascii((unsigned char)s[end])) end++;
    key = fhp_lookup_key(s + start, end - start);
    return key == KEY(FHP_TAG_PRE) || key == KEY(FHP_TAG_LISTING) || key == KEY(FHP_TAG_PLAINTEXT) ? sign : 0;
}

/* Whether trimmed text has whitespace to collapse: a line break, tab or other space than ' ', or two spaces in a row. */
int fhp_needs_collapse(fhp_str str) {
    const char *s = str.ptr;
    size_t len = str.len, i = 0, size;
    while (i < len) {
        if (s[i] == ' ') {
            if (i + 1 < len && fhp_space_at(s, len, i + 1, &size)) return 1;
            i++;
            continue;
        }
        if (fhp_space_at(s, len, i, &size)) return 1;
        i += size;
    }
    return 0;
}

/* Removes tags, comments and invisible content, collapses whitespace runs into one ' ' and trims: the visible text in
   one line, as jsoup's text(). <br>, block elements and table cells separate words ("a<br>b", a row of cells: "a b"),
   inline tags do not ("a<b>b</b>": "ab"). Inside <pre> (in_pre: the text itself is in one) whitespace is kept. */
static void strip_tags(fhp_str str, fhp_buf *dst, int in_pre) {
    const char *s = str.ptr;
    size_t len = str.len, i = 0, run = 0;
    int prev_space = 1; /* left trim */
    int pre_depth = in_pre ? 1 : 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t size;
        if (c == '<' && fhp_is_markup_start(s, len, i)) {
            int ok;
            size_t last;
            fhp_buf_add(dst, s + run, i - run);
            last = skip_markup(s, len, i, &ok);
            if (!ok) {
                run = len;
                break;
            }
            if (!in_pre) {
                pre_depth += pre_tag(s, len, i);
                if (pre_depth < 0) pre_depth = 0;
            }
            /* only after a word: next to whitespace the tag changes nothing (and costs no lookup) */
            if (!prev_space && separates_words(s, len, i)) {
                fhp_buf_addc(dst, ' ');
                prev_space = 1;
            }
            i = last + 1;
            run = i;
            continue;
        }
        if (pre_depth > 0) {
            if (fhp_space_at(s, len, i, &size)) {
                /* as written (left in the run), but not at the start */
                if (dst->len == 0 && run == i) run = i + size;
                prev_space = 1;
            } else {
                prev_space = 0;
            }
            i += size;
            continue;
        }
        if (c < 0x80) {
            if (fhp_is_space(c)) {
                fhp_buf_add(dst, s + run, i - run);
                if (!prev_space) {
                    fhp_buf_addc(dst, ' ');
                    prev_space = 1;
                }
                run = i + 1;
            } else {
                prev_space = 0;
            }
            i++;
            continue;
        }
        if (fhp_space_at(s, len, i, &size)) {
            fhp_buf_add(dst, s + run, i - run);
            if (!prev_space) {
                fhp_buf_addc(dst, ' ');
                prev_space = 1;
            }
            run = i + size;
        } else {
            prev_space = 0;
        }
        i += size;
    }
    if (run < len) fhp_buf_add(dst, s + run, len - run);
    /* Right trim (inside <pre> there may be more than one trailing whitespace char). */
    while (dst->len) {
        size_t k = dst->len - 1, size;
        while (k > 0 && ((unsigned char)dst->p[k] & 0xC0) == 0x80) k--; /* the start of the last char */
        if (!fhp_space_at(dst->p, dst->len, k, &size)) break;
        dst->len = k;
    }
}

/* The visible text of an HTML fragment in one line: tags removed, whitespace collapsed, entities decoded. */
fhp_str fhp_clear_tags(fhp_arena *a, fhp_str s) { return fhp_clear_tags_in(a, s, 0); }

fhp_str fhp_clear_tags_in(fhp_arena *a, fhp_str s, int in_pre) {
    fhp_buf b;
    if (s.len == 0) return FHP_EMPTY;
    fhp_buf_init(&b, a, s.len);
    strip_tags(s, &b, in_pre);
    if (b.len && fhp_memchr(b.p, '&', b.len)) return fhp_decode(a, fhp_buf_str(&b), 0);
    return fhp_buf_str(&b);
}

fhp_str fhp_html_to_text(const char *html, size_t len, fhp_arena *a) {
    if (!a || !html) return FHP_EMPTY;
    return fhp_clear_tags(a, fhp_s(html, len));
}

/* Whether the visible text equals expected, comparing on the fly without allocations. */
int fhp_is_text(const fhp_doc *doc, fhp_node n, fhp_str expected) {
    fhp_str value = fhp_body_s(doc, n);
    const char *v = value.ptr, *e = expected.ptr;
    size_t len = value.len, matched = 0, i = 0;
    int prev_space = 1, pending_space = 0;
    if (value.len == 0 || expected.len == 0) return 0;
    /* <pre> keeps its whitespace in the text, raw text is as written: compare with the text itself (rare, the slow way) */
    if ((!(n + 1 < (fhp_node)doc->count && doc->nodes[n + 1].start < doc->nodes[n].end) && fhp_has_raw_text(fhp_nd(doc, n)->key))
        || fhp_doc_has_pre(doc)) {
        fhp_arena *s = fhp_scratch();
        fhp_mark m = fhp_arena_mark(s);
        int eq = fhp_str_eq(fhp_text(doc, n, s), expected);
        fhp_arena_release(s, m);
        return eq;
    }
    while (i < len) {
        unsigned char c = (unsigned char)v[i];
        size_t size;
        if (c == '<' && fhp_is_markup_start(v, len, i)) {
            int ok;
            size_t last = skip_markup(v, len, i, &ok);
            if (!ok) break;
            /* <br>, blocks and cells separate words, as in the text */
            if (!prev_space && separates_words(v, len, i)) {
                pending_space = 1;
                prev_space = 1;
            }
            i = last + 1;
            continue;
        }
        if (fhp_space_at(v, len, i, &size)) {
            if (!prev_space) {
                pending_space = 1;
                prev_space = 1;
            }
            i += size;
            continue;
        }
        if (c == '&') {
            /* Text with entities must be decoded before comparing: take the full path. */
            fhp_arena *s = fhp_scratch();
            fhp_mark m = fhp_arena_mark(s);
            int eq = fhp_str_eq(fhp_text(doc, n, s), expected);
            fhp_arena_release(s, m);
            return eq;
        }
        if (pending_space) {
            if (matched >= expected.len || e[matched] != ' ') return 0;
            matched++;
            pending_space = 0;
        }
        if (matched + size > expected.len || memcmp(e + matched, v + i, size) != 0) return 0;
        matched += size;
        prev_space = 0;
        i += size;
    }
    return matched == expected.len;
}

/* ----- text with line breaks ----- */

/* Writes text, collapsing whitespace and turning requested breaks into trimmed lines. */
typedef struct line_writer {
    fhp_buf out;
    size_t pending_breaks; /* new lines requested since the last text char */
    int pending_space;
} line_writer;

static void lw_char(line_writer *w, uint32_t c) {
    if (fhp_is_white_space(c)) {
        w->pending_space = 1;
        return;
    }
    if (w->out.len) {
        /* nothing before the first text char: leading breaks and spaces are trimmed */
        if (w->pending_breaks > 0) {
            size_t k, n = w->pending_breaks < 2 ? w->pending_breaks : 2; /* at most one blank line */
            for (k = 0; k < n; k++) fhp_buf_addc(&w->out, '\n');
        } else if (w->pending_space) {
            fhp_buf_addc(&w->out, ' ');
        }
    }
    w->pending_breaks = 0;
    w->pending_space = 0;
    if (c < 0x80)
        fhp_buf_addc(&w->out, (char)c);
    else
        fhp_buf_add_cp(&w->out, c);
}

fhp_str fhp_clear_tags_line_breaks(fhp_arena *a, fhp_str value) {
    line_writer w;
    fhp_arena *s;
    fhp_mark m;
    const char *b;
    size_t len, i = 0;
    if (value.len == 0) return FHP_EMPTY;
    fhp_buf_init(&w.out, a, value.len);
    w.pending_breaks = 0;
    w.pending_space = 0;
    s = fhp_scratch();
    m = fhp_arena_mark(s);
    value = fhp_normalize_newlines(s, value); /* values are read as a browser reads the page */
    b = value.ptr;
    len = value.len;
    while (i < len) {
        unsigned char c = (unsigned char)b[i];
        const char *close;
        fhp_str tag, name;
        int closing, known;
        size_t k;

        if (c == '&') {
            const char *rest = b + i + 1;
            size_t rest_len = len - i - 1, consumed;
            if (rest_len && rest[0] == '#') {
                uint32_t cp;
                consumed = fhp_match_numeric(rest, rest_len, &cp);
                if (consumed) {
                    lw_char(&w, cp);
                    i += 1 + consumed;
                    continue;
                }
            } else {
                fhp_str decoded;
                consumed = fhp_match_named(rest, rest_len, 0, &decoded);
                if (consumed) {
                    /* decoded chars go through the same whitespace/text logic (&nbsp; collapses, &lt; is plain text) */
                    size_t j = 0;
                    while (j < decoded.len) {
                        size_t size;
                        uint32_t cp = fhp_utf8_decode(decoded.ptr + j, decoded.len - j, &size);
                        lw_char(&w, cp);
                        j += size;
                    }
                    i += 1 + consumed;
                    continue;
                }
            }
            lw_char(&w, '&');
            i++;
            continue;
        }

        if (c != '<' || !fhp_is_markup_start(b, len, i)) {
            size_t size = 1;
            uint32_t cp = c < 0x80 ? c : fhp_utf8_decode(b + i, len - i, &size);
            lw_char(&w, cp);
            i += size;
            continue;
        }

        if (len - i >= 4 && memcmp(b + i, "<!--", 4) == 0) {
            ptrdiff_t end = fhp_index(b + i + 4, len - i - 4, "-->", 3);
            if (end < 0) break;
            i += 4 + (size_t)end + 3;
            continue;
        }

        close = (const char *)fhp_memchr(b + i, '>', len - i);
        if (!close) break; /* unterminated tag: the rest is markup */
        tag = fhp_s(b + i + 1, (size_t)(close - b) - i - 1);
        i = (size_t)(close - b) + 1;

        closing = tag.len && tag.ptr[0] == '/';
        name = closing ? fhp_s(tag.ptr + 1, tag.len - 1) : tag;
        for (k = 0; k < name.len; k++) {
            char ch = name.ptr[k];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == 0x0c || ch == '/') {
                name.len = k;
                break;
            }
        }
        known = fhp_lookup_key(name.ptr, name.len);
        if (known < 0) continue; /* unknown tag: just removed */

        switch (fhp_tag_line_break[known]) {
        case LB_LINE:
            if (known == KEY(FHP_TAG_BR))
                w.pending_breaks++; /* every <br> counts */
            else if (w.pending_breaks < 1)
                w.pending_breaks = 1;
            break;
        case LB_PARAGRAPH:
            if (w.pending_breaks < 2) w.pending_breaks = 2;
            break;
        case LB_SPACE:
            lw_char(&w, ' ');
            break;
        case LB_SKIP: {
            fhp_str t = fhp_trim_ascii_end(tag);
            if (!closing && !(t.len && t.ptr[t.len - 1] == '/')) {
                /* jump to the closing tag; it is removed on the next iteration */
                char end_tag[16];
                size_t n = fhp_tag_name_lens[known];
                ptrdiff_t end;
                end_tag[0] = '<';
                end_tag[1] = '/';
                memcpy(end_tag + 2, fhp_tag_names[known], n);
                end = fhp_index_fold(b + i, len - i, end_tag, n + 2);
                i = end >= 0 ? i + (size_t)end : len;
            }
            break;
        }
        default:
            break;
        }
    }
    {
        fhp_str result = fhp_buf_str(&w.out);
        if (a != s) fhp_arena_release(s, m); /* the result may have grown in the scratch arena itself */
        return result;
    }
}

fhp_str fhp_html_to_text_line_breaks(const char *html, size_t len, fhp_arena *a) {
    if (!a || !html) return FHP_EMPTY;
    return fhp_clear_tags_line_breaks(a, fhp_s(html, len));
}

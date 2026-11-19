/* Links: href / src made absolute against the page URL, written as .NET's System.Uri writes them.
 *
 * Resolution follows RFC 3986 the way the Go port does it (net/url: Parse + ResolveReference), byte for byte: URLs are
 * handled as byte strings, so percent-escapes of non-ASCII text work as in Go. */
#include "internal.h"

struct fhp_url {
    fhp_str scheme;
    fhp_str opaque;
    int has_user;
    fhp_str user; /* the userinfo as written back (escaped) */
    fhp_str host;
    fhp_str path;     /* decoded */
    fhp_str raw_path; /* the path as written, when it differs from the default escaping of path */
    int force_query;
    fhp_str raw_query;
    fhp_str fragment; /* decoded */
    fhp_str raw_fragment;
};

enum mode { M_PATH, M_HOST, M_USER_PASSWORD, M_FRAGMENT };

FHP_INLINE int is_alnum(unsigned char c) { return fhp_is_alpha_ascii(c) || fhp_is_digit(c); }

static int should_escape(unsigned char c, enum mode mode) {
    if (is_alnum(c)) return 0;
    if (mode == M_HOST) {
        switch (c) {
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';':
        case '=': case ':': case '[': case ']': case '<': case '>': case '"':
            return 0;
        default:
            break;
        }
    }
    switch (c) {
    case '-': case '_': case '.': case '~':
        return 0;
    case '$': case '&': case '+': case ',': case '/': case ':': case ';': case '=': case '?': case '@':
        switch (mode) {
        case M_PATH:
            return c == '?';
        case M_USER_PASSWORD:
            return c == '@' || c == '/' || c == '?' || c == ':';
        case M_FRAGMENT:
            return 0;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return !(mode == M_FRAGMENT && (c == '!' || c == '(' || c == ')' || c == '*'));
}

static unsigned char unhex(unsigned char c) {
    if (c >= '0' && c <= '9') return (unsigned char)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned char)(c - 'a' + 10);
    return (unsigned char)(c - 'A' + 10);
}

/* 0, or -1 on an invalid escape. */
static int unescape(fhp_arena *a, fhp_str s, enum mode mode, fhp_str *out) {
    size_t n = 0, i = 0;
    fhp_buf b;
    while (i < s.len) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (c == '%') {
            n++;
            if (i + 2 >= s.len || !fhp_is_hex((unsigned char)s.ptr[i + 1]) || !fhp_is_hex((unsigned char)s.ptr[i + 2]))
                return -1;
            /* in the host, %-encoding is only for non-ASCII bytes (and %25) */
            if (mode == M_HOST && unhex((unsigned char)s.ptr[i + 1]) < 8 && memcmp(s.ptr + i, "%25", 3) != 0) return -1;
            i += 3;
        } else {
            if (mode == M_HOST && c < 0x80 && should_escape(c, mode)) return -1;
            i++;
        }
    }
    if (n == 0) {
        *out = s;
        return 0;
    }
    fhp_buf_init(&b, a, s.len);
    for (i = 0; i < s.len;) {
        if (s.ptr[i] == '%') {
            fhp_buf_addc(&b, (char)(unhex((unsigned char)s.ptr[i + 1]) << 4 | unhex((unsigned char)s.ptr[i + 2])));
            i += 3;
        } else {
            fhp_buf_addc(&b, s.ptr[i]);
            i++;
        }
    }
    *out = fhp_buf_str(&b);
    return 0;
}

static const char upper_hex[] = "0123456789ABCDEF";

static fhp_str escape(fhp_arena *a, fhp_str s, enum mode mode) {
    fhp_buf b;
    size_t i;
    fhp_buf_init(&b, a, s.len + 8);
    for (i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (should_escape(c, mode)) {
            char e[3];
            e[0] = '%';
            e[1] = upper_hex[c >> 4];
            e[2] = upper_hex[c & 15];
            fhp_buf_add(&b, e, 3);
        } else {
            fhp_buf_addc(&b, (char)c);
        }
    }
    return fhp_buf_str(&b);
}

static int valid_encoded(fhp_str s, enum mode mode) {
    size_t i;
    for (i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        switch (c) {
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';':
        case '=': case ':': case '@': case '[': case ']': case '%':
            continue;
        default:
            if (should_escape(c, mode)) return 0;
        }
    }
    return 1;
}

static int get_scheme(fhp_str raw, fhp_str *scheme, fhp_str *rest) {
    size_t i;
    for (i = 0; i < raw.len; i++) {
        unsigned char c = (unsigned char)raw.ptr[i];
        if (fhp_is_alpha_ascii(c)) continue;
        if (fhp_is_digit(c) || c == '+' || c == '-' || c == '.') {
            if (i == 0) break;
            continue;
        }
        if (c == ':') {
            if (i == 0) return -1; /* missing protocol scheme */
            *scheme = fhp_s(raw.ptr, i);
            *rest = fhp_s(raw.ptr + i + 1, raw.len - i - 1);
            return 0;
        }
        break;
    }
    *scheme = FHP_EMPTY;
    *rest = raw;
    return 0;
}

static int set_path(fhp_arena *a, fhp_url *u, fhp_str p) {
    fhp_str path;
    if (unescape(a, p, M_PATH, &path) != 0) return -1;
    u->path = path;
    u->raw_path = fhp_str_eq(escape(a, path, M_PATH), p) ? FHP_EMPTY : p;
    return 0;
}

static fhp_str escaped_path(fhp_arena *a, const fhp_url *u) {
    if (u->raw_path.len && valid_encoded(u->raw_path, M_PATH)) {
        fhp_str p;
        if (unescape(a, u->raw_path, M_PATH, &p) == 0 && fhp_str_eq(p, u->path)) return u->raw_path;
    }
    if (u->path.len == 1 && u->path.ptr[0] == '*') return u->path;
    return escape(a, u->path, M_PATH);
}

static fhp_str escaped_fragment(fhp_arena *a, const fhp_url *u) {
    if (u->raw_fragment.len && valid_encoded(u->raw_fragment, M_FRAGMENT)) {
        fhp_str f;
        if (unescape(a, u->raw_fragment, M_FRAGMENT, &f) == 0 && fhp_str_eq(f, u->fragment)) return u->raw_fragment;
    }
    return escape(a, u->fragment, M_FRAGMENT);
}

static int valid_optional_port(fhp_str port) {
    size_t i;
    if (port.len == 0) return 1;
    if (port.ptr[0] != ':') return 0;
    for (i = 1; i < port.len; i++)
        if (!fhp_is_digit((unsigned char)port.ptr[i])) return 0;
    return 1;
}

static ptrdiff_t last_index(fhp_str s, char c) {
    size_t i = s.len;
    while (i > 0) {
        i--;
        if (s.ptr[i] == c) return (ptrdiff_t)i;
    }
    return -1;
}

static ptrdiff_t first_index(fhp_str s, char c) {
    const char *p = s.len ? (const char *)fhp_memchr(s.ptr, c, s.len) : NULL;
    return p ? p - s.ptr : -1;
}

FHP_INLINE fhp_str tail(fhp_str s, size_t from) { return fhp_s(s.ptr + from, s.len - from); }
FHP_INLINE fhp_str head(fhp_str s, size_t to) { return fhp_s(s.ptr, to); }

static int parse_host(fhp_arena *a, fhp_str host, fhp_str *out) {
    if (host.len && host.ptr[0] == '[') {
        ptrdiff_t i = last_index(host, ']');
        if (i < 0) return -1;
        if (!valid_optional_port(tail(host, (size_t)i + 1))) return -1;
    } else {
        ptrdiff_t i = last_index(host, ':');
        if (i >= 0 && !valid_optional_port(tail(host, (size_t)i))) return -1;
    }
    return unescape(a, host, M_HOST, out);
}

static int valid_userinfo(fhp_str s) {
    size_t i;
    for (i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (is_alnum(c) || (c && strchr("-._:~!$&'()*+,;=%@", c))) continue;
        return 0;
    }
    return 1;
}

static int parse_authority(fhp_arena *a, fhp_url *u, fhp_str authority) {
    ptrdiff_t at = last_index(authority, '@');
    fhp_str userinfo;
    ptrdiff_t colon;
    if (parse_host(a, at < 0 ? authority : tail(authority, (size_t)at + 1), &u->host) != 0) return -1;
    if (at < 0) return 0;
    userinfo = head(authority, (size_t)at);
    if (!valid_userinfo(userinfo)) return -1;
    colon = first_index(userinfo, ':');
    u->has_user = 1;
    if (colon < 0) {
        fhp_str name;
        if (unescape(a, userinfo, M_USER_PASSWORD, &name) != 0) return -1;
        u->user = escape(a, name, M_USER_PASSWORD);
    } else {
        fhp_str name, password;
        fhp_buf b;
        if (unescape(a, head(userinfo, (size_t)colon), M_USER_PASSWORD, &name) != 0) return -1;
        if (unescape(a, tail(userinfo, (size_t)colon + 1), M_USER_PASSWORD, &password) != 0) return -1;
        fhp_buf_init(&b, a, userinfo.len + 8);
        fhp_buf_adds(&b, escape(a, name, M_USER_PASSWORD));
        fhp_buf_addc(&b, ':');
        fhp_buf_adds(&b, escape(a, password, M_USER_PASSWORD));
        u->user = fhp_buf_str(&b);
    }
    return 0;
}

static int parse_no_fragment(fhp_arena *a, fhp_str raw, fhp_url *u) {
    size_t i;
    fhp_str scheme, rest;
    ptrdiff_t q;
    for (i = 0; i < raw.len; i++) {
        unsigned char c = (unsigned char)raw.ptr[i];
        if (c < 0x20 || c == 0x7f) return -1; /* invalid control character in URL */
    }
    memset(u, 0, sizeof *u);
    u->scheme = u->opaque = u->user = u->host = u->path = u->raw_path = FHP_EMPTY;
    u->raw_query = u->fragment = u->raw_fragment = FHP_EMPTY;
    if (raw.len == 1 && raw.ptr[0] == '*') {
        u->path = raw;
        return 0;
    }
    if (get_scheme(raw, &scheme, &rest) != 0) return -1;
    u->scheme = fhp_to_lower(a, scheme);

    q = first_index(rest, '?');
    if (q >= 0 && (size_t)q == rest.len - 1) {
        u->force_query = 1;
        rest.len--;
    } else if (q >= 0) {
        u->raw_query = tail(rest, (size_t)q + 1);
        rest = head(rest, (size_t)q);
    }

    if (!(rest.len && rest.ptr[0] == '/')) {
        ptrdiff_t slash;
        fhp_str segment;
        if (u->scheme.len) {
            /* rootless paths are opaque (RFC 3986) */
            u->opaque = rest;
            return 0;
        }
        /* a relative-path reference: the first segment cannot contain ':' */
        slash = first_index(rest, '/');
        segment = slash < 0 ? rest : head(rest, (size_t)slash);
        if (first_index(segment, ':') >= 0) return -1;
    }

    if ((u->scheme.len || !(rest.len >= 3 && memcmp(rest.ptr, "///", 3) == 0)) && rest.len >= 2 &&
        memcmp(rest.ptr, "//", 2) == 0) {
        fhp_str authority = tail(rest, 2);
        ptrdiff_t slash = first_index(authority, '/');
        rest = FHP_EMPTY;
        if (slash >= 0) {
            rest = tail(authority, (size_t)slash);
            authority = head(authority, (size_t)slash);
        }
        if (parse_authority(a, u, authority) != 0) return -1;
    }
    return set_path(a, u, rest);
}

/* net/url Parse on bytes. */
static int parse_url(fhp_arena *a, fhp_str raw, fhp_url *u) {
    ptrdiff_t hash = first_index(raw, '#');
    if (parse_no_fragment(a, hash < 0 ? raw : head(raw, (size_t)hash), u) != 0) return -1;
    if (hash >= 0) {
        fhp_str frag = tail(raw, (size_t)hash + 1);
        if (unescape(a, frag, M_FRAGMENT, &u->fragment) != 0) return -1;
        u->raw_fragment = fhp_str_eq(escape(a, u->fragment, M_FRAGMENT), frag) ? FHP_EMPTY : frag;
    }
    return 0;
}

static fhp_str resolve_path(fhp_arena *a, fhp_str base, fhp_str r) {
    fhp_buf full, dst;
    fhp_str remaining, elem = FHP_EMPTY;
    int first = 1, found = 1;
    fhp_buf_init(&full, a, base.len + r.len + 1);
    if (r.len == 0) {
        fhp_buf_adds(&full, base);
    } else if (r.ptr[0] != '/') {
        ptrdiff_t i = last_index(base, '/');
        fhp_buf_add(&full, base.ptr, i < 0 ? 0 : (size_t)i + 1);
        fhp_buf_adds(&full, r);
    } else {
        fhp_buf_adds(&full, r);
    }
    if (full.len == 0) return FHP_EMPTY;

    fhp_buf_init(&dst, a, full.len + 2);
    fhp_buf_addc(&dst, '/');
    remaining = fhp_buf_str(&full);
    while (found) {
        ptrdiff_t slash = first_index(remaining, '/');
        found = slash >= 0;
        if (found) {
            elem = head(remaining, (size_t)slash);
            remaining = tail(remaining, (size_t)slash + 1);
        } else {
            elem = remaining;
            remaining = FHP_EMPTY;
        }
        if (elem.len == 1 && elem.ptr[0] == '.') {
            first = 0;
            continue;
        }
        if (elem.len == 2 && elem.ptr[0] == '.' && elem.ptr[1] == '.') {
            fhp_str s = fhp_s(dst.p + 1, dst.len - 1);
            ptrdiff_t index = last_index(s, '/');
            if (index < 0) {
                dst.len = 1;
                first = 1;
            } else {
                /* "/" + s[..index] */
                dst.len = 1 + (size_t)index;
            }
        } else {
            if (!first) fhp_buf_addc(&dst, '/');
            fhp_buf_adds(&dst, elem);
            first = 0;
        }
    }
    if ((elem.len == 1 && elem.ptr[0] == '.') || (elem.len == 2 && elem.ptr[0] == '.' && elem.ptr[1] == '.'))
        fhp_buf_addc(&dst, '/');
    if (dst.len > 1 && dst.p[1] == '/') return fhp_s(dst.p + 1, dst.len - 1);
    return fhp_buf_str(&dst);
}

/* net/url ResolveReference. */
static void resolve_reference(fhp_arena *a, const fhp_url *base, const fhp_url *r, fhp_url *url) {
    *url = *r;
    if (r->scheme.len == 0) url->scheme = base->scheme;
    if (r->scheme.len || r->host.len || r->has_user) {
        fhp_str p = resolve_path(a, escaped_path(a, r), FHP_EMPTY);
        set_path(a, url, p); /* as net/url: on an error the path stays as it was */
        return;
    }
    if (r->opaque.len) {
        url->has_user = 0;
        url->user = url->host = url->path = FHP_EMPTY;
        return;
    }
    if (r->path.len == 0 && !r->force_query && r->raw_query.len == 0) {
        url->raw_query = base->raw_query;
        if (r->fragment.len == 0) {
            url->fragment = base->fragment;
            url->raw_fragment = base->raw_fragment;
        }
    }
    if (r->path.len == 0 && base->opaque.len) {
        url->opaque = base->opaque;
        url->has_user = 0;
        url->user = url->host = url->path = FHP_EMPTY;
        return;
    }
    url->host = base->host;
    url->has_user = base->has_user;
    url->user = base->user;
    set_path(a, url, resolve_path(a, escaped_path(a, base), escaped_path(a, r)));
}

static const char *default_port(fhp_str scheme) {
#define IS(w) (scheme.len == sizeof(w) - 1 && memcmp(scheme.ptr, w, scheme.len) == 0)
    if (IS("http") || IS("ws")) return "80";
    if (IS("https") || IS("wss")) return "443";
    if (IS("ftp")) return "21";
#undef IS
    return "";
}

static void split_host_port(fhp_str host_port, fhp_str *host, fhp_str *port) {
    ptrdiff_t colon = last_index(host_port, ':');
    *host = host_port;
    *port = FHP_EMPTY;
    if (colon >= 0 && valid_optional_port(tail(host_port, (size_t)colon))) {
        *port = tail(host_port, (size_t)colon + 1);
        *host = head(host_port, (size_t)colon);
    }
    if (host->len >= 2 && host->ptr[0] == '[' && host->ptr[host->len - 1] == ']') *host = fhp_s(host->ptr + 1, host->len - 2);
}

/* Decodes %XX escapes except those of reserved and control characters; escaped bytes of non-ASCII text are decoded
   when they form valid UTF-8. */
static void unescape_for_display(fhp_arena *a, fhp_str s, fhp_buf *out) {
    size_t i = 0;
#define ESCAPED(j) ((j) + 2 < s.len && s.ptr[j] == '%' && fhp_is_hex((unsigned char)s.ptr[(j) + 1]) && \
                    fhp_is_hex((unsigned char)s.ptr[(j) + 2]))
#define VALUE(j) ((unsigned char)(unhex((unsigned char)s.ptr[(j) + 1]) << 4 | unhex((unsigned char)s.ptr[(j) + 2])))
    if (first_index(s, '%') < 0) {
        fhp_buf_adds(out, s);
        return;
    }
    while (i < s.len) {
        unsigned char c;
        if (!ESCAPED(i)) {
            fhp_buf_addc(out, s.ptr[i]);
            i++;
            continue;
        }
        c = VALUE(i);
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f || strchr("%/?#[]@:!$&'()*+,;=", c))
                fhp_buf_add(out, s.ptr + i, 3);
            else
                fhp_buf_addc(out, (char)c);
            i += 3;
            continue;
        }
        {
            /* a run of escaped bytes >= 0x80: decoded together when it is valid UTF-8 */
            size_t j = i;
            fhp_buf run;
            fhp_buf_init(&run, a, 16);
            while (ESCAPED(j)) {
                unsigned char v = VALUE(j);
                if (v < 0x80) break;
                fhp_buf_addc(&run, (char)v);
                j += 3;
            }
            if (fhp_utf8_valid_prefix(run.p, run.len) == run.len)
                fhp_buf_add(out, run.p, run.len);
            else
                fhp_buf_add(out, s.ptr + i, j - i);
            i = j;
        }
    }
#undef ESCAPED
#undef VALUE
}

/* Writes a resolved URL as the .NET version does (System.Uri.ToString): the scheme and host in lower case, no default
   port, an empty path as "/", and percent-escapes decoded except those of the characters that delimit URL parts
   ("%2F" stays) and of control characters; non-ASCII text is written as is. */
static fhp_str display_string(fhp_arena *out, fhp_arena *tmp, const fhp_url *u) {
    fhp_buf b;
    fhp_str scheme = fhp_to_lower(tmp, u->scheme);
    fhp_buf_init(&b, out, 64);
    fhp_buf_adds(&b, scheme);
    fhp_buf_addc(&b, ':');
    if (u->opaque.len) {
        fhp_buf_adds(&b, u->opaque);
    } else {
        fhp_str path;
        if (u->host.len || u->has_user) {
            fhp_str host, port;
            fhp_buf_add(&b, "//", 2);
            if (u->has_user) {
                fhp_buf_adds(&b, u->user);
                fhp_buf_addc(&b, '@');
            }
            split_host_port(u->host, &host, &port);
            fhp_buf_adds(&b, fhp_to_lower(tmp, host));
            if (port.len && !fhp_str_eq(port, fhp_cs(default_port(scheme)))) {
                fhp_buf_addc(&b, ':');
                fhp_buf_adds(&b, port);
            }
        }
        path = escaped_path(tmp, u);
        if (path.len == 0 && u->host.len) path = fhp_s("/", 1);
        unescape_for_display(tmp, path, &b);
    }
    if (u->raw_query.len || u->force_query) {
        fhp_buf_addc(&b, '?');
        unescape_for_display(tmp, u->raw_query, &b);
    }
    if (u->fragment.len) {
        fhp_buf_addc(&b, '#');
        unescape_for_display(tmp, escaped_fragment(tmp, u), &b);
    }
    return fhp_buf_str(&b);
}

/* Parses a page URL with its own scheme and host; NULL for anything else. */
fhp_url *fhp_absolute_url(fhp_arena *a, fhp_str s) {
    fhp_url *u;
    if (s.len == 0) return NULL;
    u = (fhp_url *)fhp_alloc(a, sizeof(fhp_url));
    if (!u || parse_url(a, s, u) != 0) return NULL;
    if (u->scheme.len == 0 ||
        (u->host.len == 0 && u->opaque.len == 0 && !fhp_equal_fold(u->scheme, fhp_s("file", 4))))
        return NULL;
    return u;
}

/* The raw attribute value with its character references decoded and made absolute against the page. Percent-escapes
   are part of the URL and are kept. An empty link gives the page URL. */
fhp_str fhp_correct_link(fhp_arena *out, fhp_str raw, fhp_str page_url, const fhp_url *base) {
    fhp_str link, result;
    fhp_url r, u;
    fhp_arena *tmp;
    fhp_mark m;
    if (raw.len == 0) return fhp_s(fhp_strndup(out, page_url.ptr, page_url.len), page_url.len);
    link = fhp_decode(out, raw, 1);
    if (!base) return link;
    tmp = fhp_scratch();
    m = fhp_arena_mark(tmp);
    /* a link with its own scheme ("https://...", "mailto:...", "file:...") is absolute: "/path" and "//host/path"
       are relative */
    if (parse_url(tmp, fhp_trim(link), &r) != 0 || r.scheme.len) {
        fhp_arena_release(tmp, m);
        return link;
    }
    resolve_reference(tmp, base, &r, &u);
    result = display_string(out, tmp, &u);
    if (out != tmp) fhp_arena_release(tmp, m);
    return result;
}

static fhp_str link_from(const fhp_doc *doc, fhp_node n, const char *attr, const char *base_url, fhp_arena *a) {
    fhp_arena *out;
    fhp_str raw;
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    out = fhp_result_arena(doc, a);
    raw = fhp_attr_raw_s(doc, n, fhp_cs(attr));
    if (!base_url) return fhp_correct_link(out, raw, doc->url, doc->base);
    {
        fhp_arena *tmp = fhp_scratch();
        fhp_mark m = fhp_arena_mark(tmp);
        fhp_str page = fhp_cs(base_url);
        fhp_url *base = fhp_absolute_url(tmp, page);
        fhp_str result = fhp_correct_link(out, raw, page, base);
        if (out != tmp) fhp_arena_release(tmp, m);
        return result;
    }
}

fhp_str fhp_href(const fhp_doc *doc, fhp_node n, fhp_arena *a) { return link_from(doc, n, "href", NULL, a); }
fhp_str fhp_src(const fhp_doc *doc, fhp_node n, fhp_arena *a) { return link_from(doc, n, "src", NULL, a); }
fhp_str fhp_href_from(const fhp_doc *doc, fhp_node n, const char *base_url, fhp_arena *a) {
    return link_from(doc, n, "href", base_url ? base_url : "", a);
}
fhp_str fhp_src_from(const fhp_doc *doc, fhp_node n, const char *base_url, fhp_arena *a) {
    return link_from(doc, n, "src", base_url ? base_url : "", a);
}

static fhp_str href_after_scheme(const fhp_doc *doc, fhp_node n, const char *scheme, fhp_arena *a) {
    fhp_str href;
    size_t sl = strlen(scheme);
    ptrdiff_t colon;
    if (!fhp_valid(doc, n)) return FHP_EMPTY;
    href = fhp_trim(fhp_decode(fhp_result_arena(doc, a), fhp_attr_raw_s(doc, n, fhp_s("href", 4)), 1));
    if (href.len < sl || !fhp_eq_ignore_ascii_case(href.ptr, scheme, sl)) return FHP_EMPTY;
    colon = first_index(href, ':');
    return colon < 0 ? FHP_EMPTY : tail(href, (size_t)colon + 1);
}

fhp_str fhp_href_mail_to(const fhp_doc *doc, fhp_node n, fhp_arena *a) { return href_after_scheme(doc, n, "mailto", a); }
fhp_str fhp_href_telephone(const fhp_doc *doc, fhp_node n, fhp_arena *a) { return href_after_scheme(doc, n, "tel", a); }

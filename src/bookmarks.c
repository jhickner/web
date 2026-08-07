#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

#define CHROME_EPOCH 11644473600.0   // seconds between 1601-01-01 and the unix one

// empty bookmarks file, with chrome's root guids
static const char EMPTY_FILE[] =
"{\n"
"   \"roots\": {\n"
"      \"bookmark_bar\": {\n"
"         \"children\": [  ],\n"
"         \"guid\": \"0bc5d13f-2cba-5d74-951f-3f233fe6c908\",\n"
"         \"id\": \"1\",\n"
"         \"name\": \"Bookmarks bar\",\n"
"         \"type\": \"folder\"\n"
"      },\n"
"      \"other\": {\n"
"         \"children\": [  ],\n"
"         \"guid\": \"82b081ec-3dd3-529c-8475-ab6c344590dd\",\n"
"         \"id\": \"2\",\n"
"         \"name\": \"Other bookmarks\",\n"
"         \"type\": \"folder\"\n"
"      },\n"
"      \"synced\": {\n"
"         \"children\": [  ],\n"
"         \"guid\": \"4cf2e351-0e85-532b-bb37-df045d8f8d0f\",\n"
"         \"id\": \"3\",\n"
"         \"name\": \"Mobile bookmarks\",\n"
"         \"type\": \"folder\"\n"
"      }\n"
"   },\n"
"   \"version\": 1\n"
"}\n";

static Bookmark g_marks[BOOKMARK_MAX];
static int      g_n;
static long     g_maxid;      // largest node id in the file

static void bookmarks_path(char *out, size_t cap) {
    char profile[512];
    chrome_profile_path(profile, sizeof profile);
    snprintf(out, cap, "%s/Default/Bookmarks", profile);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(&b, chunk, n);
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) { buf_free(&b); return NULL; }
    if (!b.p) buf_add(&b, "", 0);
    *len = b.len;
    return b.p;                 // caller frees
}

// ------------------------------------------------------------------- json

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// past the value at `p`; the `p += (*p == '\\' && p[1]) ? 2 : 1` steps over an
// escaped quote inside a string
static const char *val_end(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
        return *p ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
                if (!*p) return p;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n')
        p++;
    return p;
}

// true when the text is one whole object with nothing after it
static bool json_whole(const char *p) {
    int depth = 0;
    p = skip_ws(p);
    if (*p != '{') return false;
    for (; *p; p++) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
            if (!*p) return false;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (--depth == 0) return *skip_ws(p + 1) == 0;
            if (depth < 0) return false;
        }
    }
    return false;
}

// this object's own keys only; returns the key's opening quote, or NULL
static const char *obj_key(const char *obj, const char *key) {
    const char *p = skip_ws(obj);
    if (*p != '{') return NULL;
    p++;
    size_t klen = strlen(key);
    for (;;) {
        p = skip_ws(p);
        if (*p == '}' || !*p) return NULL;
        if (*p != '"') return NULL;
        const char *at = p, *k = p + 1;
        const char *kend = val_end(p) - 1;          // the closing quote
        p = skip_ws(val_end(p));
        if (*p != ':') return NULL;
        const char *v = skip_ws(p + 1);
        if ((size_t)(kend - k) == klen && !memcmp(k, key, klen)) return at;
        p = skip_ws(val_end(v));
        if (*p == ',') p++;
    }
}

static const char *obj_get(const char *obj, const char *key) {
    const char *at = obj_key(obj, key);
    if (!at) return NULL;
    const char *p = skip_ws(val_end(at));
    return *p == ':' ? skip_ws(p + 1) : NULL;
}

// string value, unescaped into `out`
static void obj_str(const char *obj, const char *key, char *out, size_t cap) {
    out[0] = 0;
    const char *v = obj_get(obj, key);
    if (!v || *v != '"') return;
    const char *e = val_end(v);
    if (e <= v + 1) return;
    json_unescape(out, cap, v + 1, (size_t)(e - v - 2));
}

// chrome's microseconds from 1601, as a string, out as unix seconds
static double obj_time(const char *obj, const char *key) {
    char t[32];
    obj_str(obj, key, t, sizeof t);
    if (!t[0]) return 0;
    double us = strtod(t, NULL);
    return us > 0 ? us / 1000000.0 - CHROME_EPOCH : 0;
}

// ------------------------------------------------------------------- walking

static void walk_node(const char *node);

static void walk_array(const char *arr) {
    const char *p = skip_ws(arr);
    if (*p != '[') return;
    p++;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']' || !*p) return;
        walk_node(p);
        p = skip_ws(val_end(p));
        if (*p != ',') return;
        p++;
    }
}

static void walk_node(const char *node) {
    char type[16];
    obj_str(node, "type", type, sizeof type);
    char id[24];
    obj_str(node, "id", id, sizeof id);
    long n = strtol(id, NULL, 10);
    if (n > g_maxid) g_maxid = n;
    if (!strcmp(type, "folder")) {
        const char *kids = obj_get(node, "children");
        if (kids) walk_array(kids);
        return;
    }
    if (strcmp(type, "url") || g_n >= BOOKMARK_MAX) return;
    Bookmark *b = &g_marks[g_n];
    obj_str(node, "url", b->url, sizeof b->url);
    obj_str(node, "name", b->title, sizeof b->title);
    b->added = obj_time(node, "date_added");
    if (b->url[0]) g_n++;
}

// newest first
static int by_added(const void *x, const void *y) {
    const Bookmark *a = x, *b = y;
    if (a->added > b->added) return -1;
    if (a->added < b->added) return 1;
    return 0;
}

static bool g_roots;      // the file parsed

static char *load(size_t *len) {
    char path[600];
    bookmarks_path(path, sizeof path);
    size_t n = 0;
    char *text = read_file(path, &n);
    g_n = 0;
    g_maxid = 0;
    g_roots = false;
    if (!text) { if (len) *len = 0; return NULL; }
    if (!json_whole(text)) { if (len) *len = n; return text; }
    const char *roots = obj_get(text, "roots");
    const char *p = roots ? skip_ws(roots) : NULL;
    if (p && *p == '{') {
        g_roots = true;
        p++;
        for (;;) {
            p = skip_ws(p);
            if (*p != '"') break;
            p = skip_ws(val_end(p));
            if (*p != ':') break;
            const char *v = skip_ws(p + 1);
            if (*v == '{') walk_node(v);
            p = skip_ws(val_end(v));
            if (*p == ',') p++;
            else break;
        }
    }
    qsort(g_marks, (size_t)g_n, sizeof g_marks[0], by_added);
    if (len) *len = n;
    return text;
}

const Bookmark *bookmarks_all(int *n) {
    char *text = load(NULL);
    free(text);
    *n = g_n;
    return g_marks;
}

static char g_cur[1024];
static bool g_cur_known, g_cur_marked;

bool bookmark_current(App *a) {
    if (g_cur_known && !strcmp(g_cur, a->url)) return g_cur_marked;
    snprintf(g_cur, sizeof g_cur, "%s", a->url);
    g_cur_known = true;
    g_cur_marked = false;
    if (!a->url[0]) return false;
    char *text = load(NULL);
    free(text);
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_marks[i].url, a->url)) { g_cur_marked = true; break; }
    return g_cur_marked;
}

// -------------------------------------------------------------------- writing

static bool write_file(const char *body, size_t len) {
    char path[600], tmp[620];
    bookmarks_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.web", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(body, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

static void drop_checksum(Buf *b) {
    const char *k = obj_key(b->p, "checksum");
    if (!k) return;
    const char *e = skip_ws(val_end(k));
    if (*e != ':') return;
    e = skip_ws(val_end(e + 1));
    if (*e == ',') e++;
    size_t at = (size_t)(k - b->p), to = (size_t)(e - b->p);
    memmove(b->p + at, b->p + to, b->len - to + 1);
    b->len -= to - at;
}

// version 4 uuid; the two masks set the version and variant bits
static void make_guid(char *out, size_t cap) {
    unsigned char r[16];
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, r, sizeof r) : -1;
    if (fd >= 0) close(fd);
    if (got != (ssize_t)sizeof r) {
        snprintf(out, cap, "00000000-0000-4000-8000-%012lx", (long)(now_sec() * 1000));
        return;
    }
    r[6] = (unsigned char)((r[6] & 0x0f) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3f) | 0x80);
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

// a node in the shape chrome writes them, indented for the end of the bar
static void node_text(Buf *out, const App *a) {
    char guid[40];
    make_guid(guid, sizeof guid);
    char added[32];
    snprintf(added, sizeof added, "%.0f", (now_sec() + CHROME_EPOCH) * 1000000.0);
    buf_addf(out, "{\n            \"date_added\": \"%s\",\n", added);
    buf_addf(out, "            \"date_last_used\": \"0\",\n");
    buf_addf(out, "            \"guid\": \"%s\",\n", guid);
    buf_addf(out, "            \"id\": \"%ld\",\n", g_maxid + 1);
    buf_addf(out, "            \"name\": \"");
    json_escape_buf(out, a->title[0] ? a->title : a->url);
    buf_addf(out, "\",\n            \"type\": \"url\",\n            \"url\": \"");
    json_escape_buf(out, a->url);
    buf_addf(out, "\"\n         }");
}

// returns the `]` of the bookmark bar's children
static const char *bar_end(const char *text, bool *empty) {
    const char *roots = obj_get(text, "roots");
    const char *bar = roots ? obj_get(roots, "bookmark_bar") : NULL;
    const char *kids = bar ? obj_get(bar, "children") : NULL;
    if (!kids || *kids != '[') return NULL;
    const char *e = val_end(kids);              // past the `]`
    if (e <= kids) return NULL;
    const char *p = skip_ws(kids + 1);
    *empty = *p == ']';
    return e - 1;                               // the `]` itself
}

// the node with this url, in any folder
static const char *find_node(const char *node, const char *url, const char **end) {
    char type[16];
    obj_str(node, "type", type, sizeof type);
    if (!strcmp(type, "folder")) {
        const char *kids = obj_get(node, "children");
        const char *p = kids ? skip_ws(kids) : NULL;
        if (!p || *p != '[') return NULL;
        p++;
        for (;;) {
            p = skip_ws(p);
            if (*p == ']' || !*p) return NULL;
            const char *hit = find_node(p, url, end);
            if (hit) return hit;
            p = skip_ws(val_end(p));
            if (*p != ',') return NULL;
            p++;
        }
    }
    if (strcmp(type, "url")) return NULL;
    char u[1024];
    obj_str(node, "url", u, sizeof u);
    if (strcmp(u, url)) return NULL;
    *end = val_end(node);
    return node;
}

static const char *find_in_roots(const char *text, const char *url, const char **end) {
    const char *roots = obj_get(text, "roots");
    const char *p = roots ? skip_ws(roots) : NULL;
    if (!p || *p != '{') return NULL;
    p++;
    for (;;) {
        p = skip_ws(p);
        if (*p != '"') return NULL;
        p = skip_ws(val_end(p));
        if (*p != ':') return NULL;
        const char *v = skip_ws(p + 1);
        if (*v == '{') {
            const char *hit = find_node(v, url, end);
            if (hit) return hit;
        }
        p = skip_ws(val_end(v));
        if (*p == ',') p++;
        else return NULL;
    }
}

void bookmark_toggle(App *a) {
    g_cur_known = false;
    if (!a->url[0] || !strncmp(a->url, "about:", 6)) {
        notify(a, "nothing here to bookmark");
        return;
    }
    size_t len = 0;
    char *text = load(&len);
    if (text && !g_roots) {
        free(text);
        notify(a, "the bookmarks file is not one this can read");
        return;
    }
    Buf b = {0};
    if (text) {
        buf_add(&b, text, len);
    } else {
        buf_add(&b, EMPTY_FILE, sizeof EMPTY_FILE - 1);
        g_maxid = 3;            // the three root ids
    }
    free(text);

    const char *end = NULL;
    const char *node = find_in_roots(b.p, a->url, &end);
    if (node) {
        // cut the node and the comma on one side of it
        size_t at = (size_t)(node - b.p), to = (size_t)(end - b.p);
        while (at > 0 && (b.p[at - 1] == ' ' || b.p[at - 1] == '\n')) at--;
        if (at > 0 && b.p[at - 1] == ',') at--;
        else {
            const char *p = skip_ws(b.p + to);
            if (*p == ',') to = (size_t)(p - b.p) + 1;
        }
        memmove(b.p + at, b.p + to, b.len - to + 1);
        b.len -= to - at;
        drop_checksum(&b);
        notify(a, write_file(b.p, b.len) ? "bookmark removed"
                                         : "the bookmarks could not be written");
        buf_free(&b);
        return;
    }

    bool empty = false;
    const char *close = bar_end(b.p, &empty);
    if (!close) {
        notify(a, "the bookmarks file is not one this can add to");
        buf_free(&b);
        return;
    }
    size_t at = (size_t)(close - b.p);
    Buf node_buf = {0};
    if (!empty) buf_addf(&node_buf, ",");
    buf_addf(&node_buf, " ");
    node_text(&node_buf, a);
    buf_addf(&node_buf, " ");

    Buf out = {0};
    buf_add(&out, b.p, at);
    buf_add(&out, node_buf.p, node_buf.len);
    buf_add(&out, b.p + at, b.len - at);
    drop_checksum(&out);
    notify(a, write_file(out.p, out.len) ? "bookmarked"
                                         : "the bookmarks could not be written");
    buf_free(&node_buf);
    buf_free(&out);
    buf_free(&b);
}

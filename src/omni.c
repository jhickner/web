#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include "web.h"

#define OMNI_MAX  4000   // pages held from the history, newest first
#define OMNI_VIS  14     // rows the box will grow to
#define OMNI_MINW 40

// seconds between 1601-01-01 and the unix epoch
#define CHROME_EPOCH 11644473600.0

typedef struct {
    char   url[512];
    char   title[192];
    int    tab;          // the tab this is, or -1 for a page out of the history
    double last_visit;   // unix seconds, or 0 for a tab
    double score;
    int    seq;          // arrival order
} Row;

static Row g_rows[OMNI_MAX];
static int g_nrows;

// indices into g_rows, best first
static int g_ord[OMNI_MAX];
static int g_nord;

// ---------------------------------------------------------------- matching

#define TERMS_MAX 8

#define MATCH_ANYWHERE  1.0
#define MATCH_START     1.0
#define MATCH_WHOLE     1.0
#define MATCH_MAX       3.0     // the sum of the three

#define ONE_MONTH   (60.0 * 60 * 24 * 30)
#define RECENCY_CAL (2.0 / 3.0)   // recency against relevancy

typedef struct {
    char t[TERMS_MAX][48];
    bool cased[TERMS_MAX];   // smartcase: a capital in a term makes case matter
    int  n;
} Terms;

static void terms_split(const char *q, Terms *out) {
    out->n = 0;
    for (const char *p = q; *p && out->n < TERMS_MAX;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t n = (size_t)(p - s);
        if (n >= sizeof out->t[0]) n = sizeof out->t[0] - 1;
        memcpy(out->t[out->n], s, n);
        out->t[out->n][n] = 0;
        bool cased = false;
        for (size_t i = 0; i < n; i++)
            if (isupper((unsigned char)out->t[out->n][i])) cased = true;
        out->cased[out->n] = cased;
        out->n++;
    }
}

// returns NULL if not found; an uncased term matches either case
static const char *find_term(const char *hay, const char *term, bool cased) {
    size_t n = strlen(term);
    if (!n) return NULL;
    if (cased) return strstr(hay, term);
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, term, n)) return p;
    return NULL;
}

static bool is_word(unsigned char c) { return isalnum(c) || c == '_'; }

// `\b`: exactly one side of the index is a word character, the ends counting as not
static bool at_boundary(const char *s, size_t len, size_t i) {
    bool before = i > 0   && is_word((unsigned char)s[i - 1]);
    bool after  = i < len && is_word((unsigned char)s[i]);
    return before != after;
}

// length in utf-16 units, as javascript counts it
static int u16_len(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int len = (*p & 0xF8) == 0xF0 ? 4 : (*p & 0xF0) == 0xE0 ? 3 :
                  (*p & 0xE0) == 0xC0 ? 2 : 1;
        for (int i = 1; i < len; i++)
            if (!p[i]) { len = 1; break; }
        n += len == 4 ? 2 : 1;
        p += len;
    }
    return n;
}

// sets *count to the utf-16 units matched, capped at slen
static double score_term(const char *term, bool cased, const char *s, int slen,
                         int *count) {
    size_t tl = strlen(term), sl = strlen(s);
    *count = 0;
    if (!tl) return 0;
    int tu = u16_len(term), matched = 0;
    bool start = false, whole = false;
    for (const char *p = s; (p = find_term(p, term, cased)) != NULL; p += tl) {
        size_t i = (size_t)(p - s);
        matched += tu;
        if (at_boundary(s, sl, i)) {
            start = true;
            if (at_boundary(s, sl, i + tl)) whole = true;
        }
    }
    if (!matched) return 0;
    *count = matched > slen ? slen : matched;
    return MATCH_ANYWHERE + (start ? MATCH_START : 0) + (whole ? MATCH_WHOLE : 0);
}

// returns 0..1
static double norm_diff(double a, double b) {
    double max = a > b ? a : b;
    if (max <= 0) return 0;
    return (max - fabs(a - b)) / max;
}

static double word_relevancy(const Terms *q, const char *url, const char *title) {
    if (!q->n) return 0;
    bool titled = title && title[0];
    int ulen = u16_len(url), tlen = titled ? u16_len(title) : 0;
    double us = 0, ts = 0;
    int uc = 0, tc = 0, c;
    for (int i = 0; i < q->n; i++) {
        us += score_term(q->t[i], q->cased[i], url, ulen, &c);
        uc += c;
        if (titled) {
            ts += score_term(q->t[i], q->cased[i], title, tlen, &c);
            tc += c;
        }
    }
    double most = MATCH_MAX * q->n;
    us = us / most * norm_diff(uc, ulen);
    if (titled) ts = ts / most * norm_diff(tc, tlen);
    else        ts = us;
    if (us < ts) us = ts;
    return (us + ts) / 2;
}

static double recency_score(double last_visit) {
    if (last_visit <= 0) return 0;
    double left = ONE_MONTH - (now_sec() - last_visit);
    if (left < 0) left = 0;
    left /= ONE_MONTH;
    return left * left * left * RECENCY_CAL;
}

static bool matches(const Terms *q, const char *url, const char *title) {
    for (int i = 0; i < q->n; i++)
        if (!find_term(url, q->t[i], q->cased[i]) &&
            !(title[0] && find_term(title, q->t[i], q->cased[i])))
            return false;
    return true;
}

static int by_score(const void *a, const void *b) {
    const Row *x = &g_rows[*(const int *)a], *y = &g_rows[*(const int *)b];
    if (x->score > y->score) return -1;
    if (x->score < y->score) return 1;
    return x->seq - y->seq;
}

static void filter(App *a) {
    Terms q;
    terms_split(a->omni_q, &q);
    g_nord = 0;
    for (int i = 0; i < g_nrows; i++) {
        Row *r = &g_rows[i];
        if (q.n && !matches(&q, r->url, r->title)) continue;
        if (!q.n) {
            r->score = recency_score(r->last_visit);
        } else if (r->tab >= 0) {
            r->score = word_relevancy(&q, r->url, r->title);
        } else {
            double wr = word_relevancy(&q, r->url, r->title);
            double rs = recency_score(r->last_visit);
            r->score = (wr + (rs > wr ? rs : wr)) / 2;
        }
        g_ord[g_nord++] = i;
    }
    qsort(g_ord, (size_t)g_nord, sizeof g_ord[0], by_score);
    if (a->omni_sel >= g_nord) a->omni_sel = g_nord ? g_nord - 1 : 0;
    if (a->omni_sel < 0) a->omni_sel = 0;
}

// ------------------------------------------------------- a phrase off the line

// best bookmark for a phrase, relevancy only
bool omni_best_bookmark(const char *query, char *url, size_t cap) {
    Terms q;
    terms_split(query, &q);
    if (!q.n) return false;
    int n = 0;
    const Bookmark *b = bookmarks_all(&n);
    int best = -1;
    double top = 0;
    for (int i = 0; i < n; i++) {
        if (!matches(&q, b[i].url, b[i].title)) continue;
        double s = word_relevancy(&q, b[i].url, b[i].title);
        if (best < 0 || s > top) { best = i; top = s; }
    }
    if (best < 0) return false;
    snprintf(url, cap, "%s", b[best].url);
    return true;
}

// ----------------------------------------------------------------- loading

static void add_row(const char *url, const char *title, int tab, double last_visit) {
    if (g_nrows >= OMNI_MAX || !url || !*url) return;
    Row *r = &g_rows[g_nrows];
    snprintf(r->url, sizeof r->url, "%s", url);
    snprintf(r->title, sizeof r->title, "%s", title ? title : "");
    r->tab = tab;
    r->last_visit = last_visit;
    r->seq = g_nrows;
    r->score = 0;
    g_nrows++;
}

static void load_tabs(App *a) {
    g_nrows = 0;
    for (int i = 0; i < a->ntabs; i++)
        add_row(i == a->tab ? a->url : a->tabs[i].url,
                i == a->tab ? a->title : a->tabs[i].title, i, 0);
}

// takes an open file and closes it
static bool copy_into(const char *src, FILE *out) {
    FILE *in = fopen(src, "rb");
    if (!in) { fclose(out); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

// every folder, newest added first
static void load_bookmarks(void) {
    g_nrows = 0;
    int n = 0;
    const Bookmark *b = bookmarks_all(&n);
    for (int i = 0; i < n; i++) add_row(b[i].url, b[i].title, -1, 0);
}

// reads a throwaway copy of Chrome's history database
static bool load_history(App *a) {
    g_nrows = 0;
    char profile[512], src[640], tmp[64];
    chrome_profile_path(profile, sizeof profile);
    snprintf(src, sizeof src, "%s/Default/History", profile);
    snprintf(tmp, sizeof tmp, "/tmp/web-history-XXXXXX");
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); unlink(tmp); return false; }
    if (!copy_into(src, out)) { unlink(tmp); return false; }

    // the rollback journal, when there is one and the name can be claimed
    char sj[680], dj[96];
    snprintf(sj, sizeof sj, "%s-journal", src);
    snprintf(dj, sizeof dj, "%s-journal", tmp);
    int jfd = open(dj, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (jfd >= 0) {
        FILE *jout = fdopen(jfd, "wb");
        if (jout) copy_into(sj, jout);
        else      close(jfd);
    }

    sqlite3 *db = NULL;
    bool ok = false;
    if (sqlite3_open_v2(tmp, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_stmt *st = NULL;
        const char *SQL = "SELECT url,title,last_visit_time FROM urls WHERE hidden=0"
                          " ORDER BY last_visit_time DESC LIMIT ?";
        if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, OMNI_MAX);
            while (sqlite3_step(st) == SQLITE_ROW) {
                // microseconds since 1601
                sqlite3_int64 t = sqlite3_column_int64(st, 2);
                double when = t > 0 ? (double)t / 1000000.0 - CHROME_EPOCH : 0;
                add_row((const char *)sqlite3_column_text(st, 0),
                        (const char *)sqlite3_column_text(st, 1), -1, when);
            }
            ok = true;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    unlink(tmp);
    unlink(dj);
    return ok;
}

// ------------------------------------------------------------------ paint

typedef struct { int x, y, w, h, rows; } Shape;

static bool shape(App *a, Shape *s) {
    Term *t = &a->term;
    int rx = a->kitty.x > 0 ? a->kitty.x : 1;
    int ry = a->kitty.y > 0 ? a->kitty.y : 1;
    int rw = a->kitty.cols > 0 ? a->kitty.cols : t->cols;
    int rh = a->kitty.rows > 0 ? a->kitty.rows : t->rows;
    if (rx + rw - 1 > t->cols) rw = t->cols - rx + 1;
    if (ry + rh - 1 > t->rows) rh = t->rows - ry + 1;
    if (rw < OMNI_MINW + 4 || rh < 5) return false;

    int rows = g_nord < OMNI_VIS ? g_nord : OMNI_VIS;
    if (rows > rh - 4) rows = rh - 4;
    if (rows < 1) rows = 1;

    s->rows = rows;
    s->w = rw - 4 > 96 ? 96 : rw - 4;
    s->h = rows + 3;                    // border, the query, the rows, border
    s->x = rx + (rw - s->w) / 2;
    s->y = ry + (rh - s->h) / 4;
    if (s->y < ry) s->y = ry;
    return true;
}

static void pad(Buf *b, int n) {
    static const char sp[] = "                                ";
    while (n > 0) {
        int k = n > (int)sizeof sp - 1 ? (int)sizeof sp - 1 : n;
        buf_add(b, sp, (size_t)k);
        n -= k;
    }
}

// as much of s as fits in `cols` columns, cut on a utf-8 boundary; returns columns used
static int fit(const char *s, int cols, char *out, size_t cap) {
    int n = 0;
    size_t o = 0;
    while (s[o]) {
        unsigned char c = (unsigned char)s[o];
        if (c < 0x20) break;            // control byte
        int len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 :
                  (c & 0xE0) == 0xC0 ? 2 : 1;
        for (int i = 1; i < len; i++)
            if (!s[o + (size_t)i]) { len = 1; break; }
        unsigned cp = len == 1 ? c : (unsigned)(c & (0xFF >> (len + 1)));
        for (int i = 1; i < len; i++)
            cp = (cp << 6) | (unsigned)(s[o + (size_t)i] & 0x3F);
        int w = cp_width(cp);
        if (n + w > cols) break;
        if (o + (size_t)len + 1 >= cap) break;
        o += (size_t)len;
        n += w;
    }
    memcpy(out, s, o);
    out[o] = 0;
    return n;
}

static void border(Buf *b, const char *l, const char *r, const char *label, int w) {
    buf_addf(b, "\x1b[2m%s", l);
    int fill = w - 2;
    int ln = label ? (int)strlen(label) : 0;
    if (ln && fill > ln + 4) {
        buf_addf(b, "\xe2\x94\x80\x1b[0m\x1b[1;36m%s\x1b[0m\x1b[2m", label);
        fill -= ln + 1;
    }
    for (int i = 0; i < fill; i++) buf_add(b, "\xe2\x94\x80", 3);
    buf_addf(b, "%s\x1b[0m", r);
}

void omni_paint(App *a) {
    if (!a->has_tty || !a->omni_open) return;
    Shape s;
    if (!shape(a, &s)) return;

    if (a->omni_sel < a->omni_scroll) a->omni_scroll = a->omni_sel;
    if (a->omni_sel >= a->omni_scroll + s.rows) a->omni_scroll = a->omni_sel - s.rows + 1;
    if (a->omni_scroll > g_nord - s.rows) a->omni_scroll = g_nord - s.rows;
    if (a->omni_scroll < 0) a->omni_scroll = 0;

    int iw = s.w - 4;                   // inside the border and its two spaces
    int tw = iw * 11 / 20;              // the title's share, the address gets the rest
    if (tw < 8) tw = iw > 8 ? 8 : iw;

    Buf b = a->omni_buf;
    b.len = 0;
    for (int r = 0; r < s.h; r++) {
        buf_addf(&b, "\x1b[%d;%dH\x1b[0m", s.y + r, s.x);
        if (r == 0) {
            border(&b, "\xe2\x94\x8c", "\xe2\x94\x90",
                   a->omni_mode == OMNI_TABS  ? " tabs " :
                   a->omni_mode == OMNI_MARKS ? " bookmarks " : " history ", s.w);
            continue;
        }
        if (r == s.h - 1) {
            char more[64] = "";
            if (g_nord > s.rows)
                snprintf(more, sizeof more, " %d of %d ",
                         a->omni_sel + 1, g_nord);
            else if (!g_nord)
                snprintf(more, sizeof more, " nothing matches ");
            border(&b, "\xe2\x94\x94", "\xe2\x94\x98", more[0] ? more : NULL, s.w);
            continue;
        }
        buf_addf(&b, "\x1b[2m\xe2\x94\x82\x1b[0m ");
        if (r == 1) {
            int n = 0;
            size_t off = utf8_tail(a->omni_q, a->omni_qlen, iw - 2, &n);
            buf_addf(&b, "\x1b[1;36m>\x1b[0m %s", a->omni_q + off);
            pad(&b, iw - 2 - n);
        } else {
            int i = a->omni_scroll + (r - 2);
            if (i >= g_nord) {
                pad(&b, iw);
            } else {
                const Row *row = &g_rows[g_ord[i]];
                bool sel = i == a->omni_sel;
                char t[768], u[768];
                int tn = fit(row->title, tw, t, sizeof t);
                int un = fit(row->url, iw - tw - 1, u, sizeof u);
                if (sel) buf_addf(&b, "\x1b[7m");
                buf_addf(&b, "%s%s\x1b[0m", sel ? "" : "\x1b[1m", t);
                pad(&b, tw - tn);
                buf_addf(&b, " %s%s\x1b[0m", sel ? "\x1b[7m" : "\x1b[2m", u);
                pad(&b, iw - tw - 1 - un);
                if (sel) buf_addf(&b, "\x1b[0m");
            }
        }
        buf_addf(&b, " \x1b[2m\xe2\x94\x82\x1b[0m");
    }
    {
        int n = 0;
        utf8_tail(a->omni_q, a->omni_qlen, iw - 2, &n);
        buf_addf(&b, "\x1b[%d;%dH\x1b[?25h", s.y + 1, s.x + 4 + n);
    }
    a->omni_buf = b;

    if (b.len == a->omni_last.len && a->omni_grid == a->kitty.grid_draws &&
        (b.len == 0 || memcmp(b.p, a->omni_last.p, b.len) == 0))
        return;
    writeall(a->term.fd, b.p, b.len);
    a->omni_grid = a->kitty.grid_draws;
    a->omni_last.len = 0;
    buf_add(&a->omni_last, b.p, b.len);
}

// ------------------------------------------------------------------ input

void omni_close(App *a) {
    if (!a->omni_open) return;
    a->omni_open = false;
    a->omni_last.len = 0;
    g_nrows = g_nord = 0;
    a->status_last.len = 0;
    a->kitty.grid_dirty = true;
    a->last_hash = 0;
    relayout(a);
}

void omni_show(App *a, int mode) {
    if (!a->has_tty) return;
    a->omni_mode = mode;
    a->omni_q[0] = 0;
    a->omni_qlen = 0;
    a->omni_sel = 0;
    a->omni_scroll = 0;
    a->omni_last.len = 0;
    if (mode == OMNI_TABS) {
        load_tabs(a);
    } else if (mode == OMNI_MARKS) {
        load_bookmarks();
    } else if (!load_history(a)) {
        notify(a, "no history to search");
        return;
    }
    if (!g_nrows) {
        notify(a, mode == OMNI_TABS  ? "no tabs to search" :
                  mode == OMNI_MARKS ? "nothing has been bookmarked yet" :
                                       "no history to search");
        return;
    }
    a->omni_open = true;
    filter(a);
}

static void activate(App *a, bool tab) {
    if (a->omni_sel < 0 || a->omni_sel >= g_nord) { omni_close(a); return; }
    Row row = g_rows[g_ord[a->omni_sel]];     // copied: omni_close empties the list
    omni_close(a);
    if (row.tab >= 0) {
        tab_go(a, row.tab);
        return;
    }
    if (tab) tab_open_url(a, row.url);
    else     navigate(a, row.url);
}

bool omni_key(App *a, Event *ev) {
    if (!a->omni_open || ev->type != EV_KEY) return false;

    if (ev->key == KEY_ESC || (ev->mods == MOD_CTRL && ev->key == 'g')) {
        omni_close(a);
        return true;
    }
    if (ev->key == KEY_ENTER) {
        activate(a, ev->mods & MOD_SHIFT);
        return true;
    }
    if (ev->mods == MOD_CTRL && ev->key == 't') {
        activate(a, true);
        return true;
    }
    if (keys_lookup(ev->mods, ev->key) == ACT_QUIT) {
        omni_close(a);
        return false;
    }

    int step = 0;
    if (ev->key == KEY_DOWN || (ev->mods == MOD_CTRL && ev->key == 'n')) step = 1;
    else if (ev->key == KEY_UP || (ev->mods == MOD_CTRL && ev->key == 'p')) step = -1;
    else if (ev->key == KEY_TAB) step = (ev->mods & MOD_SHIFT) ? -1 : 1;
    else if (ev->key == KEY_PGDN) step = OMNI_VIS;
    else if (ev->key == KEY_PGUP) step = -OMNI_VIS;
    if (step) {
        if (!g_nord) return true;
        a->omni_sel += step;
        if (a->omni_sel < 0) a->omni_sel = step == -1 ? g_nord - 1 : 0;
        if (a->omni_sel >= g_nord) a->omni_sel = step == 1 ? 0 : g_nord - 1;
        return true;
    }

    if (ev->key == KEY_BACKSPACE) {
        // back over a whole utf-8 character
        while (a->omni_qlen &&
               ((unsigned char)a->omni_q[a->omni_qlen - 1] & 0xC0) == 0x80)
            a->omni_qlen--;
        if (a->omni_qlen) a->omni_qlen--;
        a->omni_q[a->omni_qlen] = 0;
        a->omni_sel = 0;
        filter(a);
        return true;
    }
    if (ev->mods == MOD_CTRL && ev->key == 'u') {
        a->omni_q[0] = 0;
        a->omni_qlen = 0;
        a->omni_sel = 0;
        filter(a);
        return true;
    }
    if (!(ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) && ev->text[0]) {
        size_t n = strlen(ev->text);
        if (a->omni_qlen + n + 1 < sizeof a->omni_q) {
            memcpy(a->omni_q + a->omni_qlen, ev->text, n);
            a->omni_qlen += n;
            a->omni_q[a->omni_qlen] = 0;
            a->omni_sel = 0;
            filter(a);
        }
        return true;
    }
    omni_close(a);
    return true;
}

void omni_paste(App *a, const char *text, size_t len) {
    size_t n = strcspn(text, "\r\n");
    if (n > len) n = len;
    if (a->omni_qlen + n + 1 > sizeof a->omni_q)
        n = sizeof a->omni_q - a->omni_qlen - 1;
    memcpy(a->omni_q + a->omni_qlen, text, n);
    a->omni_qlen += n;
    a->omni_q[a->omni_qlen] = 0;
    a->omni_sel = 0;
    filter(a);
}

void omni_free(App *a) {
    buf_free(&a->omni_buf);
    buf_free(&a->omni_last);
}

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include "web.h"

// Going somewhere by typing a few letters of it. Two lists rather than one -
// the tabs that are open, and the pages this profile has been to - because they
// are answers to different questions: one is `which of these am I already in`,
// and the other is `where was that thing`. Both are the same box, the same
// matching, and the same keys, so knowing one is knowing the other.
//
// Drawn the way the key list is, over the cells the picture is placed into
// rather than in a row of its own: a cell written as text stops being part of
// the image until the placeholders are put back, so the box costs nothing to
// draw and nothing to take away.

#define OMNI_MAX  4000   // pages held from the history, newest first
#define OMNI_VIS  14     // rows the box will grow to
#define OMNI_MINW 40

// Seconds between 1601-01-01 and the unix epoch, which is what Chrome
// timestamps have to be moved by.
#define CHROME_EPOCH 11644473600.0

typedef struct {
    char   url[512];
    char   title[192];
    int    tab;          // the tab this is, or -1 for a page out of the history
    double last_visit;   // unix seconds, or 0 for a tab
    double score;
    int    seq;          // the order it arrived in, to settle a tie by recency
} Row;

static Row g_rows[OMNI_MAX];
static int g_nrows;

// The rows that matched what has been typed, best first. Indices into g_rows,
// because the rows themselves are large and the order changes on every key.
static int g_ord[OMNI_MAX];
static int g_nord;

// ---------------------------------------------------------------- matching
//
// Vimium's ranking, followed rather than invented, because it is the thing this
// is meant to feel like and because guessing at weights is how a list ends up
// answering `ycom` with a login page. Two halves, as it has:
//
//   relevancy  every word typed has to be in the address or the name, and what
//              it scores depends on where - anywhere at all, the start of a
//              word, a whole word - scaled by how much of the line it accounts
//              for, so a term is worth more in a short title than buried in a
//              long address.
//   recency    a page seen lately is worth more, on a cube that falls away to
//              nothing at a month old. It can lift a page up the list and can
//              never push one down.
//
// Note what is not here: how often a page has been visited. Vimium does not
// look at it, and neither does this.

#define TERMS_MAX 8

// The weights, which are Vimium's own: a point for the term being there, a
// point for it starting a word, a point for it being a whole one.
#define MATCH_ANYWHERE  1.0
#define MATCH_START     1.0
#define MATCH_WHOLE     1.0
#define MATCH_MAX       3.0     // the sum of the three, for normalising by

#define ONE_MONTH   (60.0 * 60 * 24 * 30)
#define RECENCY_CAL (2.0 / 3.0)   // recency against relevancy, on Vimium's scale

typedef struct {
    char t[TERMS_MAX][48];
    bool cased[TERMS_MAX];   // smartcase: a capital in a term makes case matter
    int  n;
} Terms;

// The line typed, split on whitespace. Every word has to be found for a row to
// be shown at all, so `hacker news` is narrower than either word alone.
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

// Where a term sits in a line, or NULL. Smartcase: a term typed in lower case
// matches either, and a capital in it means the capital was meant.
static const char *find_term(const char *hay, const char *term, bool cased) {
    size_t n = strlen(term);
    if (!n) return NULL;
    if (cased) return strstr(hay, term);
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, term, n)) return p;
    return NULL;
}

static bool is_word(unsigned char c) { return isalnum(c) || c == '_'; }

// A `\b` sits at an index when exactly one side of it is a word character; the
// two ends of the line count as a side that is not.
static bool at_boundary(const char *s, size_t len, size_t i) {
    bool before = i > 0   && is_word((unsigned char)s[i - 1]);
    bool after  = i < len && is_word((unsigned char)s[i]);
    return before != after;
}

// How long a string is to javascript, which counts utf-16 units rather than
// bytes. The scores below are one length divided by another, so a title with a
// `·` in it comes out at a different number entirely if it is counted in bytes:
// the middle dot is two of those and one of these. Anything outside the basic
// plane - most emoji - is two units.
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

// One term against one line: what it scores, and how much of the line it
// accounts for across every place it appears, counted the same way.
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
        // Whole word implies start of word, so the one is asked inside the
        // other. Occurrences do not overlap, which is what splitting on the
        // term would have done.
        if (at_boundary(s, sl, i)) {
            start = true;
            if (at_boundary(s, sl, i + tl)) whole = true;
        }
    }
    if (!matched) return 0;
    *count = matched > slen ? slen : matched;
    return MATCH_ANYWHERE + (start ? MATCH_START : 0) + (whole ? MATCH_WHOLE : 0);
}

// How much of the line the matched characters are, between 0 and 1.
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
    // A poor address is not allowed to drag down a good name: an address can be
    // unreasonably long, and being long is not being wrong.
    if (us < ts) us = ts;
    return (us + ts) / 2;
}

// A cube, so yesterday is worth much more than the day before, and a month is
// where it reaches nothing at all.
static double recency_score(double last_visit) {
    if (last_visit <= 0) return 0;
    double left = ONE_MONTH - (now_sec() - last_visit);
    if (left < 0) left = 0;
    left /= ONE_MONTH;
    return left * left * left * RECENCY_CAL;
}

// Every term somewhere in the address or the name. This is the filter rather
// than part of the score: a row that fails it is not shown at all.
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
            // Nothing typed is not a question about which page is the better
            // one, so what is left is how lately it was seen - and for a tab,
            // the order the bar has it in.
            r->score = recency_score(r->last_visit);
        } else if (r->tab >= 0) {
            // A tab is scored on the words alone. It is open, so how long ago
            // it was first opened says nothing about whether it is the one.
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
        // The tab in front is the only one whose address the window is
        // watching; the rest are where each was when it was last looked away
        // from, which is what the bar draws too.
        add_row(i == a->tab ? a->url : a->tabs[i].url,
                i == a->tab ? a->title : a->tabs[i].title, i, 0);
}

// Into a file already opened, because every destination here is one this made
// itself: a name in /tmp that is guessed rather than claimed is a name someone
// else can have pointed at something of yours before the copy opens it.
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

// Chrome's own history, which is a sqlite database in the profile and is being
// written to by the browser that is still running. Read from a copy rather than
// in place: the file is locked, and a reader that waits for the lock is a
// window that stops answering the keyboard until a page finishes loading. The
// copy is a megabyte at most and is thrown away before this returns.
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

    // The rollback journal beside it, when there is one: without it a copy
    // taken part way through a write is a database sqlite will not open. The
    // name is the database's and cannot be a made-up one, so it is claimed
    // rather than opened - if something is already sitting there, it is not
    // ours and the journal is done without.
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
                // Chrome counts microseconds from 1601, which is neither the
                // unit nor the epoch anything else here uses.
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

// The picture's own rect rather than the terminal's, as the key list does:
// inline the window is narrower than the screen, and a box drawn past its edge
// reads as part of the shell rather than as part of the page.
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
    // Nearer the top than the middle, because the eye is already up at the
    // line being typed and the list grows downwards from it.
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

// How many cells a codepoint takes. Not a wcwidth - the locale is not to be
// trusted with this and the table would be most of the file - but the ranges
// that actually turn up in the title of a web page: the CJK a page in Japanese
// is written in, the emoji half of X and YouTube put in theirs, and the
// combining marks and variation selectors that hang off a character already
// counted. Anything unlisted is one cell, which is what almost everything is.
static int cp_width(unsigned c) {
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0xFE00 && c <= 0xFE0F) ||
        c == 0x200D || (c >= 0x20D0 && c <= 0x20F0)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) ||
        (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F9FF) ||
        (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}

// As much of a string as fits in `cols` columns, cut between characters rather
// than inside one: a title is whatever a page called itself, and half a utf-8
// sequence sent to the terminal is a broken cell that stays broken. Returns the
// columns used, which is what the padding after it is worked out from - a title
// counted a cell short is a border that does not line up with the one above it.
static int fit(const char *s, int cols, char *out, size_t cap) {
    int n = 0;
    size_t o = 0;
    while (s[o]) {
        unsigned char c = (unsigned char)s[o];
        if (c < 0x20) break;            // a control byte is not worth drawing
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

    // The selected row is kept on screen by moving the window of rows rather
    // than the selection: what was picked stays picked while the list scrolls
    // under it.
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
                   a->omni_mode == OMNI_TABS ? " tabs " : " history ", s.w);
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
            // The line being typed, with the cursor left sitting after it.
            char q[160];
            int n = fit(a->omni_q, iw - 2, q, sizeof q);
            buf_addf(&b, "\x1b[1;36m>\x1b[0m %s", q);
            pad(&b, iw - 2 - n);
        } else {
            int i = a->omni_scroll + (r - 2);
            if (i >= g_nord) {
                pad(&b, iw);
            } else {
                const Row *row = &g_rows[g_ord[i]];
                bool sel = i == a->omni_sel;
                char t[768], u[768];
                // A page that never said what it was leaves the name column
                // empty rather than filling it with the address that is already
                // in the next one.
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
    // Back onto the query line, where what is being typed appears.
    {
        char q[160];
        int n = fit(a->omni_q, iw - 2, q, sizeof q);
        buf_addf(&b, "\x1b[%d;%dH\x1b[?25h", s.y + 1, s.x + 4 + n);
    }
    a->omni_buf = b;

    // The same guard the key list uses: an unchanged box over an unchanged
    // grid is bytes the terminal has already been sent.
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
    // The box leaves the cursor sitting on its own line, and the status line is
    // what puts the cursor away again - but only when it redraws, and nothing
    // about it changed while this was up. Made to redraw, or the cursor is left
    // blinking on the page.
    a->status_last.len = 0;
    // The box lives in the cells that carry the picture, so taking it away is
    // putting those cells back and asking for a frame to land in them.
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
    } else if (!load_history(a)) {
        notify(a, "no history to search");
        return;
    }
    if (!g_nrows) {
        notify(a, mode == OMNI_TABS ? "no tabs to search" : "no history to search");
        return;
    }
    a->omni_open = true;
    filter(a);
}

// What the row under the selection means: switching to a tab that is already
// open, or going to a page that is not.
static void activate(App *a, bool tab) {
    if (a->omni_sel < 0 || a->omni_sel >= g_nord) { omni_close(a); return; }
    Row row = g_rows[g_ord[a->omni_sel]];     // copied: omni_close empties the list
    omni_close(a);
    if (row.tab >= 0) {
        // A tab already open is gone to rather than opened again, whichever key
        // was used to say so: there is nothing to put in a new one.
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
    // Quit is quit from here as much as from anywhere else.
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
        // Round the ends, because a list this short is quicker to walk backwards
        // off the top than to page down to the bottom of.
        a->omni_sel += step;
        if (a->omni_sel < 0) a->omni_sel = step == -1 ? g_nord - 1 : 0;
        if (a->omni_sel >= g_nord) a->omni_sel = step == 1 ? 0 : g_nord - 1;
        return true;
    }

    if (ev->key == KEY_BACKSPACE) {
        // Back over a whole character rather than a byte, so what was typed and
        // what is deleted are the same thing.
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
    // Anything else is somewhere else the user meant to be.
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

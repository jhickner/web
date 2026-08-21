#include <stdio.h>
#include <string.h>
#include "web.h"

#define GAP 3          // columns between two columns of the list
#define MAX_COLS 4
#define MARGIN_Y 2     // rows left showing above and below the box

// `what` alone: a heading; all fields empty: a blank
typedef struct {
    const char *key;
    Act         act, act2;
    const char *what;
} Bind;

static const Bind BINDS[] = {
    {.what = "Scrolling"},
    {.act = ACT_LINE_DOWN,   .act2 = ACT_LINE_UP,      .what = "a line at a time"},
    {.act = ACT_SCROLL_DOWN, .act2 = ACT_SCROLL_UP,    .what = "down / up"},
    {.act = ACT_HALF_DOWN,   .act2 = ACT_HALF_UP,      .what = "half a screen"},
    {.act = ACT_PAGE_DOWN,   .act2 = ACT_PAGE_UP,      .what = "a screen"},
    {.act = ACT_TOP,         .act2 = ACT_BOTTOM,       .what = "top / bottom"},
    {.act = ACT_SCROLL_LEFT, .act2 = ACT_SCROLL_RIGHT, .what = "left / right"},
    {0},
    {.what = "Page"},
    {.act = ACT_ADDRESS,                          .what = "address bar"},
    {.act = ACT_ADDRESS_BLANK,                    .what = "a blank one"},
    {.act = ACT_ADDRESS_TAB,                      .what = "a blank one, into a new tab"},
    {.act = ACT_BACK,     .act2 = ACT_FORWARD,    .what = "back / forward"},
    {.act = ACT_RELOAD,                           .what = "reload"},
    {.act = ACT_RELOAD_HARD,                      .what = "reload, ignoring the cache"},
    {.act = ACT_COPY,                             .what = "copy selection or address"},
    {.act = ACT_COPY_URL,                         .what = "copy the address"},
    {.act = ACT_COPY_CONSOLE,                     .what = "copy the console transcript"},
    {.act = ACT_RESUME,                           .what = "let a frozen run carry on"},
    {.act = ACT_PLAY_PAUSE,                       .what = "play or pause the video"},
    {.act = ACT_GRID,                             .what = "every tab at once, in a grid"},
    {.act = ACT_RECORD,                           .what = "write what you do as a spec"},
    {.act = ACT_EXTERNAL,                         .what = "open in the desktop browser"},
    {.act = ACT_FIND,                             .what = "find in the page"},
    {.act = ACT_FIND_NEXT, .act2 = ACT_FIND_PREV, .what = "next match / the one before"},
    {.act = ACT_INSERT,                           .what = "give the keyboard to the page"},
    {.act = ACT_INSERT_OFF,                       .what = "take it back"},
    {.act = ACT_FOCUS_INPUT,                      .what = "into the first text field"},
    {.act = ACT_PICK,                             .what = "pick a CSS selector"},
    {.act = ACT_BOOKMARK,                         .what = "bookmark this page, or unbookmark it"},
    {0},
    {.what = "Links"},
    {.act = ACT_HINT,                             .what = "label them, then type one"},
    {.act = ACT_HINT_TAB,                         .what = "one of them in a new tab"},
    {.act = ACT_HINT_COPY,                        .what = "copy one's address"},
    {.act = ACT_HINT_ALL,                         .what = "label everything, past any rule"},
    {0},
    {.what = "Tabs"},
    {.act = ACT_SEARCH_TABS,                     .what = "find an open tab by name"},
    {.act = ACT_SEARCH_HISTORY,                  .what = "find a page been to before"},
    {.act = ACT_SEARCH_BOOKMARKS,                .what = "find a bookmark"},
    {.act = ACT_TAB_NEW,  .act2 = ACT_TAB_CLOSE, .what = "new / close"},
    {.act = ACT_TAB_NEXT, .act2 = ACT_TAB_PREV,  .what = "next / previous"},
    {.act = ACT_MERGE,                           .what = "every other window's tabs, into this one"},
    {.act = ACT_TAB_1, .act2 = ACT_TAB_9,        .what = "that tab, and the ones between"},
    {0},
    {.what = "Window"},
    {.act = ACT_SMALLER,     .act2 = ACT_LARGER,        .what = "zoom out / in"},
    {.act = ACT_ZOOM_OUT,    .act2 = ACT_ZOOM_IN,       .what = "zoom out / in"},
    {.act = ACT_ZOOM_RESET,                             .what = "reset zoom and width"},
    {.act = ACT_FIT,                                    .what = "fit width on / off"},
    {.act = ACT_PAGE_WIDER,  .act2 = ACT_PAGE_NARROWER, .what = "widen / narrow the page"},
    {.act = ACT_SCALE,                                  .what = "frame size"},
    {.act = ACT_BOX_TALLER,  .act2 = ACT_BOX_SHORTER,   .what = "the window taller / shorter"},
    {.act = ACT_BOX_WIDER,   .act2 = ACT_BOX_NARROWER,  .what = "wider / narrower"},
    {0},
    {.what = "General"},
    {.act = ACT_CONSOLE,                       .what = "console"},
    {.act = ACT_STATUS,                        .what = "hide the status line"},
    {.act = ACT_TRACE,                         .what = "trace to /tmp/web_input.log"},
    {.act = ACT_BLUR_PAUSE,                    .what = "pausing and hiding on blur, on / off"},
    {.act = ACT_MOUSE_FREE,                    .what = "hand the mouse to the terminal, or take it back"},
    {.act = ACT_DAMAGE,                        .what = "send only the part of the frame that changed, on / off"},
    {.act = ACT_HELP,                          .what = "this list"},
    {.act = ACT_QUIT,                          .what = "quit"},
    {0},
    {.what = "Configuration"},
    {.key = "", .what = "~/.config/web/web.conf"},
};
#define NBINDS ((int)(sizeof BINDS / sizeof BINDS[0]))

// the rows that have a key to show, with their headings and blanks
static const Bind *g_list[NBINDS + 1];
static int         g_nlist;

#define LINES g_nlist

// start index of each column, ending with LINES
static int g_colstart[NBINDS + 2];

typedef struct {
    int x, y, w, h;        // the box, in 1-based cells
    int cols, rows;        // columns to a page, and rows in each
    int ncols, pages;      // columns in the whole list, and pages they fill
    int colw, keyw;
} Shape;

static int g_keyw, g_descw;

// NULL for a heading or a blank; otherwise e->key or buf
static const char *entry_key(const Bind *e, char *buf, size_t cap) {
    if (e->key) return e->key;
    if (!e->act) return NULL;
    char one[48], two[48];
    keys_text(e->act, one, sizeof one);
    if (e->act2) {
        keys_text(e->act2, two, sizeof two);
        snprintf(buf, cap, "%s / %s", one, two);
    } else {
        snprintf(buf, cap, "%s", one);
    }
    return buf;
}

static bool row_live(const Bind *e) {
    char buf[48];
    if (e->key || !e->act) return true;          // a heading, a blank, or a note
    if (keys_text(e->act, buf, sizeof buf)) return true;
    return e->act2 && keys_text(e->act2, buf, sizeof buf);
}

static void build_list(void) {
    static const Bind BLANK = {0};
    const Bind *head = NULL;
    g_nlist = 0;
    for (int i = 0; i < NBINDS; i++) {
        const Bind *e = &BINDS[i];
        if (!e->key && !e->act && e->what) { head = e; continue; }   // a heading
        if (!e->key && !e->act && !e->what) continue;                // a blank
        if (!row_live(e)) continue;
        if (head) {
            if (g_nlist) g_list[g_nlist++] = &BLANK;
            g_list[g_nlist++] = head;
            head = NULL;
        }
        g_list[g_nlist++] = e;
    }
}

static bool is_blank(int i) {
    return !g_list[i]->key && !g_list[i]->act && !g_list[i]->what;
}

// breaks the list into columns `rows` tall, each opening on a heading
static int layout(int rows) {
    int n = 0, used = 0;
    for (int i = 0; i < LINES; ) {
        int start = i, len = 0;
        while (i < LINES && !is_blank(i)) { i++; len++; }
        while (i < LINES && is_blank(i)) i++;
        bool tall = len > rows;    // taller than any column
        if (!n || (tall ? used + 3 > rows : used + 1 + len > rows)) {
            g_colstart[n++] = start;
            used = len;
        } else {
            used += 1 + len;
        }
        while (used > rows && n <= NBINDS) {
            g_colstart[n] = g_colstart[n - 1] + rows;
            n++;
            used -= rows;
        }
    }
    if (!n) g_colstart[n++] = 0;
    g_colstart[n] = LINES;
    return n;
}

static int col_height(int c, int rows) {
    int h = g_colstart[c + 1] - g_colstart[c];
    if (h > rows) h = rows;
    while (h > 0 && is_blank(g_colstart[c] + h - 1)) h--;
    return h;
}

static void measure(void) {
    if (g_keyw) return;
    build_list();
    for (int i = 0; i < LINES; i++) {
        char buf[104];
        const char *key = entry_key(g_list[i], buf, sizeof buf);
        int k = key ? (int)strlen(key) : 0;
        int d = g_list[i]->what ? (int)strlen(g_list[i]->what) : 0;
        if (k > g_keyw) g_keyw = k;
        if (d > g_descw) g_descw = d;
    }
}

static bool shape(App *a, Shape *s) {
    Term *t = &a->term;
    int rx = a->kitty.x > 0 ? a->kitty.x : 1;
    int ry = a->kitty.y > 0 ? a->kitty.y : 1;
    int rw = a->kitty.cols > 0 ? a->kitty.cols : t->cols;
    int rh = a->kitty.rows > 0 ? a->kitty.rows : t->rows;
    if (rx + rw - 1 > t->cols) rw = t->cols - rx + 1;
    if (ry + rh - 1 > t->rows) rh = t->rows - ry + 1;

    int avail_w = rw - 4, avail_h = rh - 2;
    if (avail_w < 20 || avail_h < 1) return false;
    if (avail_h - 2 * MARGIN_Y >= 8) avail_h -= 2 * MARGIN_Y;

    measure();
    int colw = g_keyw + 2 + g_descw;
    if (colw > avail_w) colw = avail_w;

    int maxn = 1;
    while (maxn < MAX_COLS && (maxn + 1) * colw + maxn * GAP <= avail_w) maxn++;

    int ncols = layout(avail_h);
    int pages = (ncols + maxn - 1) / maxn;
    int n = (ncols + pages - 1) / pages;

    int rows = 1;
    for (int c = 0; c < ncols; c++) {
        int h = col_height(c, avail_h);
        if (h > rows) rows = h;
    }

    s->cols = n;
    s->rows = rows;
    s->ncols = ncols;
    s->pages = pages;
    s->colw = colw;
    s->keyw = g_keyw > colw - 4 ? (colw > 5 ? colw - 4 : 1) : g_keyw;
    s->w = n * colw + (n - 1) * GAP + 4;
    s->h = rows + 2;
    s->x = rx + (rw - s->w) / 2;
    s->y = ry + (rh - s->h) / 2;
    return true;
}

// ------------------------------------------------------------------ paint

static void pad(Buf *b, int n) {
    static const char sp[] = "                                ";
    while (n > 0) {
        int k = n > (int)sizeof sp - 1 ? (int)sizeof sp - 1 : n;
        buf_add(b, sp, (size_t)k);
        n -= k;
    }
}

static void put_entry(Buf *b, const Bind *e, int keyw, int colw) {
    if (!e || (!e->key && !e->act && !e->what)) { pad(b, colw); return; }
    char buf[104];
    const char *key = entry_key(e, buf, sizeof buf);
    if (!key) {
        int n = (int)strlen(e->what);
        if (n > colw) n = colw;
        buf_addf(b, "\x1b[1;36m%.*s\x1b[0m", n, e->what);
        pad(b, colw - n);
        return;
    }
    int kn = (int)strlen(key);
    if (kn > keyw) kn = keyw;
    buf_addf(b, "\x1b[1m%*.*s\x1b[0m  ", keyw, kn, key);
    int dw = colw - keyw - 2;
    int dn = (int)strlen(e->what);
    if (dn > dw) dn = dw;
    buf_addf(b, "\x1b[2m%.*s\x1b[0m", dn, e->what);
    pad(b, dw - dn);
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

static void bottom(Buf *b, int w, int page, int pages) {
    char num[24] = "";
    int mid = pages > 1 ? 2 * pages + 1 : 0;    // the dots, a space either side
    if (pages > 1 && mid > w - 6) {
        snprintf(num, sizeof num, " %d/%d ", page + 1, pages);
        mid = (int)strlen(num);
        if (mid > w - 4) { num[0] = 0; mid = 0; }
    }
    int left = (w - 2 - mid) / 2, right = w - 2 - mid - left;
    buf_addf(b, "\x1b[2m\xe2\x94\x94");
    for (int i = 0; i < left; i++) buf_add(b, "\xe2\x94\x80", 3);
    if (num[0]) {
        buf_addf(b, "\x1b[0;1;36m%s\x1b[0m\x1b[2m", num);
    } else if (mid) {
        buf_add(b, " ", 1);
        for (int i = 0; i < pages; i++) {
            if (i) buf_add(b, " ", 1);
            if (i == page) buf_addf(b, "\x1b[0;1;36m\xe2\x97\x8f\x1b[0m\x1b[2m");
            else           buf_add(b, "\xe2\x97\x8b", 3);
        }
        buf_add(b, " ", 1);
    }
    for (int i = 0; i < right; i++) buf_add(b, "\xe2\x94\x80", 3);
    buf_addf(b, "\xe2\x94\x98\x1b[0m");
}

void help_paint(App *a) {
    if (!a->has_tty || !a->help_open) return;
    Shape s;
    if (!shape(a, &s)) return;
    if (a->help_page >= s.pages) a->help_page = s.pages - 1;
    if (a->help_page < 0) a->help_page = 0;

    Buf b = a->help_buf;
    b.len = 0;
    for (int r = 0; r < s.h; r++) {
        buf_addf(&b, "\x1b[%d;%dH\x1b[0m", s.y + r, s.x);
        if (r == 0) {
            border(&b, "\xe2\x94\x8c", "\xe2\x94\x90", " Key bindings ", s.w);
        } else if (r == s.h - 1) {
            bottom(&b, s.w, a->help_page, s.pages);
        } else {
            buf_addf(&b, "\x1b[2m\xe2\x94\x82\x1b[0m ");
            for (int c = 0; c < s.cols; c++) {
                if (c) pad(&b, GAP);
                int col = a->help_page * s.cols + c;
                int i   = col < s.ncols ? g_colstart[col] + (r - 1) : LINES;
                bool in = col < s.ncols && i < g_colstart[col + 1];
                put_entry(&b, in ? g_list[i] : NULL, s.keyw, s.colw);
            }
            buf_addf(&b, " \x1b[2m\xe2\x94\x82\x1b[0m");
        }
    }
    buf_add(&b, "\x1b[?25l", 6);
    a->help_buf = b;

    if (b.len == a->help_last.len && a->help_grid == a->kitty.grid_draws &&
        (b.len == 0 || memcmp(b.p, a->help_last.p, b.len) == 0))
        return;
    writeall(a->term.fd, b.p, b.len);
    a->help_grid = a->kitty.grid_draws;
    a->help_last.len = 0;
    buf_add(&a->help_last, b.p, b.len);
}

// ------------------------------------------------------------------ input

void help_toggle(App *a) {
    if (!a->has_tty) return;
    a->help_open = !a->help_open;
    a->help_page = 0;
    a->help_last.len = 0;
    if (!a->help_open) {
        a->kitty.grid_dirty = true;
        a->last_hash = 0;
        relayout(a);
    }
}

bool help_key(App *a, Event *ev) {
    if (!a->help_open || ev->type != EV_KEY) return false;
    Act act = keys_lookup(ev->mods, ev->key);
    if (act == ACT_QUIT) return false;

    Shape s;
    if (shape(a, &s) && s.pages > 1 &&
        !(ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
        int d = 0;
        if (ev->key == KEY_RIGHT || ev->key == ' ') d = 1;
        else if (ev->key == KEY_LEFT)               d = -1;
        else switch (act) {
        case ACT_SCROLL_RIGHT: case ACT_SCROLL_DOWN: case ACT_LINE_DOWN:
        case ACT_PAGE_DOWN:    case ACT_HALF_DOWN:  d = 1;  break;
        case ACT_SCROLL_LEFT:  case ACT_SCROLL_UP:   case ACT_LINE_UP:
        case ACT_PAGE_UP:      case ACT_HALF_UP:    d = -1; break;
        case ACT_TOP:    a->help_page = 0;            return true;
        case ACT_BOTTOM: a->help_page = s.pages - 1;  return true;
        default: break;
        }
        if (d) {
            a->help_page += d;
            if (a->help_page < 0)        a->help_page = s.pages - 1;
            if (a->help_page >= s.pages) a->help_page = 0;
            return true;
        }
    }
    help_toggle(a);          // anything else puts it away
    return true;
}

void help_free(App *a) {
    buf_free(&a->help_buf);
    buf_free(&a->help_last);
}

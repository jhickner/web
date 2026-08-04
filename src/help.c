#include <stdio.h>
#include <string.h>
#include "web.h"

// The key list, drawn over the picture rather than in a row of its own. The
// image is placed through unicode placeholders, so a cell written as ordinary
// text stops being part of it and shows the text: frames going on arriving
// underneath repaint the image data and never touch these cells. Putting the
// list away is redrawing the placeholders, which is what kitty_draw_png does
// with a dirty grid.

#define GAP 3          // columns between two columns of the list
#define MAX_COLS 4

typedef struct { const char *key, *what; } Bind;

// A row with no key is a heading; one with neither is a blank. Kept as the
// display list itself so the columns below are a straight slice of it.
static const Bind BINDS[] = {
    {NULL, "moving"},
    {"j / k",         "down / up"},
    {"d / u",         "half a screen"},
    {"space / b",     "a screen"},
    {"gg / G",        "top / bottom"},
    {"h / l",         "left / right"},
    {"arrows",        "a line at a time"},
    {"backspace",     "back"},
    {NULL, NULL},
    {NULL, "the page"},
    {"^L",            "address bar"},
    {"^O / ^P",       "back / forward"},
    {"^R",            "reload"},
    {"^Y",            "copy selection or address"},
    {"y",             "copy the address"},
    {"^E",            "open in the desktop browser"},
    {"/",             "find, then n / N"},
    {"i",             "give the keyboard to the page"},
    {"esc",           "take it back"},
    {"P",             "pick a CSS selector"},
    {NULL, NULL},
    {NULL, "tabs"},
    {"^T / ^W",       "new / close"},
    {"^N / ^B",       "next / previous"},
    {"alt+1..9",      "the tab with that number"},
    {NULL, NULL},
    {NULL, "the window"},
    {"[ / ]",         "zoom out / in"},
    {"alt+- / alt+=", "zoom out / in"},
    {"alt+0",         "reset zoom and width"},
    {"alt+f",         "fit width on / off"},
    {"w / W",         "widen / narrow the page"},
    {"s",             "frame size"},
    {"shift+arrows",  "drag the window edge"},
    {"U / D / L / R", "the same, without shift"},
    {NULL, NULL},
    {NULL, "the rest"},
    {":",             "console (^X too)"},
    {"^G",            "stats on the status line"},
    {"^S",            "hide the status line"},
    {"^D",            "trace to /tmp/web_input.log"},
    {"?",             "this list"},
    {"^Q",            "quit"},
};
#define LINES ((int)(sizeof BINDS / sizeof BINDS[0]))

typedef struct {
    int x, y, w, h;        // the box, in 1-based cells
    int cols, rows;        // columns of the list, and rows in each
    int colw, keyw;
    int max_scroll;
} Shape;

static int g_keyw, g_descw;

static bool is_heading(int i) { return !BINDS[i].key && BINDS[i].what; }

// Whether breaking the list into columns `rows` tall lands the breaks between
// sections: every column but the first opens on a heading, none closes on one,
// and none is left with nothing in it. A heading parted from the keys under it
// reads as a stray line rather than as a title.
static bool tidy(int rows, int n) {
    if ((n - 1) * rows >= LINES) return false;
    for (int c = 0; c < n; c++) {
        int first = c * rows, last = first + rows - 1;
        if (c && first < LINES && !is_heading(first)) return false;
        if (last < LINES && is_heading(last)) return false;
    }
    return true;
}

static void measure(void) {
    if (g_keyw) return;
    for (int i = 0; i < LINES; i++) {
        int k = BINDS[i].key ? (int)strlen(BINDS[i].key) : 0;
        int d = BINDS[i].what ? (int)strlen(BINDS[i].what) : 0;
        if (k > g_keyw) g_keyw = k;
        if (d > g_descw) g_descw = d;
    }
}

// Where the box lands and how the list is broken up to fill it. The picture's
// own rect, not the terminal's: inline the window is narrower than the screen,
// and a list running past its edge reads as part of the shell.
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

    measure();
    int colw = g_keyw + 2 + g_descw;
    if (colw > avail_w) colw = avail_w;

    // Another column only when the list does not already fit in the rows there
    // are, and only while one still fits sideways.
    int n = 1;
    while (n < MAX_COLS && (LINES + n - 1) / n > avail_h &&
           (n + 1) * colw + n * GAP <= avail_w)
        n++;

    int rows = (LINES + n - 1) / n;
    if (rows > avail_h) {
        rows = avail_h;                    // it does not all fit, so it scrolls
    } else {
        // Room to spare sideways is worth spending on taller columns if that is
        // what puts the breaks between sections.
        for (int r = rows; r <= avail_h; r++)
            if (tidy(r, n)) { rows = r; break; }
    }

    s->cols = n;
    s->rows = rows;
    s->colw = colw;
    s->keyw = g_keyw > colw - 4 ? (colw > 5 ? colw - 4 : 1) : g_keyw;
    s->w = n * colw + (n - 1) * GAP + 4;
    s->h = rows + 2;
    s->x = rx + (rw - s->w) / 2;
    s->y = ry + (rh - s->h) / 2;
    s->max_scroll = LINES - n * rows;
    if (s->max_scroll < 0) s->max_scroll = 0;
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
    if (!e || (!e->key && !e->what)) { pad(b, colw); return; }
    if (!e->key) {
        int n = (int)strlen(e->what);
        if (n > colw) n = colw;
        buf_addf(b, "\x1b[1;36m%.*s\x1b[0m", n, e->what);
        pad(b, colw - n);
        return;
    }
    int kn = (int)strlen(e->key);
    if (kn > keyw) kn = keyw;
    buf_addf(b, "\x1b[1m%*.*s\x1b[0m  ", keyw, kn, e->key);
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

void help_paint(App *a) {
    if (!a->has_tty || !a->help_open) return;
    Shape s;
    if (!shape(a, &s)) return;
    if (a->help_scroll > s.max_scroll) a->help_scroll = s.max_scroll;

    Buf b = a->help_buf;
    b.len = 0;
    for (int r = 0; r < s.h; r++) {
        buf_addf(&b, "\x1b[%d;%dH\x1b[0m", s.y + r, s.x);
        if (r == 0) {
            border(&b, "\xe2\x94\x8c", "\xe2\x94\x90", " keys ", s.w);
        } else if (r == s.h - 1) {
            border(&b, "\xe2\x94\x94", "\xe2\x94\x98",
                   s.max_scroll > 0 ? " j / k for more " : NULL, s.w);
        } else {
            buf_addf(&b, "\x1b[2m\xe2\x94\x82\x1b[0m ");
            for (int c = 0; c < s.cols; c++) {
                if (c) pad(&b, GAP);
                int i = a->help_scroll + c * s.rows + (r - 1);
                put_entry(&b, i < LINES ? &BINDS[i] : NULL, s.keyw, s.colw);
            }
            buf_addf(&b, " \x1b[2m\xe2\x94\x82\x1b[0m");
        }
    }
    buf_add(&b, "\x1b[?25l", 6);
    a->help_buf = b;

    // The same guard the status line and the console use, plus the count of
    // grids kitty has laid down: a grid redrawn under an open list has just
    // covered it over with placeholders, and the bytes to put it back are the
    // ones already in the buffer.
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
    a->help_scroll = 0;
    a->help_last.len = 0;
    if (!a->help_open) {
        // The list lives in the cells that carry the picture, so taking it away
        // is putting those cells back and asking for a frame to land in them.
        a->kitty.grid_dirty = true;
        a->last_hash = 0;
        relayout(a);
    }
}

bool help_key(App *a, Event *ev) {
    if (!a->help_open || ev->type != EV_KEY) return false;
    // Quit still means quit, from here as much as from anywhere else.
    if (ev->mods == MOD_CTRL && (ev->key == 'q' || ev->key == 'c')) return false;

    Shape s;
    if (shape(a, &s) && s.max_scroll > 0 &&
        !(ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
        int page = s.rows > 1 ? s.rows - 1 : 1;
        int d = 0;
        switch (ev->key) {
        case 'j': case KEY_DOWN:  d = 1;     break;
        case 'k': case KEY_UP:    d = -1;    break;
        case ' ': case KEY_PGDN:  d = page;  break;
        case 'b': case KEY_PGUP:  d = -page; break;
        case 'g': a->help_scroll = 0;            return true;
        case 'G': a->help_scroll = s.max_scroll; return true;
        }
        if (d) {
            a->help_scroll += d;
            if (a->help_scroll < 0) a->help_scroll = 0;
            if (a->help_scroll > s.max_scroll) a->help_scroll = s.max_scroll;
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

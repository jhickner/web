#include <stdio.h>
#include <string.h>
#include "web.h"

#define LABEL_ROWS    1
#define TILE_MIN_ROWS 3
#define TILE_MIN_COLS 8

#define GRID_EVERY 0.15

#define GRID_WAIT 3.0

static void grid_shape(int n, int *cols, int *rows) {
    if (n <= 1)      { *cols = 1; *rows = 1; }
    else if (n == 2) { *cols = 2; *rows = 1; }
    else if (n <= 4) { *cols = 2; *rows = 2; }
    else if (n <= 6) { *cols = 3; *rows = 2; }
    else             { *cols = 3; *rows = 3; }
}

int grid_count(const App *a) {
    return a->ntabs < GRID_MAX ? a->ntabs : GRID_MAX;
}

// tile i in 1-based cells, picture only; the label goes on the row under it
bool grid_tile_rect(const App *a, int i, int *x, int *y, int *cols, int *rows) {
    int n = grid_count(a);
    if (i < 0 || i >= n) return false;

    int gc, gr;
    grid_shape(n, &gc, &gr);

    int ax, ay, aw, ah;
    kitty_area(&a->kitty, &ax, &ay, &aw, &ah);
    if (aw < 1 || ah < 1) return false;

    int cw = aw / gc;
    int ch = ah / gr;
    if (cw < TILE_MIN_COLS || ch < TILE_MIN_ROWS + LABEL_ROWS) return false;

    int r = i / gc, c = i % gc;
    *x = ax + c * cw;
    *y = ay + r * ch;
    *cols = cw;
    *rows = ch - LABEL_ROWS;
    return true;
}

// which tile a cell is in, or -1; the label row counts as its tile
int grid_tile_at(const App *a, int col, int row) {
    int n = grid_count(a);
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (!grid_tile_rect(a, i, &x, &y, &w, &h)) return -1;
        if (col >= x && col < x + w && row >= y && row < y + h + LABEL_ROWS)
            return i;
    }
    return -1;
}

// ------------------------------------------------------------- photographs

// the page size one tile is worth, in CSS pixels
static void tile_css(const App *a, int i, int *w, int *h) {
    int x, y, cols, rows;
    *w = *h = 0;
    if (!grid_tile_rect(a, i, &x, &y, &cols, &rows)) return;
    *w = cols * (a->term.cell_w > 0 ? a->term.cell_w : 8);
    *h = rows * (a->term.cell_h > 0 ? a->term.cell_h : 17);
    if (*w < 80) *w = 80;
    if (*h < 60) *h = 60;
}

static int grid_next(App *a) {
    int n = grid_count(a);
    if (n < 1) return -1;
    if (a->grid_turn % 2 == 0 && a->tab < n) return a->tab;
    int step = (a->grid_turn / 2) % n;
    return step;
}

void grid_tick(App *a) {
    if (!a->grid_on || a->paused) return;
    double t = now_sec();

    if (a->grid_req && t - a->grid_req_at > GRID_WAIT) {
        a->grid_req = 0;
        a->grid_turn++;
        a->grid_next_at = t + GRID_EVERY;
    }
    if (a->grid_req || t < a->grid_next_at) return;

    int i = grid_next(a);
    if (i < 0 || i >= a->ntabs) return;
    a->grid_next_at = t + GRID_EVERY;

    Tab *tb = &a->tabs[i];
    if (!tb->session[0]) {
        if (!a->grid_attach_req[i])
            a->grid_attach_req[i] = cdp_session_call(&a->chrome, NULL,
                "Target.attachToTarget", "\"targetId\":\"%s\",\"flatten\":true",
                tb->target);
        a->grid_turn++;
        return;
    }

    int w, h;
    tile_css(a, i, &w, &h);
    if (w < 1 || h < 1) return;

    if (tb->shot_w != w || tb->shot_h != h) {
        cdp_session_call(&a->chrome, tb->session, "Emulation.setDeviceMetricsOverride",
                         "\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,"
                         "\"mobile\":false", w, h);
        tb->shot_w = w;
        tb->shot_h = h;
    }
    int id = cdp_session_call(&a->chrome, tb->session, "Page.captureScreenshot",
                              "\"format\":\"png\"");
    if (id < 0) { a->grid_turn++; return; }
    a->grid_req = id;
    a->grid_req_tab = i;
    a->grid_req_at = t;
    term_log("grid: turn %d asks tile %d for %dx%d (req %d)",
             a->grid_turn, i, w, h, id);
}

// true when the message was ours
bool grid_reply(App *a, const char *msg) {
    double idf = json_num(msg, "id", 0);
    int id = (int)idf;
    if (id <= 0) return false;

    for (int i = 0; i < a->ntabs && i < GRID_MAX; i++) {
        if (a->tabs[i].session[0] || a->grid_attach_req[i] != id) continue;
        a->grid_attach_req[i] = 0;
        size_t n = 0;
        const char *s = json_str(msg, "sessionId", &n);
        if (s && n && n < sizeof a->tabs[i].session) {
            memcpy(a->tabs[i].session, s, n);
            a->tabs[i].session[n] = 0;
        }
        term_log("grid: tile %d session %.180s", i,
                 a->tabs[i].session[0] ? a->tabs[i].session : "REFUSED");
        return true;
    }

    if (!a->grid_req || id != a->grid_req) return false;
    a->grid_req = 0;
    a->grid_turn++;
    a->grid_next_at = now_sec() + GRID_EVERY;

    int i = a->grid_req_tab;
    if (!a->grid_on || i < 0 || i >= a->ntabs) return true;

    size_t n = 0;
    const char *b64 = json_str(msg, "data", &n);
    term_log("grid: tile %d picture %zu b64", i, n);
    if (!b64 || !n) return true;

    Tab *t = &a->tabs[i];
    uint64_t h64 = fnv1a(b64, n);
    if (h64 == t->shot_hash && kitty_tile_live(&a->kitty, i + 1)) return true;
    t->shot_hash = h64;
    t->shot.len = 0;
    buf_add(&t->shot, b64, n);

    int x, y, w, h;
    if (!grid_tile_rect(a, i, &x, &y, &w, &h)) return true;
    kitty_use(&a->kitty, i + 1);
    kitty_set_rect(&a->kitty, x, y, w, h);
    kitty_draw_png(&a->kitty, t->shot.p, t->shot.len);
    kitty_use(&a->kitty, 0);
    return true;
}

// ------------------------------------------------------------------ paint

static void paint_label(App *a, int i, int x, int y, int cols) {
    char name[512], line[700];
    tab_label(a, i, name, sizeof name);

    int room = cols - 3;
    if (room < 1) room = 1;
    if (room > (int)sizeof line - 16) room = (int)sizeof line - 16;

    Buf b = {0};
    buf_addf(&b, "\x1b[%d;%dH\x1b[0m", y, x);
    buf_add(&b, i == a->tab ? "\x1b[7m" : "\x1b[2m", 4);
    snprintf(line, sizeof line, "%d %.*s", i + 1, room, name);
    buf_addf(&b, "%-*.*s\x1b[0m", cols, cols, line);
    if (b.p) writeall(a->kitty.ttyfd, b.p, b.len);
    buf_free(&b);
}

void grid_paint(App *a) {
    if (!a->grid_on || !a->has_tty || a->paused) return;
    int n = grid_count(a);
    bool relabel = a->grid_shown_tab != a->tab;

    if (a->kitty.slot == 0 && a->kitty.grid_dirty) {
        for (int i = 1; i <= GRID_MAX; i++) a->kitty.tiles[i].grid_dirty = true;
        a->kitty.grid_dirty = false;
    }

    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (!grid_tile_rect(a, i, &x, &y, &w, &h)) {
            grid_off(a, "the window is too small for a grid");
            return;
        }
        kitty_use(&a->kitty, i + 1);
        kitty_set_rect(&a->kitty, x, y, w, h);   // dirties the tile if it moved
        bool need = a->kitty.grid_dirty || !kitty_tile_live(&a->kitty, i + 1);
        Tab *t = &a->tabs[i];
        bool sent = false;
        if (t->shot.len && need) {
            kitty_draw_png(&a->kitty, t->shot.p, t->shot.len);
            sent = true;
        }
        kitty_use(&a->kitty, 0);
        if (relabel || sent || need) paint_label(a, i, x, y + h, w);
    }
    a->grid_shown_tab = a->tab;
}

// ------------------------------------------------------------------ on/off

static void tiles_clear(App *a) {
    for (int i = 1; i <= GRID_MAX; i++) {
        kitty_use(&a->kitty, i);
        kitty_clear(&a->kitty);
    }
    kitty_use(&a->kitty, 0);
}

static void sessions_drop(App *a) {
    for (int i = 0; i < a->ntabs && i < GRID_MAX; i++) {
        Tab *t = &a->tabs[i];
        a->grid_attach_req[i] = 0;
        if (!t->session[0]) continue;
        cdp_session_call(&a->chrome, t->session,
                         "Emulation.clearDeviceMetricsOverride", "");
        cdp_session_call(&a->chrome, NULL, "Target.detachFromTarget",
                         "\"sessionId\":\"%s\"", t->session);
        t->session[0] = 0;
    }
}

void grid_off(App *a, const char *why) {
    if (!a->grid_on) return;
    a->grid_on = false;
    a->grid_shown_tab = -1;
    a->grid_req = 0;
    sessions_drop(a);
    tiles_clear(a);
    kitty_clear(&a->kitty);
    a->kitty.grid_dirty = true;
    relayout(a);
    if (why) notify(a, why);
}

void grid_toggle(App *a) {
    if (a->grid_on) { grid_off(a, NULL); return; }
    if (a->ntabs < 2) { notify(a, "one tab is not a grid"); return; }
    if (a->chrome.watch.fd <= 0 || a->chrome.watch.closed) {
        notify(a, "no browser socket, so no grid");
        return;
    }
    int x, y, w, h;
    if (!grid_tile_rect(a, 0, &x, &y, &w, &h)) {
        notify(a, "the window is too small for a grid");
        return;
    }

    term_log("grid: on, %d tabs, watch fd %d", a->ntabs, a->chrome.watch.fd);
    a->grid_on = true;
    a->grid_shown_tab = -1;
    a->grid_turn = 0;
    a->grid_req = 0;
    a->grid_next_at = 0;

    app_cdp(a, "Page.stopScreencast", "");

    for (int i = 0; i < a->ntabs && i < GRID_MAX; i++) {
        Tab *t = &a->tabs[i];
        t->session[0] = 0;
        t->shot_w = t->shot_h = 0;
        a->grid_attach_req[i] = cdp_session_call(&a->chrome, NULL,
            "Target.attachToTarget", "\"targetId\":\"%s\",\"flatten\":true",
            t->target);
    }

    kitty_use(&a->kitty, 0);
    kitty_clear(&a->kitty);
    for (int i = 1; i <= GRID_MAX; i++) {
        a->kitty.tiles[i].grid_dirty = true;
        a->kitty.tiles[i].live = false;
    }
    grid_paint(a);
    if (a->ntabs > GRID_MAX) notify(a, "showing the first nine tabs");
}

bool grid_key(App *a, Event *ev) {
    if (!a->grid_on || ev->type != EV_KEY) return false;
    if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) return false;
    int n = grid_count(a);
    if (n < 1) return false;

    if (ev->key == KEY_ENTER || ev->key == KEY_ESC) {
        grid_off(a, NULL);
        return true;
    }

    int gc, gr;
    grid_shape(n, &gc, &gr);
    int cur = a->tab < n ? a->tab : 0;
    int r = cur / gc, c = cur % gc, want = -1;
    switch (ev->key) {
    case KEY_LEFT:  if (c > 0)       want = cur - 1;  break;
    case KEY_RIGHT: if (c + 1 < gc)  want = cur + 1;  break;
    case KEY_UP:    if (r > 0)       want = cur - gc; break;
    case KEY_DOWN:                   want = cur + gc; break;
    default: return false;
    }
    if (want >= 0 && want < n && want != a->tab) tab_go(a, want);
    return true;
}

bool grid_mouse(App *a, Event *ev) {
    if (!a->grid_on || !ev->press || ev->motion || ev->button > 2) return false;
    int i = grid_tile_at(a, ev->mx, ev->my);
    if (i < 0) return true;
    if (i != a->tab) tab_go(a, i);
    else grid_off(a, NULL);
    return true;
}

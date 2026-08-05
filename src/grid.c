#include <stdio.h>
#include <string.h>
#include "web.h"

// Every tab at once, each in a tile of its own.
//
// The tab in front is the live one - its frames arrive as they always did and
// go to its tile instead of to the whole window. The others show the last frame
// this window saw of them, which is what makes a grid of nine pages cost one
// page: eight of them are a picture the terminal already has and nothing is
// asked of Chrome for any of them.
//
// Selecting a tile is switching tab, so the tile under the cursor is the one
// that comes alive. That is the whole of the interaction: the grid is a way of
// looking at the tabs, not a second mode with rules of its own.

// A tile is its picture with a line under it for the name. Under three rows
// there is no picture worth showing, so the grid is refused rather than drawn
// as a row of labels.
#define LABEL_ROWS 1
#define TILE_MIN_ROWS 3
#define TILE_MIN_COLS 8

// Rows and columns of tiles for n pages: as square as it can be, and wider than
// it is tall, because a terminal cell is about twice as tall as it is wide and
// a page is taller than it is wide.
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

// Where tile i sits, in 1-based cells, picture only - the label goes on the row
// under what this answers. False when the window is too small to divide.
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

// Which tile a cell is in, or -1. The label row counts as its tile: a click
// meant for a page that landed a row low is still a click on that page.
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

// The name under a tile: its number, and as much of the title as there is room
// for. The tab in front is named in reverse, which is the only thing on screen
// saying which of the pictures is the live one.
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
    // Padded out to the tile's width so the name of a page that was longer
    // before does not stay behind the one that is there now.
    buf_addf(&b, "%-*.*s\x1b[0m", cols, cols, line);
    writeall(a->kitty.ttyfd, b.p, b.len);
    buf_free(&b);
}

// Put every tile up: the frame each tab was last seen at, and its name. The
// live tab's own tile is left to the frames arriving for it, which land in the
// same place by the same route.
void grid_paint(App *a) {
    if (!a->grid_on || !a->has_tty || a->paused) return;
    int n = grid_count(a);
    // The names are only worth writing again when one of them has changed
    // hands. draw_panes runs on every frame, and nine labels a frame is a
    // steady write for a row of text that says the same thing each time.
    bool relabel = a->grid_shown_tab != a->tab;

    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (!grid_tile_rect(a, i, &x, &y, &w, &h)) {
            grid_off(a, "the window is too small for a grid");
            return;
        }
        kitty_use(&a->kitty, i + 1);
        kitty_set_rect(&a->kitty, x, y, w, h);
        // A tab nothing has ever drawn has no picture to show, and one whose
        // picture the terminal still holds needs nothing sent for it.
        Tab *t = &a->tabs[i];
        bool sent = false;
        if (t->shot.len && (a->kitty.grid_dirty || !kitty_tile_live(&a->kitty, i + 1))) {
            kitty_draw_png(&a->kitty, t->shot.p, t->shot.len);
            sent = true;
        }
        kitty_use(&a->kitty, 0);
        if (relabel || sent) paint_label(a, i, x, y + h, w);
    }
    a->grid_shown_tab = a->tab;
}

// The picture goes back to being one picture. Every tile is taken down by name:
// the cells that named it are ordinary text and stay where they are, so an
// image left up would show through whatever is drawn over those rows next.
static void tiles_clear(App *a) {
    for (int i = 1; i <= GRID_MAX; i++) {
        kitty_use(&a->kitty, i);
        kitty_clear(&a->kitty);
    }
    kitty_use(&a->kitty, 0);
}

void grid_off(App *a, const char *why) {
    if (!a->grid_on) return;
    a->grid_on = false;
    a->grid_shown_tab = -1;
    tiles_clear(a);
    // The rows the tiles were on are being handed back to one picture, which
    // has to lay its own cells down again over them.
    kitty_clear(&a->kitty);
    a->kitty.grid_dirty = true;
    // The picture has to come back by itself: nothing on the page changed, so
    // no frame is on its way to redraw what the tiles were covering.
    relayout(a);
    if (why) notify(a, why);
}

void grid_toggle(App *a) {
    if (a->grid_on) { grid_off(a, NULL); return; }
    if (a->ntabs < 2) {
        notify(a, "one tab is not a grid");
        return;
    }
    int x, y, w, h;
    if (!grid_tile_rect(a, 0, &x, &y, &w, &h)) {
        notify(a, "the window is too small for a grid");
        return;
    }
    a->grid_on = true;
    a->grid_shown_tab = -1;
    // The one picture comes down first: its cells cover the whole window, and
    // the tiles are about to be laid over the same rows.
    kitty_use(&a->kitty, 0);
    kitty_clear(&a->kitty);
    for (int i = 1; i <= GRID_MAX; i++) a->kitty.tiles[i].grid_dirty = true;
    grid_paint(a);
    if (a->ntabs > GRID_MAX) notify(a, "showing the first nine tabs");
}

// A click picks the page under it, which switches to that tab and so makes it
// the live one. True when the grid took the click.
bool grid_mouse(App *a, Event *ev) {
    if (!a->grid_on || !ev->press || ev->motion || ev->button > 2) return false;
    int i = grid_tile_at(a, ev->mx, ev->my);
    if (i < 0) return true;             // inside the grid, but between tiles
    if (i != a->tab) tab_go(a, i);
    else grid_off(a, NULL);             // the one already live: show it whole
    return true;
}

// The last frame of every tab, kept so a tile has something to show for a page
// nothing is asking Chrome about. Only while there is more than one tab, since
// a window with one tab has nothing to put in a grid.
void grid_remember(App *a, const char *b64, size_t len) {
    if (a->ntabs < 2 || a->tab < 0 || a->tab >= a->ntabs) return;
    Tab *t = &a->tabs[a->tab];
    t->shot.len = 0;
    buf_add(&t->shot, b64, len);
}

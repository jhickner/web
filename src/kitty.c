#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "web.h"
#include "../vendor/rowcolumn_diacritics.h"

// Three distinct non-zero bytes. The placeholder cells carry the image id in
// their foreground colour, and anything that re-fits colours on the way (tmux
// when it is unsure the terminal takes truecolor) would turn a small id like 1
// into a palette index and lose it. This value survives the trip.
//
// The low byte is the process, because inline mode leaves its cells behind on
// purpose - the picture stays in the scrollback after quitting. Those cells go
// on naming whatever id they were drawn with, so a second run that reused the
// id would repaint the old block with the new page: the earlier window mirrors
// the new one, and a picture arriving in two places at once reads as a single
// one torn in half.
// Both process bytes are forced odd so neither can come out zero, which is what
// the three-distinct-bytes rule above is really asking for. That leaves about
// fourteen bits of the pid, so two runs sharing an id is a one-in-sixteen-
// thousand event rather than a one-in-256 one.
// The top byte carries a generation instead of a constant, because a name is
// how the picture is taken away as well as how it is put up. The cells naming
// an image are ordinary text: the terminal keeps them wherever a resize leaves
// them, and they are not ours to erase once we have lost track of where that
// was. Re-placing the image would light every one of them, which is the same
// picture in two places. Coming back under a new name leaves them behind,
// naming an image that no longer exists, and they draw nothing.
//
// Kept even so it can never collide with the two odd bytes below, and stepped
// by two so it stays that way; the modulo keeps it clear of zero, which would
// cost the id the third distinct byte it needs.
// The tile goes in the fourth byte, which is the one place it can go without
// disturbing any of the above: the three bytes below are all the foreground
// colour can carry, and the placeholder cells say the fourth in a diacritic of
// their own. A window that never opens the grid is on slot 0, whose id and
// whose cells are byte for byte what they were before tiles existed.
//
// If a terminal ignores that diacritic the tiles collide on one id and every
// one of them shows the same page, which is a loud enough failure to find.
#define IMAGE_ID_BASE(gen) ((0x0Au + 2u * ((gen) % 100u)) << 16)
static unsigned image_id_for(unsigned gen, int slot) {
    unsigned pid = (unsigned)getpid();
    unsigned hi = ((pid >> 8) & 0xff) | 1;
    unsigned lo = (pid & 0xff) | 1;
    unsigned id = IMAGE_ID_BASE(gen) | (hi << 8) | lo;
    if (slot > 0) id |= ((unsigned)slot & 0xffu) << 24;
    return id;
}
#define PLACEMENT  1
#define CHUNK      4096

// The cell that says "show a piece of an image here". Encoded from the
// codepoint rather than written out as bytes: the UTF-8 for this one is easy to
// typo into a neighbouring private-use character, which a terminal then draws
// as a meaningless glyph instead of substituting pixels.
#define PLACEHOLDER_CP 0x10EEEEu

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

// tmux only forwards escape sequences it is told to pass through, and the
// payload's own ESCs have to be doubled on the way. The wrapper is opened and
// closed separately from the payload so a frame can be appended straight into
// the output buffer instead of being staged somewhere first.
static void esc_open(Kitty *k) {
    if (k->tmux) buf_add(&k->out, "\x1bPtmux;", 7);
}

static void esc_close(Kitty *k) {
    if (k->tmux) buf_add(&k->out, "\x1b\\", 2);
}

// Payload that may contain ESCs.
static void esc_body(Kitty *k, const char *seq, size_t n) {
    if (!k->tmux) {
        buf_add(&k->out, seq, n);
        return;
    }
    const char *end = seq + n;
    while (seq < end) {
        const char *e = memchr(seq, 0x1b, (size_t)(end - seq));
        if (!e) break;
        buf_add(&k->out, seq, (size_t)(e - seq) + 1);
        buf_add(&k->out, "\x1b", 1);
        seq = e + 1;
    }
    buf_add(&k->out, seq, (size_t)(end - seq));
}

// Payload known to hold no ESCs, such as base64 image data.
static void esc_raw(Kitty *k, const char *seq, size_t n) {
    buf_add(&k->out, seq, n);
}

// A frame given up on halfway leaves the terminal inside an unterminated
// graphics escape, and everything written after it - the delete on the way out,
// the mode resets, the shell's own prompt - is eaten as more of that escape.
// That is what a quit during a busy page looks like: a shell that has lost its
// terminal. Closing the escape costs a few bytes. Written straight rather than
// through writeall, because the reason for being here may be that the terminal
// is not taking anything; a couple of tries, then leave it.
static void esc_abort(Kitty *k) {
    // Under tmux the payload sits inside a passthrough DCS with its own ESCs
    // doubled, so the inner terminator goes in escaped and the DCS is closed
    // after it.
    const char *seq = k->tmux ? "\x1b\x1b\\\x1b\\" : "\x1b\\";
    size_t n = strlen(seq);
    for (int try = 0; try < 3 && n; try++) {
        ssize_t w = write(k->ttyfd, seq, n);
        if (w == (ssize_t)n) return;
        if (w > 0) { seq += w; n -= (size_t)w; }
        struct pollfd pf = {k->ttyfd, POLLOUT, 0};
        poll(&pf, 1, 50);
    }
}

void kitty_abort(Kitty *k) { esc_abort(k); }

static void emit_esc(Kitty *k, const char *seq, size_t n) {
    esc_open(k);
    esc_body(k, seq, n);
    esc_close(k);
}

void kitty_init(Kitty *k, int ttyfd, bool tmux) {
    memset(k, 0, sizeof *k);
    k->ttyfd = ttyfd;
    k->tmux = tmux;
    k->grid_dirty = true;
}

// Point everything below at one tile. The current tile's rect and name are put
// away and the new one's taken out, so every call after this one draws, places
// and deletes under that tile's name without knowing there are others. Slot 0
// is the whole picture, which is what a window not showing a grid draws.
void kitty_use(Kitty *k, int slot) {
    if (slot < 0) slot = 0;
    if (slot > GRID_MAX) slot = GRID_MAX;
    if (slot == k->slot) return;

    KittyTile *was = &k->tiles[k->slot];
    was->x = k->x; was->y = k->y; was->cols = k->cols; was->rows = k->rows;
    was->gen = k->gen;
    was->grid_dirty = k->grid_dirty;

    KittyTile *now = &k->tiles[slot];
    k->slot = slot;
    k->x = now->x; k->y = now->y; k->cols = now->cols; k->rows = now->rows;
    k->gen = now->gen;
    k->grid_dirty = now->grid_dirty;
}

// The whole picture's rect, whichever tile happens to be current: slot 0's
// fields are only put away in the array while another tile is being drawn.
void kitty_area(const Kitty *k, int *x, int *y, int *cols, int *rows) {
    const KittyTile *t = &k->tiles[0];
    bool now = k->slot == 0;
    *x    = now ? k->x    : t->x;
    *y    = now ? k->y    : t->y;
    *cols = now ? k->cols : t->cols;
    *rows = now ? k->rows : t->rows;
}

// Whether anything has been drawn under this tile's name.
bool kitty_tile_live(const Kitty *k, int slot) {
    if (slot < 0 || slot > GRID_MAX) return false;
    return k->tiles[slot].live;
}

void kitty_set_rect(Kitty *k, int x, int y, int cols, int rows) {
    if (k->x == x && k->y == y && k->cols == cols && k->rows == rows) return;
    k->x = x;
    k->y = y;
    k->cols = cols;
    k->rows = rows;
    k->grid_dirty = true;
}

// The placeholder cells are ordinary text, so tmux tracks them like any other
// character. Once they are on screen, later frames only re-send image data and
// the terminal repaints the same cells.
static void draw_grid(Kitty *k) {
    int maxd = ROWCOLUMN_DIACRITICS_COUNT;
    int rows = k->rows > maxd ? maxd : k->rows;
    int cols = k->cols > maxd ? maxd : k->cols;
    char enc[8], cell[8];
    size_t celln = utf8_encode(PLACEHOLDER_CP, cell);

    unsigned id = image_id_for(k->gen, k->slot);
    buf_addf(&k->out, "\x1b[38;2;%u;%u;%um", (id >> 16) & 0xff,
             (id >> 8) & 0xff, id & 0xff);
    for (int r = 0; r < rows; r++) {
        buf_addf(&k->out, "\x1b[%d;%dH", k->y + r, k->x);
        for (int c = 0; c < cols; c++) {
            buf_add(&k->out, cell, celln);
            buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[r], enc));
            buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[c], enc));
            // The fourth byte of the id, and only when there is one to say:
            // a cell with two diacritics is what every terminal that draws
            // these has always been given.
            if (k->slot > 0)
                buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[k->slot], enc));
        }
    }
    buf_add(&k->out, "\x1b[39m", 5);
    k->grid_draws++;
}

int kitty_draw_png(Kitty *k, const char *b64, size_t len) {
    k->out.len = 0;

    // Every chunk grows by its own escape header and, under tmux, a passthrough
    // wrapper, so ask for the room once rather than doubling the buffer a
    // dozen times on the way through a quarter-megabyte frame.
    buf_reserve(&k->out, len + (len / CHUNK + 2) * 64 + 4096);

    // q=2 suppresses the terminal's replies, which would otherwise land in our
    // stdin as garbage keystrokes.
    size_t off = 0;
    bool first = true;
    while (off < len) {
        size_t n = len - off;
        if (n > CHUNK) n = CHUNK;
        int more = (off + n < len);

        char hdr[64];
        int hn;
        if (first)
            hn = snprintf(hdr, sizeof hdr, "\x1b_Ga=t,f=100,t=d,i=%u,q=2,m=%d;",
                          image_id_for(k->gen, k->slot), more);
        else
            hn = snprintf(hdr, sizeof hdr, "\x1b_Gm=%d;", more);

        esc_open(k);
        esc_body(k, hdr, (size_t)hn);
        esc_raw(k, b64 + off, n);       // base64 alphabet cannot contain an ESC
        esc_body(k, "\x1b\\", 2);
        esc_close(k);

        off += n;
        first = false;
    }

    // Re-assert the virtual placement each frame: replacing the image data can
    // drop placements, and one 40-byte escape is cheaper than finding out.
    char place[128];
    int pn = snprintf(place, sizeof place,
                      "\x1b_Ga=p,U=1,i=%u,p=%d,c=%d,r=%d,q=2\x1b\\",
                      image_id_for(k->gen, k->slot), PLACEMENT, k->cols, k->rows);
    emit_esc(k, place, (size_t)pn);

    if (k->grid_dirty) {
        draw_grid(k);
        k->grid_dirty = false;
    }

    // Hand the frame over in slices rather than one blocking write, so the
    // keyboard gets a look in even when the terminal is keeping up.
    const size_t SLICE = 32768;
    for (size_t off = 0; off < k->out.len; off += SLICE) {
        size_t n = k->out.len - off;
        if (n > SLICE) n = SLICE;
        if (writeall(k->ttyfd, k->out.p + off, n) < 0) { esc_abort(k); return -1; }
        if (g_input_pump) g_input_pump();
        if (g_quit) { esc_abort(k); return -1; }
    }
    k->tiles[k->slot].live = true;
    return 0;
}

void kitty_clear(Kitty *k) {
    k->out.len = 0;
    char seq[64];
    int n = snprintf(seq, sizeof seq, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\",
                     image_id_for(k->gen, k->slot));
    emit_esc(k, seq, (size_t)n);
    writeall(k->ttyfd, k->out.p, k->out.len);
    k->out.len = 0;
    k->grid_dirty = true;
    k->tiles[k->slot].live = false;
}

void kitty_renew(Kitty *k) {
    kitty_clear(k);      // takes down the image those cells still name
    k->gen++;
    k->grid_dirty = true;
}

void kitty_free(Kitty *k) {
    buf_free(&k->out);
}

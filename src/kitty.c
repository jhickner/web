#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "web.h"
#include "../vendor/rowcolumn_diacritics.h"

// Three distinct non-zero bytes. The placeholder cells carry the image id in
// their foreground colour, and anything that re-fits colours on the way (tmux
// when it is unsure the terminal takes truecolor) would turn a small id like 1
// into a palette index and lose it. This value survives the trip.
#define IMAGE_ID   0x0A5F31
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
// payload's own ESCs have to be doubled on the way.
static void emit_esc(Kitty *k, const char *seq, size_t n) {
    if (!k->tmux) {
        buf_add(&k->out, seq, n);
        return;
    }
    buf_add(&k->out, "\x1bPtmux;", 7);
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (seq[i] != 0x1b) continue;
        buf_add(&k->out, seq + start, i - start + 1);
        buf_add(&k->out, "\x1b", 1);
        start = i + 1;
    }
    buf_add(&k->out, seq + start, n - start);
    buf_add(&k->out, "\x1b\\", 2);
}

void kitty_init(Kitty *k, int ttyfd, bool tmux) {
    memset(k, 0, sizeof *k);
    k->ttyfd = ttyfd;
    k->tmux = tmux;
    k->grid_dirty = true;
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

    buf_addf(&k->out, "\x1b[38;2;%u;%u;%um", (IMAGE_ID >> 16) & 0xff,
             (IMAGE_ID >> 8) & 0xff, IMAGE_ID & 0xff);
    for (int r = 0; r < rows; r++) {
        buf_addf(&k->out, "\x1b[%d;%dH", k->y + r, k->x);
        for (int c = 0; c < cols; c++) {
            buf_add(&k->out, cell, celln);
            buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[r], enc));
            buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[c], enc));
        }
    }
    buf_add(&k->out, "\x1b[39m", 5);
}

int kitty_draw_png(Kitty *k, const char *b64, size_t len) {
    k->out.len = 0;

    // q=2 suppresses the terminal's replies, which would otherwise land in our
    // stdin as garbage keystrokes.
    size_t off = 0;
    bool first = true;
    while (off < len) {
        size_t n = len - off;
        if (n > CHUNK) n = CHUNK;
        int more = (off + n < len);

        char seq[8192 + 128];
        size_t o = 0;
        if (first)
            o += (size_t)snprintf(seq + o, sizeof seq - o,
                                  "\x1b_Ga=t,f=100,t=d,i=%d,q=2,m=%d;",
                                  IMAGE_ID, more);
        else
            o += (size_t)snprintf(seq + o, sizeof seq - o, "\x1b_Gm=%d;", more);
        memcpy(seq + o, b64 + off, n);
        o += n;
        seq[o++] = 0x1b;
        seq[o++] = '\\';
        emit_esc(k, seq, o);

        off += n;
        first = false;
    }

    // Re-assert the virtual placement each frame: replacing the image data can
    // drop placements, and one 40-byte escape is cheaper than finding out.
    char place[128];
    int pn = snprintf(place, sizeof place,
                      "\x1b_Ga=p,U=1,i=%d,p=%d,c=%d,r=%d,q=2\x1b\\",
                      IMAGE_ID, PLACEMENT, k->cols, k->rows);
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
        if (writeall(k->ttyfd, k->out.p + off, n) < 0) return -1;
        if (g_input_pump) g_input_pump();
        if (g_quit) return -1;
    }
    return 0;
}

void kitty_clear(Kitty *k) {
    k->out.len = 0;
    char seq[64];
    int n = snprintf(seq, sizeof seq, "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\", IMAGE_ID);
    emit_esc(k, seq, (size_t)n);
    writeall(k->ttyfd, k->out.p, k->out.len);
    k->out.len = 0;
    k->grid_dirty = true;
}

void kitty_free(Kitty *k) {
    buf_free(&k->out);
}

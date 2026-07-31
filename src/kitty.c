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
#define IMAGE_ID_BASE 0x0A0000
static unsigned image_id(void) {
    unsigned pid = (unsigned)getpid();
    unsigned hi = ((pid >> 8) & 0xff) | 1;
    unsigned lo = (pid & 0xff) | 1;
    return IMAGE_ID_BASE | (hi << 8) | lo;
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

    unsigned id = image_id();
    buf_addf(&k->out, "\x1b[38;2;%u;%u;%um", (id >> 16) & 0xff,
             (id >> 8) & 0xff, id & 0xff);
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
                          image_id(), more);
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
                      image_id(), PLACEMENT, k->cols, k->rows);
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
    int n = snprintf(seq, sizeof seq, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", image_id());
    emit_esc(k, seq, (size_t)n);
    writeall(k->ttyfd, k->out.p, k->out.len);
    k->out.len = 0;
    k->grid_dirty = true;
}

void kitty_free(Kitty *k) {
    buf_free(&k->out);
}

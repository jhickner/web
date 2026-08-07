#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "web.h"
#include "../vendor/rowcolumn_diacritics.h"

// image id: gen<<16 | pid hi<<8 | pid lo, tile slot in the fourth byte.
// the three low bytes must be non-zero and distinct: gen stays even, both pid
// bytes are forced odd.
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

// unicode placeholder cell for an image chunk
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

// tmux passthrough wrapper; payload ESCs must be doubled inside it
static void esc_open(Kitty *k) {
    if (k->tmux) buf_add(&k->out, "\x1bPtmux;", 7);
}

static void esc_close(Kitty *k) {
    if (k->tmux) buf_add(&k->out, "\x1b\\", 2);
}

// payload that may contain ESCs
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

// payload known to hold no ESCs
static void esc_raw(Kitty *k, const char *seq, size_t n) {
    buf_add(&k->out, seq, n);
}

static void esc_abort(Kitty *k) {
    // under tmux: escaped inner terminator, then the DCS close
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

// slot 0 is the whole picture
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

// slot 0's rect, whichever tile is current
void kitty_area(const Kitty *k, int *x, int *y, int *cols, int *rows) {
    const KittyTile *t = &k->tiles[0];
    bool now = k->slot == 0;
    *x    = now ? k->x    : t->x;
    *y    = now ? k->y    : t->y;
    *cols = now ? k->cols : t->cols;
    *rows = now ? k->rows : t->rows;
}

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
            // third diacritic carries the id's fourth byte
            if (k->slot > 0)
                buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[k->slot], enc));
        }
    }
    buf_add(&k->out, "\x1b[39m", 5);
    k->grid_draws++;
}

int kitty_draw_png(Kitty *k, const char *b64, size_t len) {
    k->out.len = 0;

    buf_reserve(&k->out, len + (len / CHUNK + 2) * 64 + 4096);

    // q=2 suppresses terminal replies
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
        esc_raw(k, b64 + off, n);
        esc_body(k, "\x1b\\", 2);
        esc_close(k);

        off += n;
        first = false;
    }

    char place[128];
    int pn = snprintf(place, sizeof place,
                      "\x1b_Ga=p,U=1,i=%u,p=%d,c=%d,r=%d,q=2\x1b\\",
                      image_id_for(k->gen, k->slot), PLACEMENT, k->cols, k->rows);
    emit_esc(k, place, (size_t)pn);

    if (k->grid_dirty) {
        draw_grid(k);
        k->grid_dirty = false;
    }

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
    kitty_clear(k);
    k->gen++;
    k->grid_dirty = true;
}

void kitty_free(Kitty *k) {
    buf_free(&k->out);
}

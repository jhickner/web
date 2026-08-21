#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#include "web.h"
#include "../vendor/rowcolumn_diacritics.h"

// image id: gen<<16 | pid hi<<8 | pid lo, tag in the fourth byte.
// the three low bytes must be non-zero and distinct: gen stays even, both pid
// bytes are forced odd. The tag names one of the pictures that can be on screen
// at once: 0 the page, 1..GRID_MAX the grid tiles, BAND_ID_BASE+b a damage
// band - which is why the last range starts clear of the others.
#define BAND_ID_BASE 16
#define IMAGE_ID_BASE(gen) ((0x0Au + 2u * ((gen) % 100u)) << 16)
static unsigned image_id_for(unsigned gen, int tag) {
    unsigned pid = (unsigned)getpid();
    unsigned hi = ((pid >> 8) & 0xff) | 1;
    unsigned lo = (pid & 0xff) | 1;
    unsigned id = IMAGE_ID_BASE(gen) | (hi << 8) | lo;
    if (tag > 0) id |= ((unsigned)tag & 0xffu) << 24;
    return id;
}

#define PLACEMENT  1
#define CHUNK      4096
// tmux resets cursor and mouse modes once per passthrough DCS, so chunks share
// a wrapper. A DCS over tmux's input-buffer-size is dropped whole; its floor is 1MB.
#define WRAP_MAX   262144

// The page is a stack of band images rather than one picture. Only ever true of
// slot 0: a grid tile is always sent whole.
static bool banded(const Kitty *k) { return k->slot == 0 && k->dmg.live; }

static int band_tag(const Kitty *k, int b) {
    return banded(k) ? BAND_ID_BASE + b : k->slot;
}

// Which cell rows of the rect band b covers. The bands tile the rect exactly,
// so a row belongs to one band and the split is the same everywhere it is
// worked out - the placement, the placeholder cells and the pixel rows.
static void band_rows(int rows, int nb, int b, int *r0, int *r1) {
    if (nb < 1) nb = 1;
    *r0 = rows * b / nb;
    *r1 = rows * (b + 1) / nb;
}

static void band_px(int h, int rows, int nb, int b, int *p0, int *p1) {
    int r0, r1;
    band_rows(rows, nb, b, &r0, &r1);
    *p0 = (int)((long)h * r0 / rows);
    *p1 = (int)((long)h * r1 / rows);
}

// A band needs a cell row and a pixel row to call its own.
static int bands_for(int rows, int h) {
    int nb = BANDS_MAX;
    if (nb > rows) nb = rows;
    if (nb > h) nb = h;
    return nb < 1 ? 1 : nb;
}

// "tmux 3.7b" / "tmux next-3.8" -> 307 / 308. 0 when it cannot be parsed.
static int tmux_version(void) {
    FILE *p = popen("tmux -V 2>/dev/null", "r");
    if (!p) return 0;
    char buf[64] = "";
    bool got = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    if (!got) return 0;
    const char *s = buf;
    while (*s && (*s < '0' || *s > '9')) s++;
    int maj = 0, min = 0;
    if (sscanf(s, "%d.%d", &maj, &min) < 2) return 0;
    return maj * 100 + min;
}

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

// Terminal-facing synchronized update, appended to k->out. Nests: only the
// outermost pair reaches the terminal, so a caller that has already opened one
// around a wider redraw keeps it open across the frame inside it.
static void sync_push(Kitty *k) {
    if (k->sync_depth++ == 0) emit_esc(k, "\x1b[?2026h", 8);
}

static void sync_pop(Kitty *k) {
    if (k->sync_depth > 0 && --k->sync_depth == 0) emit_esc(k, "\x1b[?2026l", 8);
}

// A frame that died mid-write would otherwise leave the terminal holding the
// update it never got the end of.
static void sync_drop(Kitty *k) {
    if (!k->sync_depth) return;
    k->sync_depth = 1;
    size_t at = k->out.len;
    sync_pop(k);
    writeall(k->ttyfd, k->out.p + at, k->out.len - at);
    k->out.len = at;
}

void kitty_init(Kitty *k, int ttyfd, bool tmux) {
    memset(k, 0, sizeof *k);
    k->ttyfd = ttyfd;
    k->tmux = tmux;
    k->tmux_redraw = tmux && tmux_version() >= 307;
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

    // tmux 3.7 drops a cell's combining diacritics on the way to the terminal
    // whenever the pane is not at column 0: screen_write_combine() tests
    // visibility with a pane-relative x against window coordinates, decides the
    // cell is covered by whatever pane really sits there, and skips the write.
    // The cells still land in tmux's grid intact, so ending a synchronized
    // update - which tmux consumes itself, and answers with a full pane redraw
    // out of that grid - puts them on screen correctly. Unwrapped on purpose:
    // this one is addressed to tmux, not to the terminal underneath it.
    if (k->tmux_redraw) buf_add(&k->out, "\x1b[?2026h", 8);

    // Only the fourth id byte tells the bands apart, and that one travels as a
    // diacritic - so the colour, which carries the other three, is written once.
    unsigned id = image_id_for(k->gen, 0);
    buf_addf(&k->out, "\x1b[38;2;%u;%u;%um", (id >> 16) & 0xff,
             (id >> 8) & 0xff, id & 0xff);
    int nb = banded(k) ? k->dmg.nbands : 1;
    for (int b = 0; b < nb; b++) {
        // Each band is its own picture, so its rows are numbered from its own
        // top rather than the rect's.
        int r0 = 0, r1 = rows;
        if (banded(k)) band_rows(k->rows, nb, b, &r0, &r1);
        if (r1 > rows) r1 = rows;
        int tag = band_tag(k, b);
        for (int r = r0; r < r1; r++) {
            buf_addf(&k->out, "\x1b[%d;%dH", k->y + r, k->x);
            for (int c = 0; c < cols; c++) {
                buf_add(&k->out, cell, celln);
                buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[r - r0], enc));
                buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[c], enc));
                // third diacritic carries the id's fourth byte
                if (tag > 0)
                    buf_add(&k->out, enc, utf8_encode(rowcolumn_diacritics[tag], enc));
            }
        }
    }
    buf_add(&k->out, "\x1b[39m", 5);
    if (k->tmux_redraw) buf_add(&k->out, "\x1b[?2026l", 8);
    k->grid_draws++;
}

// Chunked transmission of one image, appended to k->out. `spec` is the first
// chunk's key list, without the m= that closes it. q=2 suppresses replies.
static void emit_chunks(Kitty *k, const char *spec, const char *b64, size_t len) {
    size_t off = 0, wrapped = 0;
    bool first = true;
    while (off < len) {
        size_t n = len - off;
        if (n > CHUNK) n = CHUNK;
        int more = (off + n < len);

        char hdr[128];
        int hn = first ? snprintf(hdr, sizeof hdr, "\x1b_G%s,m=%d;", spec, more)
                       : snprintf(hdr, sizeof hdr, "\x1b_Gm=%d;", more);

        size_t piece = (size_t)hn + n + 2;
        if (wrapped && wrapped + piece > WRAP_MAX) {
            esc_close(k);
            wrapped = 0;
        }
        if (!wrapped) esc_open(k);

        esc_body(k, hdr, (size_t)hn);
        esc_raw(k, b64 + off, n);
        esc_body(k, "\x1b\\", 2);
        wrapped += piece;

        off += n;
        first = false;
    }
    if (wrapped) esc_close(k);
}

// A virtual placement, which draws nothing by itself: the placeholder cells on
// screen are what the terminal fills in. Re-sent with every transmission
// because replacing an image id also drops its placements.
static void emit_place(Kitty *k, unsigned id, int cols, int rows) {
    char place[128];
    int pn = snprintf(place, sizeof place,
                      "\x1b_Ga=p,U=1,i=%u,p=%d,c=%d,r=%d,q=2\x1b\\",
                      id, PLACEMENT, cols, rows);
    emit_esc(k, place, (size_t)pn);
}

// Out in slices, with the input pump in between: a frame is a blocking write
// big enough that keys pressed during it would otherwise wait on it.
static int kitty_flush(Kitty *k) {
    const size_t SLICE = 32768;
    for (size_t off = 0; off < k->out.len; off += SLICE) {
        size_t n = k->out.len - off;
        if (n > SLICE) n = SLICE;
        if (writeall(k->ttyfd, k->out.p + off, n) < 0) {
            esc_abort(k); sync_drop(k); return -1;
        }
        if (g_input_pump) g_input_pump();
        if (g_quit) { esc_abort(k); sync_drop(k); return -1; }
    }
    return 0;
}

int kitty_draw_png(Kitty *k, const char *b64, size_t len) {
    // A whole picture goes up under the tile's own id, so whatever bands the
    // terminal is holding stop being what the placeholder cells point at.
    if (banded(k)) kitty_clear(k);

    k->out.len = 0;
    buf_reserve(&k->out, len + (len / CHUNK + 2) * 64 + 4096);

    // A frame that lays down a fresh placeholder grid must reach the terminal
    // whole: cells arriving on their own are a picture with no image behind
    // them yet, and cells the terminal has only half of read as one image row
    // repeated down the pane. Held in a synchronized update, none of that is
    // ever presented. Frames that reuse the grid already swap atomically, on
    // the last chunk of the transmission.
    bool grid = k->grid_dirty;
    if (grid) sync_push(k);

    unsigned id = image_id_for(k->gen, k->slot);
    char spec[64];
    snprintf(spec, sizeof spec, "a=t,f=100,t=d,i=%u,q=2", id);
    emit_chunks(k, spec, b64, len);
    emit_place(k, id, k->cols, k->rows);

    if (grid) {
        draw_grid(k);
        k->grid_dirty = false;
        sync_pop(k);
    }

    if (kitty_flush(k) < 0) return -1;
    k->tiles[k->slot].live = true;
    return 0;
}

// One band: deflate, base64, transmit, place. Level 1 deflate because the tty
// is the bottleneck and not the cpu - the levels above it buy a few percent for
// several times the time. Raw pixels that refuse to shrink go out as they are.
static int emit_band(Kitty *k, int b, const uint8_t *px, int w, int bh,
                     int brows) {
    Damage *d = &k->dmg;
    size_t len = (size_t)w * bh * 3;
    const void *tx = px;
    size_t txn = len;
    bool zip = false;

    uLongf zn = compressBound((uLong)len);
    if (buf_reserve(&d->z, zn) == 0 &&
        compress2((Bytef *)d->z.p, &zn, px, (uLong)len, 1) == Z_OK &&
        (size_t)zn < len) {
        tx = d->z.p;
        txn = zn;
        zip = true;
    }
    if (buf_reserve(&d->b64, (txn + 2) / 3 * 4 + 8) < 0) return -1;
    size_t bn = base64_encode(tx, txn, d->b64.p);

    unsigned id = image_id_for(k->gen, BAND_ID_BASE + b);
    char spec[96];
    snprintf(spec, sizeof spec, "a=t,f=24%s,s=%d,v=%d,i=%u,q=2",
             zip ? ",o=z" : "", w, bh, id);
    emit_chunks(k, spec, d->b64.p, bn);
    emit_place(k, id, k->cols, brows);
    d->bytes += bn;
    return 0;
}

int kitty_draw_damage(Kitty *k, const char *b64, size_t len) {
    Damage *d = &k->dmg;
    if (!d->on || k->slot != 0 || k->rows < 1 || k->cols < 1) return -1;

    if (buf_reserve(&d->png, len / 4 * 3 + 4) < 0) return -1;
    size_t pn = base64_decode(b64, len, d->png.p);
    int w = 0, h = 0;
    uint8_t *rgb = png_decode_rgb(d->png.p, pn, &w, &h);
    if (!rgb || w < 1 || h < 1) { png_decode_free(rgb); return -1; }

    int nb = bands_for(k->rows, h);
    size_t frame = (size_t)w * h * 3;

    // Anything that stops the previous frame describing what is on screen - a
    // different picture size, a different rectangle, a grid about to be laid
    // down again - means every band goes out, not just the ones that moved.
    bool all = !d->live || k->grid_dirty || d->nbands != nb ||
               d->w != w || d->h != h || d->cols != k->cols || d->rows != k->rows;
    if (all) {
        if (d->prevcap < frame) {
            uint8_t *np = realloc(d->prev, frame);
            if (!np) { png_decode_free(rgb); return -1; }
            d->prev = np;
            d->prevcap = frame;
        }
        d->nbands = nb;
        d->w = w; d->h = h;
        d->cols = k->cols; d->rows = k->rows;
    }

    // Which bands moved is settled before anything is written, so the frame
    // knows whether it is a single band - which lands atomically on its own
    // last chunk - or several, which have to be held together.
    bool dirty[BANDS_MAX];
    unsigned sent = 0;
    for (int b = 0; b < nb; b++) {
        int p0, p1;
        band_px(h, k->rows, nb, b, &p0, &p1);
        size_t off = (size_t)p0 * w * 3, n = (size_t)(p1 - p0) * w * 3;
        dirty[b] = p1 > p0 && (all || memcmp(d->prev + off, rgb + off, n) != 0);
        if (dirty[b]) sent++;
    }

    d->sent = sent;
    d->total = (unsigned)nb;
    d->bytes = 0;
    // Whatever made this a full frame also moved the cells: a different band
    // count splits the rows differently, and coming from a whole picture leaves
    // them naming an image that is about to stop existing.
    bool grid = all;
    if (!sent && !grid) { png_decode_free(rgb); return 0; }

    d->live = true;
    k->out.len = 0;
    bool sync = grid || sent > 1;
    if (sync) sync_push(k);

    for (int b = 0; b < nb; b++) {
        if (!dirty[b]) continue;
        int p0, p1, r0, r1;
        band_px(h, k->rows, nb, b, &p0, &p1);
        band_rows(k->rows, nb, b, &r0, &r1);
        size_t off = (size_t)p0 * w * 3, n = (size_t)(p1 - p0) * w * 3;
        if (emit_band(k, b, rgb + off, w, p1 - p0, r1 - r0) < 0) continue;
        memcpy(d->prev + off, rgb + off, n);
    }

    if (grid) {
        draw_grid(k);
        k->grid_dirty = false;
    }
    if (sync) sync_pop(k);

    png_decode_free(rgb);
    if (kitty_flush(k) < 0) {
        // Half a frame is on screen and prev no longer describes it.
        d->live = false;
        return -1;
    }
    k->tiles[0].live = true;
    return 0;
}

void kitty_set_damage(Kitty *k, bool on) {
    if (k->dmg.on == on) return;
    k->dmg.on = on;
    // The placeholder cells name the image that fills them, so going between
    // one picture and a stack of bands means taking down what is up.
    int slot = k->slot;
    kitty_use(k, 0);
    kitty_renew(k);
    kitty_use(k, slot);
}

void kitty_clear(Kitty *k) {
    k->out.len = 0;
    int nb = banded(k) ? k->dmg.nbands : 1;
    for (int b = 0; b < nb; b++) {
        char seq[64];
        int n = snprintf(seq, sizeof seq, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\",
                         image_id_for(k->gen, band_tag(k, b)));
        emit_esc(k, seq, (size_t)n);
    }
    writeall(k->ttyfd, k->out.p, k->out.len);
    k->out.len = 0;
    k->grid_dirty = true;
    k->tiles[k->slot].live = false;
    if (k->slot == 0) k->dmg.live = false;
}

// Put the image the terminal already holds into the current rect: a new
// placement and a matching placeholder grid, no pixels. A resize can then show
// the old picture at the new size right away instead of a hole, and the next
// frame overwrites it. -1 when there is no image to re-place.
int kitty_replace(Kitty *k) {
    if (!k->tiles[k->slot].live) return -1;
    k->out.len = 0;
    sync_push(k);
    int nb = banded(k) ? k->dmg.nbands : 1;
    for (int b = 0; b < nb; b++) {
        int r0 = 0, r1 = k->rows;
        if (banded(k)) band_rows(k->rows, nb, b, &r0, &r1);
        if (r1 > r0)
            emit_place(k, image_id_for(k->gen, band_tag(k, b)), k->cols, r1 - r0);
    }
    draw_grid(k);
    k->grid_dirty = false;
    sync_pop(k);
    if (writeall(k->ttyfd, k->out.p, k->out.len) < 0) {
        k->out.len = 0;
        sync_drop(k);
        return -1;
    }
    k->out.len = 0;
    return 0;
}

void kitty_renew(Kitty *k) {
    kitty_clear(k);
    k->gen++;
    k->grid_dirty = true;
}

// Bracket a redraw so the terminal presents it whole rather than in the order
// it arrives. Wrapped for tmux, unlike the unwrapped pair in draw_grid: this
// one is addressed to the terminal underneath, which is what has to hold the
// old picture up until the new one is complete.
//
// A terminal without synchronized output ignores both and simply redraws as the
// bytes land, which is what it did before.
void kitty_sync_begin(Kitty *k) {
    k->out.len = 0;
    sync_push(k);
    writeall(k->ttyfd, k->out.p, k->out.len);
    k->out.len = 0;
}

void kitty_sync_end(Kitty *k) {
    k->out.len = 0;
    sync_pop(k);
    writeall(k->ttyfd, k->out.p, k->out.len);
    k->out.len = 0;
}

void kitty_free(Kitty *k) {
    buf_free(&k->out);
    buf_free(&k->dmg.png);
    buf_free(&k->dmg.z);
    buf_free(&k->dmg.b64);
    free(k->dmg.prev);
    k->dmg.prev = NULL;
    k->dmg.prevcap = 0;
}

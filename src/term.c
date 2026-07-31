#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "web.h"

static struct termios g_saved;
static bool g_have_saved = false;

#define ENTER_UI  "\x1b[?1049h\x1b[?25l\x1b[2J"
#define LEAVE_UI  "\x1b[?25h\x1b[?1049l"
// Bracketed paste is what makes the terminal's own paste key usable: without
// it a paste arrives as bare keystrokes, indistinguishable from typing, and a
// newline in the middle of it would submit whatever it landed in.
#define MOUSE_ON  "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?2004h"
#define MOUSE_OFF "\x1b[?2004l\x1b[?1006l\x1b[?1002l\x1b[?1000l"

// Ask for kitty's disambiguated key reporting, which is the only way a cmd
// chord can reach us at all: the legacy encoding has no bit for super. A
// terminal that does not know the sequence ignores it and keeps sending the
// legacy codes, so both encodings have to stay readable either way.
#define KBD_ON  "\x1b[>1u"
#define KBD_OFF "\x1b[<u"

void term_size(Term *t) {
    struct winsize ws = {0};
    if (ioctl(t->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        t->cols = ws.ws_col;
        t->rows = ws.ws_row;
        if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
            t->cell_w = ws.ws_xpixel / ws.ws_col;
            t->cell_h = ws.ws_ypixel / ws.ws_row;
        }
    }
    if (t->cols <= 0) { t->cols = 80; t->rows = 24; }

    const char *env = getenv("WEB_CELL");
    int cw = 0, ch = 0;
    if (env && sscanf(env, "%dx%d", &cw, &ch) == 2 && cw > 0 && ch > 0) {
        t->cell_w = cw;
        t->cell_h = ch;
    }
    // tmux reports no pixel geometry; the page is scaled into the cell rect
    // either way, so this only needs to be close enough to keep the aspect.
    if (t->cell_w <= 0 || t->cell_h <= 0 ||
        t->cell_w > 64 || t->cell_h > 64) {
        t->cell_w = 8;
        t->cell_h = 17;
    }
}

// Measure the terminal without changing any of its state, so the viewport can
// be sized before Chrome is told to open a window: a browser launched at the
// wrong size lays the page out twice and paints a frame nobody wanted.
int term_probe(Term *t) {
    // Open the terminal by its real device path. On macOS poll() reports
    // POLLNVAL for the /dev/tty clone device, so a descriptor from there can be
    // written to but never reports readable - the app renders fine and ignores
    // every keystroke.
    t->fd = -1;
    if (isatty(STDIN_FILENO)) {
        const char *name = ttyname(STDIN_FILENO);
        if (name) t->fd = open(name, O_RDWR);
    }
    if (t->fd < 0) t->fd = open("/dev/tty", O_RDWR);
    if (t->fd < 0) t->fd = STDIN_FILENO;

    // Whatever we ended up with, it has to be pollable for reading.
    struct pollfd probe = {t->fd, POLLIN, 0};
    if (poll(&probe, 1, 0) >= 0 && (probe.revents & POLLNVAL)) {
        if (t->fd != STDIN_FILENO) close(t->fd);
        t->fd = STDIN_FILENO;
    }
    term_size(t);
    return 0;
}

void term_enter(Term *t, bool inline_mode) {
    if (tcgetattr(t->fd, &g_saved) == 0) {
        g_have_saved = true;
        struct termios raw = g_saved;
        raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= (tcflag_t)~(OPOST);
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(t->fd, TCSAFLUSH, &raw);
        t->raw = true;
    }

    int fl = fcntl(t->fd, F_GETFL, 0);
    fcntl(t->fd, F_SETFL, fl | O_NONBLOCK);

    t->inline_mode = inline_mode;
    // Inline keeps the shell's screen and scrollback: the page becomes part of
    // the session's history rather than taking the display over.
    writeall(t->fd, inline_mode ? "\x1b[?25l" : ENTER_UI,
             inline_mode ? 6 : strlen(ENTER_UI));
    writeall(t->fd, MOUSE_ON, strlen(MOUSE_ON));
    writeall(t->fd, KBD_ON, strlen(KBD_ON));
    term_size(t);
}

// Scroll a block of rows into view and take the bottom of the screen for it.
// The origin comes from the terminal height rather than a cursor-position
// query, which tmux answers unreliably and which can swallow keystrokes.
// Where the cursor is, asked of the terminal rather than assumed. Whatever else
// arrives while the answer is on its way is kept: those are keystrokes, and the
// block is being set up at exactly the moment someone is most likely to type.
static int query_cursor_row(Term *t) {
    writeall(t->fd, "\x1b[6n", 4);

    char buf[256];
    size_t n = 0;
    double deadline = now_sec() + 0.25;
    while (now_sec() < deadline && n < sizeof buf - 1) {
        struct pollfd p = {t->fd, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        ssize_t r = read(t->fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0) continue;
        n += (size_t)r;

        // Look for CSI row ; col R anywhere in what has arrived so far.
        for (size_t i = 0; i + 2 < n; i++) {
            if (buf[i] != 0x1b || buf[i + 1] != '[') continue;
            size_t e = i + 2;
            while (e < n && ((buf[e] >= '0' && buf[e] <= '9') || buf[e] == ';')) e++;
            if (e >= n) break;                  // still arriving
            if (buf[e] != 'R') continue;
            int row = 0, col = 0;
            if (sscanf(buf + i + 2, "%d;%d", &row, &col) != 2) continue;
            buf_add(&t->in, buf, i);            // keep what came before it
            buf_add(&t->in, buf + e + 1, n - e - 1);   // and after
            return row;
        }
    }
    buf_add(&t->in, buf, n);       // no answer: none of it was for us to eat
    return 0;
}

void term_reserve_inline(Term *t, int rows) {
    if (rows > t->rows) rows = t->rows;
    if (rows < 2) rows = 2;
    Buf b = {0};
    for (int i = 0; i < rows; i++) buf_add(&b, "\r\n", 2);
    writeall(t->fd, b.p, b.len);
    buf_free(&b);

    // Climb back to the block's first row and ask where that turned out to be.
    // Those newlines only scroll the screen if the cursor was already near the
    // bottom of it; anywhere else they just walk down, and the block belongs
    // where the cursor started - under the command that was just run - rather
    // than pinned to the bottom edge.
    char up[32];
    int n = snprintf(up, sizeof up, "\x1b[%dA", rows);
    writeall(t->fd, up, (size_t)n);

    int row = query_cursor_row(t);
    t->inline_origin = row > 0 ? row : t->rows - rows + 1;
    if (t->inline_origin < 1) t->inline_origin = 1;
    t->inline_rows = rows;
}

// Regrow or shrink the reserved block in place. The caller drops the image
// first: the rows it lives on are about to mean something else, and a picture
// left addressed to them survives as a smear the terminal will not clean up.
void term_resize_inline(Term *t, int rows) {
    if (rows < 2) rows = 2;
    if (rows > t->rows) rows = t->rows;
    if (t->inline_rows < 1) { term_reserve_inline(t, rows); return; }
    if (rows == t->inline_rows) return;

    char buf[32];
    for (int i = 0; i < t->inline_rows; i++) {
        int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[2K", t->inline_origin + i);
        writeall(t->fd, buf, (size_t)n);
    }

    // Shrinking keeps the origin and simply owns fewer rows. Scrolling the
    // difference away instead would drag the shell's history up with it.
    if (rows < t->inline_rows) {
        t->inline_rows = rows;
        return;
    }

    int bottom = t->inline_origin + t->inline_rows - 1;
    int n = snprintf(buf, sizeof buf, "\x1b[%d;1H", bottom);
    writeall(t->fd, buf, (size_t)n);
    int extra = rows - t->inline_rows;
    for (int i = 0; i < extra; i++) writeall(t->fd, "\r\n", 2);

    // Where the block ended up, worked out from how far past the last row we
    // asked the terminal to go. Asking it directly means a cursor report, and
    // that read would race the keystrokes still arriving.
    int overflow = bottom + extra - t->rows;
    if (overflow > 0) t->inline_origin -= overflow;
    if (t->inline_origin < 1) t->inline_origin = 1;
    t->inline_rows = rows;
}

void term_restore(Term *t, bool clear_inline) {
    if (t->fd < 0) return;
    writeall(t->fd, KBD_OFF, strlen(KBD_OFF));
    writeall(t->fd, MOUSE_OFF, strlen(MOUSE_OFF));
    if (t->inline_mode && clear_inline) {
        // Wipe the block and hand its first row back: that is the row the
        // command was run from, so the prompt returns where it left off and
        // nothing of the window survives into the scrollback.
        char buf[32];
        for (int i = 0; i < t->inline_rows; i++) {
            int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[2K",
                             t->inline_origin + i);
            writeall(t->fd, buf, (size_t)n);
        }
        int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[?25h", t->inline_origin);
        writeall(t->fd, buf, (size_t)n);
    } else if (t->inline_mode) {
        // Park the cursor under the block and leave the picture on screen.
        char buf[32];
        int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\r\n\x1b[?25h", t->rows);
        writeall(t->fd, buf, (size_t)n);
    } else {
        writeall(t->fd, LEAVE_UI, strlen(LEAVE_UI));
    }
    if (g_have_saved) tcsetattr(t->fd, TCSAFLUSH, &g_saved);
    buf_free(&t->paste);
    buf_free(&t->in);
}

// Set WEB_DEBUG to record exactly what arrives from the terminal.
void term_log(const char *fmt, ...) {
    static FILE *f;
    static int checked;
    if (!checked) {
        checked = 1;
        if (getenv("WEB_DEBUG")) f = fopen("/tmp/web_input.log", "w");
    }
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

int term_read(Term *t) {
    char tmp[4096];
    int total = 0;
    for (;;) {
        ssize_t r = read(t->fd, tmp, sizeof tmp);
        if (r > 0) {
            char hex[256];
            size_t o = 0;
            for (ssize_t i = 0; i < r && o < sizeof hex - 4; i++)
                o += (size_t)snprintf(hex + o, sizeof hex - o, "%02x ",
                                      (unsigned char)tmp[i]);
            term_log("%.3f read %zd bytes: %s", now_sec(), r, hex);
            buf_add(&t->in, tmp, (size_t)r);
            total += (int)r;
            continue;
        }
        if (r == 0) return total;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            term_log("%.3f read error %d", now_sec(), errno);
        return total;
    }
}

static int mods_from_param(int p) {
    if (p <= 1) return 0;
    int m = p - 1;
    int out = 0;
    if (m & 1) out |= MOD_SHIFT;
    if (m & 2) out |= MOD_ALT;
    if (m & 4) out |= MOD_CTRL;
    if (m & 8) out |= MOD_SUPER;
    return out;
}

static int csi_tilde_key(int n) {
    switch (n) {
    case 1: case 7: return KEY_HOME;
    case 3:         return KEY_DELETE;
    case 4: case 8: return KEY_END;
    case 5:         return KEY_PGUP;
    case 6:         return KEY_PGDN;
    case 15:        return KEY_F5;
    case 17:        return KEY_F6;
    case 18:        return KEY_F7;
    case 19:        return KEY_F8;
    case 20:        return KEY_F9;
    case 21:        return KEY_F10;
    case 23:        return KEY_F11;
    case 24:        return KEY_F12;
    default:        return KEY_NONE;
    }
}

// Decodes one event from the head of the buffer. Returns 1 when an event was
// produced, 0 when more bytes are needed.
int term_next(Term *t, Event *ev) {
    memset(ev, 0, sizeof *ev);
    const unsigned char *b = (const unsigned char *)t->in.p;
    size_t n = t->in.len;
    if (n == 0) return 0;

    if (b[0] != 0x1b) {
        unsigned char c = b[0];
        ev->type = EV_KEY;
        if (c == 0x0d || c == 0x0a) { ev->key = KEY_ENTER; buf_consume(&t->in, 1); return 1; }
        if (c == 0x09) { ev->key = KEY_TAB; buf_consume(&t->in, 1); return 1; }
        if (c == 0x7f) { ev->key = KEY_BACKSPACE; buf_consume(&t->in, 1); return 1; }
        if (c < 0x20) {
            ev->key = 'a' + c - 1;
            ev->mods = MOD_CTRL;
            buf_consume(&t->in, 1);
            return 1;
        }
        size_t clen = 1;
        if (c >= 0xf0) clen = 4;
        else if (c >= 0xe0) clen = 3;
        else if (c >= 0xc0) clen = 2;
        if (n < clen) return 0;
        memcpy(ev->text, b, clen);
        ev->text[clen] = 0;
        ev->key = c;              // ASCII keys keep their codepoint for bindings
        buf_consume(&t->in, clen);
        return 1;
    }

    // ESC with nothing behind it: wait briefly, then call it a real Escape.
    if (n == 1) {
        if (t->esc_at == 0.0) t->esc_at = now_sec();
        if (now_sec() - t->esc_at < 0.05) return 0;
        t->esc_at = 0.0;
        ev->type = EV_KEY;
        ev->key = KEY_ESC;
        buf_consume(&t->in, 1);
        return 1;
    }
    t->esc_at = 0.0;

    if (b[1] == 'O') {
        if (n < 3) return 0;
        ev->type = EV_KEY;
        switch (b[2]) {
        case 'P': ev->key = KEY_F1; break;
        case 'Q': ev->key = KEY_F2; break;
        case 'R': ev->key = KEY_F3; break;
        case 'S': ev->key = KEY_F4; break;
        case 'A': ev->key = KEY_UP; break;
        case 'B': ev->key = KEY_DOWN; break;
        case 'C': ev->key = KEY_RIGHT; break;
        case 'D': ev->key = KEY_LEFT; break;
        default:  ev->key = KEY_NONE; break;
        }
        buf_consume(&t->in, 3);
        return ev->key != KEY_NONE;
    }

    if (b[1] != '[') {                       // ESC x  =>  Alt+x
        ev->type = EV_KEY;
        ev->key = b[1];
        ev->mods = MOD_ALT;
        buf_consume(&t->in, 2);
        return 1;
    }

    size_t i = 2;
    while (i < n && (b[i] == '<' || b[i] == '?' || b[i] == ';' || b[i] == ':' ||
                     (b[i] >= '0' && b[i] <= '9')))
        i++;
    if (i >= n) return 0;
    unsigned char final = b[i];
    size_t seqlen = i + 1;

    if (b[2] == '<' && (final == 'M' || final == 'm')) {
        int code = 0, x = 0, y = 0;
        if (sscanf((const char *)b + 3, "%d;%d;%d", &code, &x, &y) == 3) {
            ev->type = EV_MOUSE;
            ev->press = (final == 'M');
            ev->mx = x;
            ev->my = y;
            ev->motion = (code & 32) != 0;
            if (code & 64) {
                ev->button = (code & 3) == 0 ? 3 : 4;   // wheel up / down
            } else {
                ev->button = code & 3;
            }
            if (code & 4)  ev->mods |= MOD_SHIFT;
            if (code & 8)  ev->mods |= MOD_ALT;
            if (code & 16) ev->mods |= MOD_CTRL;
        }
        buf_consume(&t->in, seqlen);
        return ev->type != EV_NONE;
    }

    int p1 = 0, p2 = 0;
    sscanf((const char *)b + 2, "%d;%d", &p1, &p2);

    // A bracketed paste is one event, however much text is inside it. Until the
    // closing marker arrives the whole thing has to stay in the buffer: cut
    // short it would be delivered as two pastes, and a paste split down the
    // middle is worse than a paste that waits.
    if (final == '~' && p1 == 200) {
        static const char END[] = "\x1b[201~";
        const char *hay = t->in.p;
        const char *stop = NULL;
        for (size_t j = seqlen; j + sizeof END - 1 <= n; j++) {
            if (!memcmp(hay + j, END, sizeof END - 1)) { stop = hay + j; break; }
        }
        if (!stop) return 0;                  // wait for the rest
        size_t body = (size_t)(stop - (hay + seqlen));
        t->paste.len = 0;
        buf_add(&t->paste, hay + seqlen, body);
        buf_add(&t->paste, "", 1);            // callers read it as a C string
        t->paste.len--;
        buf_consume(&t->in, seqlen + body + sizeof END - 1);
        ev->type = EV_PASTE;
        return 1;
    }

    // Kitty's CSI u form. Once the disambiguating mode is on, everything
    // carrying a modifier arrives this way, so this is not just the cmd path:
    // ^L and alt+f come through here too and have to land on the same key and
    // modifier pair the legacy branch below would have produced.
    if (final == 'u') {
        int m = mods_from_param(p2);
        ev->type = EV_KEY;
        switch (p1) {
        case 27:  ev->key = KEY_ESC; break;
        case 13:  ev->key = KEY_ENTER; break;
        case 9:   ev->key = KEY_TAB; break;
        case 127: ev->key = KEY_BACKSPACE; break;
        default:
            // Above the unicode range are the functional keys, which this
            // build has no bindings for; the legacy forms still carry the ones
            // it does use.
            if (p1 <= 0 || p1 >= 0x110000) { ev->key = KEY_NONE; break; }
            ev->key = p1;
            // The protocol reports the unshifted key and a shift bit, but the
            // rest of the app reads a shifted letter as its own key: G is a
            // binding, shift+g is not.
            if ((m & MOD_SHIFT) && p1 >= 'a' && p1 <= 'z') {
                ev->key = p1 - 32;
                m &= ~MOD_SHIFT;
            }
            if (!(m & (MOD_CTRL | MOD_ALT | MOD_SUPER)) &&
                ev->key >= 0x20 && ev->key < 0x7f) {
                ev->text[0] = (char)ev->key;
                ev->text[1] = 0;
            }
            break;
        }
        ev->mods = m;
        buf_consume(&t->in, seqlen);
        return ev->key != KEY_NONE;
    }

    ev->type = EV_KEY;
    ev->mods = mods_from_param(p2);
    switch (final) {
    case 'A': ev->key = KEY_UP; break;
    case 'B': ev->key = KEY_DOWN; break;
    case 'C': ev->key = KEY_RIGHT; break;
    case 'D': ev->key = KEY_LEFT; break;
    case 'H': ev->key = KEY_HOME; break;
    case 'F': ev->key = KEY_END; break;
    case '~': ev->key = csi_tilde_key(p1); break;
    default:  ev->key = KEY_NONE; break;
    }
    buf_consume(&t->in, seqlen);
    return ev->key != KEY_NONE;
}

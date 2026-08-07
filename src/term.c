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
// 1000/1002 mouse, 1006 sgr coords, 2004 bracketed paste, 1004 focus, 1003 hover
#define MOUSE_ON  "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?2004h\x1b[?1004h"
#define MOUSE_OFF "\x1b[?1004l\x1b[?2004l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l"
#define HOVER_ON  "\x1b[?1003h"
#define HOVER_OFF "\x1b[?1003l"

// kitty key reporting, flag 1 (disambiguate) only
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
    bool guessed = t->cell_w <= 0 || t->cell_h <= 0 ||
                   t->cell_w > 64 || t->cell_h > 64;
    if (guessed) {
        t->cell_w = 8;
        t->cell_h = 17;
    }
    term_log("cell %dx%d (%s), winsize %dx%d px over %dx%d cells",
             t->cell_w, t->cell_h, guessed ? "guessed" : "measured",
             ws.ws_xpixel, ws.ws_ypixel, ws.ws_col, ws.ws_row);
}

int term_probe(Term *t) {
    t->fd = -1;
    if (isatty(STDIN_FILENO)) {
        const char *name = ttyname(STDIN_FILENO);
        if (name) t->fd = open(name, O_RDWR);
    }
    if (t->fd < 0) t->fd = open("/dev/tty", O_RDWR);
    if (t->fd < 0) t->fd = STDIN_FILENO;

    struct pollfd probe = {t->fd, POLLIN, 0};
    if (poll(&probe, 1, 0) >= 0 && (probe.revents & POLLNVAL)) {
        if (t->fd != STDIN_FILENO) close(t->fd);
        t->fd = STDIN_FILENO;
    }
    term_size(t);
    return 0;
}

// a=q asks whether the graphics protocol is there at all; the tmux form doubles
// the payload's ESCs inside a passthrough wrapper, as every other one does
#define GFX_Q      "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"
#define GFX_Q_TMUX "\x1bPtmux;\x1b\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\x1b\\\x1b\\"

static bool reply_ok(const char *b, size_t n) {
    for (size_t i = 0; i + 3 < n; i++) {
        if (b[i] != 0x1b || b[i + 1] != '_' || b[i + 2] != 'G') continue;
        for (size_t j = i + 3; j < n; j++) {
            if (b[j] == 0x1b) break;
            if (b[j] != ';') continue;
            return j + 2 < n && b[j + 1] == 'O' && b[j + 2] == 'K';
        }
    }
    return false;
}

// CSI ? ... c
static bool reply_da(const char *b, size_t n) {
    for (size_t i = 0; i + 3 < n; i++) {
        if (b[i] != 0x1b || b[i + 1] != '[' || b[i + 2] != '?') continue;
        for (size_t j = i + 3; j < n; j++) {
            if (b[j] == 'c') return true;
            if (!((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) break;
        }
    }
    return false;
}

// A terminal that draws kitty graphics answers the query with OK. One that does
// not answers only the device attributes question behind it, which is what tells
// us the answer is in. Under tmux the reply travels back through the passthrough
// while tmux answers the attributes itself, so there the whole window is waited
// out. Nothing back at all is a link too slow to judge on, not a no.
bool term_graphics_ok(Term *t, bool tmux) {
    if (t->fd < 0) return true;

    struct termios saved;
    bool restore = tcgetattr(t->fd, &saved) == 0;
    if (restore) {
        struct termios raw = saved;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(t->fd, TCSAFLUSH, &raw);
    }

    const char *q = tmux ? GFX_Q_TMUX : GFX_Q;
    writeall(t->fd, q, strlen(q));
    writeall(t->fd, "\x1b[c", 3);

    char buf[1024];
    size_t n = 0;
    bool ok = false, da = false;
    double deadline = now_sec() + (tmux ? 0.8 : 0.3);
    while (!ok && (tmux || !da) && now_sec() < deadline && n < sizeof buf) {
        struct pollfd p = {t->fd, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        ssize_t r = read(t->fd, buf + n, sizeof buf - n);
        if (r <= 0) continue;
        n += (size_t)r;
        ok = reply_ok(buf, n);
        da = da || reply_da(buf, n);
    }

    if (restore) tcsetattr(t->fd, TCSAFLUSH, &saved);
    term_log("graphics query: ok=%d attrs=%d, %zu bytes back", ok, da, n);
    if (!ok && da) writeall(t->fd, "\r\x1b[2K", 5);   // wipe anything that echoed
    return ok || !da;
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
    writeall(t->fd, inline_mode ? "\x1b[?25l" : ENTER_UI,
             inline_mode ? 6 : strlen(ENTER_UI));
    writeall(t->fd, MOUSE_ON, strlen(MOUSE_ON));
    writeall(t->fd, KBD_ON, strlen(KBD_ON));
    term_size(t);
}

void term_hover(Term *t, bool on) {
    if (t->fd < 0) return;
    const char *s = on ? HOVER_ON : HOVER_OFF;
    writeall(t->fd, s, strlen(s));
}

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

        // CSI row ; col R
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
    buf_add(&t->in, buf, n);       // no answer
    return 0;
}

void term_reserve_inline(Term *t, int rows) {
    if (rows > t->rows) rows = t->rows;
    if (rows < 2) rows = 2;
    Buf b = {0};
    for (int i = 0; i < rows; i++) buf_add(&b, "\r\n", 2);
    writeall(t->fd, b.p, b.len);
    buf_free(&b);

    char up[32];
    int n = snprintf(up, sizeof up, "\x1b[%dA", rows);
    writeall(t->fd, up, (size_t)n);

    int row = query_cursor_row(t);
    t->inline_origin = row > 0 ? row : t->rows - rows + 1;
    if (t->inline_origin < 1) t->inline_origin = 1;
    t->inline_rows = rows;
}

void term_clear_inline(Term *t) {
    if (!t->inline_mode || t->inline_rows < 1) return;
    char buf[32];
    for (int i = 0; i < t->inline_rows; i++) {
        if (t->inline_origin + i > t->rows) break;
        int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[2K", t->inline_origin + i);
        writeall(t->fd, buf, (size_t)n);
    }
}

void term_clear_below(Term *t) {
    if (!t->inline_mode || t->inline_rows < 1) return;
    int row = t->inline_origin + t->inline_rows;
    if (row > t->rows) return;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[J", row);
    writeall(t->fd, buf, (size_t)n);
}

// caller must drop the image first
void term_resize_inline(Term *t, int rows) {
    if (rows < 2) rows = 2;
    if (rows > t->rows) rows = t->rows;
    if (t->inline_rows < 1) { term_reserve_inline(t, rows); return; }
    if (rows == t->inline_rows) return;

    term_clear_inline(t);
    char buf[32];

    if (rows < t->inline_rows) {
        t->inline_rows = rows;
        return;
    }

    int bottom = t->inline_origin + t->inline_rows - 1;
    int n = snprintf(buf, sizeof buf, "\x1b[%d;1H", bottom);
    writeall(t->fd, buf, (size_t)n);
    int extra = rows - t->inline_rows;
    for (int i = 0; i < extra; i++) writeall(t->fd, "\r\n", 2);

    int overflow = bottom + extra - t->rows;
    if (overflow > 0) t->inline_origin -= overflow;
    if (t->inline_origin < 1) t->inline_origin = 1;
    t->inline_rows = rows;
}

// signal-handler safe
void term_panic(int fd) {
    if (fd < 0) return;
    (void)!write(fd, KBD_OFF, sizeof KBD_OFF - 1);
    (void)!write(fd, MOUSE_OFF, sizeof MOUSE_OFF - 1);
}

void term_restore(Term *t, bool clear_inline) {
    if (t->fd < 0) return;
    writeall(t->fd, KBD_OFF, strlen(KBD_OFF));
    writeall(t->fd, MOUSE_OFF, strlen(MOUSE_OFF));
    if (t->inline_mode && clear_inline) {
        char buf[32];
        for (int i = 0; i < t->inline_rows; i++) {
            int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[2K",
                             t->inline_origin + i);
            writeall(t->fd, buf, (size_t)n);
        }
        int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[?25h", t->inline_origin);
        writeall(t->fd, buf, (size_t)n);
    } else if (t->inline_mode) {
        char buf[48];
        int below = t->inline_origin + t->inline_rows;
        int n = below > t->rows
            ? snprintf(buf, sizeof buf, "\x1b[%d;1H\r\n\x1b[?25h", t->rows)
            : snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[?25h", below);
        writeall(t->fd, buf, (size_t)n);
    } else {
        writeall(t->fd, LEAVE_UI, strlen(LEAVE_UI));
    }
    if (g_have_saved) tcsetattr(t->fd, TCSAFLUSH, &g_saved);
    buf_free(&t->paste);
    buf_free(&t->in);
}

#define TRACE_PATH "/tmp/web_input.log"

static FILE *g_log;
static int   g_log_env_checked;

bool term_tracing(void) { return g_log != NULL; }

bool term_trace(int on) {
    if (on < 0) on = g_log ? 0 : 1;
    if (on && !g_log) {
        g_log_env_checked = 1;
        g_log = fopen(TRACE_PATH, "a");
    } else if (!on && g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    return g_log != NULL;
}

void term_log(const char *fmt, ...) {
    if (!g_log_env_checked) {
        g_log_env_checked = 1;
        if (getenv("WEB_DEBUG")) g_log = fopen(TRACE_PATH, "w");
    }
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
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

// returns 1 when an event was produced, 0 when more bytes are needed
int term_next(Term *t, Event *ev) {
    memset(ev, 0, sizeof *ev);
    const unsigned char *b = (const unsigned char *)t->in.p;
    size_t n = t->in.len;
    if (n == 0) return 0;

    if (b[0] != 0x1b) {
        unsigned char c = b[0];
        ev->type = EV_KEY;
        // 0x0d enter, 0x0a shift+enter
        if (c == 0x0d || c == 0x0a) {
            ev->key = KEY_ENTER;
            if (c == 0x0a) ev->mods |= MOD_SHIFT;
            buf_consume(&t->in, 1);
            return 1;
        }
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
        ev->key = c;
        buf_consume(&t->in, clen);
        return 1;
    }

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
                // buttons 4-7 are the wheel, the last two horizontal
                ev->button = 3 + (code & 3);
            } else if ((code & 3) == 3) {
                // move with nothing held
                ev->button = BTN_NONE;
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
        buf_add(&t->paste, "", 1);            // NUL-terminate
        t->paste.len--;
        buf_consume(&t->in, seqlen + body + sizeof END - 1);
        ev->type = EV_PASTE;
        return 1;
    }

    // kitty CSI u
    if (final == 'u') {
        int m = mods_from_param(p2);
        ev->type = EV_KEY;
        switch (p1) {
        case 27:  ev->key = KEY_ESC; break;
        case 13:  ev->key = KEY_ENTER; break;
        case 9:   ev->key = KEY_TAB; break;
        case 127: ev->key = KEY_BACKSPACE; break;
        default:
            if (p1 <= 0 || p1 >= 0x110000) { ev->key = KEY_NONE; break; }
            ev->key = p1;
            // kitty functional keys sit in the private use area; 57414 is keypad enter
            if (p1 >= 57344 && p1 <= 63743) {
                ev->key = p1 == 57414 ? KEY_ENTER : KEY_NONE;
                if (ev->key == KEY_NONE) break;
            }
            else if ((m & MOD_SHIFT) && p1 >= 'a' && p1 <= 'z') {
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

    // focus in (CSI I) and out (CSI O), no parameters
    if (seqlen == 3 && (final == 'I' || final == 'O')) {
        ev->type = EV_FOCUS;
        ev->press = final == 'I';
        buf_consume(&t->in, seqlen);
        return 1;
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

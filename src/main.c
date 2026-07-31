#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "web.h"

volatile sig_atomic_t g_resized = 0;
volatile sig_atomic_t g_quit = 0;

static void on_winch(int sig) { (void)sig; g_resized = 1; }
static void on_term(int sig)  { (void)sig; g_quit = 1; }

enum { PEND_NONE, PEND_MOVE };

// One pointer move held back so the next one replaces it: a drag that arrives
// while a frame is being written then costs one dispatch and one frame instead
// of one of each per step.
typedef struct {
    int         kind;
    int         x, y;
    int         mods;
    int         buttons;
    const char *btn;
} Pending;

typedef struct {
    Term    term;
    Kitty   kitty;
    Chrome  chrome;
    Pending pend;

    int     css_w, css_h;      // page viewport in CSS pixels
    int     scale;             // device pixel ratio actually in use
    int     want_scale;        // what was asked for on the command line
    int     img_rows;          // cell rows the page occupies

    char    url[1024];
    char    title[256];
    bool    loading;
    int     title_req;

    bool    editing;           // URL bar has focus
    char    edit[1024];
    size_t  edit_len;

    uint64_t last_hash;
    unsigned frames, skipped;
    double  last_draw;
    double  last_metrics_fix;  // when the viewport override was last restored
    double  expect_frame;      // something was done that should redraw; deadline
    double  last_unwedge;      // when the screencast was last restarted for it

    bool    show_stats;        // ^G
    double  zoom;              // page magnification, alt+= / alt+-
    int     copy_req;          // outstanding "give me the selection" call

    bool    inline_mode;       // a block in the shell's flow, like rom
    int     want_rows;         // rows for that block, 0 = pick one
    int     status_row;
    bool    hide_status;       // the status line is not wanted, ^S
    bool    status_open;       // whether it is on screen right now

    bool    insert;            // a text field has focus: keys belong to the page
    bool    pending_g;         // first half of gg
    int     prompt;            // 0 none, 1 address, 2 find
    char    find[256];         // last search, for n and N

    int     box_rows;          // inline: cell rows the window occupies
    int     width_idx;         // cursor into WIDTHS, 0 = derived from the cells
    bool    scale_locked;      // the render scale was chosen by hand

    bool    fit_width;         // widen the viewport so no page is cut off
    int     fit_req;
    int     fit_w;             // width the page says it needs
    size_t  last_bytes;        // base64 size of the frame just drawn
    double  last_write_ms;     // how long it took to reach the terminal
    double  fps;               // smoothed, so the number is readable
    double  bytes_per_sec;

    char    msg[256];
    double  msg_until;

    char    ua[512];           // chrome's own user agent, headless marker gone
    bool    ua_patch_req;      // this browser predates the flag and needs fixing
    bool    mute;              // start chrome with its audio switched off
    bool    clear_exit;        // erase the inline block on the way out
    bool    keep;              // leave chrome running so the next start adopts it
    Buf     status, status_last;
} App;

static App *g_app;

// ----------------------------------------------------------- cdp dispatch

static void flush_pending(App *a) {
    Pending *p = &a->pend;
    int kind = p->kind;
    p->kind = PEND_NONE;
    if (kind == PEND_MOVE) {
        cdp_call(&a->chrome, "Input.dispatchMouseEvent",
                 "\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d,\"button\":\"%s\","
                 "\"buttons\":%d,\"modifiers\":%d",
                 p->x, p->y, p->btn, p->buttons, p->mods);
    }
}

// Everything that is not a coalesced pointer event goes through here, so a held
// back scroll or move always reaches the page ahead of whatever follows it.
static int app_cdp(App *a, const char *method, const char *fmt, ...) {
    flush_pending(a);
    va_list ap;
    va_start(ap, fmt);
    int id = cdp_vcall(&a->chrome, method, fmt, ap);
    va_end(ap);
    return id;
}

static void queue_move(App *a, int x, int y, const char *btn, int buttons,
                       int mods) {
    Pending *p = &a->pend;
    if (p->kind != PEND_MOVE) flush_pending(a);
    p->kind = PEND_MOVE;
    p->x = x;
    p->y = y;
    p->btn = btn;
    p->buttons = buttons;
    p->mods = mods;
}

// Runs while a frame is being written. It only looks for the keys that must
// work at any moment; everything else waits for the normal input pass, which
// keeps this free of the reentrancy that handling input mid-draw would bring.
static void pump_input(void) {
    if (!g_app) return;
    struct pollfd p = {g_app->term.fd, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    term_read(&g_app->term);
    for (size_t i = 0; i < g_app->term.in.len; i++) {
        unsigned char c = (unsigned char)g_app->term.in.p[i];
        if (c == 0x11 || c == 0x03) { term_log("QUIT via pump byte %02x", c); g_quit = 1; return; }
    }
}

static uint64_t fnv1a(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// macOS has pbcopy; the Linux tools are checked in turn so the same build
// works there without a configuration switch.
static void clipboard_put(const char *text) {
    static const char *cmds[] = {"pbcopy", "wl-copy", "xclip -selection clipboard",
                                 "xsel --clipboard --input", NULL};
    for (int i = 0; cmds[i]; i++) {
        char probe[128];
        snprintf(probe, sizeof probe, "command -v %s >/dev/null 2>&1",
                 cmds[i][0] == 'p' ? "pbcopy" :
                 cmds[i][0] == 'w' ? "wl-copy" :
                 cmds[i][0] == 'x' && cmds[i][1] == 'c' ? "xclip" : "xsel");
        if (system(probe) != 0) continue;
        FILE *p = popen(cmds[i], "w");
        if (!p) continue;
        fwrite(text, 1, strlen(text), p);
        pclose(p);
        return;
    }
}

static void relayout(App *a);

static void notify(App *a, const char *s) {
    snprintf(a->msg, sizeof a->msg, "%s", s);
    a->msg_until = now_sec() + 2.0;
}

// ------------------------------------------------------------------ layout

// Inline mode is a window sitting in the shell's flow, so it has a shape of its
// own rather than the terminal's. Cells are taller than they are wide, so the
// proportion has to be struck in pixels or the box comes out square.
#define BOX_ASPECT (16.0 / 10.0)

static int box_cols_for(App *a, int rows) {
    Term *t = &a->term;
    int want = (int)((double)(rows * t->cell_h) * BOX_ASPECT / t->cell_w + 0.5);
    if (want > t->cols) want = t->cols;
    if (want < 10) want = 10;
    return want;
}

static void relayout(App *a) {
    Term *t = &a->term;
    term_size(t);

    int status = a->status_open ? 1 : 0;
    int rect_cols;
    if (a->inline_mode) {
        // The terminal may have shrunk under the box since it was last sized.
        int max_rows = t->rows - status;
        if (a->box_rows > max_rows) a->box_rows = max_rows;
        if (a->box_rows < 2) a->box_rows = 2;
        a->img_rows = a->box_rows;
        rect_cols = box_cols_for(a, a->img_rows);
        // Keep the whole block, status line included, on the screen. A block
        // whose last row falls past the bottom scrolls the terminal as it is
        // drawn, which lands half the picture at the top and half at the
        // bottom - and the halves never line back up.
        if (t->inline_origin + a->img_rows + status - 1 > t->rows)
            t->inline_origin = t->rows - a->img_rows - status + 1;
        if (t->inline_origin < 1) t->inline_origin = 1;
        kitty_set_rect(&a->kitty, 1, t->inline_origin, rect_cols, a->img_rows);
        a->status_row = t->inline_origin + a->img_rows;
    } else {
        a->img_rows = t->rows - status;
        if (a->img_rows < 1) a->img_rows = 1;
        rect_cols = t->cols;
        kitty_set_rect(&a->kitty, 1, 1, rect_cols, a->img_rows);
        a->status_row = t->rows;
    }

    // The pixels the picture will actually occupy. Everything below works back
    // from these: whatever the terminal is handed gets resampled to this size,
    // and a resample by any fraction is what shaves the bottom off a line of
    // text, so the goal is to be handed exactly this and never resample at all.
    int rect_w = rect_cols * t->cell_w;
    int rect_h = a->img_rows * t->cell_h;
    if (rect_w < 1) rect_w = 1;
    if (rect_h < 1) rect_h = 1;

    double z = a->zoom > 0 ? a->zoom : 1.0;

    int w = (int)(rect_w / z);
    // A page with a minimum layout width would otherwise hang off the side.
    // Widening the viewport to what it asks for keeps all of it in view; the
    // text ends up smaller, which is the honest trade.
    if (a->fit_width && a->fit_w > w) w = a->fit_w;
    // Low enough that an inline window has to be truly tiny before the number
    // it reports stops tracking its actual size, which is the one thing
    // resizing the box is for.
    if (w < 120) w = 120;

    a->css_w = w;
    // Derived from the width and never clamped on its own: a floor applied to
    // one axis alone changes the viewport's shape, and the whole point of the
    // size below is that the shape already matches the cells it lands on.
    a->css_h = (int)((double)w * rect_h / rect_w + 0.5);
    if (a->css_h < 1) a->css_h = 1;

    // Every frame crosses the terminal as base64, so the pixel count sets the
    // cost of everything: Chrome's encode, the write, and how long a keypress
    // waits behind it. A 2x ratio quadruples that, so cap it on wide panes.
    a->scale = a->want_scale;
    if (!a->scale_locked)
        while (a->scale > 1 && (long)rect_w * a->scale > 1920) a->scale--;

    // The ratio that turns the viewport into those pixels. Zoom and fit-width
    // both make it fractional, and it is Chrome's job rather than the
    // terminal's: asked for the final size it lays the text out at that size
    // and hints it there, where the terminal could only stretch a bitmap that
    // was already wrong.
    double dsf = (double)rect_w * a->scale / (double)a->css_w;

    a->status_last.len = 0;    // the status line may have moved rows

    // Both calls go out every time, including when the numbers have not moved.
    // Restarting the screencast is what makes Chrome hand over a fresh frame,
    // so this is the only way anything asking for a redraw gets one.
    app_cdp(a, "Emulation.setDeviceMetricsOverride",
             "\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%.6f,\"mobile\":false",
             a->css_w, a->css_h, dsf);
    app_cdp(a, "Page.startScreencast",
             "\"format\":\"png\",\"maxWidth\":%d,\"maxHeight\":%d,\"everyNthFrame\":1",
             rect_w * a->scale, rect_h * a->scale);
}

// ------------------------------------------------------------------ status

// The address bar and the find prompt are drawn on the status line, so a line
// that has been hidden comes back for as long as one of them is open.
static void status_sync(App *a) {
    bool want = !a->hide_status || a->editing;
    if (want == a->status_open) return;
    a->status_open = want;
    a->status_last.len = 0;
    kitty_clear(&a->kitty);          // the row it lived on changes hands
    // Inline, the page itself is not resized by this, so the frame that comes
    // back is the one already on screen - and a duplicate is normally dropped,
    // which would leave the block empty for as long as the page sits still.
    a->last_hash = 0;
    if (a->inline_mode)
        term_resize_inline(&a->term, a->box_rows + (want ? 1 : 0));
    else
        writeall(a->term.fd, "\x1b[2J", 4);
    relayout(a);
}

// Called after every input batch and every frame, so it keeps its buffer and
// stays quiet when the line has not changed: an unnecessary repaint here lands
// in the middle of a stream of image data.
static void draw_status(App *a) {
    status_sync(a);
    if (!a->status_open) return;
    Term *t = &a->term;
    Buf b = a->status;
    b.len = 0;
    int row = a->status_row > 0 ? a->status_row : t->rows;

    // The line belongs to the window above it, not to the terminal: inline, the
    // box is narrower than the screen, and a status bar running past its edge
    // reads as part of the shell rather than part of the page.
    int sx = a->kitty.x > 0 ? a->kitty.x : 1;
    int sw = a->kitty.cols > 0 ? a->kitty.cols : t->cols;
    if (sx + sw - 1 > t->cols) sw = t->cols - sx + 1;
    if (sw < 8) sw = 8;

    buf_addf(&b, "\x1b[%d;1H\x1b[2K\x1b[%d;%dH", row, row, sx);

    if (a->editing) {
        buf_addf(&b, "\x1b[7m %s \x1b[0m %.*s\x1b[?25h",
                 a->prompt == 2 ? "find" : "go", (int)a->edit_len, a->edit);
        // Park the cursor after the text being typed.
        buf_addf(&b, "\x1b[%d;%dH", row, sx + (int)a->edit_len + 5);
    } else {
        const char *left = a->title[0] ? a->title : a->url;
        char hint[64];
        if (a->show_stats)
            snprintf(hint, sizeof hint, "%zuKB %.0fms %.1ffps  %dx%d@%dx z%.0f%%",
                     a->last_bytes / 1024, a->last_write_ms, a->fps,
                     a->css_w, a->css_h, a->scale,
                     100.0 * (sw * a->term.cell_w) / (a->css_w ? a->css_w : 1));
        else
            snprintf(hint, sizeof hint, "^L url  ^O back  ^R reload  ^Q quit");
        int hintlen = (int)strlen(hint);
        // A narrow window drops the hint rather than shrinking the address to
        // nothing; when it goes, its room goes to the address.
        bool show_hint = sw > hintlen + 4;
        int avail = show_hint ? sw - hintlen - 3 : sw - 2;
        if (avail < 8) avail = 8;

        if (a->insert)
            buf_addf(&b, "\x1b[1;33m INSERT\x1b[0m ");
        if (a->msg_until > now_sec())
            buf_addf(&b, " \x1b[1m%.*s\x1b[0m", avail, a->msg);
        else
            buf_addf(&b, " %s%.*s\x1b[0m", a->loading ? "\x1b[33m" : "\x1b[2m",
                     avail, left);

        if (show_hint)
            buf_addf(&b, "\x1b[%d;%dH\x1b[2m%s\x1b[0m", row,
                     sx + sw - hintlen - 1, hint);
        buf_add(&b, "\x1b[?25l", 6);
    }

    a->status = b;
    if (b.len == a->status_last.len &&
        (b.len == 0 || memcmp(b.p, a->status_last.p, b.len) == 0))
        return;

    writeall(t->fd, b.p, b.len);
    a->status_last.len = 0;
    buf_add(&a->status_last, b.p, b.len);
}

// ------------------------------------------------------------------ input

static void send_char(App *a, const char *text) {
    char esc[64];
    json_escape(esc, sizeof esc, text);
    app_cdp(a, "Input.dispatchKeyEvent",
             "\"type\":\"char\",\"text\":\"%s\"", esc);
}

static void send_key(App *a, int vk, const char *key, const char *code,
                     const char *text, int mods) {
    int cdp_mods = 0;
    if (mods & MOD_ALT)   cdp_mods |= 1;
    if (mods & MOD_CTRL)  cdp_mods |= 2;
    if (mods & MOD_SHIFT) cdp_mods |= 8;

    app_cdp(a, "Input.dispatchKeyEvent",
             "\"type\":\"%s\",\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d,"
             "\"key\":\"%s\",\"code\":\"%s\",\"modifiers\":%d%s%s%s",
             text ? "keyDown" : "rawKeyDown", vk, vk, key, code, cdp_mods,
             text ? ",\"text\":\"" : "", text ? text : "", text ? "\"" : "");
    app_cdp(a, "Input.dispatchKeyEvent",
             "\"type\":\"keyUp\",\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d,"
             "\"key\":\"%s\",\"code\":\"%s\",\"modifiers\":%d",
             vk, vk, key, code, cdp_mods);
}

static bool special_key(App *a, int key, int mods) {
    switch (key) {
    case KEY_ENTER:     send_key(a, 13, "Enter", "Enter", "\\r", mods); return true;
    case KEY_TAB:       send_key(a, 9, "Tab", "Tab", NULL, mods); return true;
    case KEY_BACKSPACE: send_key(a, 8, "Backspace", "Backspace", NULL, mods); return true;
    case KEY_DELETE:    send_key(a, 46, "Delete", "Delete", NULL, mods); return true;
    case KEY_ESC:       send_key(a, 27, "Escape", "Escape", NULL, mods); return true;
    case KEY_UP:        send_key(a, 38, "ArrowUp", "ArrowUp", NULL, mods); return true;
    case KEY_DOWN:      send_key(a, 40, "ArrowDown", "ArrowDown", NULL, mods); return true;
    case KEY_LEFT:      send_key(a, 37, "ArrowLeft", "ArrowLeft", NULL, mods); return true;
    case KEY_RIGHT:     send_key(a, 39, "ArrowRight", "ArrowRight", NULL, mods); return true;
    case KEY_HOME:      send_key(a, 36, "Home", "Home", NULL, mods); return true;
    case KEY_END:       send_key(a, 35, "End", "End", NULL, mods); return true;
    case KEY_PGUP:      send_key(a, 33, "PageUp", "PageUp", NULL, mods); return true;
    case KEY_PGDN:      send_key(a, 34, "PageDown", "PageDown", NULL, mods); return true;
    default:            return false;
    }
}

// Something that names a file on disk becomes a file:// URL. A name is only
// taken as a path if it resolves to something that exists, so a host that looks
// like one - example.com, or a bare word - is left alone unless there really is
// a file of that name here, in which case the file is what was meant.
static bool file_url(const char *raw, char *out, size_t cap) {
    if (!raw || !*raw) return false;
    if (strstr(raw, "://") || !strncmp(raw, "about:", 6)) return false;

    char path[PATH_MAX];
    if (raw[0] == '~' && (raw[1] == '/' || raw[1] == 0)) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        snprintf(path, sizeof path, "%s%s", home, raw + 1);
    } else {
        snprintf(path, sizeof path, "%s", raw);
    }

    // Also the existence test: there is nothing to resolve a path against
    // unless every part of it is really there.
    char real[PATH_MAX];
    if (!realpath(path, real)) return false;

    size_t o = 0;
    o += (size_t)snprintf(out, cap, "file://");
    for (const unsigned char *s = (const unsigned char *)real; *s; s++) {
        if (o + 4 >= cap) return false;
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
            (*s >= '0' && *s <= '9') || strchr("-_.~/", *s))
            out[o++] = (char)*s;
        else
            o += (size_t)snprintf(out + o, cap - o, "%%%02X", *s);
    }
    out[o] = 0;
    return true;
}

static void navigate(App *a, const char *raw) {
    char url[1100];
    if (strstr(raw, "://") || strncmp(raw, "about:", 6) == 0) {
        snprintf(url, sizeof url, "%s", raw);
    } else if (!file_url(raw, url, sizeof url)) {
        if (strchr(raw, ' ') || !strchr(raw, '.')) {
            char q[1024];
            size_t o = 0;
            for (const unsigned char *s = (const unsigned char *)raw;
                 *s && o < sizeof q - 4; s++) {
                if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                    (*s >= '0' && *s <= '9') || strchr("-_.~", *s)) {
                    q[o++] = (char)*s;
                } else {
                    o += (size_t)snprintf(q + o, sizeof q - o, "%%%02X", *s);
                }
            }
            q[o] = 0;
            snprintf(url, sizeof url, "https://duckduckgo.com/?q=%s", q);
        } else {
            snprintf(url, sizeof url, "https://%s", raw);
        }
    }

    char esc[2200];
    json_escape(esc, sizeof esc, url);
    app_cdp(a, "Page.navigate", "\"url\":\"%s\"", esc);
    a->loading = true;
}

// Ask the page whether it actually fits the viewport it was just given.
// scrollWidth never reports less than the viewport, so a reply wider than the
// viewport means real horizontal overflow, and nothing else does.
static void request_fit(App *a) {
    if (!a->fit_width) return;
    a->fit_req = app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"Math.max(document.documentElement.scrollWidth,"
        "document.body?document.body.scrollWidth:0)\",\"returnByValue\":true");
}

// What is worth outliving the process: the zoom and the height of the inline
// window, which belong to the terminal they are being read in rather than to
// any page, and the user agent, which has to be known before Chrome starts and
// can only be learned from a Chrome already running. All of it is keyed to
// nothing - one browser, one terminal, one file.
static void state_path(char *out, size_t cap) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (cfg && *cfg) snprintf(out, cap, "%s/web", cfg);
    else             snprintf(out, cap, "%s/.config/web", home ? home : "/tmp");
}

static void load_state(App *a) {
    char dir[512], path[600];
    state_path(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/state", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "zoom=", 5)) {
            double z = atof(line + 5);
            if (z >= 0.4 && z <= 4.0) a->zoom = z;
        } else if (!strncmp(line, "rows=", 5)) {
            int r = atoi(line + 5);
            if (r >= 2 && r <= 500) a->want_rows = r;
        } else if (!strncmp(line, "ua=", 3) && line[3]) {
            snprintf(a->ua, sizeof a->ua, "%s", line + 3);
        }
    }
    fclose(f);
}

static void save_state(App *a) {
    char dir[512], path[600];
    state_path(dir, sizeof dir);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/state", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "zoom=%.4f\n", a->zoom);
    // --full has no window of its own, so it carries whatever was stored for
    // the inline one through rather than dropping it.
    int rows = a->box_rows > 0 ? a->box_rows : a->want_rows;
    if (rows > 0) fprintf(f, "rows=%d\n", rows);
    if (a->ua[0]) fprintf(f, "ua=%s\n", a->ua);
    fclose(f);
}

// The width the page is told it has. It is the same knob as zoom seen from the
// other end - the viewport is the cell rect divided by the zoom - but a page
// that breaks at 1024 is easier to ask for by name than by percentage.
static const int WIDTHS[] = {800, 1024, 1280, 1440, 1600, 1920};

static void cycle_width(App *a, int step) {
    int n = (int)(sizeof WIDTHS / sizeof *WIDTHS);
    a->width_idx += step;
    if (a->width_idx < 0) a->width_idx = n;
    if (a->width_idx > n) a->width_idx = 0;

    char m[80];
    if (a->width_idx == 0) {
        a->zoom = 1.0;              // back to whatever the cell rect gives
        snprintf(m, sizeof m, "width auto");
    } else {
        int want = WIDTHS[a->width_idx - 1];
        int rect_w = a->term.cols * a->term.cell_w;
        a->zoom = (double)rect_w / want;
        if (a->zoom < 0.4) a->zoom = 0.4;
        if (a->zoom > 4.0) a->zoom = 4.0;
        snprintf(m, sizeof m, "width %dpx (zoom %.0f%%)", want, a->zoom * 100);
    }
    a->fit_w = 0;
    relayout(a);
    save_state(a);
    notify(a, m);
    request_fit(a);
}

// Resize the inline window. The page is told about the new size the same way it
// would be told about a dragged window corner: the box sets the cell rect, the
// cell rect sets the viewport, and the layout follows from there.
static void resize_box(App *a, int delta) {
    int status = a->status_open ? 1 : 0;
    int want = a->box_rows + delta;
    if (want < 2) want = 2;
    if (want > a->term.rows - status) want = a->term.rows - status;
    if (want == a->box_rows) return;

    a->box_rows = want;
    kitty_clear(&a->kitty);            // the rows underneath are about to move
    term_resize_inline(&a->term, want + status);   // the status line sits below
    a->status_last.len = 0;
    relayout(a);
    save_state(a);

    char m[64];
    snprintf(m, sizeof m, "window %dx%d", a->css_w, a->css_h);
    notify(a, m);
    request_fit(a);
}

// How many pixels Chrome renders per pixel the terminal will show. Above 1 it
// is supersampling: the page is drawn larger and comes down to the cell rect,
// which is the only way to get detail past what the cells can hold. It costs
// the square of itself in bytes across the terminal, so it is a choice.
static void cycle_scale(App *a) {
    a->want_scale = a->want_scale >= 3 ? 1 : a->want_scale + 1;
    a->scale_locked = true;         // an explicit ask outranks the width cap
    relayout(a);
    char m[48];
    snprintf(m, sizeof m, "render %dx", a->want_scale);
    notify(a, m);
}

// Zoom is a request, not a command: the viewport narrows to magnify, and a page
// that cannot reflow that narrow gets widened back until it fits. Zooming into
// a wide layout would otherwise just push half of it off the screen.
static void zoom_by(App *a, double factor) {
    double before = a->zoom;
    a->zoom *= factor;
    if (a->zoom < 0.4) a->zoom = 0.4;
    if (a->zoom > 4.0) a->zoom = 4.0;
    if (a->zoom == before) return;

    a->fit_w = 0;                 // re-measure from the width just asked for
    relayout(a);
    // Only what was asked for is remembered. The fit pass below can lower
    // a->zoom to whatever a stubbornly wide page allows, and saving that would
    // let one such page quietly become the setting for every later run.
    save_state(a);

    char m[64];
    snprintf(m, sizeof m, "zoom %.0f%%", a->zoom * 100);
    notify(a, m);
    request_fit(a);
}

static void run_js(App *a, const char *js) {
    char esc[2048];
    json_escape(esc, sizeof esc, js);
    app_cdp(a, "Runtime.evaluate", "\"expression\":\"%s\"", esc);
}

// One jump, landed on immediately. The scroller under the point is the one that
// moves, so panes and inner scrollers behave the way they look; the target is
// clamped to the ends first, so a step at the top or bottom of a page simply
// does nothing instead of leaving something behind to unwind.
static void scroll_at(App *a, int x, int y, int dy) {
    char js[768];
    snprintf(js, sizeof js,
             "(function(x,y,d){"
             "var e=document.elementFromPoint(x,y);"
             "while(e){var o=getComputedStyle(e).overflowY;"
             "if((o==='auto'||o==='scroll')&&e.scrollHeight>e.clientHeight+1)break;"
             "e=e.parentElement;}"
             "var t=e||document.scrollingElement||document.documentElement;"
             "var m=t.scrollHeight-t.clientHeight,v=t.scrollTop+d;"
             "if(v<0)v=0;if(v>m)v=m;"
             // 'instant' and not the scrollTop setter: the setter obeys a page
             // that asks for smooth scrolling in CSS, and every step of that
             // animation is another full-page PNG across the terminal.
             "t.scrollTo({top:v,left:t.scrollLeft,behavior:'instant'});"
             "})(%d,%d,%d)",
             x, y, dy);
    run_js(a, js);
}

static void scroll_by(App *a, int dy) {
    scroll_at(a, a->css_w / 2, a->css_h / 2, dy);
}

// gg and G mean the page, not whatever pane happens to sit under the middle of
// the view: "top" is somewhere you can name, and a step is not.
static void scroll_page_end(App *a, bool bottom) {
    run_js(a, bottom
        ? "(function(t){t.scrollTo({top:t.scrollHeight,behavior:'instant'});})"
          "(document.scrollingElement||document.documentElement)"
        : "(function(t){t.scrollTo({top:0,behavior:'instant'});})"
          "(document.scrollingElement||document.documentElement)");
}

static void nav_history(App *a, int delta) {
    run_js(a, delta < 0 ? "history.back()" : "history.forward()");
}

static void find_next(App *a, bool backwards) {
    if (!a->find[0]) return;
    char js[512], q[256];
    json_escape(q, sizeof q, a->find);      // once for JS, once more for JSON
    snprintf(js, sizeof js,
             "window.find('%s',false,%s,true,false,true,false)",
             q, backwards ? "true" : "false");
    run_js(a, js);
}

// The page tells us when focus lands on something typable, so j and k scroll
// when you are reading and type themselves when you are filling in a form.
static const char FOCUS_WATCHER[] =
    "(function(){"
    "function ed(e){if(!e)return false;var t=e.tagName;"
    "return e.isContentEditable||t==='INPUT'||t==='TEXTAREA'||t==='SELECT';}"
    "function rep(){try{__webmode(ed(document.activeElement)?'1':'0');}catch(e){}}"
    "document.addEventListener('focusin',rep,true);"
    "document.addEventListener('focusout',function(){setTimeout(rep,0);},true);"
    "rep();})()";

static void handle_mouse(App *a, Event *ev) {
    Kitty *k = &a->kitty;
    if (ev->my < k->y || ev->my >= k->y + k->rows) return;

    // Aim at the middle of the cell: the terminal only tells us which cell was
    // clicked, so the center is the least wrong point inside it.
    double fx = (ev->mx - k->x + 0.5) / (double)k->cols;
    double fy = (ev->my - k->y + 0.5) / (double)k->rows;
    int x = (int)(fx * a->css_w);
    int y = (int)(fy * a->css_h);

    int cdp_mods = 0;
    if (ev->mods & MOD_ALT)   cdp_mods |= 1;
    if (ev->mods & MOD_CTRL)  cdp_mods |= 2;
    if (ev->mods & MOD_SHIFT) cdp_mods |= 8;

    if (ev->button >= 3) {
        // Only the vertical notches move the page. A trackpad reports a
        // sideways one for any swipe that is not perfectly straight, and
        // folding those into a vertical step tells most at the ends of a page:
        // the direction being pushed is clamped to nothing, so the drift is
        // all that is left moving and the view lurches off the edge and back.
        if (ev->button == 3 || ev->button == 4) {
            // A notch moves the page down, so a wheel-up is negative.
            int step = a->css_h / 8;
            scroll_at(a, x, y, ev->button == 3 ? -step : step);
        }
        return;
    }

    const char *btn = ev->button == 1 ? "middle" : ev->button == 2 ? "right" : "left";
    if (ev->motion) {
        queue_move(a, x, y, btn, ev->press ? 1 : 0, cdp_mods);
        return;
    }
    app_cdp(a, "Input.dispatchMouseEvent",
             "\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\","
             "\"buttons\":%d,\"clickCount\":1,\"modifiers\":%d",
             ev->press ? "mousePressed" : "mouseReleased", x, y, btn,
             ev->press ? 1 : 0, cdp_mods);
}

// The terminal's own paste key reaches us as text, so it works without the page
// ever seeing a modifier: whatever has the keyboard here gets it.
static void handle_paste(App *a, const char *text, size_t len) {
    if (!len) return;
    if (a->editing) {
        // A pasted newline is a line break in text but an instruction here, so
        // the address bar takes the first line and stops.
        size_t n = strcspn(text, "\r\n");
        if (n > len) n = len;
        if (a->edit_len + n + 1 > sizeof a->edit) n = sizeof a->edit - a->edit_len - 1;
        memcpy(a->edit + a->edit_len, text, n);
        a->edit_len += n;
        return;
    }
    char esc[8192];
    json_escape(esc, sizeof esc, text);
    app_cdp(a, "Input.insertText", "\"text\":\"%s\"", esc);
}

// Ask the page for its selection; the reply decides whether we copy the
// selected text or fall back to the address.
static void copy_selection(App *a) {
    a->copy_req = app_cdp(a, "Runtime.evaluate",
                           "\"expression\":\"window.getSelection().toString()\","
                           "\"returnByValue\":true");
}

static void handle_key(App *a, Event *ev) {
    if (a->editing) {
        if (ev->key == KEY_ENTER) {
            a->edit[a->edit_len] = 0;
            a->editing = false;
            if (a->edit_len) {
                if (a->prompt == 2) {
                    snprintf(a->find, sizeof a->find, "%s", a->edit);
                    find_next(a, false);
                } else {
                    navigate(a, a->edit);
                }
            }
            a->prompt = 0;
            return;
        }
        if (ev->key == KEY_ESC || (ev->mods == MOD_CTRL && ev->key == 'g')) {
            a->editing = false;
            a->prompt = 0;
            return;
        }
        if (ev->key == KEY_BACKSPACE) {
            if (a->edit_len) a->edit_len--;
            return;
        }
        if (ev->mods == MOD_CTRL && ev->key == 'u') { a->edit_len = 0; return; }
        if (ev->text[0] && a->edit_len + 8 < sizeof a->edit) {
            size_t n = strlen(ev->text);
            memcpy(a->edit + a->edit_len, ev->text, n);
            a->edit_len += n;
        }
        return;
    }

    // cmd only reaches us at all where the terminal has been told to let it
    // through, and only for chords it does not claim for itself.
    if (ev->mods == MOD_SUPER) {
        if (ev->key == 'c') { copy_selection(a); return; }
        return;                 // anything else is the terminal's business
    }

    if (ev->mods == MOD_CTRL) {
        switch (ev->key) {
        case 'q': case 'c': term_log("QUIT via ^%c", ev->key); g_quit = 1; return;
        case 'l':
            a->editing = true;
            a->prompt = 1;
            snprintf(a->edit, sizeof a->edit, "%s", a->url);
            a->edit_len = strlen(a->edit);
            return;
        case 'g':
            a->show_stats = !a->show_stats;
            return;
        case 's':
            a->hide_status = !a->hide_status;
            return;
        case 'y': copy_selection(a); return;
        case 'r':
            app_cdp(a, "Page.reload", "\"ignoreCache\":false");
            a->loading = true;
            notify(a, "reloading");
            return;
        case 'o': nav_history(a, -1); return;
        case 'p': nav_history(a, +1); return;
        }
    }

    // With a text field focused the page gets everything: a browser you cannot
    // type "j" into is not a browser.
    if (!a->insert && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        int page = (int)(a->css_h * 0.9);
        int half = a->css_h / 2;

        if (a->pending_g && ev->key != 'g') a->pending_g = false;

        switch (ev->key) {
        // Chrome moves 40 CSS pixels per arrow press; matching it means a page
        // scrolls here at the speed it does in a window.
        case KEY_DOWN:  scroll_by(a, 40);  return;
        case KEY_UP:    scroll_by(a, -40); return;
        case KEY_LEFT:  nav_history(a, -1); return;
        case KEY_RIGHT: nav_history(a, +1); return;
        case 'j': scroll_by(a, 60);    return;
        case 'k': scroll_by(a, -60);   return;
        case 'd': scroll_by(a, half);  return;
        case 'u': scroll_by(a, -half); return;
        case ' ': scroll_by(a, page);  return;
        case 'b': scroll_by(a, -page); return;
        case 'g':
            if (a->pending_g) {
                a->pending_g = false;
                scroll_page_end(a, false);
            } else {
                a->pending_g = true;
            }
            return;
        case 'G':
            scroll_page_end(a, true);
            return;
        // Inline draws a window, so the brackets resize it; taking the whole
        // screen there is no window to resize and they zoom instead. alt+= and
        // alt+- zoom either way.
        case '[':
            if (a->inline_mode) resize_box(a, -1); else zoom_by(a, 1.0 / 1.25);
            return;
        case ']':
            if (a->inline_mode) resize_box(a, +1); else zoom_by(a, 1.25);
            return;
        case 'w': cycle_width(a, +1); return;
        case 'W': cycle_width(a, -1); return;
        case 's': cycle_scale(a);     return;
        case 'n': find_next(a, false); return;
        case 'N': find_next(a, true);  return;
        case '/':
            a->editing = true;
            a->prompt = 2;
            a->edit_len = 0;
            return;
        case 'i':
            a->insert = true;
            notify(a, "insert mode - esc to leave");
            return;
        }
    }

    if (ev->key == KEY_ESC && a->insert) {
        // Drop focus so the page stops claiming the keyboard.
        run_js(a, "document.activeElement&&document.activeElement.blur()");
        a->insert = false;
        return;
    }

    if (ev->mods == MOD_ALT) {
        double before = a->zoom;
        if (ev->key == 'f') {
            a->fit_width = !a->fit_width;
            if (!a->fit_width) a->fit_w = 0;
            notify(a, a->fit_width ? "fit width on" : "fit width off");
            relayout(a);
            return;
        }
        if (ev->key == '=' || ev->key == '+') { zoom_by(a, 1.25); return; }
        if (ev->key == '-' || ev->key == '_') { zoom_by(a, 1.0 / 1.25); return; }
        if (ev->key == '0') {
            a->zoom = 1.0;
            a->fit_w = 0;
            relayout(a);
            save_state(a);
            notify(a, "zoom 100%");
            request_fit(a);
            return;
        }
        (void)before;
    }

    if (special_key(a, ev->key, ev->mods)) return;
    if (ev->text[0] && !(ev->mods & (MOD_CTRL | MOD_ALT))) send_char(a, ev->text);
}

// ------------------------------------------------------------------ events

static void on_cdp_message(App *a, char *msg, size_t len) {
    term_log("cdp<- [%zu] %.140s", len, msg);

    const char *data = strstr(msg, "\"data\":\"");
    if (data && strstr(msg, "Page.screencastFrame")) {
        data += 8;
        const char *end = strchr(data, '"');
        size_t dlen = end ? (size_t)(end - data) : 0;

        double sid = json_num(msg, "sessionId", 0);
        a->expect_frame = 0;      // the page is still answering

        if (dlen) {
            uint64_t h = fnv1a(data, dlen);
            if (h != a->last_hash) {
                a->last_hash = h;
                double t0 = now_sec();
                kitty_draw_png(&a->kitty, data, dlen);
                double t1 = now_sec();

                double gap = a->last_draw > 0 ? t1 - a->last_draw : 0;
                if (gap > 0) {
                    double inst = 1.0 / gap;
                    a->fps = a->fps > 0 ? a->fps * 0.7 + inst * 0.3 : inst;
                    double bps = (double)dlen / gap;
                    a->bytes_per_sec = a->bytes_per_sec > 0
                        ? a->bytes_per_sec * 0.7 + bps * 0.3 : bps;
                }
                a->last_bytes = dlen;
                a->last_write_ms = (t1 - t0) * 1000.0;
                a->frames++;
                a->last_draw = t1;
                term_log("%.3f frame %u: %zu KB b64, write %.1f ms, %.1f fps",
                         t1, a->frames, dlen / 1024, a->last_write_ms, a->fps);
            } else {
                a->skipped++;
            }
        }

        // Ack only once the frame is on screen. Chrome holds the next frame
        // until then, which is the only thing keeping a slow terminal from
        // being buried in frames it cannot draw - and keeps the loop reaching
        // the keyboard between frames.
        app_cdp(a, "Page.screencastFrameAck", "\"sessionId\":%d", (int)sid);

        // Going fullscreen throws the viewport override away and puts the page
        // back on the real screen, so the frames stop matching the cells they
        // are drawn into. Every frame says what size it thinks the device is,
        // which is enough to notice and put it back. Rate limited because the
        // repair restarts the screencast, and a mismatch that survives it would
        // otherwise loop.
        double dw = json_num(msg, "deviceWidth", 0);
        if (dw > 0 && (int)(dw + 0.5) != a->css_w &&
            now_sec() - a->last_metrics_fix > 1.0) {
            a->last_metrics_fix = now_sec();
            term_log("device metrics went to %.0f, wanted %d: reapplying",
                     dw, a->css_w);
            relayout(a);
        }
        return;
    }

    if (strstr(msg, "Page.frameNavigated") && !strstr(msg, "\"parentId\"")) {
        // A new document should always paint something, and the placeholder
        // cells are worth writing again: anything that scrolled or overwrote
        // them leaves the picture stranded where it was.
        a->expect_frame = now_sec() + 2.0;
        a->kitty.grid_dirty = true;
        size_t n;
        const char *u = json_str(msg, "url", &n);
        if (u && n < sizeof a->url) {
            memcpy(a->url, u, n);
            a->url[n] = 0;
            a->title[0] = 0;
            a->fit_w = 0;              // measured per page
        }
        return;
    }

    if (strstr(msg, "Runtime.bindingCalled") && strstr(msg, "__webmode")) {
        size_t n;
        const char *p = json_str(msg, "payload", &n);
        a->insert = (p && n && p[0] == '1');
        return;
    }

    if (strstr(msg, "Page.frameStartedLoading")) { a->loading = true; return; }

    if (strstr(msg, "Page.loadEventFired")) {
        a->loading = false;
        request_fit(a);
        a->title_req = app_cdp(a, "Runtime.evaluate",
                                "\"expression\":\"document.title\",\"returnByValue\":true");
        return;
    }

    if (strstr(msg, "Page.javascriptDialogOpening")) {
        app_cdp(a, "Page.handleJavaScriptDialog", "\"accept\":false");
        return;
    }

    if (a->fit_req && (int)json_num(msg, "id", 0) == a->fit_req) {
        a->fit_req = 0;
        int want = (int)json_num(msg, "value", 0);
        if (want > a->css_w + 8 && want < 8000) {
            a->fit_w = want;
            relayout(a);
            // The zoom that survived is whatever the widened viewport allows,
            // so say so rather than showing a number the page overruled.
            double eff = (double)(a->term.cols * a->term.cell_w) / a->css_w;
            if (eff < a->zoom * 0.97) {
                // Keep the stored zoom honest. Left at the requested value it
                // would climb invisibly, and zooming back out would do nothing
                // until those phantom steps had been unwound.
                a->zoom = eff;
                char m[80];
                snprintf(m, sizeof m, "zoom %.0f%% - page needs %dpx",
                         eff * 100, want);
                notify(a, m);
            }
        }
        // No re-measure here: the reply above already reflects the widened
        // viewport, so one round is always enough and cannot loop.
        return;
    }

    if (a->copy_req && (int)json_num(msg, "id", 0) == a->copy_req) {
        size_t n;
        const char *v = json_str(msg, "value", &n);
        char text[8192];
        size_t len = (v && n) ? json_unescape(text, sizeof text, v, n) : 0;
        if (len) {
            clipboard_put(text);
            char m[64];
            snprintf(m, sizeof m, "copied %zu chars", len);
            notify(a, m);
        } else {
            clipboard_put(a->url);          // nothing selected: take the address
            notify(a, "copied url");
        }
        a->copy_req = 0;
        return;
    }

    if (a->title_req && (int)json_num(msg, "id", 0) == a->title_req) {
        size_t n;
        const char *v = json_str(msg, "value", &n);
        if (v && n && n < sizeof a->title) {
            memcpy(a->title, v, n);
            a->title[n] = 0;
        }
        a->title_req = 0;
    }
}

// -------------------------------------------------------------------- main

static void usage(void) {
    fprintf(stderr,
        "usage: web [options] <url>\n"
        "  --scale N   device pixel ratio (default 1; 2 is sharper but 4x the data)\n"
        "  --show      run Chrome with a visible window too\n"
        "  --zoom F    page magnification (default 1.0)\n"
        "  --full      take over the whole terminal instead of drawing a window\n"
        "  --rows N    how many cell rows the window gets\n"
        "  --no-status start with the status line hidden (^S toggles it)\n"
        "  --clear     erase the window on exit instead of leaving it behind\n"
        "  --mute      start with the page's audio switched off\n"
        "  --login     open a window to sign in with, on the same profile\n"
        "  --keep      leave chrome running on exit so the next start is instant\n");
}

// The size Chrome should open at, close enough that the page lays out once.
// relayout settles the exact numbers as soon as the terminal is fully set up.
static void first_size(App *a, int *w, int *h) {
    int rows = a->inline_mode
        ? (a->want_rows > 0 ? a->want_rows : a->term.rows / 2)
        : a->term.rows - (a->status_open ? 1 : 0);
    if (rows < 1) rows = 1;
    double z = a->zoom > 0 ? a->zoom : 1.0;
    *w = (int)(a->term.cols * a->term.cell_w / z);
    *h = (int)(rows * a->term.cell_h / z);
    if (*w < 200) *w = 200;
    if (*h < 200) *h = 200;
}

int main(int argc, char **argv) {
    App a = {0};
    a.want_scale = 1;
    a.zoom = 1.0;
    load_state(&a);                   // --zoom below still wins over it
    a.fit_width = true;
    a.inline_mode = true;             // a window in the shell, unless --full
    bool show = false, login = false;
    const char *start = "https://duckduckgo.com";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            a.want_scale = atoi(argv[++i]);
            if (a.want_scale < 1) a.want_scale = 1;
            if (a.want_scale > 3) a.want_scale = 3;
        } else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
            a.zoom = atof(argv[++i]);
            if (a.zoom < 0.5) a.zoom = 0.5;
            if (a.zoom > 3.0) a.zoom = 3.0;
        } else if (!strcmp(argv[i], "--inline")) {
            a.inline_mode = true;      // the default; kept so scripts still work
        } else if (!strcmp(argv[i], "--full")) {
            a.inline_mode = false;
        } else if (!strcmp(argv[i], "--rows") && i + 1 < argc) {
            a.want_rows = atoi(argv[++i]);
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--clear")) {
            a.clear_exit = true;
        } else if (!strcmp(argv[i], "--no-status")) {
            a.hide_status = true;
        } else if (!strcmp(argv[i], "--show")) {
            show = true;
        } else if (!strcmp(argv[i], "--keep")) {
            a.keep = true;
        } else if (!strcmp(argv[i], "--mute")) {
            a.mute = true;
        } else if (!strcmp(argv[i], "--login")) {
            login = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            start = argv[i];
        }
    }
    a.status_open = !a.hide_status;

    char first[1200];
    if (strstr(start, "://") || !strncmp(start, "about:", 6))
        snprintf(first, sizeof first, "%s", start);
    else if (!file_url(start, first, sizeof first))
        snprintf(first, sizeof first, "https://%s", start);
    snprintf(a.url, sizeof a.url, "%s", first);

    term_log("%.3f start", now_sec());
    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_winch);
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_term);

    // Measure the terminal before Chrome starts. Launched at some other size it
    // would lay the page out, paint it, and then have to do both again the
    // moment the real viewport arrived.
    term_probe(&a.term);
    int fw, fh;
    first_size(&a, &fw, &fh);

    // A window to sign in with, opened on the same profile and with the same
    // flags every other run uses. The flags are the point: Chrome seals its
    // cookies with a key chosen by the keychain options, so a sign-in done in
    // an ordinary browser leaves a session this one cannot unseal.
    if (login) {
        if (chrome_launch(&a.chrome, first, 1200, 900, true, a.mute, a.ua, false) < 0)
            return 1;
        // Closing the window is not the same as quitting on macOS - the browser
        // stays running with nothing on screen - so waiting for it to exit on
        // its own would wait forever. Waiting on the keyboard instead also
        // gives us the moment to shut it down properly.
        fprintf(stderr, "web: sign in, then press Enter here.\n");
        for (;;) {
            if (a.chrome.pid > 0 &&
                waitpid(a.chrome.pid, NULL, WNOHANG) == a.chrome.pid) {
                a.chrome.pid = 0;         // quit from the menu; nothing to do
                break;
            }
            struct pollfd p = {STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 200) > 0 && (p.revents & POLLIN)) {
                char buf[64];
                (void)!read(STDIN_FILENO, buf, sizeof buf);
                break;
            }
        }
        if (a.chrome.pid > 0) {
            // The browser itself, not its process group: signalled as a group
            // the helpers go down first and the orderly shutdown never happens,
            // which loses the cookie store - and with it the sign-in.
            kill(a.chrome.pid, SIGTERM);
            for (int i = 0; i < 200; i++) {
                if (waitpid(a.chrome.pid, NULL, WNOHANG) == a.chrome.pid) break;
                struct timespec ts = {0, 25 * 1000000};
                nanosleep(&ts, NULL);
            }
        }
        return 0;
    }

    // Started on a blank page rather than straight at the address, so the first
    // request to anywhere real is made after everything below is set up.
    if (chrome_launch(&a.chrome, "about:blank", fw, fh, show, a.mute, a.ua, true) < 0)
        return 1;
    if (chrome_attach(&a.chrome) < 0) { chrome_kill(&a.chrome); return 1; }

    // Ask the browser what it is calling itself and keep the corrected answer
    // for next time. The launch flag above needs the string before there is a
    // browser to ask, so the first run of a new Chrome build is the one that
    // learns it; every run after that starts out right.
    {
        char ua[512];
        int rc = chrome_user_agent(&a.chrome, ua, sizeof ua);
        if (rc >= 0 && strcmp(ua, a.ua) != 0) {
            snprintf(a.ua, sizeof a.ua, "%s", ua);
            save_state(&a);
        }
        a.ua_patch_req = rc == 1;   // applied below, once the session is up
    }

    term_enter(&a.term, a.inline_mode);
    if (a.inline_mode) {
        int status = a.status_open ? 1 : 0;   // the row below the box, if shown
        int rows = a.want_rows > 0 ? a.want_rows + status : a.term.rows / 2;
        // A height remembered from a taller terminal, or asked for on the
        // command line, comes back down to what this one has - and never takes
        // the last row, which the shell gets its prompt back on.
        if (rows > a.term.rows - 1) rows = a.term.rows - 1;
        if (rows < 4) rows = 4;
        term_reserve_inline(&a.term, rows);
        a.box_rows = rows - status;
    }
    g_app = &a;
    g_input_pump = pump_input;
    bool tmux = getenv("TMUX") != NULL;
    kitty_init(&a.kitty, a.term.fd, tmux);

    app_cdp(&a, "Page.enable", "");
    app_cdp(&a, "Runtime.enable", "");

    // A browser we adopted, or the first run against a new Chrome, was started
    // before its own user agent could be read, so it is still saying headless.
    // Overriding it here costs navigator.userAgentData, which the launch flag
    // would have kept - the lesser of the two tells, and only until this
    // browser is replaced by one started the right way.
    if (a.ua_patch_req && a.ua[0]) {
        char esc[1100];
        json_escape(esc, sizeof esc, a.ua);
        app_cdp(&a, "Emulation.setUserAgentOverride", "\"userAgent\":\"%s\"", esc);
    }
    // The overlay scrollbar appears on a scroll, waits half a second, then
    // fades out over a dozen compositor frames - and every one of those is a
    // full-page PNG across the terminal. It costs more than everything else
    // scrolling does, and there is nothing to see: the terminal has no pointer
    // to grab it with.
    app_cdp(&a, "Emulation.setScrollbarsHidden", "\"hidden\":true");
    {
        char esc[2048];
        json_escape(esc, sizeof esc, FOCUS_WATCHER);
        app_cdp(&a, "Runtime.addBinding", "\"name\":\"__webmode\"");
        app_cdp(&a, "Page.addScriptToEvaluateOnNewDocument",
                 "\"source\":\"%s\"", esc);
        app_cdp(&a, "Runtime.evaluate", "\"expression\":\"%s\"", esc);
    }
    navigate(&a, first);       // the blank page above is not where we are going
    relayout(&a);
    draw_status(&a);

    while (!g_quit) {
        if (g_resized) {
            g_resized = 0;
            if (a.inline_mode) {
                term_size(&a.term);
                a.term.inline_origin = a.term.rows - a.term.inline_rows + 1;
            } else {
                writeall(a.term.fd, "\x1b[2J", 4);
                // The clear took the placeholder cells with it, and a resize
                // that lands on the same rect would not otherwise redraw them.
                a.kitty.grid_dirty = true;
            }
            a.status_last.len = 0;      // the screen it was on is gone
            relayout(&a);
        }

        struct pollfd fds[2] = {{0}};   // poll leaves revents alone on EINTR
        fds[0].fd = a.term.fd;
        fds[0].events = POLLIN;
        fds[1].fd = a.chrome.ws.fd;
        fds[1].events = POLLIN;

        // Nothing here is on a timer except an undecided ESC and an expiring
        // notice, so the loop sleeps until something actually happens.
        int wait = (a.term.in.len || a.msg_until > now_sec() ||
                    a.expect_frame > 0) ? 20 : -1;
        int rc = poll(fds, 2, wait);
        if (rc < 0 && !g_resized) continue;

        if (fds[0].revents & POLLIN) {
            term_read(&a.term);
            Event ev;
            while (term_next(&a.term, &ev)) {
                term_log("%.3f event type=%d key=%d mods=%d text=%s", now_sec(),
                         ev.type, ev.key, ev.mods, ev.text[0] ? ev.text : "");
                if (ev.type == EV_KEY) handle_key(&a, &ev);
                else if (ev.type == EV_MOUSE) handle_mouse(&a, &ev);
                else if (ev.type == EV_PASTE)
                    handle_paste(&a, a.term.paste.p, a.term.paste.len);
                if (g_quit) break;
            }
            // A move held back during the batch goes out now.
            flush_pending(&a);
            draw_status(&a);
            // Whatever that was, the page should have something to say about
            // it. If it does not, the watchdog below finds out.
            if (a.expect_frame == 0) a.expect_frame = now_sec() + 2.0;
        }

        // Chrome sends the next frame only once the last one is acknowledged,
        // so anything that breaks that chain stops the picture for good while
        // the rest of the session carries on: the address bar still moves, the
        // title still changes, and nothing is drawn again. Restarting the
        // screencast is the one thing that always brings a frame back.
        if (a.expect_frame > 0 && now_sec() > a.expect_frame &&
            now_sec() - a.last_unwedge > 3.0) {
            a.last_unwedge = now_sec();
            a.expect_frame = 0;
            term_log("no frame after acting on input; restarting screencast");
            a.kitty.grid_dirty = true;
            relayout(&a);
        }

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            if (ws_fill(&a.chrome.ws) < 0) break;
            char *msg;
            size_t len;
            while (ws_next(&a.chrome.ws, &msg, &len) == 1) {
                on_cdp_message(&a, msg, len);
                a.chrome.ws.msg.len = 0;
            }
            draw_status(&a);
        }
        if (a.chrome.ws.closed) break;

        // A lone ESC only resolves on a later pass, once the wait has expired.
        Event ev;
        if (a.term.in.len && term_next(&a.term, &ev)) {
            if (ev.type == EV_KEY) handle_key(&a, &ev);
            else if (ev.type == EV_MOUSE) handle_mouse(&a, &ev);
            else if (ev.type == EV_PASTE)
                handle_paste(&a, a.term.paste.p, a.term.paste.len);
            flush_pending(&a);
            draw_status(&a);
        }
    }

    // Inline leaves the page behind unless it was asked not to.
    if (!a.inline_mode || a.clear_exit) kitty_clear(&a.kitty);
    kitty_free(&a.kitty);
    term_restore(&a.term, a.clear_exit);
    buf_free(&a.status);
    buf_free(&a.status_last);
    // Most of a cold start is Chrome coming up. Left running, it holds the
    // profile and the next run adopts it instead of paying for that again.
    if (a.keep) ws_close(&a.chrome.ws);
    else chrome_kill(&a.chrome);
    // Into the debug log rather than the terminal: quitting should hand the
    // shell back the way it found it.
    term_log("%u frames drawn, %u duplicates skipped", a.frames, a.skipped);
    return 0;
}

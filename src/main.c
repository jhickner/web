#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

volatile sig_atomic_t g_resized = 0;
volatile sig_atomic_t g_quit = 0;

static void on_winch(int sig) { (void)sig; g_resized = 1; }
static void on_term(int sig)  { (void)sig; g_quit = 1; }

typedef struct {
    Term    term;
    Kitty   kitty;
    Chrome  chrome;

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

    bool    show_stats;        // ^G
    double  zoom;              // page magnification, alt+= / alt+-
    int     copy_req;          // outstanding "give me the selection" call

    bool    inline_mode;       // a block in the shell's flow, like rom
    int     want_rows;         // rows for that block, 0 = pick one
    int     status_row;

    bool    insert;            // a text field has focus: keys belong to the page
    bool    pending_g;         // first half of gg
    int     prompt;            // 0 none, 1 address, 2 find
    char    find[256];         // last search, for n and N

    bool    fit_width;         // widen the viewport so no page is cut off
    int     fit_req;
    int     fit_w;             // width the page says it needs
    size_t  last_bytes;        // base64 size of the frame just drawn
    double  last_write_ms;     // how long it took to reach the terminal
    double  fps;               // smoothed, so the number is readable
    double  bytes_per_sec;

    char    msg[256];
    double  msg_until;
} App;

static App *g_app;

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

static void relayout(App *a) {
    Term *t = &a->term;
    term_size(t);

    if (a->inline_mode) {
        a->img_rows = t->inline_rows - 1;
        if (a->img_rows < 1) a->img_rows = 1;
        kitty_set_rect(&a->kitty, 1, t->inline_origin, t->cols, a->img_rows);
        a->status_row = t->inline_origin + a->img_rows;
    } else {
        a->img_rows = t->rows - 1;
        if (a->img_rows < 1) a->img_rows = 1;
        kitty_set_rect(&a->kitty, 1, 1, t->cols, a->img_rows);
        a->status_row = t->rows;
    }

    // The picture is stretched into the cell rect, so the viewport has to keep
    // that same shape or the page comes out distorted.
    double z = a->zoom > 0 ? a->zoom : 1.0;
    double cell_aspect = (double)(a->img_rows * t->cell_h) /
                         (double)(t->cols * t->cell_w);

    int w = (int)(t->cols * t->cell_w / z);
    // A page with a minimum layout width would otherwise hang off the side.
    // Widening the viewport to what it asks for keeps all of it in view; the
    // text ends up smaller, which is the honest trade.
    if (a->fit_width && a->fit_w > w) w = a->fit_w;
    if (w < 200) w = 200;

    a->css_w = w;
    a->css_h = (int)(w * cell_aspect);
    if (a->css_h < 200) a->css_h = 200;

    // Every frame crosses the terminal as base64, so the pixel count sets the
    // cost of everything: Chrome's encode, the write, and how long a keypress
    // waits behind it. A 2x ratio quadruples that, so cap it on wide panes.
    a->scale = a->want_scale;
    while (a->scale > 1 && (long)a->css_w * a->scale > 1920) a->scale--;

    cdp_call(&a->chrome, "Emulation.setDeviceMetricsOverride",
             "\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%d,\"mobile\":false",
             a->css_w, a->css_h, a->scale);
    cdp_call(&a->chrome, "Page.startScreencast",
             "\"format\":\"png\",\"maxWidth\":%d,\"maxHeight\":%d,\"everyNthFrame\":1",
             a->css_w * a->scale, a->css_h * a->scale);
}

// ------------------------------------------------------------------ status

static void draw_status(App *a) {
    Term *t = &a->term;
    Buf b = {0};
    int row = a->status_row > 0 ? a->status_row : t->rows;

    buf_addf(&b, "\x1b[%d;1H\x1b[2K", row);

    if (a->editing) {
        buf_addf(&b, "\x1b[7m %s \x1b[0m %.*s\x1b[?25h",
                 a->prompt == 2 ? "find" : "go", (int)a->edit_len, a->edit);
        // Park the cursor after the text being typed.
        buf_addf(&b, "\x1b[%d;%dH", row, (int)a->edit_len + 6);
    } else {
        const char *left = a->title[0] ? a->title : a->url;
        char hint[64];
        if (a->show_stats)
            snprintf(hint, sizeof hint, "%zuKB %.0fms %.1ffps  %dx%d z%.0f%%",
                     a->last_bytes / 1024, a->last_write_ms, a->fps,
                     a->css_w, a->css_h,
                     100.0 * (a->term.cols * a->term.cell_w) / (a->css_w ? a->css_w : 1));
        else
            snprintf(hint, sizeof hint, "^L url  ^O back  ^R reload  ^Q quit");
        int hintlen = (int)strlen(hint);
        int avail = t->cols - hintlen - 3;
        if (avail < 8) avail = 8;

        if (a->insert)
            buf_addf(&b, "\x1b[1;33m INSERT\x1b[0m ");
        if (a->msg_until > now_sec())
            buf_addf(&b, " \x1b[1m%.*s\x1b[0m", avail, a->msg);
        else
            buf_addf(&b, " %s%.*s\x1b[0m", a->loading ? "\x1b[33m" : "\x1b[2m",
                     avail, left);

        if (t->cols > hintlen + 4)
            buf_addf(&b, "\x1b[%d;%dH\x1b[2m%s\x1b[0m", row,
                     t->cols - hintlen, hint);
        buf_add(&b, "\x1b[?25l", 6);
    }

    writeall(t->fd, b.p, b.len);
    buf_free(&b);
}

// ------------------------------------------------------------------ input

static void send_char(App *a, const char *text) {
    char esc[64];
    json_escape(esc, sizeof esc, text);
    cdp_call(&a->chrome, "Input.dispatchKeyEvent",
             "\"type\":\"char\",\"text\":\"%s\"", esc);
}

static void send_key(App *a, int vk, const char *key, const char *code,
                     const char *text, int mods) {
    int cdp_mods = 0;
    if (mods & MOD_ALT)   cdp_mods |= 1;
    if (mods & MOD_CTRL)  cdp_mods |= 2;
    if (mods & MOD_SHIFT) cdp_mods |= 8;

    cdp_call(&a->chrome, "Input.dispatchKeyEvent",
             "\"type\":\"%s\",\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d,"
             "\"key\":\"%s\",\"code\":\"%s\",\"modifiers\":%d%s%s%s",
             text ? "keyDown" : "rawKeyDown", vk, vk, key, code, cdp_mods,
             text ? ",\"text\":\"" : "", text ? text : "", text ? "\"" : "");
    cdp_call(&a->chrome, "Input.dispatchKeyEvent",
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

static void navigate(App *a, const char *raw) {
    char url[1100];
    if (strstr(raw, "://") || strncmp(raw, "about:", 6) == 0) {
        snprintf(url, sizeof url, "%s", raw);
    } else if (strchr(raw, ' ') || !strchr(raw, '.')) {
        char q[1024];
        size_t o = 0;
        for (const unsigned char *s = (const unsigned char *)raw; *s && o < sizeof q - 4; s++) {
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

    char esc[2200];
    json_escape(esc, sizeof esc, url);
    cdp_call(&a->chrome, "Page.navigate", "\"url\":\"%s\"", esc);
    a->loading = true;
}

// Scroll with a wheel event at the middle of the view rather than
// window.scrollBy: the wheel goes to whatever scrollable thing is under the
// pointer, so panes and inner scrollers behave the way they look.
static void scroll_by(App *a, int dy) {
    cdp_call(&a->chrome, "Input.dispatchMouseEvent",
             "\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,\"deltaX\":0,\"deltaY\":%d",
             a->css_w / 2, a->css_h / 2, dy);
}

// Ask the page whether it actually fits the viewport it was just given.
// scrollWidth never reports less than the viewport, so a reply wider than the
// viewport means real horizontal overflow, and nothing else does.
static void request_fit(App *a) {
    if (!a->fit_width) return;
    a->fit_req = cdp_call(&a->chrome, "Runtime.evaluate",
        "\"expression\":\"Math.max(document.documentElement.scrollWidth,"
        "document.body?document.body.scrollWidth:0)\",\"returnByValue\":true");
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

    char m[64];
    snprintf(m, sizeof m, "zoom %.0f%%", a->zoom * 100);
    notify(a, m);
    request_fit(a);
}

static void run_js(App *a, const char *js) {
    char esc[2048];
    json_escape(esc, sizeof esc, js);
    cdp_call(&a->chrome, "Runtime.evaluate", "\"expression\":\"%s\"", esc);
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

    if (ev->button == 3 || ev->button == 4) {
        // Positive deltaY scrolls the page down, so a wheel-up is negative.
        int dy = ev->button == 3 ? -120 : 120;
        cdp_call(&a->chrome, "Input.dispatchMouseEvent",
                 "\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,\"deltaX\":0,"
                 "\"deltaY\":%d,\"modifiers\":%d", x, y, dy, cdp_mods);
        return;
    }

    const char *btn = ev->button == 1 ? "middle" : ev->button == 2 ? "right" : "left";
    if (ev->motion) {
        cdp_call(&a->chrome, "Input.dispatchMouseEvent",
                 "\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d,\"button\":\"%s\","
                 "\"buttons\":%d,\"modifiers\":%d",
                 x, y, btn, ev->press ? 1 : 0, cdp_mods);
        return;
    }
    cdp_call(&a->chrome, "Input.dispatchMouseEvent",
             "\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\","
             "\"buttons\":%d,\"clickCount\":1,\"modifiers\":%d",
             ev->press ? "mousePressed" : "mouseReleased", x, y, btn,
             ev->press ? 1 : 0, cdp_mods);
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
        case 'y':
            // Ask the page for its selection; the reply decides whether we copy
            // the selected text or fall back to the address.
            a->copy_req = cdp_call(&a->chrome, "Runtime.evaluate",
                                   "\"expression\":\"window.getSelection().toString()\","
                                   "\"returnByValue\":true");
            return;
        case 'r':
            cdp_call(&a->chrome, "Page.reload", "\"ignoreCache\":false");
            a->loading = true;
            notify(a, "reloading");
            return;
        case 'o':
            cdp_call(&a->chrome, "Runtime.evaluate", "\"expression\":\"history.back()\"");
            return;
        case 'p':
            cdp_call(&a->chrome, "Runtime.evaluate", "\"expression\":\"history.forward()\"");
            return;
        }
    }

    // With a text field focused the page gets everything: a browser you cannot
    // type "j" into is not a browser.
    if (!a->insert && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        int page = (int)(a->css_h * 0.9);
        int half = a->css_h / 2;

        if (a->pending_g && ev->key != 'g') a->pending_g = false;

        switch (ev->key) {
        case 'j': scroll_by(a, 60);    return;
        case 'k': scroll_by(a, -60);   return;
        case 'd': scroll_by(a, half);  return;
        case 'u': scroll_by(a, -half); return;
        case ' ': scroll_by(a, page);  return;
        case 'b': scroll_by(a, -page); return;
        case 'g':
            if (a->pending_g) {
                a->pending_g = false;
                run_js(a, "window.scrollTo(0,0)");
            } else {
                a->pending_g = true;
            }
            return;
        case 'G':
            run_js(a, "window.scrollTo(0,document.body.scrollHeight)");
            return;
        case '[': zoom_by(a, 1.0 / 1.25); return;
        case ']': zoom_by(a, 1.25);        return;
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
                term_log("frame %u: %zu KB b64, write %.1f ms, %.1f fps",
                         a->frames, dlen / 1024, a->last_write_ms, a->fps);
            } else {
                a->skipped++;
            }
        }

        // Ack only once the frame is on screen. Chrome holds the next frame
        // until then, which is the only thing keeping a slow terminal from
        // being buried in frames it cannot draw - and keeps the loop reaching
        // the keyboard between frames.
        cdp_call(&a->chrome, "Page.screencastFrameAck", "\"sessionId\":%d", (int)sid);
        return;
    }

    if (strstr(msg, "Page.frameNavigated") && !strstr(msg, "\"parentId\"")) {
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
        a->title_req = cdp_call(&a->chrome, "Runtime.evaluate",
                                "\"expression\":\"document.title\",\"returnByValue\":true");
        return;
    }

    if (strstr(msg, "Page.javascriptDialogOpening")) {
        cdp_call(&a->chrome, "Page.handleJavaScriptDialog", "\"accept\":false");
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
        "  --inline    draw a block in the shell's flow instead of taking over\n"
        "  --rows N    rows for that block (implies --inline)\n");
}

int main(int argc, char **argv) {
    App a = {0};
    a.want_scale = 1;
    a.zoom = 1.0;
    a.fit_width = true;
    bool show = false;
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
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--rows") && i + 1 < argc) {
            a.want_rows = atoi(argv[++i]);
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--show")) {
            show = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            start = argv[i];
        }
    }

    char first[1200];
    if (strstr(start, "://") || !strncmp(start, "about:", 6))
        snprintf(first, sizeof first, "%s", start);
    else
        snprintf(first, sizeof first, "https://%s", start);
    snprintf(a.url, sizeof a.url, "%s", first);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_winch);
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_term);

    fprintf(stderr, "web: starting chrome...\n");
    if (chrome_launch(&a.chrome, first, 1280, 800, show) < 0) return 1;
    if (chrome_attach(&a.chrome) < 0) { chrome_kill(&a.chrome); return 1; }

    term_open(&a.term, a.inline_mode);
    if (a.inline_mode) {
        int rows = a.want_rows > 0 ? a.want_rows + 1 : a.term.rows / 2;
        if (rows > a.term.rows) rows = a.term.rows;
        if (rows < 4) rows = 4;
        term_reserve_inline(&a.term, rows);
    }
    g_app = &a;
    g_input_pump = pump_input;
    bool tmux = getenv("TMUX") != NULL;
    kitty_init(&a.kitty, a.term.fd, tmux);

    cdp_call(&a.chrome, "Page.enable", "");
    cdp_call(&a.chrome, "Runtime.enable", "");
    {
        char esc[2048];
        json_escape(esc, sizeof esc, FOCUS_WATCHER);
        cdp_call(&a.chrome, "Runtime.addBinding", "\"name\":\"__webmode\"");
        cdp_call(&a.chrome, "Page.addScriptToEvaluateOnNewDocument",
                 "\"source\":\"%s\"", esc);
        cdp_call(&a.chrome, "Runtime.evaluate", "\"expression\":\"%s\"", esc);
    }
    if (a.chrome.adopted) navigate(&a, first);
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
            }
            relayout(&a);
        }

        struct pollfd fds[2];
        fds[0].fd = a.term.fd;
        fds[0].events = POLLIN;
        fds[1].fd = a.chrome.ws.fd;
        fds[1].events = POLLIN;

        int rc = poll(fds, 2, 100);
        if (rc < 0 && !g_resized) continue;

        if (fds[0].revents & POLLIN) {
            term_read(&a.term);
            Event ev;
            while (term_next(&a.term, &ev)) {
                term_log("%.3f event type=%d key=%d mods=%d text=%s", now_sec(),
                         ev.type, ev.key, ev.mods, ev.text[0] ? ev.text : "");
                if (ev.type == EV_KEY) handle_key(&a, &ev);
                else if (ev.type == EV_MOUSE) handle_mouse(&a, &ev);
                if (g_quit) break;
            }
            draw_status(&a);
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
            draw_status(&a);
        }
    }

    if (!a.inline_mode) kitty_clear(&a.kitty);   // inline leaves the page behind
    kitty_free(&a.kitty);
    term_restore(&a.term);
    chrome_kill(&a.chrome);
    printf("web: %u frames drawn, %u duplicates skipped\n", a.frames, a.skipped);
    return 0;
}

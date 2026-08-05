#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "web.h"

volatile sig_atomic_t g_resized = 0;
volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_handed = 0;

static void on_winch(int sig) { (void)sig; g_resized = 1; }
static void on_term(int sig)  { (void)sig; g_quit = 1; }
static void on_hand(int sig)  { (void)sig; g_handed = 1; }

static App *g_app;

// A crash, rather than a quit. The keyboard is asked for in a mode the terminal
// keeps until it is told otherwise, so it is put back here and the signal is
// then allowed to do what it would have done - the point is the terminal the
// user is left sitting in, not saving the run.
static int g_panic_fd = -1;
static void on_crash(int sig) {
    term_panic(g_panic_fd);
    signal(sig, SIG_DFL);
    raise(sig);
}

// ----------------------------------------------------------- cdp dispatch

// The table is small and a reply always comes back quickly, so a full slot is
// the oldest one and dropping it loses nothing anybody is still waiting on.
void app_req_note(App *a, int id, int kind) {
    if (id <= 0) return;
    int oldest = 0;
    for (int i = 0; i < REQ_MAX; i++) {
        if (a->reqs[i].kind == RQ_NONE) { oldest = i; break; }
        if (a->reqs[i].id < a->reqs[oldest].id) oldest = i;
    }
    a->reqs[oldest].id = id;
    a->reqs[oldest].kind = kind;
}

int app_req_take(App *a, int id) {
    for (int i = 0; i < REQ_MAX; i++) {
        if (a->reqs[i].kind != RQ_NONE && a->reqs[i].id == id) {
            int kind = a->reqs[i].kind;
            a->reqs[i].kind = RQ_NONE;
            a->reqs[i].id = 0;
            return kind;
        }
    }
    return RQ_NONE;
}

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

int app_cdp(App *a, const char *method, const char *fmt, ...) {
    flush_pending(a);
    va_list ap;
    va_start(ap, fmt);
    int id = cdp_vcall(&a->chrome, method, fmt, ap);
    va_end(ap);
    return id;
}

// Something the user did, and whether it is the kind of thing that sets the
// page moving. Both halves matter. The moving half starts the resolution drop
// on the input itself rather than three frames later, which is what makes it
// worth having: a scroll used to pay full price for its first frames, and over
// ssh it never engaged at all - the run of quick frames it waits for is
// measured on frames that are slow because they are still full size, so the
// test could not pass until the thing it was testing for had already happened.
//
// The other half is the record of when input last arrived, which is what tells
// motion apart from a page that has merely gone quiet: Chrome dropping frames
// mid-scroll leaves the frame clock stale, and a scroll called over on that
// alone comes back sharp in the middle of itself.
static void note_input(App *a, bool moving) {
    a->last_input = now_sec();
    if (!moving || !a->motion_auto || a->in_motion) return;
    if (!a->has_tty || a->paused) return;
    a->in_motion = true;
    a->still_at = 0;              // whatever was owed, the page is moving again
    term_log("%.3f motion on (input)", a->last_input);
    relayout(a);
}

static void queue_move(App *a, int x, int y, const char *btn, int buttons,
                       int mods) {
    Pending *p = &a->pend;
    // A drag is the page moving; a bare hover is not, and softening the picture
    // under a pointer that is only crossing it would be a poor trade.
    note_input(a, buttons != 0);
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
    // Without a terminal, term.fd fell back to stdin - which is then a script,
    // not keystrokes, and reading it here would feed it to the page a byte at a
    // time.
    if (!g_app || !g_app->has_tty) return;
    struct pollfd p = {g_app->term.fd, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    term_read(&g_app->term);
    for (size_t i = 0; i < g_app->term.in.len; i++) {
        unsigned char c = (unsigned char)g_app->term.in.p[i];
        // ^C is the editor's clear-line while the console holds the keyboard, and
        // this runs mid-frame where that distinction cannot be made later.
        if (c == 0x11 || (c == 0x03 && !g_app->console_focus)) {
            term_log("QUIT via pump byte %02x", c);
            g_quit = 1;
            return;
        }
    }
}

// How wide the frame actually is. The screencast metadata says how big the
// page thinks the device is, in CSS pixels, which is the one number the frame
// size does not follow - so it comes out of the picture itself. A PNG opens
// with an 8-byte signature and an IHDR whose width sits at byte 16, and the
// first 32 characters of base64 carry the 24 bytes that reach it.
static int png_width(const char *b64, size_t len) {
    if (len < 32) return 0;
    unsigned char h[24];
    if (base64_decode(b64, 32, (char *)h) < 24) return 0;
    static const unsigned char sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    if (memcmp(h, sig, 8) || memcmp(h + 12, "IHDR", 4)) return 0;
    return (int)(((unsigned)h[16] << 24) | ((unsigned)h[17] << 16) |
                 ((unsigned)h[18] << 8) | (unsigned)h[19]);
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
void clipboard_put(const char *text) {
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

// The address, handed to whatever the desktop opens links with. exec rather
// than a shell: a url is made of the characters a shell would act on. The
// double fork orphans it, so the browser is nobody's child to reap here and
// outlives us either way.
static void open_external(App *a) {
    if (!a->url[0] || !strncmp(a->url, "about:", 6)) {
        notify(a, "nowhere to open");
        return;
    }
#ifdef __APPLE__
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            // Whatever it has to say goes nowhere: this terminal is a picture.
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) close(devnull);
            }
            execlp(opener, opener, a->url, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
        notify(a, "opened in the default browser");
    }
}

void notify(App *a, const char *s) {
    snprintf(a->msg, sizeof a->msg, "%s", s);
    a->msg_until = now_sec() + 2.0;
}

// ------------------------------------------------------------------ layout

// Inline mode is a window sitting in the shell's flow, so it has a shape of its
// own rather than the terminal's. Cells are taller than they are wide, so the
// proportion has to be struck in pixels or the box comes out square.
#define BOX_ASPECT (16.0 / 10.0)
#define BOX_MIN_COLS 10

// A cell is about twice as tall as it is wide, so a column is a smaller step
// than a row. Two of them move the edge about as far as one row moves the
// bottom, which is what makes the two directions feel like the same key.
#define BOX_COL_STEP 2

// The width the page is told it has, in CSS pixels. It is the same knob as zoom
// seen from the other end, but a layout breaks at a width rather than at a
// percentage, so this is the end worth walking: `w` and `W` step it, and the
// step is fine enough to find the edge a layout breaks at.
#define WIDTH_MIN  320
#define WIDTH_MAX  2560
#define WIDTH_STEP 40

// A moving picture is worth fewer pixels than a still one. The pixel count is
// paid three times over - Chrome's encode, our write, the terminal's decode -
// and measured on a scroll it is 86% of the frame time, nearly all of it
// Chrome's. Dropping it while the page slides past and putting it back the
// moment it stops is the one lever that shortens all three at once.
// It is spent on the screencast cap and nowhere else: the frame that arrives is
// the viewport in CSS pixels and the device scale factor has no say in it at
// all. Measured against this Chrome, a viewport of 480 asked for at a factor of
// 2.325 and again at 1 hands back the same 480-pixel frame, byte for byte - so
// a factor above 1 only buys the page a raster five times the size of anything
// that will be sent. The cap is the one end Chrome listens to.
// The linear scale; the pixels are its square, so 0.65 is 42% of them.
#define MOTION_SCALE 0.65
// Over ssh the bytes are the whole of the cost rather than a third of it, so
// the same trade is worth making harder: half the width is a quarter of them.
#define MOTION_SCALE_SSH 0.5
#define MOTION_RUN   3       // quick frames in a row before it is a scroll
#define MOTION_GAP   0.20    // a frame this soon after the last is still moving
#define MOTION_IDLE  0.30    // and this long without one is stopped
#define MOTION_QUIET 0.25    // and this long since the last key or wheel
// Long enough that a frame Chrome was going to send anyway arrives first and
// cancels the ask, short enough that the picture is not left soft for a beat
// somebody would notice.
#define STILL_WAIT   0.15
// A capture can itself provoke a compositor frame, which would arrive as an
// ordinary screencast frame and ask for another still. This is what stops the
// two chasing each other on a page that will not settle.
#define STILL_GAP    1.0
#define STILL_TRIES  3       // asks before the screencast is the one at fault
#define STILL_SEND_MAX 2.0   // how long a reply has to come back

static int box_cols_for(App *a, int rows) {
    Term *t = &a->term;
    int want = (int)((double)(rows * t->cell_h) * BOX_ASPECT / t->cell_w + 0.5);
    if (want > t->cols) want = t->cols;
    if (want < BOX_MIN_COLS) want = BOX_MIN_COLS;
    return want;
}

// The width the window is actually drawn at: whatever was asked for sideways,
// or the proportion a window opens with until something asks.
static int box_cols_now(App *a, int rows) {
    if (a->box_cols <= 0) return box_cols_for(a, rows);
    int cols = a->box_cols;
    if (cols > a->term.cols) cols = a->term.cols;   // the terminal may have shrunk
    if (cols < BOX_MIN_COLS) cols = BOX_MIN_COLS;
    return cols;
}

// Asking again is what makes Chrome hand over a fresh frame, so this is how
// anything wanting a redraw gets one.
static void screencast_start(App *a) {
    // The single gate: a resize, a zoom and the no-frame watchdog all come
    // through here, and any of them would otherwise start the picture up again
    // behind a terminal nobody is looking at.
    if (!a->has_tty || a->paused || a->cast_w < 1 || a->cast_h < 1) return;
    app_cdp(a, "Page.startScreencast",
             "\"format\":\"png\",\"maxWidth\":%d,\"maxHeight\":%d,\"everyNthFrame\":1",
             a->cast_w, a->cast_h);
}

// The sharp picture, asked for outright. Restarting the screencast above is an
// ask with no answer: it brings a frame back when Chrome has one to bring and
// says nothing when it does not, which is a page with nothing moving on it, a
// frame rastered before the size changed, or a frame dropped for being one too
// many in flight. That is the whole of why a scroll used to end on a soft
// picture that never came back. A screenshot is a call with a reply, so a still
// that does not turn up is something this can see, and ask for again.
//
// No clip: the reply is the viewport at the device scale factor, and relayout
// holds that factor at exactly the width the screencast delivers when the page
// is still. Asking for a region instead would be both bigger and wrong - clip
// scales on top of the factor rather than instead of it, and its origin is the
// document rather than the viewport, so a clipped still of a scrolled page
// photographs the top of the page.
// Nothing outstanding and nothing owed. For the places where the picture is
// about to be replaced wholesale - another page, another tab, a window nobody
// is looking at - so a reply arriving late cannot draw the page that was.
void still_cancel(App *a) {
    a->still_at = 0;
    a->still_sent = 0;
    a->still_tries = 0;
}

// Owe one, shortly. For anything that has just asked the screencast to redraw
// and would be left with the wrong picture up if Chrome had nothing to send.
void still_soon(App *a) {
    a->still_at = now_sec() + STILL_WAIT;
}

static void still_request(App *a) {
    // Cleared first, and on every path out of here: a debt left sitting in the
    // past is one the loop finds due on every pass, and the sleep it computes
    // from it is no sleep at all.
    a->still_at = 0;
    if (!a->has_tty || a->paused || a->in_motion || a->cast_w < 1) return;
    // --screenshot issues this very call for its own purposes; two of them in
    // flight would be two kinds waiting on one reply.
    if (a->shot_path) return;
    // A capture can provoke a compositor frame, which arrives as an ordinary
    // frame and asks for another still. Held off rather than dropped: the page
    // still owes a sharp picture, and this only says not yet.
    double now = now_sec();
    if (now - a->last_still < STILL_GAP) {
        a->still_at = a->last_still + STILL_GAP;
        return;
    }
    a->still_sent = now + STILL_SEND_MAX;
    app_req_note(a, app_cdp(a, "Page.captureScreenshot",
        "\"format\":\"png\",\"fromSurface\":true,\"captureBeyondViewport\":false"),
        RQ_STILL);
}

// The viewport a screenshot run gets when there is no terminal to take the
// shape from. Everything below works back from the cell rect, and with no tty
// that rect is invented out of the fallback terminal size - a couple of dozen
// pixels of window that nothing was ever going to be drawn into. A picture
// asked for from a pipeline is a picture of a page, so it gets a page's
// viewport, at whatever pixel ratio --scale asked for. Zoom is left out of it
// deliberately: it is the size of a window in a terminal, remembered from
// whichever terminal last set it, and there is no window here for it to mean
// anything about.
#define SHOT_CSS_W 1280
#define SHOT_CSS_H 800

void relayout(App *a) {
    Term *t = &a->term;
    term_size(t);

    // Every label is pinned to a viewport that is about to be a different size,
    // so all of them are about to be in the wrong place. Cheap when there are
    // none, which is nearly always.
    hint_cancel(a);

    if (!a->has_tty && a->shot_path) {
        a->css_w = SHOT_CSS_W;
        a->css_h = SHOT_CSS_H;
        a->scale = a->want_scale;
        app_cdp(a, "Emulation.setDeviceMetricsOverride",
                 "\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%.6f,"
                 "\"mobile\":false",
                 a->css_w, a->css_h, a->scale);
        return;
    }

    int status = a->status_open ? 1 : 0;
    int above = a->tabs_open ? 1 : 0;          // the tab bar, when it has earned it
    int below = status + a->console_rows;      // everything under the picture
    int rect_cols;
    if (a->inline_mode) {
        // The terminal may have shrunk under the box since it was last sized.
        int max_rows = t->rows - below - above;
        if (a->box_rows > max_rows) a->box_rows = max_rows;
        if (a->box_rows < 2) a->box_rows = 2;
        a->img_rows = a->box_rows;
        rect_cols = box_cols_now(a, a->img_rows);
        // Keep the whole block, tab bar and status line included, on the
        // screen. A block whose last row falls past the bottom scrolls the
        // terminal as it is drawn, which lands half the picture at the top and
        // half at the bottom - and the halves never line back up.
        if (t->inline_origin + above + a->img_rows + below - 1 > t->rows)
            t->inline_origin = t->rows - a->img_rows - below - above + 1;
        if (t->inline_origin < 1) t->inline_origin = 1;
        a->tabs_row = t->inline_origin;
        kitty_set_rect(&a->kitty, 1, t->inline_origin + above, rect_cols,
                       a->img_rows);
        a->status_row = t->inline_origin + above + a->img_rows;
    } else {
        a->img_rows = t->rows - below - above;
        if (a->img_rows < 1) a->img_rows = 1;
        rect_cols = t->cols;
        a->tabs_row = 1;
        kitty_set_rect(&a->kitty, 1, 1 + above, rect_cols, a->img_rows);
        a->status_row = t->rows - a->console_rows;
    }
    a->console_row = a->status_row + status;

    // The pixels the picture will actually occupy. Everything below works back
    // from these: whatever the terminal is handed gets resampled to this size,
    // and a resample by any fraction is what shaves the bottom off a line of
    // text, so the goal is to be handed exactly this and never resample at all.
    int rect_w = rect_cols * t->cell_w;
    int rect_h = a->img_rows * t->cell_h;
    if (rect_w < 1) rect_w = 1;
    if (rect_h < 1) rect_h = 1;

    // A width asked for by number is what the page is told, whatever the window
    // is doing; the zoom is kept in step with it so the magnification keys pick
    // up from where the width left off rather than jumping.
    if (a->want_width > 0) a->zoom = (double)rect_w / a->want_width;
    double z = a->zoom > 0 ? a->zoom : 1.0;

    int w = a->want_width > 0 ? a->want_width : (int)(rect_w / z);
    // A page with a minimum layout width would otherwise hang off the side.
    // Widening the viewport to what it asks for keeps all of it in view; the
    // text ends up smaller, which is the honest trade.
    if (a->fit_width && a->want_width <= 0 && a->fit_w > w) w = a->fit_w;
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
    // Only ever downwards, and never past 1: below that the ratio is already
    // costing less than the cells it lands in, which is what somebody asking
    // for it wanted.
    a->scale = a->want_scale;
    if (!a->scale_locked && a->scale > 1.0) {
        double fits = 1920.0 / rect_w;
        if (fits < 1.0) fits = 1.0;
        if (a->scale > fits) a->scale = fits;
    }

    double ms = a->in_motion
        ? (a->motion_scale > 0 ? a->motion_scale : MOTION_SCALE) : 1.0;

    // The ratio the page is rastered at. It used to be struck against the cell
    // rect, on the reasoning that a page laid out at the size it lands at is
    // sharper than a bitmap the terminal stretched - but the screencast never
    // sent those pixels. It caps against the viewport in CSS pixels and ignores
    // the factor entirely, so raising it only made Chrome raster a frame five
    // times the size of the one it then handed over. What it costs is real and
    // what it buys is nothing, so it is held at the ratio actually being asked
    // for, and never above what the pane can show.
    //
    // Motion is deliberately not in here. It rides on the cap alone, so a scroll
    // starting or stopping no longer changes the device metrics - which means
    // Chrome keeps the tiles it has already rastered instead of throwing them
    // away twice per scroll, and the page is never re-laid-out for a resolution
    // change. What that costs is the repaint a metrics change used to force:
    // identical metrics give Chrome no reason to draw, so the sharp frame is
    // fetched by still_request() rather than waited for.
    double dsf = a->scale;
    double fits = (double)rect_w * a->scale / (double)a->css_w;
    if (fits < dsf) dsf = fits;

    a->status_last.len = 0;    // the status line may have moved rows

    // Both calls go out every time, including when the numbers have not moved.
    // Restarting the screencast is what usually makes Chrome hand over a fresh
    // frame, and it is the cheap way to ask - but it is only an ask, and the
    // still is what makes sure one arrives.
    app_cdp(a, "Emulation.setDeviceMetricsOverride",
             "\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%.6f,\"mobile\":false",
             a->css_w, a->css_h, dsf);
    // The cap Chrome is given is measured against the viewport in CSS pixels,
    // and it only ever scales a frame down: `scale = 1` and then a min against
    // each limit, so anything above the viewport is the viewport. Struck
    // against the cell rect instead, as it was, the number meant nothing at any
    // zoom - at 2.3x a request for 842 came back 560 wide, unchanged - and
    // --scale did nothing until it fell under 1/zoom. Against css_w it means
    // what it says: half is half the width and a quarter of the pixels.
    // Above 1 it cannot mean anything, here or anywhere: the screencast has no
    // way to hand over more pixels than the viewport has.
    a->cast_w = (int)((double)a->css_w * dsf * ms + 0.5);
    a->cast_h = (int)((double)a->css_h * dsf * ms + 0.5);
    if (a->cast_w < 1) a->cast_w = 1;
    if (a->cast_h < 1) a->cast_h = 1;

    // The two sizes a picture can arrive at, kept so that whatever turns up can
    // be measured against what was asked for. A screencast frame is the cap or
    // the viewport, whichever bites first - the factor has no say. A screenshot
    // is the viewport at the factor, which is the whole of why the factor is
    // held where it is above: the two come out equal whenever the page is still,
    // so the still is the same picture the screencast would have sent and not a
    // sharper one that would visibly drop back on the next ordinary frame.
    int cap_w = a->cast_w < a->css_w ? a->cast_w : a->css_w;
    a->frame_w = cap_w;
    a->still_w = (int)((double)a->css_w * dsf + 0.5);

    screencast_start(a);
}

// ----------------------------------------------------------------- session

// What it takes to drive this window from outside. The port alone used to say
// it, but a window per run means the browser has several pages and the port no
// longer picks one out - so the id of ours is the part that matters. One file
// per run, named for the pid, which is also what says the file is not a
// leftover from a window that died without tidying up.
static void session_file(App *a, char *out, size_t cap) {
    snprintf(out, cap, "%s/sessions/%d.json", a->chrome.profile, (int)getpid());
}

void session_write(App *a) {
    if (!a->chrome.profile[0] || a->chrome.port <= 0 || !a->chrome.target[0])
        return;
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/sessions", a->chrome.profile);
    mkdirs(dir);
    session_file(a, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    char url[2100], title[600];
    json_escape(url, sizeof url, a->url);
    json_escape(title, sizeof title, a->title);
    // "handoff" says this window listens for a url handed to it. Nothing but
    // its presence matters: a window from a version that predates the signal
    // would be killed by it, and the file it left behind is the only place a
    // sender can find that out before sending.
    fprintf(f, "{\"pid\":%d,\"port\":%d,\"cdp\":\"http://127.0.0.1:%d\","
               "\"target\":\"%s\",\"url\":\"%s\",\"title\":\"%s\","
               "\"handoff\":true}\n",
            (int)getpid(), a->chrome.port, a->chrome.port,
            a->chrome.target, url, title);
    fclose(f);
}

// ------------------------------------------------------------- handed a url

// An address given to a window that is already up, so that `web --open` - and
// the system link handler standing on it - lands in a tab of a window the user
// already has rather than in a terminal of its own.
//
// One file per request, named for the window it is for and the process that
// wrote it, so two arriving at once are two files instead of two writes racing
// for one. It is written under .tmp and renamed into place, which is what makes
// a file the reader finds a whole one. SIGUSR1 is only the nudge: the files are
// the request, and one that arrives while the reader is mid-pass is picked up
// by the pass the signal after it provokes.
static void handoff_dir(const char *profile, char *out, size_t cap) {
    snprintf(out, cap, "%s/handoff", profile);
}

static void handle_focus(App *a, bool focused);

// tmux draws one window of a session at a time, and a url arriving moves
// nothing: the tab would be made in a pane behind whichever one the client is
// looking at. Selecting our own pane is the half of coming forward that can be
// done from in here - the terminal application itself is the sender's to raise,
// since a window has no handle on the one drawing it.
//
// Done in a child because tmux talks to its server and waits for the answer,
// and the window has a page to be drawing. Double-forked so there is no child
// left to reap: this is called from the main loop, which waits for nothing.
static void raise_pane(void) {
    const char *pane = getenv("TMUX_PANE");
    if (!getenv("TMUX") || !pane || pane[0] != '%') return;
    for (const char *p = pane + 1; *p; p++)
        if (*p < '0' || *p > '9') return;      // not a pane id; not for sh

    pid_t mid = fork();
    if (mid < 0) return;
    if (mid == 0) {
        if (fork() == 0) {
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
            char cmd[128];
            // The window first and then the pane inside it: selecting a pane
            // of another window is not on its own a move to that window.
            snprintf(cmd, sizeof cmd,
                     "tmux select-window -t %s; tmux select-pane -t %s",
                     pane, pane);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(mid, NULL, 0);
}

// Until when a handed page is still owed its picture. Nothing about a link
// clicked in another application arrives in a helpful order: the terminal loses
// the focus as the handler is launched and gains it again as the handler brings
// it back, and either can land on either side of the url itself. A blur that
// arrives just after the handoff would otherwise stop the screencast that the
// handoff had just started and cancel the still it had asked for, leaving the
// new tab drawn over by the old page - so for as long as this is in the future
// the window does not pause, whoever tells it to.
static double g_handoff_owed;

#define HANDOFF_DRAW_WAIT 5.0

// The picture that was owed is up: an ordinary blur may stop the drawing again.
static void handoff_drawn(void) { g_handoff_owed = 0; }

// A url handed in is a page somebody is waiting to look at, so the window comes
// forward and draws it whether or not the terminal is focused at this moment.
// Without this the tab arrives titled and empty, or titled and still showing
// the page before it: the bar and the status line are text and go out
// regardless, and the picture is the one thing pause-on-blur holds back - which
// is exactly a link clicked in another application, where the terminal is by
// definition not the focused one.
//
// The restart is unconditional rather than a focus event, because the window
// may never have been paused at all: the tab it has just switched to is a page
// nothing has drawn yet either way, and Chrome hands over a frame only when
// asked.
static void handoff_arrived(App *a) {
    g_handoff_owed = now_sec() + HANDOFF_DRAW_WAIT;
    raise_pane();
    a->paused = false;
    a->last_hash = 0;                 // an unchanged frame is still a new page
    a->kitty.grid_dirty = true;
    a->expect_frame = now_sec() + 2.0;
    screencast_start(a);
    still_soon(a);
}

// Every request for `pid`, removed as it goes. With an App it is opened in a
// tab; without one it is only cleared away, which is what a window leaving does
// with anything that arrived too late for it to draw.
static void handoff_take(const char *profile, pid_t pid, App *a) {
    char dir[600];
    handoff_dir(profile, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    char prefix[32];
    int plen = snprintf(prefix, sizeof prefix, "%d-", (int)pid);
    int taken = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, (size_t)plen) != 0) continue;
        if (strstr(e->d_name, ".tmp")) continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        unlink(path);
        if (!f) continue;
        char url[1100];
        if (a && fgets(url, sizeof url, f)) {
            url[strcspn(url, "\r\n")] = 0;
            if (url[0]) {
                term_log("%.3f handed %s", now_sec(), url);
                if (tab_open_url(a, url)) taken++;
                else notify(a, "no room for another tab");
            }
        }
        fclose(f);
    }
    closedir(d);
    // Once for the pass rather than once per url: the window comes forward and
    // starts drawing the tab it ended on, and doing that per file would be the
    // same work repeated for pages already switched away from.
    if (taken) handoff_arrived(a);
}

// The window a url handed in from outside belongs to: the one whose session
// file was written last, which is the one most recently moved and so the one
// most recently looked at. Only a window that said it listens for one is a
// candidate, since the signal that carries it is fatal to a window that does
// not. 0 when there is no such window.
static pid_t newest_window(const char *profile) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/sessions", profile);
    DIR *d = opendir(dir);
    if (!d) return 0;
    pid_t best = 0;
    time_t best_at = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int pid = atoi(e->d_name);
        const char *dot = strrchr(e->d_name, '.');
        if (pid <= 0 || !dot || strcmp(dot, ".json") != 0) continue;
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) continue;
        char path[800];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[4096];
        bool listens = fgets(line, sizeof line, f) && json_has(line, "handoff");
        fclose(f);
        if (!listens) continue;
        // A tie is two windows written in the same second; the later pid is the
        // later window, which is the better guess of the two.
        if (st.st_mtime > best_at || (st.st_mtime == best_at && pid > best)) {
            best_at = st.st_mtime;
            best = (pid_t)pid;
        }
    }
    closedir(d);
    return best;
}

static int hand_url(const char *url) {
    char profile[512];
    chrome_profile_path(profile, sizeof profile);
    pid_t pid = newest_window(profile);
    if (pid <= 0) {
        fprintf(stderr, "web: no window is running\n");
        return 1;
    }
    char dir[600], path[800], tmp[820];
    handoff_dir(profile, dir, sizeof dir);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/%d-%d", dir, (int)pid, (int)getpid());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "web: cannot write %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    fprintf(f, "%s\n", url);
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return 1; }
    if (kill(pid, SIGUSR1) != 0) {
        unlink(path);
        fprintf(stderr, "web: window %d has gone\n", (int)pid);
        return 1;
    }
    return 0;
}

static void session_forget(App *a) {
    if (!a->chrome.profile[0]) return;
    char path[700];
    session_file(a, path, sizeof path);
    unlink(path);
    handoff_take(a->chrome.profile, getpid(), NULL);
}

// Every window running now, one JSON object to a line. Nothing else tidies the
// files up, so a pid that has gone takes its own with it here.
static int print_sessions(void) {
    char profile[512], dir[600];
    chrome_profile_path(profile, sizeof profile);
    snprintf(dir, sizeof dir, "%s/sessions", profile);
    DIR *d = opendir(dir);
    int found = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int pid = atoi(e->d_name);
            if (pid <= 0) continue;
            char path[800];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
                unlink(path);
                continue;
            }
            FILE *f = fopen(path, "r");
            if (!f) continue;
            char line[4096];
            if (fgets(line, sizeof line, f)) {
                fputs(line, stdout);
                found++;
            }
            fclose(f);
        }
        closedir(d);
    }
    if (!found) fprintf(stderr, "web: no window is running\n");
    return found ? 0 : 1;
}

// ------------------------------------------------------------------ browsers

#define PROC_MAX 64

// The pid of every window running now. The file is named for it, which is also
// what says the file is not a leftover: a pid that has gone takes its own with
// it here, the same way --endpoint tidies up.
static int running_windows(const char *profile, pid_t *out, int cap) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/sessions", profile);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < cap && (e = readdir(d)) != NULL) {
        int pid = atoi(e->d_name);
        if (pid <= 0 || pid == (int)getpid()) continue;
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
            char path[800];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
            continue;
        }
        out[n++] = (pid_t)pid;
    }
    closedir(d);
    return n;
}

// Windows the sessions directory does not know about: one that never got as far
// as registering, or that removed its entry and then wedged before it could
// exit. Matched on argv[0] alone, so a url or an argument with "web" in it is
// not a window and neither is anything else that merely mentions one.
//
// These are reported rather than ended. A window says which profile it belongs
// to nowhere a process table can be asked about, so this cannot tell one
// profile's windows from another's - and a --kill aimed at one profile that
// reaches into another is a session somebody was using, gone without being
// asked. The registry is what makes a window this profile's to end; anything
// else is somebody's to look at and decide about.
static int stuck_windows(pid_t *out, int cap, const pid_t *known, int nknown) {
    FILE *p = popen("ps -axww -o pid=,command= 2>/dev/null", "r");
    if (!p) return 0;
    int n = 0;
    char line[8192];
    while (n < cap && fgets(line, sizeof line, p)) {
        int pid = 0;
        char cmd[1024] = {0};
        if (sscanf(line, "%d %1023s", &pid, cmd) != 2 || pid <= 0) continue;
        if (pid == (int)getpid()) continue;
        const char *base = strrchr(cmd, '/');
        base = base ? base + 1 : cmd;
        if (strcmp(base, "web") != 0) continue;
        bool seen = false;
        for (int i = 0; i < nknown; i++) if (known[i] == (pid_t)pid) seen = true;
        if (!seen) out[n++] = (pid_t)pid;
    }
    pclose(p);
    return n;
}

static bool proc_alive(pid_t p) {
    return kill(p, 0) == 0 || errno != ESRCH;
}

// Whether a window has finished leaving. Its session file is the last thing it
// removes, after the terminal has been handed back, so the file going is the
// whole of the shutdown having run. Better than its pid: a process that has
// exited but has not yet been reaped by the shell that started it still answers
// kill(pid, 0), and would read as one that ignored the signal.
static bool window_gone(const char *profile, pid_t pid) {
    char path[700];
    snprintf(path, sizeof path, "%s/sessions/%d.json", profile, (int)pid);
    return access(path, F_OK) != 0 || !proc_alive(pid);
}

static bool wait_windows(const char *profile, const pid_t *pids, int n,
                         double secs) {
    double deadline = now_sec() + secs;
    for (;;) {
        bool any = false;
        for (int i = 0; i < n; i++)
            if (!window_gone(profile, pids[i])) any = true;
        if (!any || now_sec() >= deadline) return !any;
        struct timespec ts = {0, 25 * 1000000};
        nanosleep(&ts, NULL);
    }
}

// Wait for a list of processes to go, and say whether any is left. They are
// nobody's children here - a browser we adopted belongs to init - so there is
// nothing to reap and asking is the only way to know.
static bool wait_gone(const pid_t *pids, int n, double secs) {
    double deadline = now_sec() + secs;
    for (;;) {
        bool any = false;
        for (int i = 0; i < n; i++) if (pids[i] > 0 && proc_alive(pids[i])) any = true;
        if (!any || now_sec() >= deadline) return !any;
        struct timespec ts = {0, 25 * 1000000};
        nanosleep(&ts, NULL);
    }
}

// Every browser of ours that is up, and whether a new window could still find
// it. One that cannot be found is the thing worth knowing about: it goes on
// holding the profile, and every later start launches a second browser beside
// it instead of adopting this one.
static int print_browsers(void) {
    char profile[512];
    chrome_profile_path(profile, sizeof profile);

    ChromeProc procs[PROC_MAX];
    int n = chrome_running(profile, procs, PROC_MAX);
    if (!n) {
        fprintf(stderr, "web: no Chrome instances are running on this profile\n");
        return 1;
    }

    pid_t holder = 0;
    int port = chrome_adoptable(profile, &holder);
    int unreachable = 0;
    for (int i = 0; i < n; i++) {
        // The lock names at most one browser, and only while it is there to be
        // read. With nothing to go on, a single browser beside a live endpoint
        // is that endpoint's - and several of them cannot all be.
        bool adoptable = port > 0 &&
                         (holder > 0 ? holder == procs[i].pid : n == 1);
        if (adoptable)
            printf("pid %-7d up %-12s port %d\n",
                   (int)procs[i].pid, procs[i].age, port);
        else
            printf("pid %-7d up %-12s unreachable (no debugging endpoint)\n",
                   (int)procs[i].pid, procs[i].age);
        unreachable += !adoptable;
    }
    // The hint goes to stderr so the list stays pipeable, which means it has to
    // wait for the list: the two streams buffer differently and it would
    // otherwise land above the thing it is about.
    fflush(stdout);
    if (unreachable)
        fprintf(stderr, "web: %d unreachable browser%s holding the profile; "
                        "run 'web --kill' to terminate %s\n",
                unreachable, unreachable == 1 ? "" : "s",
                unreachable == 1 ? "it" : "them");
    return 0;
}

// Everything this program has left running. The windows go first: each one
// shuts its own browser down on the way out and hands its terminal back, which
// is a tidier end than pulling the browser out from under one still drawing it.
// Whatever browsers are left after that are the ones no window ever claimed,
// which is what this is really for.
static int kill_everything(void) {
    char profile[512];
    chrome_profile_path(profile, sizeof profile);

    pid_t windows[PROC_MAX];
    int w = running_windows(profile, windows, PROC_MAX);
    for (int i = 0; i < w; i++) kill(windows[i], SIGTERM);
    if (w) {
        printf("web: asked %d window%s to quit\n", w, w == 1 ? "" : "s");
        // A window wedged writing to a terminal that went away never gets to
        // its own signal handler, and waiting on it forever is how this option
        // fails to do the one thing it is for.
        if (!wait_windows(profile, windows, w, 3.0)) {
            for (int i = 0; i < w; i++)
                if (!window_gone(profile, windows[i])) {
                    printf("web: window %d would not quit; ending it\n",
                           (int)windows[i]);
                    kill(windows[i], SIGKILL);
                }
        }
    }

    // Named, not ended: which profile one of these belongs to is not a question
    // the process table can answer, and ending one on a guess is somebody's
    // session gone without being asked.
    pid_t stray[PROC_MAX];
    int s = stuck_windows(stray, PROC_MAX, windows, w);

    // Re-read after the windows have gone: most browsers will have left with
    // the window that started them, and the list is shorter for it.
    ChromeProc procs[PROC_MAX];
    int n = chrome_running(profile, procs, PROC_MAX);
    pid_t pids[PROC_MAX];
    for (int i = 0; i < n; i++) {
        pids[i] = procs[i].pid;
        // The browser, not its group. Chrome commits its cookie store on an
        // orderly shutdown, and the group signal takes the helpers down first
        // so that shutdown never runs.
        kill(pids[i], SIGTERM);
        printf("web: ending chrome %d\n", (int)pids[i]);
    }
    if (n && !wait_gone(pids, n, 5.0)) {
        for (int i = 0; i < n; i++)
            if (proc_alive(pids[i])) {
                printf("web: chrome %d would not go; ending its group\n",
                       (int)pids[i]);
                if (kill(-pids[i], SIGKILL) < 0) kill(pids[i], SIGKILL);
            }
    }

    if (w || n) {
        // The notes name a browser that is not there any more. Left behind they
        // cost the next run a probe apiece before it gives up on them.
        char path[700];
        snprintf(path, sizeof path, "%s/DevToolsActivePort", profile);
        unlink(path);
        snprintf(path, sizeof path, "%s/web-port", profile);
        unlink(path);
        snprintf(path, sizeof path, "%s/web-keep", profile);
        unlink(path);
    } else if (!s) {
        printf("web: nothing of its own was running\n");
    }

    // Last, so it is the thing left on screen. Said rather than acted on: see
    // stuck_windows. The pid is what somebody needs to deal with it.
    fflush(stdout);
    for (int i = 0; i < s; i++)
        fprintf(stderr, "web: window %d is running but is not this profile's "
                        "to end; kill %d if it is yours\n",
                (int)stray[i], (int)stray[i]);
    return 0;
}

// -------------------------------------------------------------------- exec

// Run a program against this window. It is handed the devtools endpoint and the
// id of our page, which together are the whole of what an outside driver needs:
// with them a playwright script attaches to the page on screen rather than
// guessing among the browser's tabs. Its output goes to the console, which
// is where everything else this window has to say already goes.
static void exec_start(App *a, const char *cmd) {
    int fds[2];
    if (pipe(fds) < 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        // Both streams into the pipe: a script's diagnostics are as much a part
        // of watching it run as what it prints, and there is nowhere else for
        // them to go - stderr here is the middle of the picture.
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        if (fds[1] > 2) close(fds[1]);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null > 2) close(null);
        }
        char url[64], port[16];
        snprintf(url, sizeof url, "http://127.0.0.1:%d", a->chrome.port);
        snprintf(port, sizeof port, "%d", a->chrome.port);
        setenv("WEB_CDP_URL", url, 1);
        setenv("WEB_CDP_PORT", port, 1);
        setenv("WEB_TARGET_ID", a->chrome.target, 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    int fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    a->exec_fd = fds[0];
    a->exec_pid = pid;
    // Somewhere to watch it from. Opened without the keyboard: the page is what
    // is being watched, and a console that took the keys would stop the window
    // being usable while the script runs. With no terminal there is nothing to
    // open and nobody to watch, and an open console would only keep a draining
    // script from ever reaching its end.
    if (a->has_tty && !a->console_open) a->console_open = true;
    console_log(a, "");
    char m[300];
    snprintf(m, sizeof m, "$ %.280s", cmd);
    console_log(a, m);
}

static void exec_done(App *a) {
    close(a->exec_fd);
    a->exec_fd = -1;
    int st = 0;
    char m[64];
    if (waitpid(a->exec_pid, &st, 0) == a->exec_pid && WIFEXITED(st))
        snprintf(m, sizeof m, "[exit %d]", WEXITSTATUS(st));
    else
        snprintf(m, sizeof m, "[stopped]");
    a->exec_pid = 0;
    console_log(a, m);
}

// Whole lines only: the console's transcript is a list of lines, and half of one
// would be a line in it that the rest of the output could never join.
static void exec_pump(App *a) {
    if (a->exec_fd < 0) return;
    bool eof = false;
    for (;;) {
        char tmp[4096];
        ssize_t r = read(a->exec_fd, tmp, sizeof tmp);
        if (r > 0) { buf_add(&a->exec_buf, tmp, (size_t)r); continue; }
        if (r == 0) { eof = true; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        eof = true;
        break;
    }

    while (a->exec_buf.len) {
        char *nl = memchr(a->exec_buf.p, '\n', a->exec_buf.len);
        size_t len = nl ? (size_t)(nl - a->exec_buf.p) : a->exec_buf.len;
        if (!nl && !eof) break;                 // the rest of it is still coming
        char line[512];
        size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, a->exec_buf.p, n);
        line[n] = 0;
        for (char *p = line; *p; p++) if (*p == '\r' || *p == '\t') *p = ' ';
        console_log(a, line);
        buf_consume(&a->exec_buf, nl ? len + 1 : len);
    }
    if (eof) exec_done(a);
}

// -------------------------------------------------------------- screenshot

// How long the page gets to arrive before it is photographed as it stands. A
// picture of a half-drawn page is worth more than a run that never returns.
#define SHOT_LOAD_MAX   30.0
// The two round trips after that are the browser answering, not the network.
#define SHOT_SETTLE_MAX  3.0
#define SHOT_SEND_MAX   15.0

// Between the load event and the shutter: the fonts the page asked for, and
// two frames after them. A webfont swaps in after the load event and a first
// paint can still be on its way, and either one photographs as a page that is
// not the page.
static const char SHOT_READY_JS[] =
    "new Promise(function(r){"
    "(document.fonts?document.fonts.ready:Promise.resolve()).then(function(){"
    "requestAnimationFrame(function(){requestAnimationFrame(function(){r(1)})})"
    "})})";

static void shot_fail(App *a, const char *why) {
    fprintf(stderr, "web: --screenshot: %s\n", why);
    a->shot_state = SHOT_FAIL;
}

// The picture, out of base64 and onto disk.
static void shot_write(App *a, const char *msg) {
    size_t n = 0;
    const char *b64 = json_str(msg, "data", &n);
    if (!b64 || !n) { shot_fail(a, "the browser sent no picture back"); return; }

    Buf png = {0};
    if (buf_reserve(&png, n / 4 * 3 + 4) < 0) {
        shot_fail(a, "out of memory for the picture");
        return;
    }
    png.len = base64_decode(b64, n, png.p);

    int rc = 0;
    if (!strcmp(a->shot_path, "-")) {
        rc = writeall(STDOUT_FILENO, png.p, png.len);
        if (rc < 0) fprintf(stderr, "web: --screenshot: %s\n", strerror(errno));
    } else {
        FILE *f = fopen(a->shot_path, "wb");
        if (!f || fwrite(png.p, 1, png.len, f) != png.len) rc = -1;
        if (f && fclose(f) != 0) rc = -1;
        if (rc < 0)
            fprintf(stderr, "web: %s: %s\n", a->shot_path, strerror(errno));
    }
    buf_free(&png);
    a->shot_state = rc < 0 ? SHOT_FAIL : SHOT_DONE;
}

// A still that came back. It goes up the same way a frame does - the base64 is
// handed to the terminal without ever being decoded - and it counts as the
// picture on screen, so a screencast frame carrying the same bytes would be
// skipped. Only that direction is safe, and it is: the two encoders never agree
// byte for byte, so this can dedupe one still against the next and can never
// hide a real frame behind one.
//
// Left alone deliberately: fps and the frame count, which describe the
// screencast and would read as a stutter if a still were counted among them.
static void still_draw(App *a, const char *msg) {
    // A reply that outlived the blur it was asked before. Nothing is being
    // looked at, and the unpause redraws from scratch anyway.
    if (a->paused) return;
    size_t n = 0;
    const char *b64 = json_str(msg, "data", &n);
    if (!b64 || !n) return;    // the deadline in the loop asks again

    double t0 = now_sec();
    kitty_draw_png(&a->kitty, b64, n);
    double t1 = now_sec();
    handoff_drawn();

    a->last_hash = fnv1a(b64, n);
    a->last_draw = t1;
    a->last_still = t1;
    a->still_sent = 0;
    a->still_tries = 0;
    a->last_bytes = n;
    a->last_write_ms = (t1 - t0) * 1000.0;
    a->total_bytes += n;
    a->stills++;
    if (a->last_write_ms > a->worst_write_ms) a->worst_write_ms = a->last_write_ms;
    term_log("%.3f still %u: %dpx of %d, %zu KB b64, ours %.1f ms",
             t1, a->stills, png_width(b64, n), a->still_w, n / 1024,
             a->last_write_ms);
}

// The shutter. It photographs the viewport, so a run with a terminal under it
// files what the window was showing and one without files the page-sized
// viewport relayout gave it instead.
static void shot_capture(App *a) {
    app_req_note(a, app_cdp(a, "Page.captureScreenshot", "\"format\":\"png\""),
                 RQ_SHOT);
    a->shot_state = SHOT_SENT;
    a->shot_deadline = now_sec() + SHOT_SEND_MAX;
}

// Driven once per pass of the main loop while a shot is outstanding. Each state
// waits for one thing and gives up on it at a deadline of its own, so nothing
// the page or the browser fails to do can leave the run parked.
static void shot_step(App *a) {
    switch (a->shot_state) {
    case SHOT_LOAD: {
        // The page, then whatever javascript was piped in against it, then the
        // pause after the last line of it: the same three the script runner's
        // own exit waits on, because a shot taken between them is a shot of a
        // page mid-sentence.
        bool ready = !a->loading && !script_busy(a) && !a->console_open &&
                     a->exec_fd < 0 && now_sec() >= a->script.next_at;
        if (!ready && now_sec() < a->shot_deadline) return;
        char esc[sizeof SHOT_READY_JS * 2];
        json_escape(esc, sizeof esc, SHOT_READY_JS);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"%s\",\"returnByValue\":true,\"awaitPromise\":true",
            esc), RQ_SHOT_READY);
        a->shot_state = SHOT_SETTLE;
        a->shot_deadline = now_sec() + SHOT_SETTLE_MAX;
        return;
    }

    case SHOT_SETTLE:
        if (now_sec() < a->shot_deadline) return;
        term_log("shot: the page never reported itself painted; shooting anyway");
        shot_capture(a);
        return;

    case SHOT_SENT:
        if (now_sec() < a->shot_deadline) return;
        shot_fail(a, "the browser never answered with a picture");
        return;
    }
}

// ------------------------------------------------------------------ status

// Every row the inline block owns: the bar, the picture, the status line and
// the console. Worked out in one place because two of them are settled here and
// two are settled by keys, and a count that disagrees with the layout is a row
// of somebody else's screen that never gets cleaned up.
static int block_rows(App *a) {
    return (a->tabs_open ? 1 : 0) + a->box_rows +
           (a->status_open ? 1 : 0) + a->console_rows;
}

// The address bar and the find prompt are drawn on the status line, so a line
// that has been hidden comes back for as long as one of them is open. The tab
// bar comes and goes with there being more than one tab to name.
static void status_sync(App *a) {
    bool want = !a->hide_status || a->editing;
    int  rows = console_rows(a);
    bool tabs = tabs_wanted(a);
    if (want == a->status_open && rows == a->console_rows &&
        tabs == a->tabs_open) return;
    a->status_open = want;
    a->console_rows = rows;
    a->tabs_open = tabs;
    a->status_last.len = 0;
    a->console_last.len = 0;
    a->tabs_last.len = 0;
    kitty_clear(&a->kitty);          // the rows it lived on change hands
    // Inline, the page itself is not resized by this, so the frame that comes
    // back is the one already on screen - and a duplicate is normally dropped,
    // which would leave the block empty for as long as the page sits still.
    a->last_hash = 0;
    if (a->inline_mode)
        term_resize_inline(&a->term, block_rows(a));
    else
        writeall(a->term.fd, "\x1b[2J", 4);
    relayout(a);
    still_soon(a);      // same reason: unchanged metrics, and the image is gone
}

// Called after every input batch and every frame, so it keeps its buffer and
// stays quiet when the line has not changed: an unnecessary repaint here lands
// in the middle of a stream of image data.
static void draw_status(App *a) {
    if (!a->has_tty || !a->status_open) return;
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
        const char *label = a->prompt == 2 ? "find" : a->prompt == 3 ? "tab" : "go";
        buf_addf(&b, "\x1b[7m %s \x1b[0m %.*s\x1b[?25h",
                 label, (int)a->edit_len, a->edit);
        // Park the cursor after the text being typed, which is as far past the
        // label as the label is long: the space either side of it, and the one
        // the text starts after.
        buf_addf(&b, "\x1b[%d;%dH", row,
                 sx + (int)strlen(label) + 3 + (int)a->edit_len);
    } else {
        // The rest of the list is one keypress away, so the line only has to
        // name that keypress. It is the one key nobody could have guessed at,
        // and naming it costs the address six columns instead of forty.
        static const char KEYS[] = "? keys";

        const char *left = a->title[0] ? a->title : a->url;
        char stats[160];
        const char *hint;
        if (a->show_stats) {
            // The port leads: it is the one number here that something outside
            // this process needs, and it is how playwright finds the browser.
            // The cell and the frame it implies come last: they are what says
            // whether the size everything else is derived from was measured or
            // guessed, which is not visible from any of the numbers before them.
            snprintf(stats, sizeof stats,
                     "cdp:%d  %zuKB %.0fms %.1ffps  %dx%d@%gx z%.0f%%  "
                     "cell %dx%d cast %dx%d  %us%s",
                     a->chrome.port,
                     a->last_bytes / 1024, a->last_write_ms, a->fps,
                     a->css_w, a->css_h, a->scale,
                     100.0 * (sw * a->term.cell_w) / (a->css_w ? a->css_w : 1),
                     a->term.cell_w, a->term.cell_h, a->cast_w, a->cast_h,
                     a->stills, a->in_motion ? " moving" : "");
            hint = stats;
        } else {
            hint = KEYS;
        }
        int hintlen = (int)strlen(hint);
        // A narrow window drops the hint rather than shrinking the address to
        // nothing; when it goes, its room goes to the address.
        bool show_hint = sw > hintlen + 4;
        int avail = show_hint ? sw - hintlen - 3 : sw - 2;
        if (avail < 8) avail = 8;

        if (a->insert)
            buf_addf(&b, "\x1b[1;33m INSERT\x1b[0m ");
        else if (a->hint_on)
            buf_addf(&b, "\x1b[1;36m LINKS %s\x1b[0m ", a->hint_typed);
        else if (a->pend_key) {
            // Half a pair is state with nothing to show for it otherwise: the
            // next key means something different and the keyboard looks stuck.
            char spec[48];
            key_text(a->pend_mods, a->pend_key, spec, sizeof spec);
            buf_addf(&b, "\x1b[1;36m %s-\x1b[0m ", spec);
        }
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

// The rows settle first, then everything on them. The status line goes last of
// the two above the console because it is the one that parks the cursor while
// the address bar is open, and whatever draws after it moves the cursor again.
static void draw_panes(App *a) {
    if (!a->has_tty) return;
    status_sync(a);
    tabs_paint(a);
    draw_status(a);
    console_paint(a);
    help_paint(a);          // over the picture, so last of all
    omni_paint(a);
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

bool special_key(App *a, int key, int mods) {
    // The keys below that move the page, told apart from the keys that edit
    // with it. Held down, these are a scroll like any other and are worth the
    // same trade - but not with a text field focused, where the very same keys
    // are walking a caret through it and the picture wants to stay sharp.
    bool moves = !a->insert;
    switch (key) {
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
    case KEY_HOME: case KEY_END: case KEY_PGUP: case KEY_PGDN:
        break;
    default:
        moves = false;
    }
    note_input(a, moves);
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

// An address off the command line, made into one a browser will take. Kept
// apart from what the address bar does with a phrase, because a word with no
// dot in it is a search there and is a host here: `web localhost:8080` means
// the server, and nobody types a search into a shell argument.
static void start_url(const char *raw, char *out, size_t cap) {
    if (strstr(raw, "://") || !strncmp(raw, "about:", 6))
        snprintf(out, cap, "%s", raw);
    else if (!file_url(raw, out, cap))
        snprintf(out, cap, "https://%s", raw);
}

// What the address bar makes of a line: an address as it stands, a path that
// is really there, a bare host, or - a space in it, or no dot anywhere - the
// search nothing else could have been. Apart from navigate itself because the
// address bar now has two ways out of it, and a line typed for a new tab has
// to become the same address it would have become for this one.
void bar_url(const char *raw, char *url, size_t cap) {
    if (strstr(raw, "://") || strncmp(raw, "about:", 6) == 0) {
        snprintf(url, cap, "%s", raw);
    } else if (!file_url(raw, url, cap)) {
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
            snprintf(url, cap, "https://duckduckgo.com/?q=%s", q);
        } else {
            snprintf(url, cap, "https://%s", raw);
        }
    }
}

void navigate(App *a, const char *raw) {
    char url[1100];
    bar_url(raw, url, sizeof url);

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
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"Math.max(document.documentElement.scrollWidth,"
        "document.body?document.body.scrollWidth:0)\",\"returnByValue\":true"),
        RQ_FIT);
}

// The user agent Chrome is calling itself, which has to be known before Chrome
// starts and can only be learned from a Chrome already running. Not a setting
// and not worth a config line: it is a fact about the browser, so it is cached
// beside the browser, in the profile the answer came from.
//
// Nothing else outlives the process. The zoom, the pinned width and the size
// of the window are what this window is doing now - several of them are
// usually up at once, each one somewhere different, and a file they all wrote
// to would only be the last one to quit.
static void ua_path(char *out, size_t cap) {
    char profile[512];
    chrome_profile_path(profile, sizeof profile);
    snprintf(out, cap, "%s/ua", profile);
}

static void load_ua(App *a) {
    char path[600];
    ua_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(a->ua, sizeof a->ua, f)) a->ua[strcspn(a->ua, "\r\n")] = 0;
    fclose(f);
}

static void save_ua(App *a) {
    if (!a->ua[0]) return;
    char path[600];
    ua_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", a->ua);
    fclose(f);
}

// Walk the width the page is told it has. Pinned, it is the width that stays
// put and the window's size decides the magnification instead - the opposite of
// the zoom keys, and the reason it is worth pinning: a layout can be held at
// 360px while the picture of it is made as large as the terminal allows.
static void step_width(App *a, int step) {
    // Unpinned the walk starts from what the cells are giving the page now, so
    // the first press moves from what is on screen rather than from a number.
    int cur = a->want_width > 0 ? a->want_width : a->css_w;
    int want = cur + step * WIDTH_STEP;
    want -= want % WIDTH_STEP;    // round onto the step, whatever it started from
    if (want < WIDTH_MIN) want = WIDTH_MIN;
    if (want > WIDTH_MAX) want = WIDTH_MAX;

    char m[80];
    if (want == a->want_width) {
        snprintf(m, sizeof m, "width %dpx - as %s as it goes",
                 want, step < 0 ? "narrow" : "wide");
        notify(a, m);
        return;
    }
    a->want_width = want;
    a->fit_w = 0;
    relayout(a);
    // The two numbers are one setting seen from two ends, and moving either one
    // moves the other, so both are said whichever end the press came from.
    snprintf(m, sizeof m, "width %dpx - zoom %.0f%%", a->css_w, a->zoom * 100);
    notify(a, m);
    request_fit(a);
}

// Resize the inline window. The page is told about the new size the same way it
// would be told about a dragged window corner: the box sets the cell rect, the
// cell rect sets the viewport, and the layout follows from there. The corner
// being dragged is the bottom right one - the window keeps the row it opened
// on, and grows down and to the right from there.
//
// Two different gestures come through here. `drows`/`dcols` drag one edge and
// leave the other where it is, which is what the arrows do. `scale` asks for
// the other edge to come along in proportion, which is what makes the brackets
// a smaller and a larger window rather than a shorter and a taller one.
static void resize_box(App *a, int drows, int dcols, bool scale) {
    if (!a->inline_mode) {
        notify(a, "--full has no window to resize");
        return;
    }
    Term *t = &a->term;
    int fixed = block_rows(a) - a->box_rows;   // the bar, the status line, the console

    int rows = a->box_rows + drows;
    if (rows < 2) rows = 2;
    if (rows > t->rows - fixed) rows = t->rows - fixed;

    int was_cols = box_cols_now(a, a->box_rows);
    int cols;
    if (dcols) {
        cols = was_cols + dcols;            // that edge, and nothing else
    } else if (!scale) {
        cols = was_cols;                    // this edge, and nothing else
    } else if (a->box_cols > 0) {
        // Scaled from the width the window actually has rather than reset to
        // the default proportion, so a shape chosen by hand is kept and simply
        // gets smaller. Before this, a window whose width had ever been set
        // could only be made shorter - it never got narrower again.
        cols = (int)((double)was_cols * rows / a->box_rows + 0.5);
    } else {
        cols = box_cols_for(a, rows);       // never set: the standard proportion
    }
    if (cols < BOX_MIN_COLS) cols = BOX_MIN_COLS;
    if (cols > t->cols) cols = t->cols;
    if (rows == a->box_rows && cols == was_cols) return;

    a->box_rows = rows;
    // Whatever the width came out as has to be remembered, or the proportion
    // would work it out again from the new height and undo this. The one case
    // that must not be written down is a scaling step on a window that has
    // never had a width of its own: that one is still following the proportion,
    // and should go on doing so.
    if (dcols || !scale || a->box_cols > 0) a->box_cols = cols;

    kitty_clear(&a->kitty);            // the image those cells named is going
    term_clear_inline(t);              // and so are the cells that named it
    term_resize_inline(t, block_rows(a));   // the bar above, the rest below
    a->status_last.len = 0;
    a->tabs_last.len = 0;
    a->console_last.len = 0;
    // The cells were just blanked, so the next frame has to land whatever it
    // looks like: a page that resizes to the same picture would otherwise be
    // dropped as a duplicate and leave the window empty until it moved.
    a->last_hash = 0;
    relayout(a);

    // A pinned width does not move when the window does, so the magnification
    // is the half that changed and the number worth showing next to it.
    char m[64];
    snprintf(m, sizeof m, "window %dx%d - zoom %.0f%%",
             a->css_w, a->css_h, a->zoom * 100);
    notify(a, m);
    request_fit(a);
}

// How big a frame to ask for, against the viewport. This used to step upwards,
// on the idea that drawing larger and coming back down to the cell rect would
// buy detail past what the cells can hold. It cannot: the screencast starts at
// a scale of one and only ever takes the smaller of that and what it was asked
// for, so the viewport is the ceiling and every step above it was a no-op.
// Downwards is the direction that does something, and on a slow link it is the
// direction worth having under a key.
// Auto leads because it is where the key starts and where it comes back to:
// held sizes are the exception, and one of them has to be leaveable.
static void cycle_scale(App *a) {
    static const double SCALES[] = {1.0, 0.75, 0.5};
    int n = (int)(sizeof SCALES / sizeof *SCALES);

    if (a->motion_auto) {
        a->motion_auto = false;
        a->want_scale = SCALES[0];
    } else {
        // Nearest rather than equal: --scale takes any fraction it likes, and
        // the key has to start from wherever that left it.
        int idx = 0;
        double best = 1e9;
        for (int i = 0; i < n; i++) {
            double d = a->want_scale - SCALES[i];
            if (d < 0) d = -d;
            if (d < best) { best = d; idx = i; }
        }
        if (idx == n - 1) {
            a->motion_auto = true;      // round the end and back to auto
            a->want_scale = 1.0;
        } else {
            a->want_scale = SCALES[idx + 1];
        }
    }
    a->in_motion = false;               // whichever way, start from full size
    a->motion_run = 0;
    a->scale_locked = true;             // an explicit ask outranks the width cap
    still_cancel(a);                    // the size it was asked at is not this one
    relayout(a);
    still_soon(a);
    // The size it works out to, because a percentage of a viewport nobody has
    // memorised is not something to picture.
    char m[64];
    if (a->motion_auto)
        snprintf(m, sizeof m, "frame auto - %d%% while moving",
                 (int)(a->motion_scale * 100 + 0.5));
    else
        snprintf(m, sizeof m, "frame %.0f%% - %dx%d", a->want_scale * 100,
                 a->cast_w, a->cast_h);
    notify(a, m);
}

// Start and stop the trace, and bracket it with what it is a trace of. The
// figures either side are the whole point: a picture that costs a megabyte
// thirty times a second is a window that cannot answer the keyboard, and no
// amount of reading the frames one at a time says so as plainly as the totals
// do. Everything between the two marks is in /tmp/web_input.log.
static void trace_toggle(App *a) {
    static unsigned at_frames, at_stills;
    static size_t   at_bytes;
    static double   at_time;

    if (term_tracing()) {
        double secs = now_sec() - at_time;
        unsigned n = a->frames - at_frames;
        size_t bytes = a->total_bytes - at_bytes;
        term_log("=== trace off after %.1fs: %u frames, %u stills, %.1f MB "
                 "base64 (%.1f MB/s), %.0f KB a frame, worst write %.0f ms",
                 secs, n, a->stills - at_stills, bytes / 1048576.0,
                 secs > 0 ? bytes / 1048576.0 / secs : 0,
                 n ? bytes / 1024.0 / n : 0, a->worst_write_ms);
        term_trace(0);
        char m[96];
        snprintf(m, sizeof m, "trace off - %u frames, %.1f MB, worst %.0fms",
                 n, bytes / 1048576.0, a->worst_write_ms);
        notify(a, m);
        return;
    }

    if (!term_trace(1)) {
        notify(a, "could not open /tmp/web_input.log");
        return;
    }
    at_frames = a->frames;
    at_stills = a->stills;
    at_bytes = a->total_bytes;
    at_time = now_sec();
    a->worst_write_ms = 0;
    term_log("\n=== trace on at %.3f: %s", at_time, a->url);
    term_log("=== viewport %dx%d css, cast %dx%d, frame %d, still %d, scale %g, "
             "%s, term %dx%d cells of %dx%d px%s%s",
             a->css_w, a->css_h, a->cast_w, a->cast_h,
             a->frame_w, a->still_w, a->scale,
             a->in_motion ? "moving" : "still", a->term.cols, a->term.rows,
             a->term.cell_w, a->term.cell_h,
             a->kitty.tmux ? ", tmux" : "",
             (getenv("SSH_CONNECTION") || getenv("SSH_TTY")) ? ", ssh" : "");
    notify(a, "trace on - /tmp/web_input.log");
}

// Zoom is a request, not a command: the viewport narrows to magnify, and a page
// that cannot reflow that narrow gets widened back until it fits. Zooming into
// a wide layout would otherwise just push half of it off the screen.
static void zoom_by(App *a, double factor) {
    double before = a->zoom;
    a->zoom *= factor;
    if (a->zoom < 0.4) a->zoom = 0.4;
    if (a->zoom > 4.0) a->zoom = 4.0;
    // A narrow pinned width can leave the zoom past the range these keys walk,
    // and clamping it there would send the press the other way - zooming in to
    // magnify less. A press that cannot go its own way does nothing instead.
    if ((factor > 1.0 && a->zoom < before) || (factor < 1.0 && a->zoom > before))
        a->zoom = before;
    if (a->zoom == before) return;
    // Magnifying is the other half of the same knob, so it takes the width off
    // its pin: from here the window's size decides the width again.
    a->want_width = 0;

    a->fit_w = 0;                 // re-measure from the width just asked for
    relayout(a);

    char m[64];
    snprintf(m, sizeof m, "zoom %.0f%% - width %dpx", a->zoom * 100, a->css_w);
    notify(a, m);
    request_fit(a);
}

void run_js(App *a, const char *js) {
    char esc[2048];
    json_escape(esc, sizeof esc, js);
    app_cdp(a, "Runtime.evaluate", "\"expression\":\"%s\"", esc);
}

// The element a scroll has to move, given a point to look under: the nearest
// scroller above it, and the document's own when there is none. Asking the
// document alone is not enough - an app that scrolls a pane inside itself
// leaves document.scrollingElement the size of the window, and so does a page
// Chrome puts in quirks mode, where scrollingElement is the body while the
// scrolling happens on the html element. Either way the document reports
// nothing to scroll and every call aimed at it does nothing at all.
#define SCROLLER_FN \
    "function hunt(x,y){var e=document.elementFromPoint(x,y);" \
    "while(e){var o=getComputedStyle(e).overflowY;" \
    "if((o==='auto'||o==='scroll')&&e.scrollHeight>e.clientHeight+1)break;" \
    "e=e.parentElement;}return e;}" \
    "function doc(){return document.scrollingElement||document.documentElement;}" \
    "function moves(e){return e&&e.scrollHeight>e.clientHeight+1;}" \
    /* A step moves what is under the pointer, so a pane scrolls where it is */ \
    "function sc(x,y){return hunt(x,y)||doc();}" \
    /* An end means the page, so the document comes first and the pane only  */ \
    /* stands in for it when the document is not what scrolls               */ \
    "function pg(x,y){var d=doc();return moves(d)?d:(hunt(x,y)||d);}"

// One jump, landed on immediately. The scroller under the point is the one that
// moves, so panes and inner scrollers behave the way they look; the target is
// clamped to the ends first, so a step at the top or bottom of a page simply
// does nothing instead of leaving something behind to unwind.
void scroll_at(App *a, int x, int y, int dy) {
    note_input(a, true);
    // The hunt below runs over a document that, for a PDF, is a stub: an empty
    // body and a stylesheet link, with the viewer itself in a frame of another
    // process that this one cannot see. There is no scroller here to find. A
    // wheel event is routed by the browser to whatever is under the point
    // instead, which is the viewer.
    if (a->pdf) {
        app_cdp(a, "Input.dispatchMouseEvent",
                 "\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,"
                 "\"deltaX\":0,\"deltaY\":%d,\"modifiers\":0", x, y, dy);
        return;
    }

    char js[1024];
    snprintf(js, sizeof js,
             "(function(x,y,d){" SCROLLER_FN
             "var t=sc(x,y);"
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

void scroll_by(App *a, int dy) {
    scroll_at(a, a->css_w / 2, a->css_h / 2, dy);
}

// Sideways, which only a width narrower than the layout has any use for. No
// hunt for a scroller: a viewport too narrow for the page overflows the page
// itself, not some pane inside it.
static void scroll_side(App *a, int dx) {
    note_input(a, true);
    if (a->pdf) {
        app_cdp(a, "Input.dispatchMouseEvent",
                 "\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,"
                 "\"deltaX\":%d,\"deltaY\":0,\"modifiers\":0",
                 a->css_w / 2, a->css_h / 2, dx);
        return;
    }
    char js[384];
    snprintf(js, sizeof js,
             "(function(d){"
             "var t=document.scrollingElement||document.documentElement;"
             "var m=t.scrollWidth-t.clientWidth,v=t.scrollLeft+d;"
             "if(v<0)v=0;if(v>m)v=m;"
             "t.scrollTo({left:v,top:t.scrollTop,behavior:'instant'});"
             "})(%d)",
             dx);
    run_js(a, js);
}

// gg and G mean the page, not whatever pane happens to sit under the middle of
// the view: "top" is somewhere you can name, and a step is not.
void scroll_page_end(App *a, bool bottom) {
    note_input(a, true);
    // An end is a distance only the viewer knows: it cannot be asked for as a
    // wheel the way a step can, and the #page= fragment it reads on the way in
    // is ignored once it is up. Home and End are its own, and plain - with
    // ctrl, the pair it takes everywhere else, they do nothing.
    if (a->pdf) {
        if (!a->pdf_clicked) {
            notify(a, "click the pdf first - it takes keys only once clicked");
            return;
        }
        send_key(a, bottom ? 35 : 36, bottom ? "End" : "Home",
                 bottom ? "End" : "Home", NULL, 0);
        return;
    }
    // Aimed at the middle of the view, as a step is, so that a page which
    // scrolls something other than its own document - an app with the article
    // in a pane, a document Chrome reads as quirks mode - ends up where `j`
    // and `k` have been moving all along. Asking the document alone left `gg`
    // and `G` doing nothing at all on those pages.
    char js[1024];
    snprintf(js, sizeof js,
             "(function(x,y,b){" SCROLLER_FN
             "var t=pg(x,y);"
             "t.scrollTo({top:b?t.scrollHeight:0,left:t.scrollLeft,"
             "behavior:'instant'});"
             "})(%d,%d,%d)",
             a->css_w / 2, a->css_h / 2, bottom ? 1 : 0);
    run_js(a, js);
}

// Through the browser's own list rather than history.back(). A page that keeps
// the keyboard, a pdf, an about:blank left behind by a back press: the script
// that would have to run is not always there to run, and asking the browser
// works from all of them. The list also says when there is nowhere to go,
// which a call into the page cannot: it returns whether it moved or not.
void nav_history(App *a, int delta) {
    a->hist_delta = delta;
    app_req_note(a, app_cdp(a, "Page.getNavigationHistory", ""), RQ_HISTORY);
}

// The `n`th element of the array at `arr`, or NULL when the array ends first.
// Strings are stepped over whole, so a brace inside a url cannot be read as
// the start of an element.
static const char *json_array_at(const char *arr, int n) {
    if (!arr || *arr != '[') return NULL;
    int depth = 0, idx = -1;
    for (const char *p = arr; *p; p++) {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++)
                if (*p == '\\' && p[1]) p++;
            if (!*p) return NULL;
            continue;
        }
        if (*p == '[' || *p == '{') {
            if (++depth == 2 && ++idx == n) return p;
        } else if (*p == ']' || *p == '}') {
            if (--depth == 0) return NULL;
        }
    }
    return NULL;
}

static void find_next(App *a, bool backwards) {
    if (!a->find[0]) return;
    // window.find searches this document, and a PDF's text is not in it.
    if (a->pdf) {
        notify(a, "find is not available on a pdf");
        return;
    }
    char js[512], q[256];
    json_escape(q, sizeof q, a->find);      // once for JS, once more for JSON
    snprintf(js, sizeof js,
             "window.find('%s',false,%s,true,false,true,false)",
             q, backwards ? "true" : "false");
    run_js(a, js);
}

// Whether the keyboard belongs to the page: the elements that swallow a
// keystroke rather than letting it mean a command.
#define EDITABLE_FN \
    "function ed(e){if(!e)return false;var t=e.tagName;" \
    "return e.isContentEditable||t==='INPUT'||t==='TEXTAREA'||t==='SELECT';}"

// The page tells us when focus lands on something typable, so j and k scroll
// when you are reading and type themselves when you are filling in a form.
// The listeners go on once per document, whatever else happens: switching tabs
// runs this again against a page that may already be carrying it, and a second
// set of them would report every focus change twice.
//
// This runs in a world of its own - see WEB_WORLD - so `__webmode` and the
// guard below are ours alone. Left in the page's world they would be globals
// with names nothing else has, which is the whole of what a script looking for
// automation is looking for. The DOM is shared either way, so the listeners
// hear the same events from here.
static const char FOCUS_WATCHER[] =
    "(function(){"
    EDITABLE_FN
    "function rep(){try{__webmode(ed(document.activeElement)?'1':'0');}catch(e){}}"
    "if(!window.__webwatch){window.__webwatch=1;"
    "document.addEventListener('focusin',rep,true);"
    "document.addEventListener('focusout',function(){setTimeout(rep,0);},true);}"
    "rep();})()";

// The same question, asked once rather than watched for. A session that has
// just arrived on a document that is already loaded has heard no focus event
// and cannot wait for one. Asked of the page's own world, where it leaves
// nothing behind: it defines no globals and the answer comes back by value.
static const char FOCUS_READ[] =
    "(function(){" EDITABLE_FN
    "return ed(document.activeElement)?'1':'0';})()";

// A key the page does not claim is handed back to the browser process, and on
// macOS that means the menu bar: the keystroke is routed through
// performKeyEquivalent, which validates the whole menu before concluding that
// nothing wanted it. That validation can take seconds, and the thread it runs
// on is the one that dispatches every reply and encodes every frame - so a few
// arrow presses in an image viewer are enough to stop the window drawing at
// all, while clicking the same arrows on screen costs nothing. preventDefault
// is what marks a key as claimed, so this claims the ones whose default action
// is something web does for itself anyway.
//
// This does not take the key away from the page. preventDefault cancels the
// browser's own action - scrolling - and nothing else: every handler the page
// registered still runs, so a viewer that pages through images on an arrow goes
// on doing it.
//
// In the capture phase on the window, which is the first place a key can be
// seen and the one place it cannot be taken away from. It was in the bubble
// phase to begin with, on the reasoning that running last would let the page
// speak first - but a modal that binds the arrows calls stopPropagation, and a
// listener behind that never runs at all. The key then reaches nobody, which
// is exactly the case this is here to catch.
//
// A text field is left alone - the editor claims those keys itself, and
// cancelling them would stop the caret moving.
static const char KEY_CLAIMER[] =
    "(function(){if(window.__webkeys)return;window.__webkeys=1;"
    "var K={ArrowLeft:1,ArrowRight:1,ArrowUp:1,ArrowDown:1};"
    "window.addEventListener('keydown',function(e){"
    "if(!K[e.key])return;"
    "var t=document.activeElement,n=t&&t.tagName;"
    "if(t&&(t.isContentEditable||n==='INPUT'||n==='TEXTAREA'||n==='SELECT'))return;"
    "e.preventDefault();},true);})()";

// The shortest selector that finds the element again: its id where it has one,
// otherwise a CSS path shortened to the first ancestor that is already unique.
// CSS because that is what the console can spend - `document.querySelector` is
// where a picked selector is going.
#define SELECTOR_FN \
    "function ws(e){if(!e||e.nodeType!==1)return '';" \
    "if(e.id)return '#'+CSS.escape(e.id);var p=[];" \
    "while(e&&e.nodeType===1&&e!==document.body){" \
    "var s=e.tagName.toLowerCase(),i=1,q=e;while((q=q.previousElementSibling))" \
    "if(q.tagName===e.tagName)i++;if(i>1)s+=':nth-of-type('+i+')';p.unshift(s);" \
    "var z=p.join(' > ');try{if(document.querySelectorAll(z).length===1)return z}catch(_){}" \
    "e=e.parentElement}return p.join(' > ')}"

// The terminal has said whether anyone is looking. Every frame costs a PNG out
// of Chrome and a base64 write across the terminal, and both are wasted on a
// pane that is not on screen - so the screencast stops with the focus and comes
// back with it. The page itself is left running: timers, sockets and audio
// carry on, which is the difference between not drawing something and freezing
// it, and the reason this stops at the picture.
static void handle_focus(App *a, bool focused) {
    if (!a->pause_on_blur || focused == !a->paused) return;
    if (!focused) {
        // A page just handed in has not been drawn yet, and the blur that says
        // to stop drawing is half of the same click that asked for it.
        if (g_handoff_owed > now_sec()) return;
        a->paused = true;
        a->expect_frame = 0;        // no frame is coming, and none is owed
        a->in_motion = false;       // and it is not moving, it is not drawing
        a->motion_run = 0;
        still_cancel(a);            // nothing to photograph for nobody to see
        app_cdp(a, "Page.stopScreencast", "");
        // Not drawing is the whole of it: the page goes on animating into a
        // screencast nobody is reading, and stopping that is the saving.
        //
        // Emulation.setCPUThrottlingRate is not the other half of this and must
        // not be added back. Chrome emulates a slower processor rather than
        // asking for less work: a thread of its own interrupts the renderer's
        // main thread with a signal, and the handler busy-waits on
        // mach_absolute_time to burn away the share of the quantum the rate
        // says it should not have had. Throttling a blurred window that way
        // spends a whole core doing nothing, answers no javascript and paints
        // nothing - the opposite of what the name promises, and on this side of
        // a blur indistinguishable from a page that has hung.
        return;
    }
    a->paused = false;
    // The page may not have changed while it was away, and an unchanged frame
    // is hash-skipped - which would leave the block empty until something on
    // the page moved. Ask for it as though it were new.
    a->last_hash = 0;
    a->kitty.grid_dirty = true;
    screencast_start(a);
    // The restart is an ask like any other, and a page that did not change
    // while it was away gives Chrome nothing to answer it with.
    still_soon(a);
}

static void handle_mouse(App *a, Event *ev) {
    // Not while a button is held: a drag that wandered up over the bar is the
    // page's until it is let go of, and switching tabs under it would leave the
    // page it started on holding a button nobody released.
    // A click that lands on the key list is a click on the list, not on the page
    // it is covering. A wheel is left alone: it is not a claim on anything.
    if (a->help_open) {
        if (ev->press && !ev->motion && ev->button < 3) help_toggle(a);
        return;
    }
    if (a->omni_open) {
        if (ev->press && !ev->motion && ev->button < 3) omni_close(a);
        return;
    }
    if (!a->mouse_down && tabs_mouse(a, ev)) return;
    if (console_mouse(a, ev)) return;
    Kitty *k = &a->kitty;
    bool inside = ev->mx >= k->x && ev->mx < k->x + k->cols &&
                  ev->my >= k->y && ev->my < k->y + k->rows;
    // Anywhere else on the screen belongs to the shell - until a button is
    // down. From then until it comes back up the pointer is the page's wherever
    // it goes, because the release has to arrive: dropped for landing a row
    // below the picture, it leaves the page holding a button nobody let go of,
    // and every later click extends that abandoned selection instead of
    // starting a new one.
    if (!inside && !a->mouse_down) return;

    // Pointing at the page is asking for the page. The console holds the keyboard
    // from the moment it is opened until something else is clicked, which is
    // what lets a click into a form field be followed by typing into it. A
    // wheel is not a claim on anything: scrolling what you are reading should
    // not take the keyboard away from a half typed command.
    if (ev->press && !ev->motion && ev->button < 3) a->console_focus = false;

    // Aim at the middle of the cell: the terminal only tells us which cell was
    // clicked, so the center is the least wrong point inside it. A drag that
    // has wandered off the picture is answered by the nearest edge cell, which
    // is what a window does with a pointer dragged past its frame.
    int cx = ev->mx, cy = ev->my;
    if (cx < k->x)               cx = k->x;
    if (cx > k->x + k->cols - 1) cx = k->x + k->cols - 1;
    if (cy < k->y)               cy = k->y;
    if (cy > k->y + k->rows - 1) cy = k->y + k->rows - 1;

    double fx = (cx - k->x + 0.5) / (double)k->cols;
    double fy = (cy - k->y + 0.5) / (double)k->rows;
    int x = (int)(fx * a->css_w);
    int y = (int)(fy * a->css_h);

    // Picker mode deliberately consumes the click. Keeping it armed makes it
    // useful for inspecting several controls in a row; `pick` toggles it off.
    if (a->selector_pick && ev->button == 0 && !ev->motion) {
        if (ev->press) {
            char js[2600], esc[5400];
            snprintf(js, sizeof js,
                     "(function(x,y){" SELECTOR_FN
                     "return ws(document.elementFromPoint(x,y))})(%d,%d)", x, y);
            json_escape(esc, sizeof esc, js);
            app_req_note(a, app_cdp(a, "Runtime.evaluate",
                "\"expression\":\"%s\",\"returnByValue\":true", esc), RQ_SELECTOR);
        }
        return;
    }

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
    a->mouse_down = ev->press;
    // Whatever it landed on - a page, the thumbnail rail, the toolbar - the
    // viewer now holds the keyboard, and the keys it knows start working.
    if (a->pdf && ev->press) a->pdf_clicked = true;
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
    if (a->omni_open) {
        omni_paste(a, text, len);
        return;
    }
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
// selected text or fall back to the address. A text field keeps its selection
// to itself - window.getSelection() reads empty while an input has focus - so
// the focused element is asked first, and answers nothing unless it is one.
static void copy_selection(App *a) {
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"(function(){try{var e=document.activeElement;"
        "if(e&&(e.tagName==='INPUT'||e.tagName==='TEXTAREA')&&"
        "e.selectionStart!==e.selectionEnd)"
        "return e.value.substring(e.selectionStart,e.selectionEnd);}catch(x){}"
        "return window.getSelection().toString();})()\","
        "\"returnByValue\":true"), RQ_COPY);
}

// One key, already turned into what it was asked to do. What key that was is
// keys.c's business and none of this function's; false means nothing was done
// here and the key still belongs to whatever is underneath.
static bool do_action(App *a, Event *ev, Act act) {
    switch (act) {
    case ACT_NONE:
    case ACT_INVALID:
        return false;

    case ACT_QUIT: {
        char spec[32];
        key_text(ev->mods, ev->key, spec, sizeof spec);
        term_log("QUIT via %s", spec);
        g_quit = 1;
        return true;
    }

    // ------------------------------------------------------------- moving

    // Chrome moves 40 CSS pixels per arrow press; matching it means a page
    // scrolls here at the speed it does in a window.
    // On a clicked-into PDF the arrows are handed over rather than turned into
    // a scroll, because where they go is the viewer's business: in the document
    // they move the view, and with the thumbnail rail focused they change the
    // page. Neither is something to imitate from here.
    case ACT_LINE_DOWN:
    case ACT_LINE_UP:
        if (a->pdf && a->pdf_clicked && special_key(a, ev->key, ev->mods)) return true;
        scroll_by(a, act == ACT_LINE_DOWN ? 40 : -40);
        return true;
    case ACT_SCROLL_DOWN:  scroll_by(a, 60);  return true;
    case ACT_SCROLL_UP:    scroll_by(a, -60); return true;
    case ACT_SCROLL_RIGHT: scroll_side(a, a->css_w / 4);  return true;
    case ACT_SCROLL_LEFT:  scroll_side(a, -a->css_w / 4); return true;
    case ACT_HALF_DOWN:    scroll_by(a, a->css_h / 2);  return true;
    case ACT_HALF_UP:      scroll_by(a, -(a->css_h / 2)); return true;
    case ACT_PAGE_DOWN:    scroll_by(a, (int)(a->css_h * 0.9));  return true;
    case ACT_PAGE_UP:      scroll_by(a, -(int)(a->css_h * 0.9)); return true;
    // Twice, the way vi asks for it: `gg` is a pair in the key table, so the
    // first `g` is the keyboard waiting rather than anything happening here.
    case ACT_TOP:
        scroll_page_end(a, false);
        return true;
    case ACT_BOTTOM:
        scroll_page_end(a, true);
        return true;

    // --------------------------------------------------------- the page

    case ACT_ADDRESS:
        a->editing = true;
        a->prompt = 1;
        snprintf(a->edit, sizeof a->edit, "%s", a->url);
        a->edit_len = strlen(a->edit);
        return true;
    // The address bar with nothing in it, for going somewhere else rather than
    // editing where you are.
    case ACT_ADDRESS_BLANK:
        a->editing = true;
        a->prompt = 1;
        a->edit[0] = 0;
        a->edit_len = 0;
        return true;
    // The same bar, for a tab that does not exist yet: nothing is put in it,
    // since the page being left is not what is being gone to.
    case ACT_ADDRESS_TAB:
        a->editing = true;
        a->prompt = 3;
        a->edit[0] = 0;
        a->edit_len = 0;
        return true;
    case ACT_FIND:
        a->editing = true;
        a->prompt = 2;
        a->edit_len = 0;
        return true;
    case ACT_FIND_NEXT: find_next(a, false); return true;
    case ACT_FIND_PREV: find_next(a, true);  return true;
    case ACT_HINT:      hint_show(a, 0, false); return true;
    case ACT_HINT_TAB:  hint_show(a, 1, false); return true;
    case ACT_HINT_COPY: hint_show(a, 2, false); return true;
    case ACT_HINT_ALL:  hint_show(a, 0, true);  return true;
    case ACT_SEARCH_TABS:    omni_show(a, OMNI_TABS); return true;
    case ACT_SEARCH_HISTORY: omni_show(a, OMNI_HIST); return true;
    case ACT_BACK:    nav_history(a, -1); return true;
    case ACT_FORWARD: nav_history(a, +1); return true;
    case ACT_RELOAD:
        app_cdp(a, "Page.reload", "\"ignoreCache\":false");
        a->loading = true;
        notify(a, "reloading");
        return true;
    case ACT_RELOAD_HARD:
        app_cdp(a, "Page.reload", "\"ignoreCache\":true");
        a->loading = true;
        notify(a, "reloading, cache ignored");
        return true;
    case ACT_COPY: copy_selection(a); return true;
    case ACT_COPY_URL:
        clipboard_put(a->url);
        notify(a, "copied url");
        return true;
    case ACT_EXTERNAL: open_external(a); return true;
    case ACT_PICK:
        a->selector_pick = !a->selector_pick;
        notify(a, a->selector_pick ? "picking: click for a selector"
                                   : "picking off");
        return true;
    case ACT_INSERT:
        a->insert = true;
        notify(a, "insert mode - esc to leave");
        return true;
    case ACT_INSERT_OFF:
        if (!a->insert) return false;   // reading already: the page can have it
        // Drop focus so the page stops claiming the keyboard.
        run_js(a, "document.activeElement&&document.activeElement.blur()");
        a->insert = false;
        return true;
    // The first field worth typing in, focused from here. Nothing sets insert
    // mode: focusing one is what the page's own watcher reports, and the answer
    // comes back the same way it does when a click lands on one.
    case ACT_FOCUS_INPUT:
        run_js(a,
            "(function(){var l=document.querySelectorAll("
            "'input:not([type=hidden]):not([type=checkbox]):not([type=radio])"
            ":not([type=submit]):not([type=button]):not([type=file]),"
            "textarea,[contenteditable]');"
            "for(var i=0;i<l.length;i++){var e=l[i],r=e.getBoundingClientRect();"
            "if(e.disabled||e.readOnly||r.width<2||r.height<2)continue;"
            "if(r.bottom<0||r.top>innerHeight)continue;"
            "e.focus();return;}})()");
        return true;

    // --------------------------------------------------------------- tabs

    case ACT_TAB_NEW:   tab_new(a);      return true;
    case ACT_TAB_CLOSE: tab_close(a);    return true;
    case ACT_TAB_NEXT:  tab_step(a, +1); return true;
    case ACT_TAB_PREV:  tab_step(a, -1); return true;
    case ACT_TAB_1: case ACT_TAB_2: case ACT_TAB_3:
    case ACT_TAB_4: case ACT_TAB_5: case ACT_TAB_6:
    case ACT_TAB_7: case ACT_TAB_8: case ACT_TAB_9:
        tab_go(a, act - ACT_TAB_1);
        return true;

    // ------------------------------------------------------------- window

    case ACT_ZOOM_IN:  zoom_by(a, 1.25);       return true;
    case ACT_ZOOM_OUT: zoom_by(a, 1.0 / 1.25); return true;
    // Inline draws a window, so these resize it; taking the whole screen there
    // is no window to resize and they zoom instead.
    case ACT_LARGER:
        if (a->inline_mode) resize_box(a, +1, 0, true); else zoom_by(a, 1.25);
        return true;
    case ACT_SMALLER:
        if (a->inline_mode) resize_box(a, -1, 0, true); else zoom_by(a, 1.0 / 1.25);
        return true;
    case ACT_ZOOM_RESET: {
        a->zoom = 1.0;
        a->want_width = 0;         // and the width goes back to the cells
        a->fit_w = 0;
        // The window's own width as well as the page's, so the box goes back to
        // the proportion it opens with rather than to whatever shape it was
        // last nudged into. Dropped from the remembered state too, or the next
        // run reads it straight back in.
        bool was_pinned = a->box_cols > 0;
        a->box_cols = a->want_cols = 0;
        if (was_pinned) {
            // The cells the picture named are about to belong to something
            // narrower, and nothing else will write over the ones it gives up.
            // Same order resize_box uses, and for the same reason.
            kitty_clear(&a->kitty);
            term_clear_inline(&a->term);
            a->status_last.len = 0;
            a->tabs_last.len = 0;
            a->console_last.len = 0;
            a->last_hash = 0;
        }
        relayout(a);
        char m[64];
        snprintf(m, sizeof m, "zoom 100%% - window %d cells, width %dpx",
                 a->kitty.cols, a->css_w);
        notify(a, m);
        request_fit(a);
        return true;
    }
    case ACT_FIT:
        a->fit_width = !a->fit_width;
        if (!a->fit_width) a->fit_w = 0;
        notify(a, a->fit_width ? "fit width on" : "fit width off");
        relayout(a);
        return true;
    case ACT_PAGE_WIDER:    step_width(a, +1); return true;
    case ACT_PAGE_NARROWER: step_width(a, -1); return true;
    case ACT_SCALE:         cycle_scale(a);    return true;
    case ACT_BOX_TALLER:    resize_box(a, +1, 0, false); return true;
    case ACT_BOX_SHORTER:   resize_box(a, -1, 0, false); return true;
    case ACT_BOX_WIDER:     resize_box(a, 0, +BOX_COL_STEP, false); return true;
    case ACT_BOX_NARROWER:  resize_box(a, 0, -BOX_COL_STEP, false); return true;

    // ---------------------------------------------------------- the rest

    case ACT_CONSOLE: console_toggle(a); return true;
    case ACT_HELP:    help_toggle(a);    return true;
    case ACT_STATS:   a->show_stats = !a->show_stats;   return true;
    case ACT_STATUS:  a->hide_status = !a->hide_status; return true;
    case ACT_TRACE:   trace_toggle(a);   return true;
    }
    return false;
}

static void handle_key(App *a, Event *ev) {
    // Ahead of everything: a focused console is where the keyboard is, and quit
    // is the one key that still means what it always did. Along with the key
    // that opens the console, which otherwise could not put it away from
    // inside it: the editor swallows every control key it does not use. Both
    // are looked up rather than spelled out, so moving them moves them here too.
    if (a->console_focus) {
        Act in_console = keys_lookup(ev->mods, ev->key);
        if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) {
            if (in_console == ACT_QUIT)    { g_quit = 1; return; }
            if (in_console == ACT_CONSOLE) { console_toggle(a); return; }
        }
        if (console_key(a, ev)) return;
    }
    // The key list is over the page, so nothing under it can be reached while
    // it is up: the next key scrolls it or puts it away.
    if (help_key(a, ev)) return;
    // The tab and history lists take every key they can use, since a letter is
    // what is being typed at them rather than what it usually means.
    if (omni_key(a, ev)) return;
    // Labels on the links take every key they can use, so a half-typed label
    // can never fall through into the page's own search box.
    if (hint_key(a, ev)) return;
    // --step parks the runner after every line, and this is the key it waits
    // for. Swallowed rather than acted on: the point of it is to let the next
    // line go, and a script being walked through is not one being typed over.
    if (a->script.stepping) {
        a->script.stepping = false;
        return;
    }
    if (a->editing) {
        if (ev->key == KEY_ENTER) {
            a->edit[a->edit_len] = 0;
            a->editing = false;
            if (a->edit_len) {
                if (a->prompt == 2) {
                    snprintf(a->find, sizeof a->find, "%s", a->edit);
                    find_next(a, false);
                } else if (a->prompt == 3) {
                    char url[1100];
                    bar_url(a->edit, url, sizeof url);
                    tab_open_url(a, url);
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

    // A binding may be two keys long, so the key already waiting is part of the
    // question. Whatever the answer is, it has been used up by asking: either
    // the pair is one and has just happened, or the first key is dropped and
    // this one stands on its own.
    bool prefix = false;
    Act act = keys_lookup_seq(a->pend_mods, a->pend_key, ev->mods, ev->key, &prefix);
    a->pend_mods = a->pend_key = 0;

    // Without ctrl, alt or cmd, a key is only ours while reading: a browser you
    // cannot type "j" into is not a browser. Leaving insert mode is the one
    // thing still listened for from inside it, or there is no way back out.
    if (!(ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) && a->insert &&
        act != ACT_INSERT_OFF) {
        act = ACT_NONE;
        prefix = false;                 // and the page gets its `g` as a `g`
    }
    // The start of something longer: nothing has happened yet, and nothing is
    // handed to the page either, since the next key is what says what this was.
    if (prefix) {
        a->pend_mods = ev->mods;
        a->pend_key = ev->key;
        return;
    }
    if (do_action(a, ev, act)) return;

    // An unclaimed cmd chord is the terminal's business, and a page handed one
    // makes nothing of it.
    if (ev->mods & MOD_SUPER) return;
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

        // Whether this frame is as good a picture as a still would be, which is
        // the one question worth asking of it. Chrome captures on its own clock,
        // so a frame rastered before the last size change arrives after it and
        // comes back at the size before last; and the answer is no in any case
        // at a scale the screencast cannot reach, where the cap is the viewport
        // and the still is not. Either way this is what decides whether the
        // still already owed can be called off. The tolerance is wide because
        // rounding moves a pixel or two while the motion scales move a third.
        int pw = png_width(data, dlen);
        bool sharp = pw > 0 && a->frame_w > 0 &&
                     pw * 20 >= a->frame_w * 19 &&
                     a->frame_w * 20 >= a->still_w * 19;
        // A frame is a frame whatever size it came back at: with the sharp
        // picture now fetched rather than waited for, this flag has only one
        // job left, which is to say whether the page is answering at all.
        a->expect_frame = 0;
        a->unwedge_run = 0;

        // Taken before the hash rather than before the write: everything from
        // here on is ours, and the point of the split below is to say how much
        // of the gap between frames is us and how much is Chrome.
        double t_arrive = now_sec();

        if (dlen) {
            uint64_t h = fnv1a(data, dlen);
            if (h != a->last_hash) {
                a->last_hash = h;
                double t0 = now_sec();
                kitty_draw_png(&a->kitty, data, dlen);
                double t1 = now_sec();
                handoff_drawn();

                double prev_draw = a->last_draw;
                double gap = prev_draw > 0 ? t1 - prev_draw : 0;
                if (gap > 0) {
                    double inst = 1.0 / gap;
                    a->fps = a->fps > 0 ? a->fps * 0.7 + inst * 0.3 : inst;
                    double bps = (double)dlen / gap;
                    a->bytes_per_sec = a->bytes_per_sec > 0
                        ? a->bytes_per_sec * 0.7 + bps * 0.3 : bps;
                }
                a->last_bytes = dlen;
                a->last_write_ms = (t1 - t0) * 1000.0;
                a->total_bytes += dlen;
                if (a->last_write_ms > a->worst_write_ms)
                    a->worst_write_ms = a->last_write_ms;
                a->frames++;
                a->last_draw = t1;
                // The gap splits in two at the moment the frame landed: what
                // came before it is Chrome's - capture, encode and the wire -
                // and what came after is ours. Which half is the larger is the
                // whole question, and one number for the gap could not say.
                double chrome_ms = prev_draw > 0
                    ? (t_arrive - prev_draw) * 1000.0 : 0;
                term_log("%.3f frame %u: %dpx of %d, %zu KB b64, chrome %.1f ms, "
                         "ours %.1f ms (write %.1f), gap %.1f ms, %.1f fps%s%s",
                         t1, a->frames, pw, a->frame_w, dlen / 1024, chrome_ms,
                         (t1 - t_arrive) * 1000.0, a->last_write_ms,
                         gap * 1000.0, a->fps, a->in_motion ? " [motion]" : "",
                         sharp ? "" : " [soft]");

                // One quick frame is a click landing; a run of them is the page
                // sliding past. Waiting for the run is what keeps a single
                // keypress from paying for a resolution change it cannot use.
                // A frame at the wrong size is not the page sliding past at
                // all: it is the last size arriving late, and counting it turns
                // the restart that ends a scroll into evidence of another one.
                if (!sharp) {
                    a->motion_run = 0;
                } else if (gap > 0 && gap < MOTION_GAP) {
                    if (a->motion_run < MOTION_RUN) a->motion_run++;
                } else {
                    a->motion_run = 0;
                }
            } else {
                a->skipped++;
            }
        }

        // Ack only once the frame is on screen. Chrome holds the next frame
        // until then, which is the only thing keeping a slow terminal from
        // being buried in frames it cannot draw - and keeps the loop reaching
        // the keyboard between frames. Sending it early was tried, so that
        // Chrome could encode the next frame under our write: it bought
        // nothing, and the note in the README says what that means.
        app_cdp(a, "Page.screencastFrameAck", "\"sessionId\":%d", (int)sid);

        // After the ack rather than before it: dropping the resolution restarts
        // the screencast, and the frame just drawn is still owed an answer on
        // the session it arrived on.
        if (a->motion_auto && !a->in_motion && a->motion_run >= MOTION_RUN) {
            a->in_motion = true;
            term_log("%.3f motion on", now_sec());
            relayout(a);
        }

        // Any soft frame drawn while the page is not moving owes a sharp one.
        // Hung on the frame rather than on the moment motion ends, which is what
        // makes this reliable: a scroll called over early, a transition whose
        // frame never came, a stray repaint at the moving size long afterwards -
        // none of them are special cases any more, because whatever put a soft
        // picture up is the same thing that asks for the sharp one.
        //
        // Not only under auto. A ratio above 1 is the other way a frame comes
        // back softer than it was asked for - the screencast cannot hand over
        // more pixels than the viewport has, and a screenshot can - so --scale 2
        // is a size only a still can deliver, and it is owed one every time.
        if (!a->in_motion)
            a->still_at = sharp ? 0 : now_sec() + STILL_WAIT;

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
        a->pdf = a->pdf_clicked = false;   // until the new document says so
        size_t n;
        // The frame object leads with its own id, and this is the one message
        // that says which frame is the page rather than something inside it.
        const char *f = json_str(msg, "id", &n);
        if (f && n < sizeof a->frame) snprintf(a->frame, sizeof a->frame, "%.*s",
                                               (int)n, f);
        const char *u = json_str(msg, "url", &n);
        // Escaped, like every other string that arrives this way.
        if (u) {
            json_unescape(a->url, sizeof a->url, u, n);
            a->title[0] = 0;
            a->fit_w = 0;              // measured per page
            session_write(a);          // what anything attaching would look for
        }
        return;
    }

    // The address moved without a document being loaded, which is how a page
    // that routes in javascript navigates: clicking a post on x.com, or opening
    // an image viewer, changes the address through the history API and nothing
    // is fetched. There is no frameNavigated for it, so without this the bar
    // goes on naming whatever the window last really loaded - and so does the
    // note on disk, which is what anything attaching from outside reads.
    if (strstr(msg, "Page.navigatedWithinDocument")) {
        size_t n;
        // An advert in a frame of its own routes the same way and is not the
        // window's address. Only checked once the page has said which frame it
        // is; before that there is nothing to check against.
        const char *f = json_str(msg, "frameId", &n);
        if (a->frame[0] && (!f || n != strlen(a->frame) ||
                            memcmp(f, a->frame, n) != 0))
            return;
        const char *u = json_str(msg, "url", &n);
        if (!u) return;
        json_unescape(a->url, sizeof a->url, u, n);
        session_write(a);
        // The document is the same one, so no load event is coming to carry the
        // title the app has just set alongside the address.
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"document.title\",\"returnByValue\":true"), RQ_TITLE);
        return;
    }

    if (strstr(msg, "Runtime.bindingCalled") && strstr(msg, "__webmode")) {
        size_t n;
        const char *p = json_str(msg, "payload", &n);
        a->insert = (p && n && p[0] == '1');
        return;
    }

    if (strstr(msg, "Runtime.bindingCalled") && strstr(msg, "__webhint")) {
        size_t n;
        const char *p = json_str(msg, "payload", &n);
        char pl[2048];
        json_unescape(pl, sizeof pl, p ? p : "", p ? n : 0);
        hint_reply(a, pl);
        return;
    }

    // The document the labels were drawn into is on its way out, and with it
    // everything they were pointing at.
    if (strstr(msg, "Page.frameStartedLoading")) {
        a->loading = true;
        hint_cancel(a);
        return;
    }

    // Either of these means the page has arrived, and neither can be relied on
    // alone: a cold browser can finish loading before Page.enable takes effect,
    // and the load event it would have reported is then never sent. The first
    // one to show up wins and the other finds nothing left to do.
    if (strstr(msg, "Page.loadEventFired") ||
        strstr(msg, "Page.frameStoppedLoading")) {
        if (!a->loading) return;
        a->loading = false;
        a->load_seq++;
        a->last_hash = 0;          // the new page may hash to the old frame
        a->kitty.grid_dirty = true;
        screencast_start(a);
        still_soon(a);             // in case the restart above goes unanswered
        request_fit(a);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"document.title\",\"returnByValue\":true"), RQ_TITLE);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"document.contentType\",\"returnByValue\":true"),
            RQ_PDF);
        return;
    }

    if (strstr(msg, "Page.javascriptDialogOpening")) {
        app_cdp(a, "Page.handleJavaScriptDialog", "\"accept\":false");
        return;
    }

    // A reply always leads with its id; an event never does. Looked for anywhere
    // in the body instead, an id nested in a page's own text could claim one.
    int id = 0;
    if (msg[0] == '{' && !strncmp(msg + 1, "\"id\":", 5))
        id = (int)strtol(msg + 6, NULL, 10);
    switch (id ? app_req_take(a, id) : RQ_NONE) {
    case RQ_FIT: {
        int want = (int)json_num(msg, "value", 0);
        if (want > a->css_w + 8 && want < 8000) {
            // A width asked for by number is a command rather than a request:
            // the page is held at it and magnified to suit, overflow and all.
            // Widening it back would make the narrow widths unreachable, which
            // is the one thing walking down to them is for - so this only says
            // that the page did not fit, and the sideways scroll deals with it.
            if (a->want_width > 0) {
                char m[80];
                snprintf(m, sizeof m, "width %dpx - page needs %dpx",
                         a->css_w, want);
                notify(a, m);
                return;
            }
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

    case RQ_PDF: {
        size_t n;
        const char *v = json_str(msg, "value", &n);
        a->pdf = v && n == 15 && !memcmp(v, "application/pdf", 15);
        return;
    }

    // Read the same way the watcher reports, so both ends say '1' or nothing.
    case RQ_MODE: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        a->insert = v && n && v[0] == '1';
        return;
    }

    case RQ_FRAME: {
        // The tree leads with the top frame, whose own id is the first one in
        // it; searched from there rather than from the front, where the id of
        // the reply itself would answer first.
        const char *tree = strstr(msg, "\"frameTree\"");
        size_t n;
        const char *f = tree ? json_str(tree, "id", &n) : NULL;
        if (f && n < sizeof a->frame)
            snprintf(a->frame, sizeof a->frame, "%.*s", (int)n, f);
        return;
    }

    case RQ_COPY: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
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
        return;
    }

    case RQ_SELECTOR: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        char selector[2048];
        size_t len = (v && n) ? json_unescape(selector, sizeof selector, v, n) : 0;
        if (len) {
            // The selector to read, and a line to run: pressing up in the
            // console gets a query for what was just clicked, which is what
            // the selector was wanted for.
            console_log(a, selector);
            char line[2100];
            snprintf(line, sizeof line, "document.querySelector('%s')", selector);
            console_history_add(a, line);
            notify(a, "selector sent to console");
        } else {
            console_log(a, "error: no element at that point");
        }
        return;
    }

    case RQ_TITLE: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        // The reply is JSON, so the title arrives escaped: the middle dot in
        // GitHub's comes over as a six character u-escape, and stored as it
        // came it would be drawn as those six characters.
        if (v) {
            json_unescape(a->title, sizeof a->title, v, n);
            session_write(a);
        }
        return;
    }

    // Only asked for after an attach: every other way of arriving at a page
    // goes past Page.frameNavigated, which carries the address with it.
    case RQ_URL: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        if (v) {
            json_unescape(a->url, sizeof a->url, v, n);
            session_write(a);
        }
        return;
    }

    case RQ_HISTORY: {
        const char *arr = strstr(msg, "\"entries\":");
        if (arr) { arr += 10; while (*arr == ' ') arr++; }
        int want = (int)json_num(msg, "currentIndex", -1) + a->hist_delta;
        const char *e = (arr && want >= 0) ? json_array_at(arr, want) : NULL;
        int entry = e ? (int)json_num(e, "id", -1) : -1;
        if (entry < 0) {
            notify(a, a->hist_delta < 0 ? "nothing to go back to"
                                        : "nothing to go forward to");
            return;
        }
        app_cdp(a, "Page.navigateToHistoryEntry", "\"entryId\":%d", entry);
        a->loading = true;
        return;
    }

    case RQ_SCRIPT: script_reply(a, msg); return;

    // The page says it has finished painting. Whether it answered or threw,
    // there is nothing more to wait for.
    case RQ_SHOT_READY:
        if (a->shot_state == SHOT_SETTLE) shot_capture(a);
        return;

    case RQ_SHOT:
        if (a->shot_state == SHOT_SENT) shot_write(a, msg);
        return;

    // The sharp picture, come back. Drawn even if the page has started moving
    // again since it was asked for: it is a true picture of the page either
    // way, and the frames now arriving replace it within a frame or two.
    case RQ_STILL:
        still_draw(a, msg);
        return;
    }
}

// ------------------------------------------------------------------ popups

// Whether a target id is one of the pages this window is driving.
static bool tab_target_is(const App *a, const char *id, size_t n) {
    for (int i = 0; i < a->ntabs; i++) {
        const char *t = a->tabs[i].target;
        if (strlen(t) == n && memcmp(t, id, n) == 0) return true;
    }
    return false;
}

static int popup_find(const App *a, const char *id, size_t n) {
    for (int i = 0; i < a->npopups; i++) {
        const char *t = a->popups[i].target;
        if (strlen(t) == n && memcmp(t, id, n) == 0) return i;
    }
    return -1;
}

static void popup_drop(App *a, int i) {
    if (i < 0 || i >= a->npopups) return;
    for (; i + 1 < a->npopups; i++) a->popups[i] = a->popups[i + 1];
    a->npopups--;
}

// The oldest goes when the list is full: a popup that has not said where it is
// going by then has had its fifteen seconds, and the one that just appeared is
// the one a click is waiting on.
static void popup_note(App *a, const char *id, size_t n) {
    if (popup_find(a, id, n) >= 0) return;
    if (a->npopups >= POPUP_MAX) popup_drop(a, 0);
    Popup *p = &a->popups[a->npopups++];
    snprintf(p->target, sizeof p->target, "%.*s", (int)n, id);
    p->at = now_sec();
}

// An address a tab can be pointed at. A popup is made before it is sent
// anywhere, so this is usually blank at first and answered a message later;
// anything else - a blob, a javascript: url, a page written into by the opener -
// belongs to the page that made it and does not survive being reopened.
static bool popup_navigable(const char *url, size_t n) {
    return (n > 8 && memcmp(url, "https://", 8) == 0) ||
           (n > 7 && memcmp(url, "http://", 7) == 0) ||
           (n > 7 && memcmp(url, "file://", 7) == 0);
}

// News off the browser socket: a page appearing, moving, or going away. Only
// the pages our own pages opened are any of this window's business - the rest
// belong to another window sharing this browser, or to the user.
//
// A page is taken only if it was seen appearing, and only for as long as the
// grace: every title change on every page of the browser comes through here, and
// without that a popup left open for an hour would be yanked into a tab the
// moment it happened to navigate.
static void on_target_message(App *a, const char *msg) {
    bool made = strstr(msg, "Target.targetCreated") != NULL;
    bool moved = !made && strstr(msg, "Target.targetInfoChanged") != NULL;
    bool gone = !made && !moved && strstr(msg, "Target.targetDestroyed") != NULL;
    if (!made && !moved && !gone) return;

    size_t n = 0;
    const char *id = json_str(msg, "targetId", &n);
    if (!id || !n || n >= sizeof a->popups[0].target) return;

    if (gone) { popup_drop(a, popup_find(a, id, n)); return; }
    if (tab_target_is(a, id, n)) return;      // one of ours, and already drawn

    size_t tn = 0;
    const char *type = json_str(msg, "type", &tn);
    if (!type || tn != 4 || memcmp(type, "page", 4) != 0) return;

    if (made) {
        // Whose page it is. Discovery replays everything already open, so a
        // browser we adopted arrives as a handful of these; only a page opened
        // by one of ours has an opener we know. The opener survives the implicit
        // noopener of target=_blank - that clears the page's own handle on it,
        // which is a different field - so this holds for ordinary links too.
        size_t on = 0;
        const char *opener = json_str(msg, "openerId", &on);
        bool mine = opener && on && tab_target_is(a, opener, on);
        term_log("%.3f page target %.*s appeared, opener %.*s (%s)", now_sec(),
                 (int)n, id, (int)(opener ? on : 1), opener ? opener : "-",
                 mine ? "ours" : "not ours");
        if (!mine) return;
    }
    int i = popup_find(a, id, n);
    if (!made && i < 0) return;               // not one we saw appear
    if (i >= 0 && now_sec() - a->popups[i].at > POPUP_GRACE) {
        popup_drop(a, i);
        return;
    }

    size_t un = 0;
    const char *u = json_str(msg, "url", &un);
    char url[1100];
    if (!u || !un || !popup_navigable(u, un)) {
        if (made) popup_note(a, id, n);       // it will say in a moment
        return;
    }
    json_unescape(url, sizeof url, u, un);

    char target[96];
    snprintf(target, sizeof target, "%.*s", (int)n, id);
    popup_drop(a, popup_find(a, id, n));
    term_log("%.3f page opened a window at %s", now_sec(), url);
    tab_from_popup(a, target, url);
}

// -------------------------------------------------------------------- main

static void usage(void) {
    fprintf(stderr,
        "usage: web [options] <url>...\n"
        "  A second address, and every one after it, opens in a tab of its\n"
        "  own. The window starts on the first.\n"
        "  --scale F   hold the frame at F of the viewport (0.5 is a quarter of\n"
        "              the data and blurrier; above 1 does nothing, the\n"
        "              screencast will not hand over more than the viewport).\n"
        "              The default is 'auto': full size when the page is still,\n"
        "              smaller while it is moving\n"
        "  --show      run Chrome with a visible window too\n"
        "  --zoom F    page magnification (default 1.5)\n"
        "  --full      take over the whole terminal instead of drawing a window\n"
        "  --rows N    how many cell rows the window gets (default 40)\n"
        "  --cols N    how many cell columns the window gets (default 80)\n"
        "  --no-status start with the status line hidden (^S toggles it)\n"
        "  --no-clear  leave the window on screen on exit instead of erasing it\n"
        "  --mute      start with the page's audio switched off\n"
        "  --eval JS   run javascript in the page and print what it answers\n"
        "  --delay MS  pause between lines of piped javascript\n"
        "  --step      wait for a key between those lines\n"
        "  --timeout S how long a line waits before giving up (default 5)\n"
        "  --json      script output as one JSON object per value\n"
        "  --screenshot F   write the loaded page to F as a png and exit\n"
        "              (- for stdout; the shot waits for the page to arrive)\n"
        "  --login     open a window to sign in with, on the same profile\n"
        "  --keep      leave chrome running on exit so the next start is instant\n"
        "  --open URL  open URL in a tab of the window most recently used,\n"
        "              and exit. Nothing running is an error, so a caller can\n"
        "              fall back to starting one\n"
        "  --endpoint  print every running window as JSON and exit\n"
        "  --browsers  list the chrome processes web has running, with pids,\n"
        "              and say which of them a new window could still adopt\n"
        "  --kill      quit this profile's windows and end its browsers,\n"
        "              including any nothing can reach any more. A window it\n"
        "              cannot place is named rather than ended\n"
        "  --exec CMD  run CMD against this window, its output in the console\n"
        "  --port N    fix chrome's devtools port so playwright can find it\n"
        "  --no-pause  keep drawing while the terminal is not focused\n"
        "  --raw-keys  let a key the page did not want reach the window\n"
        "              system. On macOS that routes it through the menu bar,\n"
        "              which on some pages costs seconds of the thread every\n"
        "              frame and every reply comes from\n");
}

// Everything a fresh CDP session needs before it is worth drawing: the domains
// the events come from, the overrides the picture depends on, and the watcher
// the page reports focus through. None of it survives a change of browser, so
// it lives here rather than inline in main.
void session_init(App *a) {
    app_cdp(a, "Page.enable", "");
    app_cdp(a, "Runtime.enable", "");

    // Which frame is the page. A window that navigates somewhere is told by the
    // event, but one that arrives on a page already loaded - an adopted browser,
    // or a tab switched back to - never gets one, and an address moved by
    // javascript would then have nothing to be checked against.
    app_req_note(a, app_cdp(a, "Page.getFrameTree", ""), RQ_FRAME);

    // A browser we adopted, or the first run against a new Chrome, was started
    // before its own user agent could be read, so it is still saying headless.
    // Overriding it here costs navigator.userAgentData, which the launch flag
    // would have kept - the lesser of the two tells, and only until this
    // browser is replaced by one started the right way.
    if (a->ua_patch_req && a->ua[0]) {
        char esc[1100];
        json_escape(esc, sizeof esc, a->ua);
        app_cdp(a, "Emulation.setUserAgentOverride", "\"userAgent\":\"%s\"", esc);
    }
    // The overlay scrollbar appears on a scroll, waits half a second, then
    // fades out over a dozen compositor frames - and every one of those is a
    // full-page PNG across the terminal. It costs more than everything else
    // scrolling does, and there is nothing to see: the terminal has no pointer
    // to grab it with.
    app_cdp(a, "Emulation.setScrollbarsHidden", "\"hidden\":true");
    {
        char esc[2048];
        json_escape(esc, sizeof esc, FOCUS_WATCHER);
        // Named worlds have to be claimed before the script that lands in one:
        // a binding arriving afterwards is not in the world already built.
        app_cdp(a, "Runtime.addBinding",
                 "\"name\":\"__webmode\",\"executionContextName\":\"%s\"",
                 WEB_WORLD);
        app_cdp(a, "Runtime.addBinding",
                 "\"name\":\"__webrec\",\"executionContextName\":\"%s\"",
                 WEB_WORLD);
        // A registration sticks to the page rather than to the session, so a
        // tab switched back to would collect another copy of it every time.
        // `runImmediately` is what covers the document already on screen: the
        // registration alone would not be run until the next one.
        bool fresh = tab_session_new(a);
        // Its own binding first, for the same reason as the two above: the
        // world is built by the first script to land in it, and a binding
        // registered after that is not in the world already standing.
        hint_install(a, fresh);
        if (fresh)
            app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
                     "\"source\":\"%s\",\"worldName\":\"%s\","
                     "\"runImmediately\":true", esc, WEB_WORLD);
        // In the same world, and registered the same way: a listener added from
        // an isolated world sees the page's events and can cancel them, and
        // nothing it defines is visible to the page.
        if (a->claim_keys && fresh) {
            json_escape(esc, sizeof esc, KEY_CLAIMER);
            app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
                     "\"source\":\"%s\",\"worldName\":\"%s\","
                     "\"runImmediately\":true", esc, WEB_WORLD);
            json_escape(esc, sizeof esc, FOCUS_WATCHER);   // as the next call expects
        }
        // A tab switched back to has its watcher already, and no event to
        // repeat itself with, so the state comes back by asking.
        json_escape(esc, sizeof esc, FOCUS_READ);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"%s\",\"returnByValue\":true", esc), RQ_MODE);
    }
}

// Where a browser we did not navigate already is. Nothing loaded, so no event
// is going to say, and the status line has nothing to show until it is asked.
void ask_where(App *a) {
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"location.href\",\"returnByValue\":true"), RQ_URL);
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"document.title\",\"returnByValue\":true"), RQ_TITLE);
}

// Let go of the browser. It is ours to shut down unless it was someone else's
// to begin with or --keep asked for it to outlive us - and unless another run
// is sharing it, in which case only the tab is ours and the browser goes away
// with the last of us. Left running and undrivable it would hold the profile
// and its memory for nothing.
static void leave_browser(App *a) {
    Chrome *c = &a->chrome;
    chrome_unwatch(c);
    // Nothing here is going to draw another frame, and a page still being
    // captured is work the browser has to get through before it can answer any
    // of the questions below - or shut down when it is told to. On a page that
    // repaints continuously, that is most of the wait between the shell getting
    // its terminal back and the window actually going.
    if (c->ws.fd > 0 && !c->ws.closed) cdp_call(c, "Page.stopScreencast", "");

    // Every tab this window opened, whoever the browser belongs to. They exist
    // because we asked for them, so they go when we do - and closed here rather
    // than below, so the count of what is left is a count of somebody else's.
    tabs_close_others(a);

    if (c->foreign) {                    // the page it was on is not ours
        if (a->ntabs > 0 && a->tabs[a->tab].ours) chrome_close_target(c);
        if (c->ws.fd > 0) ws_close(&c->ws);
        return;
    }

    // Whether this run asked for the browser to stay, or another window did:
    // one window's --keep keeps it for all of them, whichever quits last.
    bool kept = a->keep || chrome_is_kept(c);
    int others = chrome_other_pages(c);
    if (kept) chrome_park(c);             // something for the next run to find
    if (kept || others > 0) {
        chrome_close_target(c);
        if (c->ws.fd > 0) ws_close(&c->ws);
        return;
    }
    chrome_kill_bg(c);
}

// Move onto a browser something else is driving - Playwright's, say - and carry
// on drawing it. The new one is checked before the old one is let go, so a port
// with nothing on it costs nothing but the message.
int app_attach(App *a, int port, char *msg, size_t cap) {
    Chrome *c = &a->chrome;
    if (port < 1 || port > 65535) {
        snprintf(msg, cap, "%d is not a port", port);
        return -1;
    }
    if (port == c->port && c->ws.fd > 0 && !c->ws.closed) {
        snprintf(msg, cap, "already on port %d", port);
        return 0;
    }
    if (chrome_probe(port) != 0) {
        snprintf(msg, cap, "no browser answering on port %d", port);
        return -1;
    }

    leave_browser(a);

    c->pid = 0;
    c->adopted = false;
    c->foreign = true;         // never shut down a browser we did not start
    c->port = port;
    c->ws = (WS){0};
    c->next_id = 1;
    if (chrome_attach(c) < 0) {
        snprintf(msg, cap, "port %d stopped answering; nothing left to draw", port);
        g_quit = 1;
        return -1;
    }

    // Everything below belonged to the browser that just went away: ids start
    // again from one, so a stale request would be claimed by an unrelated reply.
    memset(a->reqs, 0, sizeof a->reqs);
    a->pend.kind = PEND_NONE;
    a->title[0] = 0;
    a->frame[0] = 0;           // session_init asks the new page which frame it is
    a->loading = false;
    a->insert = false;
    a->mouse_down = false;
    a->hint_on = false;        // whatever they were drawn on is not here now
    a->hint_deadline = 0;
    a->pend_key = 0;
    a->fit_w = 0;
    a->last_hash = 0;
    a->kitty.grid_dirty = true;

    // None of the old browser's pages came with us, so the bar starts over on
    // the one page this one has.
    tabs_init(a);
    session_init(a);
    ask_where(a);
    // The viewport override goes on their page too: the frames have to match
    // the cells they are drawn into, whoever is driving.
    relayout(a);

    snprintf(msg, cap, "attached to port %d", port);
    return 0;
}

// The size Chrome should open at, close enough that the page lays out once.
// relayout settles the exact numbers as soon as the terminal is fully set up.
static void first_size(App *a, int *w, int *h) {
    if (!a->has_tty && a->shot_path) {
        *w = SHOT_CSS_W;
        *h = SHOT_CSS_H;
        return;
    }
    int rows = a->inline_mode
        ? (a->want_rows > 0 ? a->want_rows : a->term.rows / 2)
        : a->term.rows - (a->status_open ? 1 : 0);
    if (rows < 1) rows = 1;
    int cols = (a->inline_mode && a->want_cols > 0) ? a->want_cols : a->term.cols;
    if (cols > a->term.cols) cols = a->term.cols;
    if (cols < 1) cols = 1;
    double z = a->zoom > 0 ? a->zoom : 1.0;
    *w = (int)(cols * a->term.cell_w / z);
    *h = (int)(rows * a->term.cell_h / z);
    if (*w < 200) *w = 200;
    if (*h < 200) *h = 200;
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    App a = {0};
    a.want_scale = 1.0;
    // On by default: a third of the bytes and a third of our own time while the
    // page is sliding past, for detail that is not being read at the time.
    // Where the frame has to cross a network, the bytes are the whole cost and
    // the blur is worth more of a bargain.
    a.motion_auto = true;
    a.motion_scale = (getenv("SSH_CONNECTION") || getenv("SSH_TTY"))
        ? MOTION_SCALE_SSH : MOTION_SCALE;
    a.zoom = 1.5;
    a.want_rows = 40;
    a.want_cols = 80;
    a.pause_on_blur = true;
    a.claim_keys = true;              // --raw-keys hands them to the window system
    a.exec_fd = -1;
    a.fit_width = true;
    a.inline_mode = true;             // a window in the shell, unless --full
    a.clear_exit = true;              // the window goes away, unless --no-clear
    load_ua(&a);
    // Every default is in place, so the file is read over exactly what it would
    // otherwise be - and written out of it, the first time, saying the same.
    // Before the terminal is touched, so a complaint about a line of it lands
    // on the shell rather than over the window. The arguments below still win.
    config_load(&a);
    bool show = false, login = false;
    bool endpoint_only = false, browsers_only = false, kill_only = false;
    const char *exec_cmd = NULL, *hand_to_window = NULL;
    double drain_at = 0;              // when the queue first ran out
    int port = 0;                     // 0 = let chrome pick a free one
    const char *eval_js = NULL;
    // Every address on the command line, in the order it was given: the first
    // is where this window goes, and each one after it gets a tab.
    const char *urls[TAB_MAX];
    int nurls = 0, extra_urls = 0;
    // Nowhere, until told. A homepage nobody asked for is a page load, a set of
    // cookies and a network round trip spent before the first key is pressed.
    const char *start = "about:blank";
    script_init(&a);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            const char *v = argv[++i];
            // "auto" asks for the full size when the picture is still and a
            // cheaper one while it is moving, which is the only time the
            // detail it drops is detail nobody could have read anyway.
            if (!strcmp(v, "auto")) { a.motion_auto = true; continue; }
            // A size asked for by number is a size to hold: the picture stays
            // the one that was asked for whether it is moving or not.
            a.motion_auto = false;
            a.want_scale = atof(v);
            // Below 1 the page is rendered smaller than the pixels it is shown
            // in, which is a way to ask for a cheaper frame or a smaller
            // screenshot. The floor is where a viewport stops being a viewport.
            if (a.want_scale < 0.1) a.want_scale = 0.1;
            if (a.want_scale > 3.0) a.want_scale = 3.0;
        } else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
            a.zoom = atof(argv[++i]);
            a.want_width = 0;         // a ratio was asked for, so unpin the width
            if (a.zoom < 0.5) a.zoom = 0.5;
            if (a.zoom > 3.0) a.zoom = 3.0;
        } else if (!strcmp(argv[i], "--inline")) {
            a.inline_mode = true;      // the default; kept so scripts still work
        } else if (!strcmp(argv[i], "--full")) {
            a.inline_mode = false;
        } else if (!strcmp(argv[i], "--rows") && i + 1 < argc) {
            a.want_rows = atoi(argv[++i]);
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--cols") && i + 1 < argc) {
            a.want_cols = atoi(argv[++i]);
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--clear")) {
            a.clear_exit = true;       // the default; kept so scripts still work
        } else if (!strcmp(argv[i], "--no-clear")) {
            a.clear_exit = false;
        } else if (!strcmp(argv[i], "--no-status")) {
            a.hide_status = true;
        } else if (!strcmp(argv[i], "--raw-keys")) {
            a.claim_keys = false;
        } else if (!strcmp(argv[i], "--show")) {
            show = true;
        } else if (!strcmp(argv[i], "--keep")) {
            a.keep = true;
        } else if (!strcmp(argv[i], "--open") && i + 1 < argc) {
            hand_to_window = argv[++i];
        } else if (!strcmp(argv[i], "--endpoint")) {
            endpoint_only = true;
        } else if (!strcmp(argv[i], "--browsers")) {
            browsers_only = true;
        } else if (!strcmp(argv[i], "--kill")) {
            kill_only = true;
        } else if (!strcmp(argv[i], "--exec") && i + 1 < argc) {
            exec_cmd = argv[++i];
        } else if (!strcmp(argv[i], "--mute")) {
            a.mute = true;
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            a.shot_path = argv[++i];
        } else if (!strcmp(argv[i], "--eval") && i + 1 < argc) {
            eval_js = argv[++i];
        } else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            a.script.delay = atof(argv[++i]) / 1000.0;
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            a.script.timeout = atof(argv[++i]);
            if (a.script.timeout < 0.1) a.script.timeout = 0.1;
        } else if (!strcmp(argv[i], "--step")) {
            a.script.step = true;
        } else if (!strcmp(argv[i], "--json")) {
            a.script.json = true;
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port < 1 || port > 65535) {
                fprintf(stderr, "web: --port wants a number from 1 to 65535\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--no-pause")) {
            a.pause_on_blur = false;      // this run; the file is not written back
        } else if (!strcmp(argv[i], "--login")) {
            login = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        // An option that got this far is either not one or is missing what it
        // needs. Left to the branch below it would quietly become the address,
        // and `web --eval` would open a browser on a page called --eval.
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "web: unknown or incomplete option '%s'\n", argv[i]);
            usage();
            return 1;
        } else if (nurls < TAB_MAX) {
            urls[nurls++] = argv[i];
        } else {
            extra_urls++;
        }
    }
    if (extra_urls)
        fprintf(stderr, "web: %d tabs is the limit; the last %d address%s "
                        "ignored\n", TAB_MAX, extra_urls,
                extra_urls == 1 ? " was" : "es were");
    if (nurls) start = urls[0];
    // Questions about, and an end to, what is already running - all answered
    // without starting anything of our own.
    if (endpoint_only) return print_sessions();
    if (browsers_only) return print_browsers();
    if (kill_only)     return kill_everything();
    if (hand_to_window) return hand_url(hand_to_window);

    a.status_open = !a.hide_status;

    // Before anything else opens a descriptor or asks the terminal a question:
    // javascript arriving on stdin is gone the moment something else reads it.
    // Either way it is the whole of what was asked for, so the run ends with
    // it - a file needs no flag of its own, `web url < check.js` being the same
    // thing as piping it in.
    if (eval_js) {
        script_push(&a, eval_js);
        a.script.drain_exit = true;
    } else if (!isatty(STDIN_FILENO)) {
        script_load(&a, "-");
    }

    char first[1200];
    start_url(start, first, sizeof first);
    snprintf(a.url, sizeof a.url, "%s", first);

    term_log("%.3f start", now_sec());
    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_winch);
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_term);
    signal(SIGUSR1, on_hand);

    // Measure the terminal before Chrome starts. Launched at some other size it
    // would lay the page out, paint it, and then have to do both again the
    // moment the real viewport arrived.
    term_probe(&a.term);
    // Two different questions with two different answers in a pipeline: whether
    // there is a terminal to draw the page into, and whether fd 1 is that same
    // terminal - because if it is, writing data there scrolls the picture away.
    a.has_tty = isatty(a.term.fd);
    a.stdout_tty = isatty(STDOUT_FILENO);
    // A png down a terminal is noise. Said now rather than after a browser has
    // been started and a page fetched for a picture with nowhere to go.
    a.shot_stdout = a.shot_path && !strcmp(a.shot_path, "-");
    if (a.shot_stdout && a.stdout_tty) {
        fprintf(stderr, "web: --screenshot - needs somewhere to write to; "
                        "redirect it or name a file\n");
        return 1;
    }
    // The fit pass measures the page against the cell rect and lowers the zoom
    // to what it allows. There is no cell rect here, and the viewport is not
    // something the page gets a say in.
    if (a.shot_path && !a.has_tty) a.fit_width = false;
    int fw, fh;
    first_size(&a, &fw, &fh);

    // A window to sign in with, opened on the same profile and with the same
    // flags every other run uses. The flags are the point: Chrome seals its
    // cookies with a key chosen by the keychain options, so a sign-in done in
    // an ordinary browser leaves a session this one cannot unseal.
    if (login) {
        if (chrome_launch(&a.chrome, first, 1200, 900, true, a.mute, a.ua, false, 0) < 0)
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

    // The address goes on Chrome's own command line, so the fetch is under way
    // while the browser is still starting and while we are still attaching to
    // it: a page's whole time to first byte, overlapped with a couple of
    // hundred milliseconds that were being spent anyway. It costs one extra
    // layout, because the first document is laid out at --window-size rather
    // than at the viewport relayout settles on a moment later.
    //
    // Only with a user agent already in hand. Without one the correction is
    // applied after attaching, which is too late for a document that is already
    // on its way - and the first run against a new Chrome build is exactly when
    // that string is not known yet.
    bool early_url = a.ua[0] != 0;
    if (chrome_launch(&a.chrome, early_url ? first : "about:blank",
                      fw, fh, show, a.mute, a.ua, true, port) < 0)
        return 1;
    term_log("%.3f chrome up on port %d (%s)", now_sec(), a.chrome.port,
             a.chrome.adopted ? "adopted" : "launched");
    if (chrome_attach(&a.chrome) < 0) { chrome_kill(&a.chrome); return 1; }
    term_log("%.3f attached", now_sec());
    tabs_init(&a);              // one tab: whatever the attach landed on
    // Taking over a page the browser opened moves the session onto it, which is
    // what a person clicking a link wanted and the last thing a run with a job
    // to do wants: a shot, or a script, is aimed at the page it was given, and
    // an advert calling open() would have it aimed somewhere else. So only a run
    // that is somebody sitting there watches for them.
    //
    // Best effort, and after the list exists: the first thing the browser says
    // is what is already open, and a page only means something against the tabs.
    if (a.has_tty && !a.shot_path && !a.script.drain_exit &&
        chrome_watch(&a.chrome) < 0)
        term_log("no browser socket; pages that open windows will be missed");
    // Marked now rather than on the way out, so a window that quits before this
    // one already knows the browser has been asked to stay. Somebody else's is
    // never ours to mark: we do not shut it down either way.
    if (a.keep && !a.chrome.foreign) chrome_mark_kept(&a.chrome);
    session_write(&a);          // findable from the moment there is a page

    // Ask the browser what it is calling itself and keep the corrected answer
    // for next time. The launch flag above needs the string before there is a
    // browser to ask, so the first run of a new Chrome build is the one that
    // learns it; every run after that starts out right.
    {
        char ua[512];
        int rc = chrome_user_agent(&a.chrome, ua, sizeof ua);
        if (rc >= 0 && strcmp(ua, a.ua) != 0) {
            snprintf(a.ua, sizeof a.ua, "%s", ua);
            save_ua(&a);
        }
        a.ua_patch_req = rc == 1;   // applied below, once the session is up
    }

    if (a.has_tty) term_enter(&a.term, a.inline_mode);
    // Armed only now, because only now is there a mode to put back: term_enter
    // is what asks for the keyboard, and before it there is nothing a crash
    // would leave behind.
    if (a.has_tty) {
        g_panic_fd = a.term.fd;
        signal(SIGSEGV, on_crash);
        signal(SIGBUS,  on_crash);
        signal(SIGABRT, on_crash);
        signal(SIGILL,  on_crash);
        signal(SIGFPE,  on_crash);
        signal(SIGINT,  on_crash);
    }
    if (a.has_tty && a.inline_mode) {
        int status = a.status_open ? 1 : 0;   // the row below the box, if shown
        int rows = a.want_rows > 0 ? a.want_rows + status : a.term.rows / 2;
        // A height remembered from a taller terminal, or asked for on the
        // command line, comes back down to what this one has - and never takes
        // the last row, which the shell gets its prompt back on.
        if (rows > a.term.rows - 1) rows = a.term.rows - 1;
        if (rows < 4) rows = 4;
        term_reserve_inline(&a.term, rows);
        a.box_rows = rows - status;
        // A width narrower than this terminal, or nothing at all and the
        // proportion decides. Either way it is the same rule the resize keys
        // work under, so relayout is left to apply it.
        if (a.want_cols > 0) a.box_cols = a.want_cols;
    }
    g_app = &a;
    g_input_pump = pump_input;
    bool tmux = getenv("TMUX") != NULL;
    if (a.has_tty) kitty_init(&a.kitty, a.term.fd, tmux);

    session_init(&a);
    // The blank page above is not where we are going - unless the browser was
    // already running on the port we were pointed at and no address was asked
    // for, in which case it is somewhere of its own and loading a first page
    // would take it off whatever that is.
    // A browser we adopted never saw the command line, so it still has to be
    // sent somewhere; one we started is already fetching, and navigating again
    // would throw that head start away and ask for the same page twice.
    bool launched = !a.chrome.adopted && !a.chrome.foreign;
    if (a.chrome.foreign && !nurls) ask_where(&a);
    else if (early_url && launched) a.loading = true;
    else navigate(&a, first);
    // The first request to anywhere real leaves here, which is what the whole
    // startup chain above is in front of. Everything after this is the page's.
    term_log("%.3f navigate %s", now_sec(),
             early_url && launched ? "was on the command line" : "sent");

    // The rest of the command line, one tab apiece. After the first has been
    // sent, so the page somebody is waiting for is already on its way while the
    // others are still being opened - and the window goes back to it at the
    // end, because the first address is the one that was asked for.
    //
    // Not for a run with a job to do: a shot or a script is aimed at one page,
    // and the tabs would be loads paid for and never looked at.
    if (nurls > 1 && !a.shot_path && !a.script.drain_exit) {
        for (int i = 1; i < nurls; i++) {
            char u[1200];
            start_url(urls[i], u, sizeof u);
            if (!tab_open_url(&a, u)) break;
        }
        tab_go(&a, 0);
    }
    // Timed from here rather than from the top of main: what the picture is
    // waiting for is the page, and none of starting a browser was that.
    if (a.shot_path) {
        a.shot_state = SHOT_LOAD;
        a.shot_deadline = now_sec() + SHOT_LOAD_MAX;
    }
    relayout(&a);
    // The vim map is laid under web.conf, so a file that already names one of
    // its keys keeps that key - which from the outside looks like `vim = yes`
    // having done nothing. Said once, here, where it can still be read.
    if (a.vim && a.vim_shadowed) {
        char m[96];
        snprintf(m, sizeof m, "vim: %d key%s kept by web.conf", a.vim_shadowed,
                 a.vim_shadowed == 1 ? "" : "s");
        notify(&a, m);
    }
    draw_panes(&a);

    // Started once the page is on its way and the window has a shape, so a
    // script that attaches immediately finds the viewport it will be driving
    // rather than the one this run began with.
    if (exec_cmd) exec_start(&a, exec_cmd);

    // A script named on the command line, or one piped in. Reading stdin only
    // when it is not a terminal is what keeps `web url` interactive.

    while (!g_quit) {
        if (g_handed) {
            g_handed = 0;
            handoff_take(a.chrome.profile, getpid(), &a);
        }
        if (g_resized) {
            g_resized = 0;
            if (a.inline_mode) {
                term_size(&a.term);
                // Blank the rows the block owns before it goes anywhere. The
                // status line is ordinary text, so a block that lands on
                // different rows - which is what relayout does when the pane no
                // longer fits it - leaves the old one behind, and a few resizes
                // leave a stack of them. The picture below is taken down by
                // name rather than by row, so only this has to go by row.
                term_clear_inline(&a.term);
                // The block stays on the rows it was already on: a terminal
                // keeps what is on its screen where it is, and a pane that
                // grew has only put empty rows underneath. Pinning it to the
                // bottom of the new size instead sent it to the foot of the
                // screen on every resize, away from the command it belongs
                // under. relayout pulls it back up if it no longer fits, which
                // is the only case where it has really moved.
                //
                // Its old cells are still up there whatever it does, wherever
                // the terminal has put them, and they name the image. Placing
                // it again would light those too, and the same page would be
                // up in two places. It comes back under a name they do not
                // know instead.
                kitty_renew(&a.kitty);
            } else {
                writeall(a.term.fd, "\x1b[2J", 4);
                // The clear took the placeholder cells with it, and a resize
                // that lands on the same rect would not otherwise redraw them.
                a.kitty.grid_dirty = true;
            }
            a.status_last.len = 0;      // the screen it was on is gone
            a.tabs_last.len = 0;
            a.console_last.len = 0;
            // Inline, a resize leaves the page the size it was, so the frame
            // that follows is the one already drawn and would be hash-skipped
            // - and the picture, just taken down, would not come back until
            // something on the page moved.
            a.last_hash = 0;
            relayout(&a);
            // relayout may have taken rows off the box to fit it, and the
            // count of what the block owns is what erases it on the way out.
            if (a.inline_mode) {
                a.term.inline_rows = a.img_rows + (a.tabs_open ? 1 : 0) +
                                     (a.status_open ? 1 : 0) + a.console_rows;
                // Now that the block has settled, sweep whatever is under it.
                // A status line pushed off the bottom by a shrink is in the
                // terminal's history by the time we hear about the resize, and
                // growing the pane again brings it back below the new one.
                term_clear_below(&a.term);
            }
            // The picture was taken down above, and relayout only asks for a
            // new one: a terminal that shrank without shrinking the box hands
            // the page metrics it already has, which gives Chrome nothing to
            // answer the restart with. The still is the ask that has a reply,
            // so the window comes back whether or not the page redraws.
            still_soon(&a);
        }

        struct pollfd fds[4] = {{0}};   // poll leaves revents alone on EINTR
        // Without a terminal, term.fd fell back to stdin - which in that case is
        // a script rather than keystrokes. poll ignores a negative fd.
        fds[0].fd = a.has_tty ? a.term.fd : -1;
        fds[0].events = POLLIN;
        fds[1].fd = a.chrome.ws.fd;
        fds[1].events = POLLIN;
        fds[2].fd = a.exec_fd;
        fds[2].events = POLLIN;
        // Never opened, or given up on: the window goes on working without it,
        // and a page that opens a window is simply not heard about again.
        fds[3].fd = a.chrome.watch.fd > 0 ? a.chrome.watch.fd : -1;
        fds[3].events = POLLIN;

        // Nothing here is on a timer except an undecided ESC and an expiring
        // notice, so the loop sleeps until something actually happens.
        // Waiting for the picture to go quiet is waiting for nothing to happen,
        // which is the one thing poll cannot be woken by.
        bool draining = a.script.drain_exit && !script_busy(&a);
        int wait = (a.term.in.len || a.msg_until > now_sec() ||
                    a.expect_frame > 0 || a.hint_deadline > 0 ||
                    draining) ? 20 : -1;
        int sw = script_wait_ms(&a);
        if (sw >= 0 && (wait < 0 || sw < wait)) wait = sw;
        // Everything a pending shot waits for arrives as an event except the
        // deadlines, and those need the loop to come round on its own.
        if (a.shot_path && (wait < 0 || wait > 100)) wait = 100;
        // The picture stopping is the absence of frames, which is the one thing
        // poll cannot be woken by. One wakeup at the deadline is all it takes -
        // and while frames are still arriving they do the waking themselves.
        if (a.in_motion) {
            // Whichever of the two goes quiet last, since motion ends only when
            // both have.
            double f = a.last_draw + MOTION_IDLE;
            double k = a.last_input + MOTION_QUIET;
            double left = (f > k ? f : k) - now_sec();
            int ms = left > 0 ? (int)(left * 1000.0) + 1 : 0;
            if (wait < 0 || ms < wait) wait = ms;
        }
        // A still owed, and a still asked for and not answered, are both the
        // same kind of nothing: no event will arrive to say so.
        for (int i = 0; i < 2; i++) {
            double at = i ? a.still_sent : a.still_at;
            if (at <= 0) continue;
            double left = at - now_sec();
            int ms = left > 0 ? (int)(left * 1000.0) + 1 : 0;
            if (wait < 0 || ms < wait) wait = ms;
        }
        int rc = poll(fds, 4, wait);
        if (rc < 0 && !g_resized) continue;

        if (fds[2].revents & (POLLIN | POLLHUP)) {
            exec_pump(&a);
            draw_panes(&a);
        }

        if (fds[0].revents & POLLIN) {
            term_read(&a.term);
            Event ev;
            while (term_next(&a.term, &ev)) {
                // Mouse events say where and which button rather than key=0:
                // a trace of something that was clicked has to name the click.
                if (ev.type == EV_MOUSE)
                    term_log("%.3f event mouse %s btn=%d cell %d,%d mods=%d",
                             now_sec(), ev.motion ? "move" :
                             ev.press ? "press" : "release",
                             ev.button, ev.mx, ev.my, ev.mods);
                else
                    term_log("%.3f event type=%d key=%d mods=%d text=%s", now_sec(),
                             ev.type, ev.key, ev.mods, ev.text[0] ? ev.text : "");
                if (ev.type == EV_KEY) handle_key(&a, &ev);
                else if (ev.type == EV_MOUSE) handle_mouse(&a, &ev);
                else if (ev.type == EV_FOCUS) handle_focus(&a, ev.press);
                else if (ev.type == EV_PASTE) {
                    if (!console_key(&a, &ev))
                        handle_paste(&a, a.term.paste.p, a.term.paste.len);
                }
                if (g_quit) break;
            }
            // A move held back during the batch goes out now.
            flush_pending(&a);
            draw_panes(&a);
            // Whatever that was, the page should have something to say about
            // it. If it does not, the watchdog below finds out.
            if (a.expect_frame == 0) a.expect_frame = now_sec() + 2.0;
        }

        // Chrome sends the next frame only once the last one is acknowledged,
        // so anything that breaks that chain stops the picture for good while
        // the rest of the session carries on: the address bar still moves, the
        // title still changes, and nothing is drawn again. Restarting the
        // screencast is the one thing that always brings a frame back.
        // Backing off rather than asking again on the same beat, and asking for
        // less. relayout sends the device metrics with it, which is a whole
        // page relayout - and the browser this is aimed at has by now stopped
        // answering anything, so the ask lands on a queue rather than on a
        // page. Every trace of a wedged window shows those calls going out and
        // never coming back. Restarting the screencast is the part that brings
        // a frame back when there is one to bring, and it is much the cheaper
        // of the two.
        // Labels asked for and never reported: the keyboard comes back rather
        // than waiting on a page that is not going to answer.
        hint_tick(&a);

        double due = 3.0 * (1 << (a.unwedge_run < 3 ? a.unwedge_run : 3));
        if (a.expect_frame > 0 && !a.paused && now_sec() > a.expect_frame &&
            now_sec() - a.last_unwedge > due) {
            a.last_unwedge = now_sec();
            a.expect_frame = 0;
            a.unwedge_run++;
            term_log("no frame after acting on input; restarting screencast (%d)",
                     a.unwedge_run);
            a.kitty.grid_dirty = true;
            screencast_start(&a);
            // Said out loud once it is clear this is not a frame running late.
            // A window that has stopped drawing and says nothing is the whole
            // of what a hang looks like from the outside; one that says the
            // page has stopped answering is a different experience of the same
            // fault, and the only part of it this program can fix.
            if (a.unwedge_run == 3)
                notify(&a, "page has stopped answering - ^R reloads, ^Q quits");
        }

        // A socket that has gone is a page that has gone: closed by itself, or
        // by whoever else is driving this browser. With another tab to fall
        // back on that is one tab less rather than the end of the window.
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            if (ws_fill(&a.chrome.ws) < 0) {
                if (!tab_lost(&a)) break;
            } else {
                char *msg;
                size_t len;
                while (ws_next(&a.chrome.ws, &msg, &len) == 1) {
                    on_cdp_message(&a, msg, len);
                    a.chrome.ws.msg.len = 0;
                }
            }
            draw_panes(&a);
        }
        if (a.chrome.ws.closed && !tab_lost(&a)) break;

        // Read after the page socket, because taking over a popup moves the
        // session onto a page of its own: the frame the old one had already
        // handed over is drawn first, rather than thrown away by the move.
        if (fds[3].revents & (POLLIN | POLLHUP)) {
            char *msg;
            size_t len;
            if (ws_fill(&a.chrome.watch) >= 0) {
                while (ws_next(&a.chrome.watch, &msg, &len) == 1) {
                    on_target_message(&a, msg);
                    a.chrome.watch.msg.len = 0;
                }
            }
            // Losing this socket is not losing the window: it costs the news
            // about pages opening, and nothing else.
            if (a.chrome.watch.closed) {
                chrome_unwatch(&a.chrome);
                term_log("%.3f browser socket closed; not watching for popups",
                         now_sec());
            }
            draw_panes(&a);
        }

        // The page has stopped. Going back to full resolution restarts the
        // screencast, which is what puts the sharp frame up: the last one drawn
        // was a moving one, and nothing on the page has to change for it to be
        // replaced.
        // Decided after the socket has been read rather than before it: a frame
        // already sitting in the buffer is the page still moving, and calling
        // the scroll over one pass early is what put a frame captured at the
        // small size on the far side of the size change.
        // Both halves, because either alone is wrong: the frames stop while a
        // trackpad is still coasting, and the input stops while Chrome is
        // dropping frames in the middle of a scroll.
        if (a.in_motion && now_sec() - a.last_draw > MOTION_IDLE &&
            now_sec() - a.last_input > MOTION_QUIET) {
            a.in_motion = false;
            a.motion_run = 0;
            term_log("%.3f motion off", now_sec());
            a.last_hash = 0;        // the same picture, at a size worth drawing
            // The cap has to go back up whether or not a frame comes of it: it
            // is what the next frame will be measured against, and what a still
            // is cancelled by.
            relayout(&a);
            still_soon(&a);
        }

        // The sharp picture, owed and now due. Nothing here waits on Chrome
        // choosing to draw: the ask has a reply, and a reply that does not come
        // is asked for again.
        if (a.still_at > 0 && now_sec() > a.still_at) still_request(&a);

        // Asked for and never answered. Bounded, and the bound hands the
        // problem on rather than dropping it: a page that will not photograph
        // is not a resolution problem any more, it is a page that has stopped
        // answering, which is what the screencast restart below is for.
        if (a.still_sent > 0 && now_sec() > a.still_sent) {
            a.still_sent = 0;
            a.still_tries++;
            term_log("%.3f no still after %.1fs (%d)",
                     now_sec(), STILL_SEND_MAX, a.still_tries);
            if (a.still_tries < STILL_TRIES) {
                a.still_at = now_sec();
            } else {
                a.still_tries = 0;
                a.kitty.grid_dirty = true;
                screencast_start(&a);
                if (a.expect_frame == 0) a.expect_frame = now_sec() + 1.0;
            }
        }

        script_step(&a);
        draw_panes(&a);
        // A script that was the only reason to be here is also the only thing
        // keeping us here.
        // Not while a page is still on its way in, and not before the pause
        // that follows the last command: --delay is what a replay is watched
        // at, and it applies to the end of one as much as the middle.
        // The queue has run out, but the last command may still be arriving.
        // Leaving on the load event is leaving before the frame that carries
        // the result has been drawn, and a fixed delay is either too short for
        // a slow page or wasted on a quick one. What actually says "it got
        // there" is the picture: nothing loading, and no frame that differs
        // from the one on screen for half a second. Capped, because a page with
        // something animating on it never goes quiet at all.
        // Not while --exec is still running either: the script it was given may
        // be the whole reason this window exists, and leaving mid-sentence
        // would take the page out from under it.
        //
        // A run that was asked for a picture waits for that instead, on its own
        // clock: the settle above is measured from a queue that has already run
        // out, and a page still on its way in has not started spending it.
        if (a.shot_path) {
            shot_step(&a);
            if (a.shot_state == SHOT_DONE || a.shot_state == SHOT_FAIL)
                g_quit = 1;
        } else if (a.script.drain_exit && !script_busy(&a) && !a.console_open &&
                   a.exec_fd < 0 && now_sec() >= a.script.next_at) {
            if (drain_at == 0) drain_at = now_sec();
            bool settled = !a.loading && now_sec() - a.last_draw > 0.5;
            if (settled || now_sec() - drain_at > 3.0) g_quit = 1;
        }

        // A lone ESC only resolves on a later pass, once the wait has expired.
        Event ev;
        if (a.has_tty && a.term.in.len && term_next(&a.term, &ev)) {
            if (ev.type == EV_KEY) handle_key(&a, &ev);
            else if (ev.type == EV_MOUSE) handle_mouse(&a, &ev);
            else if (ev.type == EV_FOCUS) handle_focus(&a, ev.press);
            else if (ev.type == EV_PASTE)
                handle_paste(&a, a.term.paste.p, a.term.paste.len);
            if (g_quit) break;      // no frame, and nothing to tell the page
            flush_pending(&a);
            draw_panes(&a);
        }
    }

    // Inline leaves the page behind unless it was asked not to.
    if (a.has_tty) {
        // From here everything written is the handover itself, so it waits for
        // the terminal rather than giving up on it - and the first thing out is
        // the terminator for whatever escape a dropped frame left open, because
        // until that lands the resets after it are read as more of the frame.
        g_write_force = 1;
        kitty_abort(&a.kitty);
        if (!a.inline_mode || a.clear_exit) kitty_clear(&a.kitty);
        kitty_free(&a.kitty);
        term_restore(&a.term, a.clear_exit);
    }
    session_forget(&a);
    // The child is driving the page we are about to take away, so it goes
    // first; anything it still had to say goes to a terminal being handed back.
    if (a.exec_pid > 0) {
        kill(a.exec_pid, SIGTERM);
        waitpid(a.exec_pid, NULL, 0);
        a.exec_pid = 0;
    }
    if (a.exec_fd >= 0) close(a.exec_fd);
    buf_free(&a.exec_buf);
    buf_free(&a.status);
    buf_free(&a.status_last);
    tabs_free(&a);
    console_free(&a);
    help_free(&a);
    omni_free(&a);
    // Most of a cold start is Chrome coming up. Left running, it holds the
    // profile and the next run adopts it instead of paying for that again. A
    // browser we only attached to is not ours to close at all.
    leave_browser(&a);
    // Into the debug log rather than the terminal: quitting should hand the
    // shell back the way it found it.
    term_log("%u frames drawn, %u duplicates skipped", a.frames, a.skipped);
    int rc = a.script.failures ? 1 : 0;
    // A run asked for a picture and quit without one - the browser went away
    // under it, or the terminal did - has failed whatever the script did.
    if (a.shot_path && a.shot_state != SHOT_DONE) rc = 1;
    script_free(&a);
    return rc;
}

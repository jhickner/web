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
#include <sys/wait.h>
#include <unistd.h>
#include "web.h"

volatile sig_atomic_t g_resized = 0;
volatile sig_atomic_t g_quit = 0;

static void on_winch(int sig) { (void)sig; g_resized = 1; }
static void on_term(int sig)  { (void)sig; g_quit = 1; }

static App *g_app;

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
// It is spent on the screencast cap rather than on the device scale factor:
// the frame that arrives is the viewport in CSS pixels whatever the scale
// factor says, so the cap is the only end of it Chrome listens to - and this
// way the page is never re-rastered on the way in or out, only re-scaled.
// The linear scale; the pixels are its square, so 0.65 is 42% of them.
#define MOTION_SCALE 0.65
// Over ssh the bytes are the whole of the cost rather than a third of it, so
// the same trade is worth making harder: half the width is a quarter of them.
#define MOTION_SCALE_SSH 0.5
#define MOTION_RUN   3       // quick frames in a row before it is a scroll
#define MOTION_GAP   0.20    // a frame this soon after the last is still moving
#define MOTION_IDLE  0.30    // and this long without one is stopped

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
    int below = status + a->console_rows;      // everything under the picture
    int rect_cols;
    if (a->inline_mode) {
        // The terminal may have shrunk under the box since it was last sized.
        int max_rows = t->rows - below;
        if (a->box_rows > max_rows) a->box_rows = max_rows;
        if (a->box_rows < 2) a->box_rows = 2;
        a->img_rows = a->box_rows;
        rect_cols = box_cols_now(a, a->img_rows);
        // Keep the whole block, status line included, on the screen. A block
        // whose last row falls past the bottom scrolls the terminal as it is
        // drawn, which lands half the picture at the top and half at the
        // bottom - and the halves never line back up.
        if (t->inline_origin + a->img_rows + below - 1 > t->rows)
            t->inline_origin = t->rows - a->img_rows - below + 1;
        if (t->inline_origin < 1) t->inline_origin = 1;
        kitty_set_rect(&a->kitty, 1, t->inline_origin, rect_cols, a->img_rows);
        a->status_row = t->inline_origin + a->img_rows;
    } else {
        a->img_rows = t->rows - below;
        if (a->img_rows < 1) a->img_rows = 1;
        rect_cols = t->cols;
        kitty_set_rect(&a->kitty, 1, 1, rect_cols, a->img_rows);
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

    // The ratio that turns the viewport into those pixels. Zoom and fit-width
    // both make it fractional, and it is Chrome's job rather than the
    // terminal's: asked for the final size it lays the text out at that size
    // and hints it there, where the terminal could only stretch a bitmap that
    // was already wrong.
    // Motion moves both ends of the size, and it has to move both. The cap is
    // what shrinks the frame, but a cap is not a property of the page: Chrome
    // relayouts and hands a fresh frame over when the device metrics change,
    // and metrics that go out identical give it no reason to draw anything.
    // Moving the cap alone left the small frame on screen with nothing coming
    // to replace it - which is what a short scroll looked like, motion ending
    // and the picture never coming back. Rendering at the size being delivered
    // also stops the page being rastered larger than it is sent.
    double ms = a->in_motion
        ? (a->motion_scale > 0 ? a->motion_scale : MOTION_SCALE) : 1.0;

    double dsf = (double)rect_w * a->scale * ms / (double)a->css_w;

    a->status_last.len = 0;    // the status line may have moved rows

    // Both calls go out every time, including when the numbers have not moved.
    // Restarting the screencast is what makes Chrome hand over a fresh frame,
    // so this is the only way anything asking for a redraw gets one.
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
    a->cast_w = (int)((double)a->css_w * a->scale * ms + 0.5);
    a->cast_h = (int)((double)a->css_h * a->scale * ms + 0.5);
    if (a->cast_w < 1) a->cast_w = 1;
    if (a->cast_h < 1) a->cast_h = 1;
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

static void session_write(App *a) {
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
    fprintf(f, "{\"pid\":%d,\"port\":%d,\"cdp\":\"http://127.0.0.1:%d\","
               "\"target\":\"%s\",\"url\":\"%s\",\"title\":\"%s\"}\n",
            (int)getpid(), a->chrome.port, a->chrome.port,
            a->chrome.target, url, title);
    fclose(f);
}

static void session_forget(App *a) {
    if (!a->chrome.profile[0]) return;
    char path[700];
    session_file(a, path, sizeof path);
    unlink(path);
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

// The address bar and the find prompt are drawn on the status line, so a line
// that has been hidden comes back for as long as one of them is open.
static void status_sync(App *a) {
    bool want = !a->hide_status || a->editing;
    int  rows = console_rows(a);
    if (want == a->status_open && rows == a->console_rows) return;
    a->status_open = want;
    a->console_rows = rows;
    a->status_last.len = 0;
    a->console_last.len = 0;
    kitty_clear(&a->kitty);          // the rows it lived on change hands
    // Inline, the page itself is not resized by this, so the frame that comes
    // back is the one already on screen - and a duplicate is normally dropped,
    // which would leave the block empty for as long as the page sits still.
    a->last_hash = 0;
    if (a->inline_mode)
        term_resize_inline(&a->term, a->box_rows + (want ? 1 : 0) + rows);
    else
        writeall(a->term.fd, "\x1b[2J", 4);
    relayout(a);
}

// Called after every input batch and every frame, so it keeps its buffer and
// stays quiet when the line has not changed: an unnecessary repaint here lands
// in the middle of a stream of image data.
static void draw_status(App *a) {
    if (!a->has_tty) return;
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
        // Two lengths of the same list. Copy earns its place in both: the page
        // is an image, so the terminal's own copy has nothing to take, and a
        // key nobody can guess is a key nobody uses.
        static const char KEYS[]   = "^L url  ^O back  ^R reload  ^Y copy  ^Q quit";
        static const char KEYS_S[] = "^L url  ^Y copy  ^Q quit";

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
                     "cell %dx%d cast %dx%d%s",
                     a->chrome.port,
                     a->last_bytes / 1024, a->last_write_ms, a->fps,
                     a->css_w, a->css_h, a->scale,
                     100.0 * (sw * a->term.cell_w) / (a->css_w ? a->css_w : 1),
                     a->term.cell_w, a->term.cell_h, a->cast_w, a->cast_h,
                     a->in_motion ? " moving" : "");
            hint = stats;
        } else {
            hint = sw > (int)sizeof KEYS + 3 ? KEYS : KEYS_S;
        }
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

static void draw_panes(App *a) {
    draw_status(a);
    console_paint(a);
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

void navigate(App *a, const char *raw) {
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
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"Math.max(document.documentElement.scrollWidth,"
        "document.body?document.body.scrollWidth:0)\",\"returnByValue\":true"),
        RQ_FIT);
}

// What is worth outliving the process: the zoom, the pinned width and the
// height of the inline window, which belong to the terminal they are being read
// in rather than to any page, and the user agent, which has to be known before
// Chrome starts and can only be learned from a Chrome already running. All of
// it is keyed to nothing - one browser, one terminal, one file.
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
        } else if (!strncmp(line, "width=", 6)) {
            int w = atoi(line + 6);
            if (w >= WIDTH_MIN && w <= WIDTH_MAX) a->want_width = w;
        } else if (!strncmp(line, "rows=", 5)) {
            int r = atoi(line + 5);
            if (r >= 2 && r <= 500) a->want_rows = r;
        } else if (!strncmp(line, "cols=", 5)) {
            int c = atoi(line + 5);
            if (c >= BOX_MIN_COLS && c <= 2000) a->want_cols = c;
        } else if (!strncmp(line, "ua=", 3) && line[3]) {
            snprintf(a->ua, sizeof a->ua, "%s", line + 3);
        } else if (!strncmp(line, "pause_on_blur=", 14)) {
            a->pause_cfg = a->pause_on_blur = atoi(line + 14) != 0;
        } else if (!strncmp(line, "blur_cpu_rate=", 14)) {
            int r = atoi(line + 14);
            if (r >= 1 && r <= 100) a->blur_cpu_rate = r;
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
    // A pinned width outranks the zoom on the way back in: it is a width the
    // page was held at rather than a ratio of a terminal that may be gone, so
    // it means the same thing in the next terminal and the ratio does not.
    if (a->want_width > 0) fprintf(f, "width=%d\n", a->want_width);
    // --full has no window of its own, so it carries whatever was stored for
    // the inline one through rather than dropping it.
    int rows = a->box_rows > 0 ? a->box_rows : a->want_rows;
    if (rows > 0) fprintf(f, "rows=%d\n", rows);
    // Only a width that was asked for. Left out, the window opens at the
    // proportion it always has, which is the right answer for a terminal that
    // is not the one the number came from.
    int cols = a->box_cols > 0 ? a->box_cols : a->want_cols;
    if (cols > 0) fprintf(f, "cols=%d\n", cols);
    // What the file said, not what this run is doing: --no-pause is for one
    // session, and a flag that quietly rewrote the setting would outlive it.
    fprintf(f, "pause_on_blur=%d\n", a->pause_cfg ? 1 : 0);
    fprintf(f, "blur_cpu_rate=%d\n", a->blur_cpu_rate);
    if (a->ua[0]) fprintf(f, "ua=%s\n", a->ua);
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
    save_state(a);
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
static void resize_box(App *a, int drows, int dcols) {
    if (!a->inline_mode) {
        notify(a, "--full has no window to resize");
        return;
    }
    Term *t = &a->term;
    int status = a->status_open ? 1 : 0;

    int rows = a->box_rows + drows;
    if (rows < 2) rows = 2;
    if (rows > t->rows - status) rows = t->rows - status;

    int was_cols = box_cols_now(a, a->box_rows);
    int cols = box_cols_now(a, rows) + dcols;
    if (cols < BOX_MIN_COLS) cols = BOX_MIN_COLS;
    if (cols > t->cols) cols = t->cols;
    if (rows == a->box_rows && cols == was_cols) return;

    a->box_rows = rows;
    // Only a sideways nudge pins the width. Until one arrives the window keeps
    // its proportion as it grows, which is what makes the height key alone
    // behave the way it always has.
    if (dcols) a->box_cols = cols;

    kitty_clear(&a->kitty);            // the image those cells named is going
    term_clear_inline(t);              // and so are the cells that named it
    term_resize_inline(t, rows + status);   // the status line sits below
    a->status_last.len = 0;
    // The cells were just blanked, so the next frame has to land whatever it
    // looks like: a page that resizes to the same picture would otherwise be
    // dropped as a duplicate and leave the window empty until it moved.
    a->last_hash = 0;
    relayout(a);
    save_state(a);

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
    relayout(a);
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
    // Only what was asked for is remembered. The fit pass below can lower
    // a->zoom to whatever a stubbornly wide page allows, and saving that would
    // let one such page quietly become the setting for every later run.
    save_state(a);

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

// One jump, landed on immediately. The scroller under the point is the one that
// moves, so panes and inner scrollers behave the way they look; the target is
// clamped to the ends first, so a step at the top or bottom of a page simply
// does nothing instead of leaving something behind to unwind.
void scroll_at(App *a, int x, int y, int dy) {
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

void scroll_by(App *a, int dy) {
    scroll_at(a, a->css_w / 2, a->css_h / 2, dy);
}

// Sideways, which only a width narrower than the layout has any use for. No
// hunt for a scroller: a viewport too narrow for the page overflows the page
// itself, not some pane inside it.
static void scroll_side(App *a, int dx) {
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
    run_js(a, bottom
        ? "(function(t){t.scrollTo({top:t.scrollHeight,behavior:'instant'});})"
          "(document.scrollingElement||document.documentElement)"
        : "(function(t){t.scrollTo({top:0,behavior:'instant'});})"
          "(document.scrollingElement||document.documentElement)");
}

void nav_history(App *a, int delta) {
    run_js(a, delta < 0 ? "history.back()" : "history.forward()");
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
        a->paused = true;
        a->expect_frame = 0;        // no frame is coming, and none is owed
        app_cdp(a, "Page.stopScreencast", "");
        // Not drawing is only half of it: the page goes on animating into a
        // screencast nobody is reading, and on a WebGL demo that is the whole
        // cost. Throttling the renderer slows what it asks for, so the raster
        // behind it falls away too. Audio is decoded off this thread, so
        // something you switched away from to keep listening to keeps playing.
        if (a->blur_cpu_rate > 1)
            app_cdp(a, "Emulation.setCPUThrottlingRate", "\"rate\":%d",
                    a->blur_cpu_rate);
        return;
    }
    a->paused = false;
    if (a->blur_cpu_rate > 1)
        app_cdp(a, "Emulation.setCPUThrottlingRate", "\"rate\":1");
    // The page may not have changed while it was away, and an unchanged frame
    // is hash-skipped - which would leave the block empty until something on
    // the page moved. Ask for it as though it were new.
    a->last_hash = 0;
    a->kitty.grid_dirty = true;
    screencast_start(a);
}

static void handle_mouse(App *a, Event *ev) {
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

static void handle_key(App *a, Event *ev) {
    // Ahead of everything: a focused console is where the keyboard is, and ^Q is
    // the one key that still means what it always did.
    if (a->console_focus) {
        if (ev->mods == MOD_CTRL && ev->key == 'q') { g_quit = 1; return; }
        // And ^X, or the key that opens the console cannot put it away from
        // inside it: the editor swallows every control key it does not use.
        if (ev->mods == MOD_CTRL && ev->key == 'x') { console_toggle(a); return; }
        if (console_key(a, ev)) return;
    }
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
        case 'x': console_toggle(a); return;
        case 'r':
            app_cdp(a, "Page.reload", "\"ignoreCache\":false");
            a->loading = true;
            notify(a, "reloading");
            return;
        case 'o': nav_history(a, -1); return;
        case 'p': nav_history(a, +1); return;
        case 'e': open_external(a); return;
        }
    }

    // With a text field focused the page gets everything: a browser you cannot
    // type "j" into is not a browser.
    if (!a->insert && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        int page = (int)(a->css_h * 0.9);
        int half = a->css_h / 2;

        if (a->pending_g && ev->key != 'g') a->pending_g = false;

        // Shift and an arrow drag the window's bottom right corner: down and
        // right let it out, up and left take it back. Taken before the table
        // below, where the same arrows without shift scroll and go back.
        if (ev->mods & MOD_SHIFT) {
            switch (ev->key) {
            case KEY_DOWN:  resize_box(a, +1, 0); return;
            case KEY_UP:    resize_box(a, -1, 0); return;
            case KEY_RIGHT: resize_box(a, 0, +BOX_COL_STEP); return;
            case KEY_LEFT:  resize_box(a, 0, -BOX_COL_STEP); return;
            }
        }

        switch (ev->key) {
        // Chrome moves 40 CSS pixels per arrow press; matching it means a page
        // scrolls here at the speed it does in a window.
        // On a clicked-into PDF the arrows are handed over rather than turned
        // into a scroll, because where they go is the viewer's business: in
        // the document they move the view, and with the thumbnail rail focused
        // they change the page. Neither is something to imitate from here.
        case KEY_DOWN:
        case KEY_UP:
            if (a->pdf && a->pdf_clicked) { special_key(a, ev->key, ev->mods); return; }
            scroll_by(a, ev->key == KEY_DOWN ? 40 : -40);
            return;
        case KEY_LEFT:  nav_history(a, -1); return;
        case KEY_RIGHT: nav_history(a, +1); return;
        case 'j': scroll_by(a, 60);    return;
        case 'k': scroll_by(a, -60);   return;
        case 'l': scroll_side(a, a->css_w / 4);  return;
        case 'h': scroll_side(a, -a->css_w / 4); return;
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
            if (a->inline_mode) resize_box(a, -1, 0); else zoom_by(a, 1.0 / 1.25);
            return;
        case ']':
            if (a->inline_mode) resize_box(a, +1, 0); else zoom_by(a, 1.25);
            return;
        // The same four, for a terminal that keeps the shifted arrows to itself.
        case 'D': resize_box(a, +1, 0); return;
        case 'U': resize_box(a, -1, 0); return;
        case 'R': resize_box(a, 0, +BOX_COL_STEP); return;
        case 'L': resize_box(a, 0, -BOX_COL_STEP); return;
        case 'P':
            a->selector_pick = !a->selector_pick;
            notify(a, a->selector_pick ? "picking: click for a selector"
                                       : "picking off");
            return;
        case 'w': step_width(a, +1); return;
        case 'W': step_width(a, -1); return;
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
        case ':':
            console_toggle(a);
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
            a->want_width = 0;         // and the width goes back to the cells
            a->fit_w = 0;
            relayout(a);
            save_state(a);
            char m[64];
            snprintf(m, sizeof m, "zoom 100%% - width %dpx", a->css_w);
            notify(a, m);
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
                a->frames++;
                a->last_draw = t1;
                // The gap splits in two at the moment the frame landed: what
                // came before it is Chrome's - capture, encode and the wire -
                // and what came after is ours. Which half is the larger is the
                // whole question, and one number for the gap could not say.
                double chrome_ms = prev_draw > 0
                    ? (t_arrive - prev_draw) * 1000.0 : 0;
                term_log("%.3f frame %u: %zu KB b64, chrome %.1f ms, "
                         "ours %.1f ms (write %.1f), gap %.1f ms, %.1f fps%s",
                         t1, a->frames, dlen / 1024, chrome_ms,
                         (t1 - t_arrive) * 1000.0, a->last_write_ms,
                         gap * 1000.0, a->fps, a->in_motion ? " [motion]" : "");

                // One quick frame is a click landing; a run of them is the page
                // sliding past. Waiting for the run is what keeps a single
                // keypress from paying for a resolution change it cannot use.
                if (gap > 0 && gap < MOTION_GAP) {
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
            // A size change that produces no frame leaves the wrong picture up
            // with nothing coming for it, so the watchdog is told to expect one.
            if (a->expect_frame == 0) a->expect_frame = now_sec() + 1.0;
        }

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
        const char *u = json_str(msg, "url", &n);
        if (u && n < sizeof a->url) {
            memcpy(a->url, u, n);
            a->url[n] = 0;
            a->title[0] = 0;
            a->fit_w = 0;              // measured per page
            session_write(a);          // what anything attaching would look for
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
        if (v && n && n < sizeof a->title) {
            memcpy(a->title, v, n);
            a->title[n] = 0;
            session_write(a);
        }
        return;
    }

    // Only asked for after an attach: every other way of arriving at a page
    // goes past Page.frameNavigated, which carries the address with it.
    case RQ_URL: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        if (v && n && n < sizeof a->url) {
            memcpy(a->url, v, n);
            a->url[n] = 0;
            session_write(a);
        }
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
    }
}

// -------------------------------------------------------------------- main

static void usage(void) {
    fprintf(stderr,
        "usage: web [options] <url>\n"
        "  --scale F   hold the frame at F of the viewport (0.5 is a quarter of\n"
        "              the data and blurrier; above 1 does nothing, the\n"
        "              screencast will not hand over more than the viewport).\n"
        "              The default is 'auto': full size when the page is still,\n"
        "              smaller while it is moving\n"
        "  --show      run Chrome with a visible window too\n"
        "  --zoom F    page magnification (default 1.0)\n"
        "  --full      take over the whole terminal instead of drawing a window\n"
        "  --rows N    how many cell rows the window gets\n"
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
        "  --endpoint  print every running window as JSON and exit\n"
        "  --exec CMD  run CMD against this window, its output in the console\n"
        "  --port N    fix chrome's devtools port so playwright can find it\n"
        "  --no-pause  keep drawing while the terminal is not focused\n");
}

// Everything a fresh CDP session needs before it is worth drawing: the domains
// the events come from, the overrides the picture depends on, and the watcher
// the page reports focus through. None of it survives a change of browser, so
// it lives here rather than inline in main.
static void session_init(App *a) {
    app_cdp(a, "Page.enable", "");
    app_cdp(a, "Runtime.enable", "");

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
        app_cdp(a, "Runtime.addBinding", "\"name\":\"__webmode\"");
        app_cdp(a, "Runtime.addBinding", "\"name\":\"__webrec\"");
        app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
                 "\"source\":\"%s\"", esc);
        app_cdp(a, "Runtime.evaluate", "\"expression\":\"%s\"", esc);
    }
}

// Where a browser we did not navigate already is. Nothing loaded, so no event
// is going to say, and the status line has nothing to show until it is asked.
static void ask_where(App *a) {
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
    if (c->foreign) {                    // never ours, not even the tab
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
    a->loading = false;
    a->insert = false;
    a->mouse_down = false;
    a->fit_w = 0;
    a->last_hash = 0;
    a->kitty.grid_dirty = true;

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
    double z = a->zoom > 0 ? a->zoom : 1.0;
    *w = (int)(a->term.cols * a->term.cell_w / z);
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
    a.zoom = 1.0;
    a.pause_on_blur = a.pause_cfg = true;
    a.blur_cpu_rate = 20;
    a.exec_fd = -1;
    load_state(&a);                   // --zoom below still wins over it
    a.fit_width = true;
    a.inline_mode = true;             // a window in the shell, unless --full
    a.clear_exit = true;              // the window goes away, unless --no-clear
    bool show = false, login = false, url_given = false;
    bool endpoint_only = false;
    const char *exec_cmd = NULL;
    double drain_at = 0;              // when the queue first ran out
    int port = 0;                     // 0 = let chrome pick a free one
    const char *eval_js = NULL;
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
        } else if (!strcmp(argv[i], "--clear")) {
            a.clear_exit = true;       // the default; kept so scripts still work
        } else if (!strcmp(argv[i], "--no-clear")) {
            a.clear_exit = false;
        } else if (!strcmp(argv[i], "--no-status")) {
            a.hide_status = true;
        } else if (!strcmp(argv[i], "--show")) {
            show = true;
        } else if (!strcmp(argv[i], "--keep")) {
            a.keep = true;
        } else if (!strcmp(argv[i], "--endpoint")) {
            endpoint_only = true;
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
            a.pause_on_blur = false;      // this run only; the file keeps its own
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
        } else {
            start = argv[i];
            url_given = true;
        }
    }
    // A question about the windows already running, answered without starting
    // anything of our own.
    if (endpoint_only) return print_sessions();

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
            save_state(&a);
        }
        a.ua_patch_req = rc == 1;   // applied below, once the session is up
    }

    if (a.has_tty) term_enter(&a.term, a.inline_mode);
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
    if (a.chrome.foreign && !url_given) ask_where(&a);
    else if (early_url && launched) a.loading = true;
    else navigate(&a, first);
    // The first request to anywhere real leaves here, which is what the whole
    // startup chain above is in front of. Everything after this is the page's.
    term_log("%.3f navigate %s", now_sec(),
             early_url && launched ? "was on the command line" : "sent");
    // Timed from here rather than from the top of main: what the picture is
    // waiting for is the page, and none of starting a browser was that.
    if (a.shot_path) {
        a.shot_state = SHOT_LOAD;
        a.shot_deadline = now_sec() + SHOT_LOAD_MAX;
    }
    relayout(&a);
    draw_panes(&a);

    // Started once the page is on its way and the window has a shape, so a
    // script that attaches immediately finds the viewport it will be driving
    // rather than the one this run began with.
    if (exec_cmd) exec_start(&a, exec_cmd);

    // A script named on the command line, or one piped in. Reading stdin only
    // when it is not a terminal is what keeps `web url` interactive.

    while (!g_quit) {
        if (g_resized) {
            g_resized = 0;
            if (a.inline_mode) {
                term_size(&a.term);
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
            // Inline, a resize leaves the page the size it was, so the frame
            // that follows is the one already drawn and would be hash-skipped
            // - and the picture, just taken down, would not come back until
            // something on the page moved.
            a.last_hash = 0;
            relayout(&a);
            // relayout may have taken rows off the box to fit it, and the
            // count of what the block owns is what erases it on the way out.
            if (a.inline_mode)
                a.term.inline_rows = a.img_rows +
                                     (a.status_open ? 1 : 0) + a.console_rows;
        }

        struct pollfd fds[3] = {{0}};   // poll leaves revents alone on EINTR
        // Without a terminal, term.fd fell back to stdin - which in that case is
        // a script rather than keystrokes. poll ignores a negative fd.
        fds[0].fd = a.has_tty ? a.term.fd : -1;
        fds[0].events = POLLIN;
        fds[1].fd = a.chrome.ws.fd;
        fds[1].events = POLLIN;
        fds[2].fd = a.exec_fd;
        fds[2].events = POLLIN;

        // Nothing here is on a timer except an undecided ESC and an expiring
        // notice, so the loop sleeps until something actually happens.
        // Waiting for the picture to go quiet is waiting for nothing to happen,
        // which is the one thing poll cannot be woken by.
        bool draining = a.script.drain_exit && !script_busy(&a);
        int wait = (a.term.in.len || a.msg_until > now_sec() ||
                    a.expect_frame > 0 || draining) ? 20 : -1;
        int sw = script_wait_ms(&a);
        if (sw >= 0 && (wait < 0 || sw < wait)) wait = sw;
        // Everything a pending shot waits for arrives as an event except the
        // deadlines, and those need the loop to come round on its own.
        if (a.shot_path && (wait < 0 || wait > 100)) wait = 100;
        // The picture stopping is the absence of frames, which is the one thing
        // poll cannot be woken by. One wakeup at the deadline is all it takes -
        // and while frames are still arriving they do the waking themselves.
        if (a.in_motion) {
            double left = a.last_draw + MOTION_IDLE - now_sec();
            int ms = left > 0 ? (int)(left * 1000.0) + 1 : 0;
            if (wait < 0 || ms < wait) wait = ms;
        }
        int rc = poll(fds, 3, wait);
        if (rc < 0 && !g_resized) continue;

        if (fds[2].revents & (POLLIN | POLLHUP)) {
            exec_pump(&a);
            draw_panes(&a);
        }

        if (fds[0].revents & POLLIN) {
            term_read(&a.term);
            Event ev;
            while (term_next(&a.term, &ev)) {
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
        if (a.expect_frame > 0 && !a.paused && now_sec() > a.expect_frame &&
            now_sec() - a.last_unwedge > 3.0) {
            a.last_unwedge = now_sec();
            a.expect_frame = 0;
            term_log("no frame after acting on input; restarting screencast");
            a.kitty.grid_dirty = true;
            relayout(&a);
        }

        // The page has stopped. Going back to full resolution restarts the
        // screencast, which is what puts the sharp frame up: the last one drawn
        // was a moving one, and nothing on the page has to change for it to be
        // replaced.
        if (a.in_motion && now_sec() - a.last_draw > MOTION_IDLE) {
            a.in_motion = false;
            a.motion_run = 0;
            term_log("%.3f motion off", now_sec());
            a.last_hash = 0;        // the same picture, at a size worth drawing
            relayout(&a);
            // Going back up is the half that shows: a frame that never comes
            // leaves the small one on screen. Watched for, and sooner than a
            // keypress is, because this picture is already wrong rather than
            // merely late.
            if (a.expect_frame == 0) a.expect_frame = now_sec() + 1.0;
        }

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            if (ws_fill(&a.chrome.ws) < 0) break;
            char *msg;
            size_t len;
            while (ws_next(&a.chrome.ws, &msg, &len) == 1) {
                on_cdp_message(&a, msg, len);
                a.chrome.ws.msg.len = 0;
            }
            draw_panes(&a);
        }
        if (a.chrome.ws.closed) break;

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
    console_free(&a);
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

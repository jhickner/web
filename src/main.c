#include <ctype.h>
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

static int g_panic_fd = -1;
static void on_crash(int sig) {
    term_panic(g_panic_fd);
    signal(sig, SIG_DFL);
    raise(sig);
}

// ----------------------------------------------------------- cdp dispatch

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

static void note_input(App *a, bool moving) {
    a->last_input = now_sec();
    if (!moving || !a->motion_auto || a->in_motion) return;
    if (!a->has_tty || a->paused) return;
    a->in_motion = true;
    a->still_at = 0;
    term_log("%.3f motion on (input)", a->last_input);
    relayout(a);
}

static void queue_move(App *a, int x, int y, const char *btn, int buttons,
                       int mods) {
    Pending *p = &a->pend;
    if (buttons) note_input(a, true);
    if (p->kind != PEND_MOVE) flush_pending(a);
    p->kind = PEND_MOVE;
    p->x = x;
    p->y = y;
    p->btn = btn;
    p->buttons = buttons;
    p->mods = mods;
}

static void pump_input(void) {
    if (!g_app || !g_app->has_tty) return;
    struct pollfd p = {g_app->term.fd, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    term_read(&g_app->term);
    for (size_t i = 0; i < g_app->term.in.len; i++) {
        unsigned char c = (unsigned char)g_app->term.in.p[i];
        if (c == 0x11 || (c == 0x03 && !g_app->console_focus)) {
            term_log("QUIT via pump byte %02x", c);
            g_quit = 1;
            return;
        }
    }
}

// png width is a big-endian u32 at byte 16; 32 base64 chars decode to 24 bytes
static int png_width(const char *b64, size_t len) {
    if (len < 32) return 0;
    unsigned char h[24];
    if (base64_decode(b64, 32, (char *)h) < 24) return 0;
    static const unsigned char sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    if (memcmp(h, sig, 8) || memcmp(h + 12, "IHDR", 4)) return 0;
    return (int)(((unsigned)h[16] << 24) | ((unsigned)h[17] << 16) |
                 ((unsigned)h[18] << 8) | (unsigned)h[19]);
}

uint64_t fnv1a(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

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

#define BOX_ASPECT (16.0 / 10.0)
#define BOX_MIN_COLS 10

#define BOX_COL_STEP 2

// the width the page is told it has, in css pixels
#define WIDTH_MIN  320
#define WIDTH_MAX  2560
#define WIDTH_STEP 40

// linear scale, applied to the screencast cap
#define MOTION_SCALE 0.65
#define MOTION_SCALE_SSH 0.5
#define MOTION_RUN   3       // quick frames in a row before it is a scroll
#define MOTION_GAP   0.20    // a frame this soon after the last is still moving
#define SETTLE_WAIT  300     // ms
#define MOTION_HOLD  1.5     // seconds since the last key, wheel or drag
#define STILL_WAIT   0.15
#define STILL_GAP    1.0
#define RESIZE_WAIT  0.25    // how long the teardown waits for a frame to ride out on
#define STILL_TRIES  3       // asks before giving up
#define STILL_SEND_MAX 2.0   // how long a reply has to come back

static int box_cols_for(App *a, int rows) {
    Term *t = &a->term;
    int want = (int)((double)(rows * t->cell_h) * BOX_ASPECT / t->cell_w + 0.5);
    if (want > t->cols) want = t->cols;
    if (want < BOX_MIN_COLS) want = BOX_MIN_COLS;
    return want;
}

static int box_cols_now(App *a, int rows) {
    if (a->box_cols <= 0) return box_cols_for(a, rows);
    int cols = a->box_cols;
    if (cols > a->term.cols) cols = a->term.cols;
    if (cols < BOX_MIN_COLS) cols = BOX_MIN_COLS;
    return cols;
}

static void screencast_start(App *a) {
    if (!a->has_tty || a->paused || a->cast_w < 1 || a->cast_h < 1) return;
    if (a->grid_on) return;
    app_cdp(a, "Page.startScreencast",
             "\"format\":\"png\",\"maxWidth\":%d,\"maxHeight\":%d,\"everyNthFrame\":1",
             a->cast_w, a->cast_h);
}

void still_cancel(App *a) {
    a->still_at = 0;
    a->still_sent = 0;
    a->still_tries = 0;
}

void still_soon(App *a) {
    a->still_at = now_sec() + STILL_WAIT;
}

static void still_request(App *a) {
    a->still_at = 0;
    if (!a->has_tty || a->paused || a->in_motion || a->cast_w < 1) return;
    if (a->shot_path) return;
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

// viewport for a screenshot run with no tty, in css pixels
#define SHOT_CSS_W 1280
#define SHOT_CSS_H 800

void relayout(App *a) {
    Term *t = &a->term;
    term_size(t);

    hint_cancel(a);

    if (!a->has_tty && a->shot_path) {
        a->css_w = a->shot_w ? a->shot_w : SHOT_CSS_W;
        a->css_h = a->shot_h ? a->shot_h : SHOT_CSS_H;
        a->scale = a->shot_scale > 0 ? a->shot_scale : a->want_scale;
        app_cdp(a, "Emulation.setDeviceMetricsOverride",
                 "\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%.6f,"
                 "\"mobile\":false",
                 a->css_w, a->css_h, a->scale);
        return;
    }

    int status = a->status_open ? 1 : 0;
    int above = a->tabs_open ? 1 : 0;
    int below = status + a->console_rows;
    int rect_cols;
    if (a->inline_mode) {
        int max_rows = t->rows - below - above;
        if (a->box_rows > max_rows) a->box_rows = max_rows;
        if (a->box_rows < BOX_MIN_ROWS) a->box_rows = BOX_MIN_ROWS;
        a->img_rows = a->box_rows;
        rect_cols = box_cols_now(a, a->img_rows);
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

    int rect_w = rect_cols * t->cell_w;
    int rect_h = a->img_rows * t->cell_h;
    if (rect_w < 1) rect_w = 1;
    if (rect_h < 1) rect_h = 1;

    if (a->want_width > 0) a->zoom = (double)rect_w / a->want_width;
    double z = a->zoom > 0 ? a->zoom : 1.0;

    int w = a->want_width > 0 ? a->want_width : (int)(rect_w / z);
    if (a->fit_width && a->want_width <= 0 && a->fit_w > w) w = a->fit_w;
    if (w < 120) w = 120;

    a->css_w = w;
    a->zoom_eff = (double)rect_w / a->css_w;
    a->css_h = (int)((double)w * rect_h / rect_w + 0.5);
    if (a->css_h < 1) a->css_h = 1;

    a->scale = a->want_scale;
    if (!a->scale_locked && a->scale > 1.0) {
        double fits = 1920.0 / rect_w;
        if (fits < 1.0) fits = 1.0;
        if (a->scale > fits) a->scale = fits;
    }

    double ms = a->in_motion
        ? (a->motion_scale > 0 ? a->motion_scale : MOTION_SCALE) : 1.0;

    double dsf = a->scale;
    double fits = (double)rect_w * a->scale / (double)a->css_w;
    if (fits < dsf) dsf = fits;

    a->status_last.len = 0;

    if (!a->grid_on)
        app_cdp(a, "Emulation.setDeviceMetricsOverride",
                 "\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%.6f,\"mobile\":false",
                 a->css_w, a->css_h, dsf);
    a->cast_w = (int)((double)a->css_w * dsf * ms + 0.5);
    a->cast_h = (int)((double)a->css_h * dsf * ms + 0.5);
    if (a->cast_w < 1) a->cast_w = 1;
    if (a->cast_h < 1) a->cast_h = 1;

    int cap_w = a->cast_w < a->css_w ? a->cast_w : a->css_w;
    a->frame_w = cap_w;
    a->still_w = (int)((double)a->css_w * dsf + 0.5);

    screencast_start(a);
}

// ----------------------------------------------------------------- session

static void session_file(App *a, char *out, size_t cap) {
    snprintf(out, cap, "%s/sessions/%d.json", a->chrome.profile, (int)getpid());
}

// the driver writes its own pid into this file
static void drive_file(App *a, char *out, size_t cap) {
    snprintf(out, cap, "%s/driving/%d", a->chrome.profile, (int)getpid());
}

// one file per target id
static void claims_dir(App *a, char *out, size_t cap) {
    snprintf(out, cap, "%s/driving/%d.pages", a->chrome.profile, (int)getpid());
}

static void freeze_base(App *a, char *out, size_t cap);
static bool being_driven(App *a);
static void show_window(App *a);

void session_write(App *a) {
    if (!a->chrome.profile[0] || a->chrome.port <= 0 || !a->chrome.target[0])
        return;
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/sessions", a->chrome.profile);
    mkdirs(dir);
    snprintf(dir, sizeof dir, "%s/driving", a->chrome.profile);
    mkdirs(dir);
    claims_dir(a, dir, sizeof dir);
    mkdirs(dir);
    session_file(a, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    char url[2100], title[600], drive[700], drive_esc[1400];
    char freeze[700] = "", freeze_esc[1400] = "";
    char pages[700], pages_esc[1400], name_esc[200];
    json_escape(url, sizeof url, a->url);
    json_escape(title, sizeof title, a->title);
    json_escape(name_esc, sizeof name_esc, a->name);
    drive_file(a, drive, sizeof drive);
    json_escape(drive_esc, sizeof drive_esc, drive);
    claims_dir(a, pages, sizeof pages);
    json_escape(pages_esc, sizeof pages_esc, pages);
    if (a->freeze) {
        freeze_base(a, freeze, sizeof freeze);
        json_escape(freeze_esc, sizeof freeze_esc, freeze);
    }
    // "handoff" and "merge": presence alone is what a sender checks for
    fprintf(f, "{\"pid\":%d,\"name\":\"%s\",\"port\":%d,"
               "\"cdp\":\"http://127.0.0.1:%d\","
               "\"target\":\"%s\",\"url\":\"%s\",\"title\":\"%s\","
               "\"handoff\":true,\"merge\":true,\"drive\":\"%s\",\"freeze\":\"%s\","
               "\"pages\":\"%s\"}\n",
            (int)getpid(), name_esc, a->chrome.port, a->chrome.port,
            a->chrome.target, url, title, drive_esc, freeze_esc, pages_esc);
    fclose(f);
}

// ------------------------------------------------------------- handed a url

// one file per request, named "<window pid>-<sender pid>"
static void handoff_dir(const char *profile, char *out, size_t cap) {
    snprintf(out, cap, "%s/handoff", profile);
}

static void handle_focus(App *a, bool focused);

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
            // window first, then the pane inside it
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

// while this is in the future the window does not pause
static double g_handoff_owed;

#define HANDOFF_DRAW_WAIT 5.0

static void handoff_drawn(void) { g_handoff_owed = 0; }

static void handoff_arrived(App *a) {
    g_handoff_owed = now_sec() + HANDOFF_DRAW_WAIT;
    raise_pane();
    a->paused = false;
    show_window(a);
    a->last_hash = 0;
    a->kitty.grid_dirty = true;
    a->expect_frame = now_sec() + 2.0;
    screencast_start(a);
    still_soon(a);
}

// a==NULL: the files are removed without being opened in a tab
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
    if (taken) handoff_arrived(a);
}

// 0 when there is no such window
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
        // same mtime second: the later pid wins
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

static void merge_forget(const char *profile, pid_t pid);

// the claims directories only ever hold files
static void dir_wipe(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static void session_forget(App *a) {
    if (!a->chrome.profile[0]) return;
    char path[700];
    session_file(a, path, sizeof path);
    unlink(path);
    drive_file(a, path, sizeof path);
    unlink(path);
    claims_dir(a, path, sizeof path);
    dir_wipe(path);
    handoff_take(a->chrome.profile, getpid(), NULL);
    merge_forget(a->chrome.profile, getpid());
}

// what windows that never got to clean up after themselves left behind
static void driving_sweep(const char *profile) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/driving", profile);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int pid = atoi(e->d_name);          // <pid> to drive, <pid>.pages to claim
        if (pid <= 0 || (pid_t)pid == getpid()) continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (strstr(e->d_name, ".pages")) dir_wipe(path);
        else                             unlink(path);
    }
    closedir(d);
}

static void copy_endpoint(const char *line) {
    size_t n = 0;
    const char *v = json_str(line, "cdp", &n);
    if (!v || !n) return;
    char url[300];
    json_unescape(url, sizeof url, v, n);
    clipboard_put(url);
}

// one json object per line, the first window's endpoint on the clipboard
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
                if (!found++) copy_endpoint(line);
            }
            fclose(f);
        }
        closedir(d);
    }
    return found;
}

// the window we forked writes its session file once chrome answers
static int endpoint_await(pid_t pid) {
    char profile[512], path[700];
    chrome_profile_path(profile, sizeof profile);
    snprintf(path, sizeof path, "%s/sessions/%d.json", profile, (int)pid);
    double deadline = now_sec() + 30.0;
    for (;;) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[4096];
            bool got = fgets(line, sizeof line, f) != NULL;
            fclose(f);
            if (got) {
                fputs(line, stdout);
                fflush(stdout);
                copy_endpoint(line);
                return 0;
            }
        }
        if (kill(pid, 0) != 0 && errno == ESRCH) break;
        if (now_sec() >= deadline) break;
        struct timespec ts = {0, 25 * 1000000};
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "web: the window did not come up\n");
    return 1;
}

// this window's own line, before it takes the screen
static void endpoint_announce(App *a) {
    char path[700], line[4096];
    session_write(a);
    session_file(a, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(line, sizeof line, f)) {
        fputs(line, stdout);
        fflush(stdout);
        copy_endpoint(line);
    }
    fclose(f);
}

// carry on as a window with no terminal of its own, the caller printing the
// endpoint as soon as it lands
static bool endpoint_detach(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "web: cannot start a window: %s\n", strerror(errno));
        exit(1);
    }
    if (pid > 0) exit(endpoint_await(pid));
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        if (null > STDERR_FILENO) close(null);
    }
    return true;
}

// ------------------------------------------------------------------ browsers

#define PROC_MAX 64

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

// a whole word in the command line, so a url holding the text does not count
static bool has_arg(const char *line, const char *flag) {
    size_t n = strlen(flag);
    for (const char *p = strstr(line, flag); p; p = strstr(p + n, flag)) {
        if (p > line && !isspace((unsigned char)p[-1])) continue;
        if (p[n] == 0 || isspace((unsigned char)p[n])) return true;
    }
    return false;
}

// the modes that are not a window of their own: an mcp bridge drives one that
// is already up, and the rest do their work and go
static const char *NOT_WINDOW[] = {"--mcp", "--open", "--endpoint", "--list",
                                   "--kill", "--help", "-h"};

typedef struct { pid_t pid; char profile[80]; } Stray;

// which profile's sessions dir holds this pid. false when no profile claims it,
// which is the answer for a web process that is not a window at all: a fork on
// its way out or to an exec still carries its parent's command line.
static bool window_profile(pid_t pid, char *out, size_t cap) {
    char base[400], path[1024];
    web_cache_path(base, sizeof base);

    snprintf(path, sizeof path, "%s/profile/sessions/%d.json", base, (int)pid);
    if (access(path, F_OK) == 0) { snprintf(out, cap, "the shared profile"); return true; }

    char dir[512];
    snprintf(dir, sizeof dir, "%s/profiles", base);
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s/sessions/%d.json", dir, e->d_name,
                 (int)pid);
        if (access(path, F_OK) != 0) continue;
        snprintf(out, cap, "profile %s", e->d_name);
        found = true;
    }
    closedir(d);
    return found;
}

static int stuck_windows(Stray *out, int cap, const pid_t *known, int nknown) {
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
        bool mode = false;
        for (size_t i = 0; i < sizeof NOT_WINDOW / sizeof *NOT_WINDOW; i++)
            if (has_arg(line, NOT_WINDOW[i])) mode = true;
        if (mode) continue;
        bool seen = false;
        for (int i = 0; i < nknown; i++) if (known[i] == (pid_t)pid) seen = true;
        if (seen) continue;
        if (!window_profile((pid_t)pid, out[n].profile, sizeof out[n].profile))
            continue;
        out[n++].pid = (pid_t)pid;
    }
    pclose(p);
    return n;
}

static bool proc_alive(pid_t p) {
    return kill(p, 0) == 0 || errno != ESRCH;
}

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
    fflush(stdout);
    if (unreachable)
        fprintf(stderr, "web: %d unreachable browser%s holding the profile; "
                        "run 'web --kill' to terminate %s\n",
                unreachable, unreachable == 1 ? "" : "s",
                unreachable == 1 ? "it" : "them");
    return 0;
}

static int kill_everything(void) {
    char profile[512];
    chrome_profile_path(profile, sizeof profile);

    pid_t windows[PROC_MAX];
    int w = running_windows(profile, windows, PROC_MAX);
    for (int i = 0; i < w; i++) kill(windows[i], SIGTERM);
    if (w) {
        printf("web: stopping %d window%s\n", w, w == 1 ? "" : "s");
        if (!wait_windows(profile, windows, w, 3.0)) {
            for (int i = 0; i < w; i++)
                if (!window_gone(profile, windows[i])) {
                    printf("web: window %d did not exit; killed\n",
                           (int)windows[i]);
                    kill(windows[i], SIGKILL);
                }
        }
    }

    Stray stray[PROC_MAX];
    int s = stuck_windows(stray, PROC_MAX, windows, w);

    ChromeProc procs[PROC_MAX];
    int n = chrome_running(profile, procs, PROC_MAX);
    pid_t pids[PROC_MAX];
    for (int i = 0; i < n; i++) {
        pids[i] = procs[i].pid;
        kill(pids[i], SIGTERM);
        printf("web: stopping chrome %d\n", (int)pids[i]);
    }
    if (n && !wait_gone(pids, n, 5.0)) {
        for (int i = 0; i < n; i++)
            if (proc_alive(pids[i])) {
                printf("web: chrome %d did not exit; killed its process group\n",
                       (int)pids[i]);
                if (kill(-pids[i], SIGKILL) < 0) kill(pids[i], SIGKILL);
            }
    }

    if (w || n) {
        char path[700];
        snprintf(path, sizeof path, "%s/DevToolsActivePort", profile);
        unlink(path);
        snprintf(path, sizeof path, "%s/web-port", profile);
        unlink(path);
        snprintf(path, sizeof path, "%s/web-keep", profile);
        unlink(path);
    } else if (!s) {
        printf("web: nothing running on this profile\n");
    }

    fflush(stdout);
    for (int i = 0; i < s; i++)
        fprintf(stderr, "web: window %d belongs to %s; left running\n",
                (int)stray[i].pid, stray[i].profile);
    return 0;
}

// ------------------------------------------------------------------- merge

static void merge_dir(const char *profile, char *out, size_t cap) {
    snprintf(out, cap, "%s/merge", profile);
}

// give_wait must stay longer than merge_wait
#define MERGE_WAIT 5.0
#define GIVE_WAIT  8.0

static bool window_merges(const char *profile, pid_t pid) {
    char path[800];
    snprintf(path, sizeof path, "%s/sessions/%d.json", profile, (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[4096];
    bool ok = fgets(line, sizeof line, f) && json_has(line, "merge");
    fclose(f);
    return ok;
}

// merge file names are <target-pid>-<writer-pid>
static void merge_forget(const char *profile, pid_t pid) {
    char dir[600];
    merge_dir(profile, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    char first[32], last[32];
    size_t flen = (size_t)snprintf(first, sizeof first, "%d-", (int)pid);
    size_t llen = (size_t)snprintf(last, sizeof last, "-%d", (int)pid);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        bool mine = strncmp(e->d_name, first, flen) == 0 ||
                    (n > llen && !strcmp(e->d_name + n - llen, last));
        if (!mine) continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
}

static void oneline(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (; src[o] && o + 1 < cap; o++)
        dst[o] = (unsigned char)src[o] < 0x20 ? ' ' : src[o];
    dst[o] = 0;
}

static void merge_ask(App *a) {
    if (a->giving_tabs) { notify(a, "these tabs are already going elsewhere"); return; }

    pid_t windows[PROC_MAX];
    int n = running_windows(a->chrome.profile, windows, PROC_MAX);
    char dir[600];
    merge_dir(a->chrome.profile, dir, sizeof dir);
    mkdirs(dir);
    int asked = 0;
    for (int i = 0; i < n; i++) {
        if (!window_merges(a->chrome.profile, windows[i])) continue;
        char path[820], tmp[840];
        snprintf(path, sizeof path, "%s/%d-%d", dir, (int)windows[i], (int)getpid());
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "w");
        if (!f) continue;
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
        if (rename(tmp, path) != 0) { unlink(tmp); continue; }
        if (kill(windows[i], SIGUSR1) != 0) { unlink(path); continue; }
        asked++;
    }
    if (!asked) { notify(a, "no other window to take tabs from"); return; }
    a->merge_until = now_sec() + MERGE_WAIT;
    a->merge_want = asked;
    a->merge_got = 0;
    notify(a, asked == 1 ? "asking the other window for its tabs"
                         : "asking the other windows for their tabs");
}

static void merge_give(App *a) {
    char dir[600];
    merge_dir(a->chrome.profile, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    char prefix[32];
    int plen = snprintf(prefix, sizeof prefix, "%d-", (int)getpid());
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, (size_t)plen) != 0) continue;
        if (strstr(e->d_name, ".tmp")) continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        unlink(path);
        if (!f) continue;
        char line[32];
        int to = fgets(line, sizeof line, f) ? atoi(line) : 0;
        fclose(f);
        if (to <= 0 || a->giving_tabs || a->merge_until > 0) continue;

        char rpath[820], rtmp[840];
        snprintf(rpath, sizeof rpath, "%s/reply-%d-%d", dir, to, (int)getpid());
        snprintf(rtmp, sizeof rtmp, "%s.tmp", rpath);
        FILE *r = fopen(rtmp, "w");
        if (!r) continue;
        int listed = 0;
        for (int i = 0; i < a->ntabs; i++) {
            if (!a->tabs[i].ours) continue;
            char url[1100], title[300];
            oneline(url, sizeof url, i == a->tab ? a->url : a->tabs[i].url);
            oneline(title, sizeof title, i == a->tab ? a->title : a->tabs[i].title);
            fprintf(r, "%s\t%s\t%s\n", a->tabs[i].target, url, title);
            listed++;
        }
        fclose(r);
        if (!listed || rename(rtmp, rpath) != 0) { unlink(rtmp); continue; }
        a->giving_tabs = true;
        a->giving_until = now_sec() + GIVE_WAIT;
        snprintf(a->giving_path, sizeof a->giving_path, "%s", rpath);
        notify(a, "handing these tabs over");
        break;
    }
    closedir(d);
}

static void give_tick(App *a) {
    if (!a->giving_tabs) return;
    if (access(a->giving_path, F_OK) != 0) {
        a->giving_tabs = false;
        a->gave_tabs = true;
        g_quit = 1;
        return;
    }
    if (now_sec() < a->giving_until) return;
    unlink(a->giving_path);
    a->giving_tabs = false;
    a->giving_path[0] = 0;
    notify(a, "nobody took the tabs");
}

static void merge_collect(App *a) {
    if (a->merge_until <= 0) return;
    static double next;
    double t = now_sec();
    if (t >= next) {
        next = t + 0.1;
        char dir[600], prefix[40];
        merge_dir(a->chrome.profile, dir, sizeof dir);
        int plen = snprintf(prefix, sizeof prefix, "reply-%d-", (int)getpid());
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strncmp(e->d_name, prefix, (size_t)plen) != 0) continue;
                if (strstr(e->d_name, ".tmp")) continue;
                char path[800];
                snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                FILE *f = fopen(path, "r");
                if (!f) continue;
                char line[1500];
                while (fgets(line, sizeof line, f)) {
                    line[strcspn(line, "\r\n")] = 0;
                    char *url = strchr(line, '\t');
                    if (!url) continue;
                    *url++ = 0;
                    char *title = strchr(url, '\t');
                    if (title) *title++ = 0;
                    if (tab_take(a, line, url, title)) a->merge_got++;
                    else if (tab_index_of(a, line) < 0) {
                        chrome_close_id(&a->chrome, line);
                        a->merge_lost++;
                    }
                }
                fclose(f);
                unlink(path);
                if (a->merge_want > 0) a->merge_want--;
            }
            closedir(d);
        }
    }
    if (a->merge_want > 0 && t < a->merge_until) return;

    char m[80];
    int got = a->merge_got, lost = a->merge_lost;
    a->merge_until = 0;
    a->merge_want = 0;
    a->merge_got = 0;
    a->merge_lost = 0;
    if (!got && !lost) snprintf(m, sizeof m, "no window had a tab to give");
    else if (!lost)    snprintf(m, sizeof m, "%d tab%s taken", got, got == 1 ? "" : "s");
    else snprintf(m, sizeof m, "%d tab%s taken, %d over the limit and closed",
                  got, got == 1 ? "" : "s", lost);
    notify(a, m);
}

// -------------------------------------------------------------------- exec

// freeze protocol: .pause is written by the driver, .resume written back to it
static void freeze_base(App *a, char *out, size_t cap) {
    char profile[512];
    chrome_profile_path(profile, sizeof profile);
    snprintf(out, cap, "%s/freeze-%d", profile, (int)getpid());
}

static void freeze_files(App *a, char *pause, size_t pn, char *resume, size_t rn) {
    char base[560];
    freeze_base(a, base, sizeof base);
    snprintf(pause, pn, "%s.pause", base);
    snprintf(resume, rn, "%s.resume", base);
}

static void exec_start(App *a, const char *cmd) {
    int fds[2];
    if (pipe(fds) < 0) return;

    char pause[600], resume[600];
    freeze_files(a, pause, sizeof pause, resume, sizeof resume);
    unlink(pause);
    unlink(resume);

    char claims[700], fbase[560];
    claims_dir(a, claims, sizeof claims);
    freeze_base(a, fbase, sizeof fbase);
    mkdirs(claims);

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
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
        char pname[64];
        if (chrome_profile_named(pname, sizeof pname)) setenv("WEB_PROFILE", pname, 1);
        setenv("FORCE_COLOR", "0", 0);
        if (a->slowmo > 0) {
            char ms[24];
            snprintf(ms, sizeof ms, "%d", a->slowmo);
            setenv("WEB_SLOWMO", ms, 0);
        }
        if (a->freeze) setenv("WEB_FREEZE", fbase, 1);
        setenv("WEB_PAGES", claims, 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    int fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    a->exec_fd = fds[0];
    a->exec_pid = pid;
    if (a->has_tty && !a->console_open) a->console_open = true;
    console_log(a, "");
    char m[300];
    snprintf(m, sizeof m, "$ %.280s", cmd);
    console_log(a, m);
}

static void exec_done(App *a) {
    close(a->exec_fd);
    a->exec_fd = -1;
    a->exec_paused = false;
    a->exec_note[0] = 0;
    char pause[600], resume[600];
    freeze_files(a, pause, sizeof pause, resume, sizeof resume);
    unlink(pause);
    unlink(resume);
    int st = 0;
    char m[64];
    if (waitpid(a->exec_pid, &st, 0) == a->exec_pid && WIFEXITED(st))
        snprintf(m, sizeof m, "[exit %d]", WEXITSTATUS(st));
    else
        snprintf(m, sizeof m, "[stopped]");
    a->exec_pid = 0;
    console_log(a, m);
}

static void exec_check_pause(App *a) {
    if (!a->freeze || a->exec_paused) return;
    if (a->exec_fd < 0 && !being_driven(a)) return;
    static double next;
    double t = now_sec();
    if (t < next) return;
    next = t + 0.2;

    char pause[600], resume[600];
    freeze_files(a, pause, sizeof pause, resume, sizeof resume);
    FILE *f = fopen(pause, "r");
    if (!f) return;
    char why[160] = {0};
    if (fgets(why, sizeof why, f)) why[strcspn(why, "\r\n")] = 0;
    fclose(f);

    a->exec_paused = true;
    snprintf(a->exec_note, sizeof a->exec_note, "%s", why);
    char m[220];
    snprintf(m, sizeof m, "-- frozen: %.150s", why[0] ? why : "the run is waiting");
    console_log(a, m);
    notify(a, "frozen - the page is as it was left; alt+enter lets it go");
}

static void exec_resume(App *a) {
    if (!a->exec_paused) {
        notify(a, "nothing is waiting");
        return;
    }
    char pause[600], resume[600];
    freeze_files(a, pause, sizeof pause, resume, sizeof resume);
    FILE *f = fopen(resume, "w");
    if (f) { fprintf(f, "go\n"); fclose(f); }
    unlink(pause);
    a->exec_paused = false;
    a->exec_note[0] = 0;
    console_log(a, "-- carrying on");
}

static void claims_scan(App *a) {
    static double next;
    double t = now_sec();
    if (t < next) return;
    next = t + 0.25;

    char dir[700];
    claims_dir(a, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;

    char seen[TAB_MAX][96];
    int nseen = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nseen < TAB_MAX) {
        if (e->d_name[0] == '.') continue;
        snprintf(seen[nseen], sizeof seen[nseen], "%s", e->d_name);
        if (tab_index_of(a, seen[nseen]) < 0) {
            if (tab_adopt(a, seen[nseen], ""))
                term_log("%.3f adopted %s: a driver claimed it", now_sec(),
                         seen[nseen]);
        }
        nseen++;
    }
    closedir(d);

    for (int i = a->ntabs - 1; i >= 0; i--) {
        if (!a->tabs[i].claimed) continue;
        bool still = false;
        for (int j = 0; j < nseen && !still; j++)
            still = !strcmp(a->tabs[i].target, seen[j]);
        if (!still) tab_forget(a, a->tabs[i].target);
    }
}

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
        if (!nl && !eof) break;
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

// seconds
#define SHOT_LOAD_MAX   30.0
#define SHOT_SETTLE_MAX  3.0
#define SHOT_SEND_MAX   15.0

// waits for document.fonts, then two frames
static const char SHOT_READY_JS[] =
    "new Promise(function(r){"
    "(document.fonts?document.fonts.ready:Promise.resolve()).then(function(){"
    "requestAnimationFrame(function(){requestAnimationFrame(function(){r(1)})})"
    "})})";

static void shot_fail(App *a, const char *why) {
    fprintf(stderr, "web: --screenshot: %s\n", why);
    a->shot_state = SHOT_FAIL;
}

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

// Draw a frame, folding in any resize teardown that is still owed.
//
// Clearing the screen at resize time and only then asking Chrome for a
// screenshot leaves the terminal showing its background for the whole round
// trip - a flash. Held back to here, the clear, the new placeholder grid and
// the picture are one synchronized update, so the terminal goes straight from
// the old image to the new one.
// The clear takes the pane rows with it, and the panes only repaint when their
// text changes - so what they last drew has to stop counting as on screen.
static void panes_forget(App *a) {
    a->status_last.len = 0;
    a->tabs_last.len = 0;
    a->console_last.len = 0;
}

static void draw_frame(App *a, const char *b64, size_t n) {
    bool resized = a->resize_redraw;
    if (resized) {
        a->resize_redraw = false;
        kitty_sync_begin(&a->kitty);
        if (a->inline_mode) term_clear_inline(&a->term);
        else writeall(a->term.fd, "\x1b[2J", 4);
        kitty_renew(&a->kitty);
        panes_forget(a);
    }
    kitty_draw_png(&a->kitty, b64, n);
    if (resized) kitty_sync_end(&a->kitty);
}

// The frame that was meant to carry the teardown never came. Waiting any
// longer leaves the terminal holding placeholder cells the resize reflowed -
// image rows landing at the wrong screen rows - so tear down now and re-place
// the picture we already sent into the new rect. Stretched until the next
// frame, but whole.
static void resize_give_up(App *a) {
    a->resize_redraw = false;
    kitty_sync_begin(&a->kitty);
    if (a->inline_mode) term_clear_inline(&a->term);
    else writeall(a->term.fd, "\x1b[2J", 4);
    if (a->grid_on || kitty_replace(&a->kitty) < 0) kitty_renew(&a->kitty);
    kitty_sync_end(&a->kitty);
    panes_forget(a);
    a->last_hash = 0;
    term_log("%.3f resize: no frame in %.2fs, redrawing without one",
             now_sec(), RESIZE_WAIT);
}

static void still_draw(App *a, const char *msg) {
    if (a->paused || a->grid_on) return;
    size_t n = 0;
    const char *b64 = json_str(msg, "data", &n);
    if (!b64 || !n) return;

    double t0 = now_sec();
    draw_frame(a, b64, n);
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

static void shot_capture(App *a) {
    app_req_note(a, app_cdp(a, "Page.captureScreenshot", "\"format\":\"png\""),
                 RQ_SHOT);
    a->shot_state = SHOT_SENT;
    a->shot_deadline = now_sec() + SHOT_SEND_MAX;
}

static void shot_step(App *a) {
    switch (a->shot_state) {
    case SHOT_LOAD: {
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

static int block_rows(App *a) {
    return (a->tabs_open ? 1 : 0) + a->box_rows +
           (a->status_open ? 1 : 0) + a->console_rows;
}

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
    kitty_clear(&a->kitty);
    a->last_hash = 0;
    if (a->inline_mode)
        term_resize_inline(&a->term, block_rows(a));
    else
        writeall(a->term.fd, "\x1b[2J", 4);
    relayout(a);
    still_soon(a);
}

static void draw_status(App *a) {
    if (!a->has_tty || !a->status_open) return;
    Term *t = &a->term;
    Buf b = a->status;
    b.len = 0;
    int row = a->status_row > 0 ? a->status_row : t->rows;

    int sx = a->kitty.x > 0 ? a->kitty.x : 1;
    int sw = a->kitty.cols > 0 ? a->kitty.cols : t->cols;
    if (sx + sw - 1 > t->cols) sw = t->cols - sx + 1;
    if (sw < 8) sw = 8;

    buf_addf(&b, "\x1b[%d;1H\x1b[2K\x1b[%d;%dH", row, row, sx);

    if (a->editing) {
        const char *label = a->prompt == 2 ? "find" : a->prompt == 3 ? "tab" : "go";
        // +3: a space either side of the label, plus one before the text
        int lead = (int)strlen(label) + 3;
        // one column short of the edge, so the cursor after the text has a cell
        int room = sw - lead - 1;
        if (room < 1) room = 1;
        int used = 0;
        size_t off = utf8_tail(a->edit, a->edit_len, room, &used);
        buf_addf(&b, "\x1b[7m %s \x1b[0m %.*s\x1b[?25h",
                 label, (int)(a->edit_len - off), a->edit + off);
        buf_addf(&b, "\x1b[%d;%dH", row, sx + lead + used);
    } else {
        static const char KEYS[] = "? keys";

        const char *left = a->title[0] ? a->title : a->url;
        const char *hint = KEYS;
        int hintlen = (int)strlen(hint);
        bool show_hint = sw > hintlen + 4;
        int avail = show_hint ? sw - hintlen - 3 : sw - 2;
        if (avail < 8) avail = 8;

        if (a->rec_on)
            buf_addf(&b, "\x1b[1;31m REC\x1b[0m ");
        else if (a->exec_paused)
            buf_addf(&b, "\x1b[1;31m FROZEN\x1b[0m ");
        else if (a->insert)
            buf_addf(&b, "\x1b[1;33m INSERT\x1b[0m ");
        else if (a->hint_on)
            buf_addf(&b, "\x1b[1;36m LINKS %s\x1b[0m ", a->hint_typed);
        else if (a->pend_key) {
            char spec[48];
            key_text(a->pend_mods, a->pend_key, spec, sizeof spec);
            buf_addf(&b, "\x1b[1;36m %s-\x1b[0m ", spec);
        }
        if (a->msg_until > now_sec()) {
            buf_addf(&b, " \x1b[1m%.*s\x1b[0m", avail, a->msg);
        } else {
            if (bookmark_current(a)) {
                buf_addf(&b, " %s\xe2\x98\x85\x1b[0m",
                         a->loading ? "\x1b[33m" : "\x1b[2m");
                avail -= 2;
            }
            buf_addf(&b, " %s%.*s\x1b[0m", a->loading ? "\x1b[33m" : "\x1b[2m",
                     avail, left);
        }

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
    if (!a->has_tty) return;
    if (a->hidden) return;
    status_sync(a);
    tabs_paint(a);
    grid_paint(a);
    draw_status(a);
    console_paint(a);
    help_paint(a);
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
    case ' ':           send_key(a, 32, " ", "Space", " ", mods); return true;
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

static bool looks_addressed(const char *s) {
    if (strchr(s, ' ')) return false;
    return strchr(s, '.') || strchr(s, ':') || strchr(s, '/') ||
           !strcmp(s, "localhost");
}

static void start_url(const char *raw, char *out, size_t cap) {
    if (strstr(raw, "://") || !strncmp(raw, "about:", 6)) {
        snprintf(out, cap, "%s", raw);
        return;
    }
    if (file_url(raw, out, cap)) return;
    if (!looks_addressed(raw) && omni_best_bookmark(raw, out, cap)) return;
    if (strchr(raw, ' ')) bar_url(raw, out, cap);
    else                  snprintf(out, cap, "https://%s", raw);
}

#define SEARCH_URL "https://www.google.com/search?q=%s"

static char g_search[SETTING_TEXT_MAX] = SEARCH_URL;

void search_set(const char *tpl) {
    if (tpl && *tpl) snprintf(g_search, sizeof g_search, "%s", tpl);
}

static void search_url(const char *raw, char *url, size_t cap) {
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

    // only the first %s is substituted
    const char *at = strstr(g_search, "%s");
    if (!at) { snprintf(url, cap, "%s", g_search); return; }
    snprintf(url, cap, "%.*s%s%s", (int)(at - g_search), g_search, q, at + 2);
}

void bar_url(const char *raw, char *url, size_t cap) {
    if (strstr(raw, "://") || strncmp(raw, "about:", 6) == 0) {
        snprintf(url, cap, "%s", raw);
    } else if (!file_url(raw, url, cap)) {
        if (!looks_addressed(raw) && omni_best_bookmark(raw, url, cap)) return;
        if (strchr(raw, ' ') || !strchr(raw, '.')) search_url(raw, url, cap);
        else snprintf(url, cap, "https://%s", raw);
    }
}

void navigate(App *a, const char *raw) {
    char url[1100];
    bar_url(raw, url, sizeof url);

    char esc[2200];
    json_escape(esc, sizeof esc, url);
    app_cdp(a, "Page.navigate", "\"url\":\"%s\"", esc);
    a->loading = true;
    a->nav_ours = true;
    record_goto(a, url);
}

static void request_fit(App *a) {
    if (!a->fit_width) return;
    a->fit_seq = a->nav_seq;
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"Math.max(document.documentElement.scrollWidth,"
        "document.body?document.body.scrollWidth:0)\",\"returnByValue\":true"),
        RQ_FIT);
}

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

static void step_width(App *a, int step) {
    int cur = a->want_width > 0 ? a->want_width : a->css_w;
    int want = cur + step * WIDTH_STEP;
    want -= want % WIDTH_STEP;    // round onto the step
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
    snprintf(m, sizeof m, "width %dpx - zoom %.0f%%", a->css_w, a->zoom * 100);
    notify(a, m);
    request_fit(a);
}

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
        cols = was_cols + dcols;
    } else if (!scale) {
        cols = was_cols;
    } else if (a->box_cols > 0) {
        cols = (int)((double)was_cols * rows / a->box_rows + 0.5);
    } else {
        cols = box_cols_for(a, rows);
    }
    if (cols < BOX_MIN_COLS) cols = BOX_MIN_COLS;
    if (cols > t->cols) cols = t->cols;
    if (rows == a->box_rows && cols == was_cols) return;

    a->box_rows = rows;
    if (dcols || !scale || a->box_cols > 0) a->box_cols = cols;

    kitty_clear(&a->kitty);
    term_clear_inline(t);
    term_resize_inline(t, block_rows(a));
    a->status_last.len = 0;
    a->tabs_last.len = 0;
    a->console_last.len = 0;
    a->last_hash = 0;
    relayout(a);

    char m[64];
    snprintf(m, sizeof m, "window %dx%d - zoom %.0f%%",
             a->css_w, a->css_h, a->zoom_eff * 100);
    notify(a, m);
    request_fit(a);
}

// whether this pane is the zoomed one of its tmux window; -1 when unknown
static int tmux_zoomed(void) {
    const char *pane = getenv("TMUX_PANE");
    if (!getenv("TMUX") || !pane || pane[0] != '%') return -1;
    for (const char *p = pane + 1; *p; p++)
        if (*p < '0' || *p > '9') return -1;      // not a pane id; not for sh
    char cmd[128];
    snprintf(cmd, sizeof cmd,
             "tmux display-message -p -t %s '#{window_zoomed_flag}' 2>/dev/null",
             pane);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char out[16] = {0};
    char *got = fgets(out, sizeof out, f);
    pclose(f);
    if (!got) return -1;
    return out[0] == '1';
}

static void tmux_zoom_track(App *a) {
    if (!a->tmux_zoom || !a->inline_mode || !a->has_tty) return;
    int z = tmux_zoomed();
    if (z < 0 || (z != 0) == a->zoomed) return;
    a->zoomed = z != 0;

    Term *t = &a->term;
    if (!a->zoomed) {
        a->box_rows = a->unzoom_rows;
        a->box_cols = a->unzoom_cols;
        return;
    }
    a->unzoom_rows = a->box_rows;
    a->unzoom_cols = a->box_cols;

    int fixed = block_rows(a) - a->box_rows;   // the bar, the status line, the console
    int rows = t->rows - fixed - 1;            // and the row the shell prompts on
    if (rows < BOX_MIN_ROWS) rows = BOX_MIN_ROWS;
    if (rows <= a->box_rows) return;
    if (a->box_cols > 0) {
        int cols = (int)((double)box_cols_now(a, a->box_rows) * rows / a->box_rows + 0.5);
        if (cols > t->cols) cols = t->cols;
        if (cols < BOX_MIN_COLS) cols = BOX_MIN_COLS;
        a->box_cols = cols;
    }
    a->box_rows = rows;
}

static void cycle_scale(App *a) {
    a->motion_auto = !a->motion_auto;
    a->want_scale = 1.0;
    a->in_motion = false;
    a->motion_run = 0;
    a->scale_locked = true;
    still_cancel(a);
    relayout(a);
    still_soon(a);
    char m[64];
    if (a->motion_auto)
        snprintf(m, sizeof m, "frame auto - %d%% while moving",
                 (int)(a->motion_scale * 100 + 0.5));
    else
        snprintf(m, sizeof m, "frame %.0f%% - %dx%d", a->want_scale * 100,
                 a->cast_w, a->cast_h);
    notify(a, m);
}

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

static void zoom_by(App *a, double factor) {
    if (a->fit_w > 0 && a->zoom_eff > 0 && a->zoom_eff < a->zoom * 0.97) {
        if (factor > 1.0) {
            char m[80];
            snprintf(m, sizeof m, "zoom %.0f%% - page needs %dpx",
                     a->zoom_eff * 100, a->fit_w);
            notify(a, m);
            return;
        }
        a->zoom = a->zoom_eff;
    }
    double before = a->zoom;
    a->zoom *= factor;
    if (a->zoom < 0.4) a->zoom = 0.4;
    if (a->zoom > 4.0) a->zoom = 4.0;
    if ((factor > 1.0 && a->zoom < before) || (factor < 1.0 && a->zoom > before))
        a->zoom = before;
    if (a->zoom == before) return;
    a->want_width = 0;

    a->fit_w = 0;
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

// js: find the scroller under a point, and the document's own
#define SCROLLER_FN \
    "function hunt(x,y){var e=document.elementFromPoint(x,y);" \
    "while(e){if(e===document.body||e===document.documentElement)return null;" \
    "var o=getComputedStyle(e).overflowY;" \
    "if((o==='auto'||o==='scroll')&&e.scrollHeight>e.clientHeight+1)break;" \
    "e=e.parentElement;}return e;}" \
    "function doc(){return document.scrollingElement||document.documentElement;}" \
    "function moves(e){return e&&e.scrollHeight>e.clientHeight+1;}" \
    "function sc(x,y){return hunt(x,y)||doc();}" \
    "function pg(x,y){var d=doc();return moves(d)?d:(hunt(x,y)||d);}"

void scroll_at(App *a, int x, int y, int dy) {
    note_input(a, true);
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
             "t.scrollTo({top:v,left:t.scrollLeft,behavior:'instant'});"
             "})(%d,%d,%d)",
             x, y, dy);
    run_js(a, js);
}

void scroll_by(App *a, int dy) {
    scroll_at(a, a->css_w / 2, a->css_h / 2, dy);
}

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

void scroll_page_end(App *a, bool bottom) {
    note_input(a, true);
    if (a->pdf) {
        if (!a->pdf_clicked) {
            notify(a, "click the pdf first - it takes keys only once clicked");
            return;
        }
        send_key(a, bottom ? 35 : 36, bottom ? "End" : "Home",
                 bottom ? "End" : "Home", NULL, 0);
        return;
    }
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

void nav_history(App *a, int delta) {
    a->hist_delta = delta;
    app_req_note(a, app_cdp(a, "Page.getNavigationHistory", ""), RQ_HISTORY);
}

// the nth element of the array at arr, or NULL; points into arr
const char *json_array_at(const char *arr, int n) {
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

// js: is the element typable
#define EDITABLE_FN \
    "function ed(e){if(!e)return false;var t=e.tagName;" \
    "return e.isContentEditable||t==='INPUT'||t==='TEXTAREA'||t==='SELECT';}"

// js: is the element a media player or a shell holding one
#define PLAYER_FN \
    "function pl(e){if(!e||e===document.body)return false;" \
    "var t=e.tagName;if(t==='VIDEO'||t==='AUDIO')return true;" \
    "return !!(e.querySelector&&e.querySelector('video,audio'));}"

// js: toggle the playing media, else the largest
static const char PLAY_PAUSE_JS[] =
    "(function(){var l=[].slice.call(document.querySelectorAll('video,audio'));"
    "l=l.filter(function(e){return e.readyState||e.currentSrc||e.src});"
    "if(!l.length)return '';"
    "var v=l.filter(function(e){return !e.paused})[0];"
    "if(!v)v=l.sort(function(a,b){return b.offsetWidth*b.offsetHeight-"
    "a.offsetWidth*a.offsetHeight})[0];"
    "if(v.paused)v.play();else v.pause();"
    "return v.paused?'paused':'playing';})()";

// js: pause everything playing, marking each one it stopped as __webheld
static const char MEDIA_HOLD_JS[] =
    "(function(){var n=0;[].forEach.call(document.querySelectorAll('video,audio'),"
    "function(e){try{if(!e.paused&&!e.ended){e.pause();e.__webheld=1;n++}}catch(x){}});"
    "return n?'held':'';})()";

// js: play the __webheld ones and unmark them
static const char MEDIA_RESUME_JS[] =
    "(function(){var n=0;[].forEach.call(document.querySelectorAll('video,audio'),"
    "function(e){try{if(!e.__webheld)return;e.__webheld=0;if(!e.paused)return;"
    "var p=e.play();if(p&&p.catch)p.catch(function(){});n++}catch(x){}});"
    "return n?'played':'';})()";

// js, isolated world: report focus changes through __webmode
static const char FOCUS_WATCHER[] =
    "(function(){"
    EDITABLE_FN
    PLAYER_FN
    // this lands in every frame, and a frame only knows its own document. An
    // iframe arriving later used to report its idle body and wipe out the
    // input the main frame had just focused, so a frame without the focus says
    // nothing, and one whose focus sits in a child defers to that child.
    "function rep(){try{if(!document.hasFocus())return;"
    "var e=document.activeElement;"
    "if(e&&(e.tagName==='IFRAME'||e.tagName==='FRAME'))return;"
    "__webmode(ed(e)?'1':pl(e)?'2':'0');}catch(e){}}"
    "if(!window.__webwatch){window.__webwatch=1;"
    "document.addEventListener('focusin',rep,true);"
    "document.addEventListener('focusout',function(){setTimeout(rep,0);},true);}"
    "rep();})()";

// js, page world: window.__web verbs; %d is --timeout in ms
static const char WEB_HELPERS[] =
    "(function(){if(window.__web)return;var T=%d;"
    "function el(s){return document.querySelector(s)}"
    "function sleep(ms){return new Promise(function(r){setTimeout(r,ms)})}"
    "async function until(t,ms){ms=ms||T;var end=Date.now()+ms;for(;;){"
    "var v=typeof t==='function'?t():(0,eval)(t);"
    "if(v)return v;"
    "if(Date.now()>end)throw new Error('waited '+ms+'ms for '+t);"
    "await sleep(50)}}"
    "async function wait(s,ms){try{return await until(function(){return el(s)},ms)}"
    "catch(e){throw new Error('no '+s+' after '+(ms||T)+'ms')}}"
    "async function gone(s,ms){try{return await until(function(){return !el(s)},ms)}"
    "catch(e){throw new Error(s+' is still there after '+(ms||T)+'ms')}}"
    "async function click(s,ms){var e=await wait(s,ms);"
    "e.scrollIntoView({block:'center'});e.click();return true}"
    "async function type(s,t,ms){var e=await wait(s,ms);e.focus();"
    "var d=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(e),'value');"
    "if(d&&d.set)d.set.call(e,t);else e.value=t;"
    "e.dispatchEvent(new Event('input',{bubbles:true}));"
    "e.dispatchEvent(new Event('change',{bubbles:true}));return true}"
    "window.__web={until:until,wait:wait,gone:gone,click:click,type:type,timeout:T,"
    "text:function(s){var e=el(s);return e?e.textContent.trim():null},"
    "count:function(s){return document.querySelectorAll(s).length},"
    "attr:function(s,n){var e=el(s);return e?e.getAttribute(n):null},"
    "all:function(s){return[].map.call(document.querySelectorAll(s),"
    "function(e){return e.textContent.trim()})}};})()";

// js: read the focus mode once, by value
static const char FOCUS_READ[] =
    "(function(){" EDITABLE_FN PLAYER_FN
    "var e=document.activeElement;return ed(e)?'1':pl(e)?'2':'0';})()";

// js: preventDefault the arrows unless a field or player has focus
static const char KEY_CLAIMER[] =
    "(function(){if(window.__webkeys)return;window.__webkeys=1;"
    EDITABLE_FN PLAYER_FN
    "var K={ArrowLeft:1,ArrowRight:1,ArrowUp:1,ArrowDown:1};"
    "window.addEventListener('keydown',function(e){"
    "if(!K[e.key])return;"
    "var t=document.activeElement;"
    "if(ed(t)||pl(t))return;"
    "e.preventDefault();},true);})()";

// a driver marks the window on every step it takes; the mark goes stale so an
// idle one does not hold the window awake for as long as it happens to live
#define DRIVE_FRESH 30.0

static bool driver_attached(App *a) {
    char path[700];
    drive_file(a, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid > 0 && kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
        unlink(path);
        return false;
    }
    return now_sec() - (double)st.st_mtime < DRIVE_FRESH;
}

static bool being_driven(App *a) {
    if (a->exec_fd >= 0 || script_busy(a)) return true;
    static double next = 0;
    static bool    was;
    double t = now_sec();
    if (t >= next) {
        next = t + 0.25;
        was = driver_attached(a);
    }
    return was;
}

#define PAUSE_WAIT 2.0

static bool picture_up(App *a) {
    if (!a->grid_on) return kitty_tile_live(&a->kitty, 0);
    for (int i = 1; i <= GRID_MAX; i++)
        if (kitty_tile_live(&a->kitty, i)) return true;
    return false;
}

static bool hiding(App *a) {
    bool v;
    if (a->no_pause_arg) return false;
    if (hide_rule(a->url, &v)) return v;
    return a->hide_on_blur;
}

static void hide_window(App *a) {
    if (!hiding(a) || a->hidden || !a->has_tty) return;
    a->hidden = true;
    for (int i = GRID_MAX; i >= 0; i--) {
        if (!kitty_tile_live(&a->kitty, i)) continue;
        kitty_use(&a->kitty, i);
        kitty_clear(&a->kitty);
    }
    kitty_use(&a->kitty, 0);
    if (a->inline_mode) term_clear_inline(&a->term);
    else                writeall(a->term.fd, "\x1b[2J", 4);
    writeall(a->term.fd, "\x1b[?25l", 6);        // cursor off
}

static void show_window(App *a) {
    if (!a->hidden) return;
    a->hidden = false;
    a->kitty.grid_dirty = true;
    a->status_last.len = 0;
    a->tabs_last.len = 0;
    a->console_last.len = 0;
    a->help_last.len = 0;
    a->omni_last.len = 0;
}

static void resume_drawing(App *a) {
    a->paused = false;
    a->pause_wait = 0;
    show_window(a);
    a->last_hash = 0;
    a->kitty.grid_dirty = true;
    screencast_start(a);
    still_soon(a);
}

static void pause_drawing(App *a) {
    a->paused = true;
    a->expect_frame = 0;
    a->in_motion = false;
    a->motion_run = 0;
    still_cancel(a);
    app_cdp(a, "Page.stopScreencast", "");
    hide_window(a);
}

static bool pausing(App *a) {
    bool v;
    if (a->no_pause_arg) return false;
    if (pause_rule(a->url, &v)) return v;
    return a->pause_on_blur || hiding(a);
}

static bool media_pausing(App *a) {
    bool v;
    if (a->no_media_arg || a->no_pause_arg) return false;
    if (media_rule(a->url, &v)) return v;
    if (pause_rule(a->url, &v) && !v) return false;
    return a->media_pause_on_blur;
}

static const char *media_js(bool focused) {
    static char hold[1600], resume[1600];
    if (!hold[0]) {
        json_escape(hold, sizeof hold, MEDIA_HOLD_JS);
        json_escape(resume, sizeof resume, MEDIA_RESUME_JS);
    }
    return focused ? resume : hold;
}

static int media_hold(App *a) {
    return app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"%s\",\"returnByValue\":true,\"userGesture\":true",
        media_js(false));
}

static void media_play(App *a) {
    app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"%s\",\"returnByValue\":true,\"userGesture\":true",
        media_js(true));
}

static void media_focus(App *a, bool focused) {
    if (focused) {
        if (!a->media_held) return;
        a->media_held = false;
        media_play(a);
        return;
    }
    if (g_handoff_owed > now_sec()) return;
    if (being_driven(a)) return;
    if (!media_pausing(a)) return;
    a->media_held = true;
    app_req_note(a, media_hold(a), RQ_HELD);
}

// call while the socket is still on the outgoing page
void media_tab_leave(App *a) {
    if (!media_pausing(a) || being_driven(a)) return;
    media_hold(a);
}

void media_tab_enter(App *a) { media_play(a); }

static void handle_focus(App *a, bool focused) {
    if (a->blurred == focused) media_focus(a, focused);
    a->blurred = !focused;
    if (!pausing(a) || focused == !a->paused) return;
    if (!focused) {
        if (g_handoff_owed > now_sec()) return;
        if (being_driven(a)) return;
        pause_drawing(a);
        return;
    }
    resume_drawing(a);
}

static void check_driven(App *a) {
    if (!a->blurred) return;
    bool driven = being_driven(a);
    if (driven) media_focus(a, true);
    if (!pausing(a)) {
        if (a->paused) resume_drawing(a);
        return;
    }
    if (driven) {
        if (a->paused) resume_drawing(a);
        return;
    }
    if (g_handoff_owed > now_sec()) return;
    if (a->hidden) return;
    if (picture_up(a)) {
        a->pause_wait = 0;
    } else if (a->pause_wait >= 0) {
        double now = now_sec();
        if (a->pause_wait == 0) {
            if (a->paused) resume_drawing(a);
            a->pause_wait = now + PAUSE_WAIT;
            return;
        }
        if (now < a->pause_wait) return;
        a->pause_wait = -1;
    }
    if (!a->paused) pause_drawing(a);
}

// css point at the center of a cell, clamped to the picture
static void page_point(App *a, int cx, int cy, int *px, int *py) {
    Kitty *k = &a->kitty;
    if (cx < k->x)               cx = k->x;
    if (cx > k->x + k->cols - 1) cx = k->x + k->cols - 1;
    if (cy < k->y)               cy = k->y;
    if (cy > k->y + k->rows - 1) cy = k->y + k->rows - 1;
    double fx = (cx - k->x + 0.5) / (double)k->cols;
    double fy = (cy - k->y + 0.5) / (double)k->rows;
    *px = (int)(fx * a->css_w);
    *py = (int)(fy * a->css_h);
}

static int cdp_mods_of(const Event *ev) {
    int m = 0;
    if (ev->mods & MOD_ALT)   m |= 1;
    if (ev->mods & MOD_CTRL)  m |= 2;
    if (ev->mods & MOD_SHIFT) m |= 8;
    return m;
}

static void handle_mouse(App *a, Event *ev) {
    if (a->help_open) {
        if (ev->press && !ev->motion && ev->button < 3) help_toggle(a);
        return;
    }
    if (a->omni_open) {
        if (ev->press && !ev->motion && ev->button < 3) omni_close(a);
        return;
    }
    if (ev->button == BTN_NONE) {
        Kitty *k = &a->kitty;
        bool on = a->hover && !a->grid_on && !a->paused && !a->mouse_down &&
                  ev->mx >= k->x - 1 && ev->mx <= k->x + k->cols &&
                  ev->my >= k->y - 1 && ev->my <= k->y + k->rows;
        if (!on) {
            if (a->hovering) {
                a->hovering = false;
                queue_move(a, -1, -1, "none", 0, 0);
                term_log("%.3f hover out", now_sec());
            }
            return;
        }
        a->hovering = true;
        int hx, hy;
        page_point(a, ev->mx, ev->my, &hx, &hy);
        term_log("%.3f hover cell %d,%d -> %d,%d", now_sec(), ev->mx, ev->my,
                 hx, hy);
        queue_move(a, hx, hy, "none", 0, cdp_mods_of(ev));
        return;
    }
    if (console_mouse(a, ev)) return;
    if (!a->mouse_down && tabs_mouse(a, ev)) return;
    if (grid_mouse(a, ev)) return;
    Kitty *k = &a->kitty;
    bool inside = ev->mx >= k->x && ev->mx < k->x + k->cols &&
                  ev->my >= k->y && ev->my < k->y + k->rows;
    if (!inside && !a->mouse_down) return;

    if (ev->press && !ev->motion && ev->button < 3) a->console_focus = false;

    int x, y;
    page_point(a, ev->mx, ev->my, &x, &y);

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

    if (ev->press && !ev->motion && (ev->button == 1 ||
                                     (ev->button == 0 && (ev->mods & MOD_ALT)))) {
        a->click_newtab = true;
        char js[512], esc[1100];
        snprintf(js, sizeof js,
                 "(function(x,y){var e=document.elementFromPoint(x,y);"
                 "e=e&&e.closest?e.closest('a[href],area[href]'):null;"
                 "if(!e)return '';var h=e.href;"
                 "if(h&&h.baseVal!==undefined)h=h.baseVal;"     // svg <a>
                 "try{h=new URL(h,location.href).href}catch(_){return ''}"
                 "return /^(https?|file):/i.test(h)?h:''})(%d,%d)", x, y);
        json_escape(esc, sizeof esc, js);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"%s\",\"returnByValue\":true", esc), RQ_LINK);
        return;
    }
    if (a->click_newtab && ev->button < 3) {
        if (!ev->press && !ev->motion) a->click_newtab = false;
        return;
    }

    int cdp_mods = cdp_mods_of(ev);

    if (ev->button >= 3) {
        if (ev->button == 3 || ev->button == 4) {
            // wheel-up is negative
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
    if (a->pdf && ev->press) a->pdf_clicked = true;
    app_cdp(a, "Input.dispatchMouseEvent",
             "\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\","
             "\"buttons\":%d,\"clickCount\":1,\"modifiers\":%d",
             ev->press ? "mousePressed" : "mouseReleased", x, y, btn,
             ev->press ? 1 : 0, cdp_mods);
}

static void handle_paste(App *a, const char *text, size_t len) {
    if (!len) return;
    if (a->omni_open) {
        omni_paste(a, text, len);
        return;
    }
    if (a->editing) {
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

static void copy_selection(App *a) {
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"(function(){try{var e=document.activeElement;"
        "if(e&&(e.tagName==='INPUT'||e.tagName==='TEXTAREA')&&"
        "e.selectionStart!==e.selectionEnd)"
        "return e.value.substring(e.selectionStart,e.selectionEnd);}catch(x){}"
        "return window.getSelection().toString();})()\","
        "\"returnByValue\":true"), RQ_COPY);
}

// false means the key was not handled here
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

    case ACT_LINE_DOWN:
    case ACT_LINE_UP:
        if (((a->pdf && a->pdf_clicked) || a->player) &&
            special_key(a, ev->key, ev->mods)) return true;
        scroll_by(a, act == ACT_LINE_DOWN ? 40 : -40);
        return true;
    case ACT_SCROLL_DOWN:  scroll_by(a, 60);  return true;
    case ACT_SCROLL_UP:    scroll_by(a, -60); return true;
    case ACT_SCROLL_RIGHT: scroll_side(a, a->css_w / 4);  return true;
    case ACT_SCROLL_LEFT:  scroll_side(a, -a->css_w / 4); return true;
    case ACT_HALF_DOWN:    scroll_by(a, a->css_h / 2);  return true;
    case ACT_HALF_UP:      scroll_by(a, -(a->css_h / 2)); return true;
    case ACT_PAGE_DOWN:
    case ACT_PAGE_UP:
        if (a->player && ev->key == ' ' &&
            special_key(a, ev->key, ev->mods)) return true;
        scroll_by(a, act == ACT_PAGE_DOWN ? (int)(a->css_h * 0.9)
                                          : -(int)(a->css_h * 0.9));
        return true;
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
    case ACT_ADDRESS_BLANK:
        a->editing = true;
        a->prompt = 1;
        a->edit[0] = 0;
        a->edit_len = 0;
        return true;
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
    case ACT_SEARCH_TABS:      omni_show(a, OMNI_TABS);  return true;
    case ACT_SEARCH_HISTORY:   omni_show(a, OMNI_HIST);  return true;
    case ACT_SEARCH_BOOKMARKS: omni_show(a, OMNI_MARKS); return true;
    case ACT_BOOKMARK:         bookmark_toggle(a);       return true;
    case ACT_BACK:    nav_history(a, -1); return true;
    case ACT_FORWARD: nav_history(a, +1); return true;
    case ACT_RELOAD:
        a->nav_ours = true;
        app_cdp(a, "Page.reload", "\"ignoreCache\":false");
        a->loading = true;
        notify(a, "reloading");
        return true;
    case ACT_RELOAD_HARD:
        a->nav_ours = true;
        app_cdp(a, "Page.reload", "\"ignoreCache\":true");
        a->loading = true;
        notify(a, "reloading, cache ignored");
        return true;
    case ACT_COPY: copy_selection(a); return true;
    case ACT_COPY_CONSOLE: {
        int n = console_copy(a);
        notify(a, n ? "console copied" : "the console has said nothing yet");
        return true;
    }
    case ACT_RESUME: exec_resume(a); return true;
    case ACT_MERGE:  merge_ask(a);   return true;
    case ACT_RECORD: record_toggle(a); return true;
    case ACT_PLAY_PAUSE: {
        char esc[1200];
        json_escape(esc, sizeof esc, PLAY_PAUSE_JS);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"%s\",\"returnByValue\":true,\"userGesture\":true",
            esc), RQ_PLAY);
        return true;
    }
    case ACT_GRID:
        if (a->grid_on) a->grid_auto = false;
        grid_toggle(a);
        return true;
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
        if (!a->insert) return false;
        run_js(a, "document.activeElement&&document.activeElement.blur()");
        a->insert = false;
        still_soon(a);   // losing the focus ring is a repaint too
        return true;
    case ACT_FOCUS_INPUT:
        // a field on screen wins, but a page whose only one is below the fold
        // (hacker news keeps its search at the very bottom) used to focus
        // nothing at all. Fall back to the first field anywhere and bring it
        // into view, centred, leaving one already in view where it is.
        run_js(a,
            "(function(){var l=document.querySelectorAll("
            "'input:not([type=hidden]):not([type=checkbox]):not([type=radio])"
            ":not([type=submit]):not([type=button]):not([type=file]),"
            "textarea,[contenteditable]');var vis=null,any=null;"
            "for(var i=0;i<l.length;i++){var e=l[i],r=e.getBoundingClientRect();"
            "if(e.disabled||e.readOnly||r.width<2||r.height<2)continue;"
            "if(!any)any=e;"
            "if(r.bottom>=0&&r.top<=innerHeight){vis=e;break;}}"
            "var t=vis||any;if(!t)return;t.focus();"
            "if(t.scrollIntoViewIfNeeded)t.scrollIntoViewIfNeeded(true);"
            "else t.scrollIntoView({block:'center'});})()");
        // a focus ring is a repaint chrome will not screencast on its own, and
        // an action that only runs js asks for no frame, so the change sits
        // there unseen until something else moves. Ask for the still.
        still_soon(a);
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
    case ACT_LARGER:
        if (a->inline_mode) resize_box(a, +1, 0, true); else zoom_by(a, 1.25);
        return true;
    case ACT_SMALLER:
        if (a->inline_mode) resize_box(a, -1, 0, true); else zoom_by(a, 1.0 / 1.25);
        return true;
    case ACT_ZOOM_RESET: {
        a->zoom = 1.0;
        a->want_width = 0;
        a->fit_w = 0;
        bool was_pinned = a->box_cols > 0;
        a->box_cols = a->want_cols = 0;
        if (was_pinned) {
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
    case ACT_STATUS:  a->hide_status = !a->hide_status; return true;
    case ACT_TRACE:   trace_toggle(a);   return true;
    case ACT_BLUR_PAUSE:
        if (a->pause_on_blur || a->hide_on_blur) {
            a->blur_was_pause = a->pause_on_blur;
            a->blur_was_hide  = a->hide_on_blur;
            a->pause_on_blur = a->hide_on_blur = false;
            if (a->paused) resume_drawing(a); else show_window(a);
            notify(a, "blur pause off");
        } else {
            a->pause_on_blur = a->blur_was_pause || !a->blur_was_hide;
            a->hide_on_blur  = a->blur_was_hide;
            notify(a, a->hide_on_blur ? "blur pause on, hiding"
                                      : "blur pause on");
        }
        return true;
    case ACT_MOUSE_FREE:
        if (!a->has_tty) return true;
        a->mouse_free = !a->mouse_free;
        term_mouse(&a->term, !a->mouse_free);
        if (a->mouse_free) {
            if (a->hovering) {
                a->hovering = false;
                queue_move(a, -1, -1, "none", 0, 0);
            }
            a->mouse_down = false;
            notify(a, "mouse to the terminal");
        } else {
            if (a->hover) term_hover(&a->term, true);
            notify(a, "mouse to the page");
        }
        return true;
    }
    return false;
}

static void handle_key(App *a, Event *ev) {
    if (a->console_focus) {
        Act in_console = keys_lookup(ev->mods, ev->key);
        if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) {
            if (in_console == ACT_QUIT)    { g_quit = 1; return; }
            if (in_console == ACT_CONSOLE) { console_toggle(a); return; }
            if (in_console == ACT_COPY_CONSOLE || in_console == ACT_RESUME) {
                do_action(a, ev, in_console);
                return;
            }
        }
        if (console_key(a, ev)) return;
    }
    if (help_key(a, ev)) return;
    if (omni_key(a, ev)) return;
    if (hint_key(a, ev)) return;
    if (grid_key(a, ev)) return;
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
        if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) {
            Act in_bar = keys_lookup(ev->mods, ev->key);
            if (in_bar == ACT_QUIT || in_bar == ACT_TAB_NEW ||
                in_bar == ACT_TAB_CLOSE || in_bar == ACT_TAB_NEXT ||
                in_bar == ACT_TAB_PREV ||
                (in_bar >= ACT_TAB_1 && in_bar <= ACT_TAB_9)) {
                a->editing = false;
                a->prompt = 0;
                do_action(a, ev, in_bar);
                return;
            }
        }
        if (ev->text[0] && a->edit_len + 8 < sizeof a->edit) {
            size_t n = strlen(ev->text);
            memcpy(a->edit + a->edit_len, ev->text, n);
            a->edit_len += n;
        }
        return;
    }

    bool prefix = false;
    Act act = keys_lookup_seq(a->pend_mods, a->pend_key, ev->mods, ev->key, &prefix);
    a->pend_mods = a->pend_key = 0;

    if (!(ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) && a->insert &&
        act != ACT_INSERT_OFF) {
        act = ACT_NONE;
        prefix = false;
    }
    if (prefix) {
        a->pend_mods = ev->mods;
        a->pend_key = ev->key;
        return;
    }
    if (do_action(a, ev, act)) return;

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

        int pw = png_width(data, dlen);
        bool sharp = pw > 0 && a->frame_w > 0 &&
                     pw * 20 >= a->frame_w * 19 &&
                     a->frame_w * 20 >= a->still_w * 19;
        a->expect_frame = 0;
        a->unwedge_run = 0;

        // ack before drawing: chrome will not start the next frame until this
        // lands, and the draw below is a blocking tty write. Sending is
        // write-only and never refills ws->msg, so data/dlen stay valid.
        app_cdp(a, "Page.screencastFrameAck", "\"sessionId\":%d", (int)sid);

        double t_arrive = now_sec();

        if (a->grid_on) dlen = 0;
        if (a->hidden) dlen = 0;

        if (dlen) {
            uint64_t h = fnv1a(data, dlen);
            if (h != a->last_hash) {
                a->last_hash = h;
                double t0 = now_sec();
                draw_frame(a, data, dlen);
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
                double chrome_ms = prev_draw > 0
                    ? (t_arrive - prev_draw) * 1000.0 : 0;
                term_log("%.3f frame %u: %dpx of %d, %zu KB b64, chrome %.1f ms, "
                         "ours %.1f ms (write %.1f), gap %.1f ms, %.1f fps%s%s",
                         t1, a->frames, pw, a->frame_w, dlen / 1024, chrome_ms,
                         (t1 - t_arrive) * 1000.0, a->last_write_ms,
                         gap * 1000.0, a->fps, a->in_motion ? " [motion]" : "",
                         sharp ? "" : " [soft]");

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

        if (a->motion_auto && !a->in_motion && a->motion_run >= MOTION_RUN &&
            now_sec() - a->last_input < MOTION_HOLD) {
            a->in_motion = true;
            term_log("%.3f motion on", now_sec());
            relayout(a);
        }

        if (!a->in_motion)
            a->still_at = sharp ? 0 : now_sec() + STILL_WAIT;

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
        a->expect_frame = now_sec() + 2.0;
        a->kitty.grid_dirty = true;
        a->pdf = a->pdf_clicked = false;
        a->player = false;
        size_t n;
        const char *f = json_str(msg, "id", &n);
        if (f && n < sizeof a->frame) snprintf(a->frame, sizeof a->frame, "%.*s",
                                               (int)n, f);
        const char *u = json_str(msg, "url", &n);
        if (u) {
            json_unescape(a->url, sizeof a->url, u, n);
            a->title[0] = 0;
            a->nav_seq++;
            if (a->fit_w > 0) {
                a->fit_w = 0;
                relayout(a);
            }
            session_write(a);
            if (!a->nav_ours && now_sec() - a->nav_asked > 15.0)
                record_goto(a, a->url);
        }
        a->nav_ours = false;
        a->nav_asked = 0;
        return;
    }

    if (strstr(msg, "Page.frameRequestedNavigation")) {
        size_t n;
        const char *f = json_str(msg, "frameId", &n);
        if (a->frame[0] && (!f || n != strlen(a->frame) ||
                            memcmp(f, a->frame, n) != 0))
            return;
        a->nav_asked = now_sec();
        return;
    }

    if (strstr(msg, "Page.navigatedWithinDocument")) {
        size_t n;
        const char *f = json_str(msg, "frameId", &n);
        if (a->frame[0] && (!f || n != strlen(a->frame) ||
                            memcmp(f, a->frame, n) != 0))
            return;
        const char *u = json_str(msg, "url", &n);
        if (!u) return;
        json_unescape(a->url, sizeof a->url, u, n);
        session_write(a);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"document.title\",\"returnByValue\":true"), RQ_TITLE);
        return;
    }

    if (strstr(msg, "Runtime.bindingCalled") && strstr(msg, "__webmode")) {
        size_t n;
        const char *p = json_str(msg, "payload", &n);
        a->insert = (p && n && p[0] == '1');
        a->player = (p && n && p[0] == '2');
        return;
    }

    if (strstr(msg, "Runtime.bindingCalled") && strstr(msg, "__webrec")) {
        size_t n;
        const char *p = json_str(msg, "payload", &n);
        char pl[2048];
        json_unescape(pl, sizeof pl, p ? p : "", p ? n : 0);
        record_event(a, pl);
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

    if (strstr(msg, "Page.frameStartedLoading")) {
        a->loading = true;
        hint_cancel(a);
        return;
    }

    if (strstr(msg, "Page.loadEventFired") ||
        strstr(msg, "Page.frameStoppedLoading")) {
        if (!a->loading) return;
        a->loading = false;
        a->load_seq++;
        a->last_hash = 0;
        a->kitty.grid_dirty = true;
        screencast_start(a);
        still_soon(a);
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

    // a reply leads with its id; an event never does
    int id = 0;
    if (msg[0] == '{' && !strncmp(msg + 1, "\"id\":", 5))
        id = (int)strtol(msg + 6, NULL, 10);
    switch (id ? app_req_take(a, id) : RQ_NONE) {
    case RQ_FIT: {
        if (a->fit_seq != a->nav_seq) return;
        int want = (int)json_num(msg, "value", 0);
        if (want > a->css_w + 8 && want < 8000) {
            if (a->want_width > 0) {
                char m[80];
                snprintf(m, sizeof m, "width %dpx - page needs %dpx",
                         a->css_w, want);
                notify(a, m);
                return;
            }
            a->fit_w = want;
            relayout(a);
            if (a->zoom_eff < a->zoom * 0.97) {
                char m[80];
                snprintf(m, sizeof m, "zoom %.0f%% - page needs %dpx",
                         a->zoom_eff * 100, want);
                notify(a, m);
            }
        }
        return;
    }

    case RQ_PDF: {
        size_t n;
        const char *v = json_str(msg, "value", &n);
        a->pdf = v && n == 15 && !memcmp(v, "application/pdf", 15);
        return;
    }

    case RQ_MODE: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        a->insert = v && n && v[0] == '1';
        a->player = v && n && v[0] == '2';
        return;
    }

    case RQ_FRAME: {
        // the top frame's id is the first one in the tree
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
            clipboard_put(a->url);
            notify(a, "copied url");
        }
        return;
    }

    case RQ_PLAY: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        if (!v || !n)                              notify(a, "nothing to play here");
        else if (n == 6 && !memcmp(v, "paused", 6)) notify(a, "paused");
        else                                        notify(a, "playing");
        return;
    }

    case RQ_HELD: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        if (!v || !n) a->media_held = false;
        return;
    }

    case RQ_LINK: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        char url[1100];
        size_t len = (v && n) ? json_unescape(url, sizeof url, v, n) : 0;
        if (!len) {
            notify(a, "no link there");
            return;
        }
        if (tab_open_bg(a, url)) notify(a, "opened in a tab behind this one");
        return;
    }

    case RQ_SELECTOR: {
        size_t n;
        const char *v = json_eval_str(msg, &n);
        char selector[2048];
        size_t len = (v && n) ? json_unescape(selector, sizeof selector, v, n) : 0;
        if (len) {
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
        if (v) {
            json_unescape(a->title, sizeof a->title, v, n);
            session_write(a);
        }
        return;
    }

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
        record_history(a, a->hist_delta);
        a->nav_ours = true;
        app_cdp(a, "Page.navigateToHistoryEntry", "\"entryId\":%d", entry);
        a->loading = true;
        return;
    }

    case RQ_SCRIPT: script_reply(a, msg); return;

    case RQ_SHOT_READY:
        if (a->shot_state == SHOT_SETTLE) shot_capture(a);
        return;

    case RQ_SHOT:
        if (a->shot_state == SHOT_SENT) shot_write(a, msg);
        return;

    case RQ_STILL:
        still_draw(a, msg);
        return;
    }
}

// ------------------------------------------------------------------ popups

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

static void popup_note(App *a, const char *id, size_t n) {
    if (popup_find(a, id, n) >= 0) return;
    if (a->npopups >= POPUP_MAX) popup_drop(a, 0);
    Popup *p = &a->popups[a->npopups++];
    snprintf(p->target, sizeof p->target, "%.*s", (int)n, id);
    p->at = now_sec();
}

static bool popup_navigable(const char *url, size_t n) {
    return (n > 8 && memcmp(url, "https://", 8) == 0) ||
           (n > 7 && memcmp(url, "http://", 7) == 0) ||
           (n > 7 && memcmp(url, "file://", 7) == 0);
}

static void on_target_message(App *a, const char *msg) {
    if (grid_reply(a, msg)) return;
    bool made = strstr(msg, "Target.targetCreated") != NULL;
    bool moved = !made && strstr(msg, "Target.targetInfoChanged") != NULL;
    bool gone = !made && !moved && strstr(msg, "Target.targetDestroyed") != NULL;
    if (!made && !moved && !gone) return;

    size_t n = 0;
    const char *id = json_str(msg, "targetId", &n);
    if (!id || !n || n >= sizeof a->popups[0].target) return;

    if (gone) {
        char t[96];
        snprintf(t, sizeof t, "%.*s", (int)n, id);
        tab_forget(a, t);
        popup_drop(a, popup_find(a, id, n));
        return;
    }
    {
        char t[96];
        snprintf(t, sizeof t, "%.*s", (int)n, id);
        int ti = tab_index_of(a, t);
        if (ti >= 0 && ti != a->tab && a->tabs[ti].claimed) {
            size_t un = 0, tn2 = 0;
            const char *u2 = json_str(msg, "url", &un);
            const char *t2 = json_str(msg, "title", &tn2);
            if (u2 && un) json_unescape(a->tabs[ti].url, sizeof a->tabs[ti].url, u2, un);
            if (t2 && tn2) json_unescape(a->tabs[ti].title, sizeof a->tabs[ti].title, t2, tn2);
        }
    }
    if (tab_target_is(a, id, n)) return;

    size_t tn = 0;
    const char *type = json_str(msg, "type", &tn);
    if (!type || tn != 4 || memcmp(type, "page", 4) != 0) return;

    if (made) {
        size_t on = 0;
        const char *opener = json_str(msg, "openerId", &on);
        bool mine = opener && on && tab_target_is(a, opener, on);
        term_log("%.3f page target %.*s appeared, opener %.*s (%s)", now_sec(),
                 (int)n, id, (int)(opener ? on : 1), opener ? opener : "-",
                 mine ? "ours" : "not ours");
        if (!mine) return;
    }
    int i = popup_find(a, id, n);
    if (!made && i < 0) return;
    if (i >= 0 && now_sec() - a->popups[i].at > POPUP_GRACE) {
        popup_drop(a, i);
        return;
    }

    size_t un = 0;
    const char *u = json_str(msg, "url", &un);
    char url[1100];
    if (!u || !un || !popup_navigable(u, un)) {
        if (made) popup_note(a, id, n);
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
        "  own. The window starts on the first. An argument that is not an\n"
        "  address opens the bookmark whose name or address holds every word\n"
        "  of it, and is a host or a search when no bookmark does.\n"
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
        "  --no-status start with the status line hidden (^G toggles it)\n"
        "  --no-clear  leave the window on screen on exit instead of erasing it\n"
        "  --mute      start with the page's audio switched off\n"
        "  --no-hover  do not tell the page where the pointer is unless a\n"
        "              button is down\n"
        "  --eval JS   run javascript in the page and print what it answers\n"
        "  --delay MS  pause between lines of piped javascript\n"
        "  --step      wait for a key between those lines\n"
        "  --timeout S how long a line waits before giving up (default 5)\n"
        "  --json      script output as one JSON object per value\n"
        "  --screenshot F   write the loaded page to F as a png and exit\n"
        "              (- for stdout; the shot waits for the page to arrive)\n"
        "  --shot-size WxH[@S]  the viewport --screenshot uses, in css\n"
        "              pixels (default 1280x800). @S draws it at S device\n"
        "              pixels per css pixel, so 702x936@2 lays the page out\n"
        "              for a 702px viewport and writes a 1404x1872 png\n"
        "  --login     open a window to sign in with, on the same profile\n"
        "  --keep      leave chrome running on exit so the next start is instant\n"
        "  --open URL  open URL in a tab of the window most recently used,\n"
        "              and exit. Nothing running is an error, so a caller can\n"
        "              fall back to starting one\n"
        "  --endpoint  print every running window as JSON, its address on the\n"
        "              clipboard, and exit. With nothing running, start one:\n"
        "              a window in this terminal when there is one, otherwise\n"
        "              a window nothing shows, and print that\n"
        "  --mcp       drive the window on screen as an MCP server on stdio,\n"
        "              for an agent to work the page you are watching\n"
        "  --list      list the chrome processes web has running, with pids,\n"
        "              and say which of them a new window could still adopt\n"
        "  --kill      quit this profile's windows and end its browsers,\n"
        "              including any nothing can reach any more. A window it\n"
        "              cannot place is named rather than ended\n"
        "  --exec CMD  run CMD against this window, its output in the console\n"
        "  --slowmo MS pause MS between the actions of what --exec starts, so\n"
        "              a run can be watched rather than only finished\n"
        "  --freeze    hold the page where a driver failed instead of tearing\n"
        "              it down: the console, `P` and the labels all work on it,\n"
        "              and alt+enter lets the run carry on\n"
        "  --grid      show the grid whenever there is more than one page, so a\n"
        "              run with several workers opens as one tile each\n"
        "  --tmux-zoom grow the window to fill the pane while tmux has it\n"
        "              zoomed, and put its size back when the zoom ends\n"
        "  --search T  the search a phrase becomes, as a url with %%s where the\n"
        "              words go (default google)\n"
        "  --record F  write what is done to the page to F as a playwright\n"
        "              spec, until alt+r stops it\n"
        "  --profile N run in a profile of its own - its own logins, history\n"
        "              and browser, and none of the windows already up. \"-\"\n"
        "              is a throwaway one, taken away again on exit\n"
        "  --name N    call this window N, so WEB_WINDOW=N picks it out for a\n"
        "              driver instead of its pid. Anything but a number\n"
        "  --port N    fix chrome's devtools port so playwright can find it\n"
        "  --no-pause  keep drawing while the terminal is not focused. What\n"
        "              --exec starts already draws through a blur\n"
        "  --hide-on-blur   take the window off the screen while the terminal\n"
        "              is not focused, instead of leaving the last frame up\n"
        "  --no-media-pause let a video or a track in the page go on playing\n"
        "              while the terminal is not focused\n"
        "  --extension D  load the unpacked extension in folder D. Whatever\n"
        "              sits in ~/.config/web/extensions is loaded anyway\n"
        "  --no-graphics-check start even when the terminal does not answer\n"
        "              the kitty graphics question\n"
        "  --raw-keys  let a key the page did not want reach the window\n"
        "              system. On macOS that routes it through the menu bar,\n"
        "              which on some pages costs seconds of the thread every\n"
        "              frame and every reply comes from\n");
}

void session_init(App *a) {
    if (a->chrome.profile[0]) driving_sweep(a->chrome.profile);
    app_cdp(a, "Page.enable", "");
    app_cdp(a, "Runtime.enable", "");

    app_req_note(a, app_cdp(a, "Page.getFrameTree", ""), RQ_FRAME);

    if (a->ua_patch_req && a->ua[0]) {
        char esc[1100];
        json_escape(esc, sizeof esc, a->ua);
        app_cdp(a, "Emulation.setUserAgentOverride", "\"userAgent\":\"%s\"", esc);
    }
    app_cdp(a, "Emulation.setScrollbarsHidden", "\"hidden\":true");
    // headless has no focused window, so a page never counts as active and
    // paints no focus ring or caret however the focus is set. Pointer activity
    // is what wakes it, which is why a hover used to be what showed the field.
    app_cdp(a, "Emulation.setFocusEmulationEnabled", "\"enabled\":true");
    {
        char esc[2048];
        json_escape(esc, sizeof esc, FOCUS_WATCHER);
        // a named world must be claimed before the script that lands in it
        app_cdp(a, "Runtime.addBinding",
                 "\"name\":\"__webmode\",\"executionContextName\":\"%s\"",
                 WEB_WORLD);
        app_cdp(a, "Runtime.addBinding",
                 "\"name\":\"__webrec\",\"executionContextName\":\"%s\"",
                 WEB_WORLD);
        hint_install(a);
        app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
                 "\"source\":\"%s\",\"worldName\":\"%s\","
                 "\"runImmediately\":true", esc, WEB_WORLD);
        {
            char src[3072], hesc[6144];
            snprintf(src, sizeof src, WEB_HELPERS, (int)(a->script.timeout * 1000));
            json_escape(hesc, sizeof hesc, src);
            app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
                     "\"source\":\"%s\",\"runImmediately\":true", hesc);
        }
        record_install(a);
        if (a->claim_keys) {
            json_escape(esc, sizeof esc, KEY_CLAIMER);
            app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
                     "\"source\":\"%s\",\"worldName\":\"%s\","
                     "\"runImmediately\":true", esc, WEB_WORLD);
        }
        json_escape(esc, sizeof esc, FOCUS_READ);
        app_req_note(a, app_cdp(a, "Runtime.evaluate",
            "\"expression\":\"%s\",\"returnByValue\":true", esc), RQ_MODE);
    }
}

void ask_where(App *a) {
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"location.href\",\"returnByValue\":true"), RQ_URL);
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"document.title\",\"returnByValue\":true"), RQ_TITLE);
}

static void leave_browser(App *a) {
    Chrome *c = &a->chrome;
    chrome_unwatch(c);
    if (c->ws.fd > 0 && !c->ws.closed) cdp_call(c, "Page.stopScreencast", "");

    if (a->gave_tabs) {
        if (c->ws.fd > 0) ws_close(&c->ws);
        return;
    }

    tabs_close_others(a);

    if (c->foreign) {
        if (a->ntabs > 0 && a->tabs[a->tab].ours) chrome_close_target(c);
        if (c->ws.fd > 0) ws_close(&c->ws);
        return;
    }

    bool kept = a->keep || chrome_is_kept(c);

    pid_t others[PROC_MAX];
    bool shared = running_windows(c->profile, others, PROC_MAX) > 0 ||
                  driver_attached(a);
    if (kept) chrome_park(c);
    if (kept || shared) {
        chrome_close_target(c);
        if (c->ws.fd > 0) ws_close(&c->ws);
        return;
    }
    chrome_kill_bg(c);
}

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
    c->foreign = true;
    c->port = port;
    c->ws = (WS){0};
    c->next_id = 1;
    if (chrome_attach(c) < 0) {
        snprintf(msg, cap, "port %d stopped answering; nothing left to draw", port);
        g_quit = 1;
        return -1;
    }

    memset(a->reqs, 0, sizeof a->reqs);
    a->pend.kind = PEND_NONE;
    a->title[0] = 0;
    a->frame[0] = 0;
    a->loading = false;
    a->insert = false;
    a->mouse_down = false;
    a->hovering = false;
    a->click_newtab = false;
    a->hint_on = false;
    a->hint_deadline = 0;
    a->pend_key = 0;
    a->nav_seq++;
    a->fit_w = 0;
    a->last_hash = 0;
    a->kitty.grid_dirty = true;

    tabs_init(a);
    session_init(a);
    ask_where(a);
    relayout(a);

    snprintf(msg, cap, "attached to port %d", port);
    return 0;
}

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
    a.motion_auto = true;
    a.motion_scale = (getenv("SSH_CONNECTION") || getenv("SSH_TTY"))
        ? MOTION_SCALE_SSH : MOTION_SCALE;
    a.settle_ms = SETTLE_WAIT;
    a.zoom = 1.5;
    a.want_rows = 40;
    a.want_cols = 80;
    snprintf(a.search, sizeof a.search, "%s", SEARCH_URL);
    a.pause_on_blur = true;
    a.media_pause_on_blur = true;
    a.hover = true;
    a.extensions = true;
    a.claim_keys = true;
    a.exec_fd = -1;
    a.fit_width = true;
    a.inline_mode = true;
    a.clear_exit = true;

    const char *want_profile = getenv("WEB_PROFILE");
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--profile")) { want_profile = argv[i + 1]; break; }
    if (want_profile && *want_profile && chrome_profile_set(want_profile) < 0) {
        fprintf(stderr, "web: --profile wants a name of letters, digits, "
                        "'.', '_' or '-', or \"-\" for a throwaway one\n");
        return 1;
    }
    load_ua(&a);
    config_load(&a);
    bool show = false, login = false;
    bool endpoint_only = false, list_only = false, kill_only = false;
    bool mcp_only = false;
    const char *exec_cmd = NULL, *hand_to_window = NULL, *rec_path = NULL;
    double drain_at = 0;
    int port = 0;                     // 0 = let chrome pick a free one
    const char *eval_js = NULL;
    const char *urls[TAB_MAX];
    int nurls = 0, extra_urls = 0;
    const char *start = "about:blank";
    script_init(&a);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "auto")) { a.motion_auto = true; continue; }
            a.motion_auto = false;
            a.want_scale = atof(v);
            if (a.want_scale < 0.1) a.want_scale = 0.1;
            if (a.want_scale > 3.0) a.want_scale = 3.0;
        } else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
            a.zoom = atof(argv[++i]);
            a.want_width = 0;
            if (a.zoom < 0.5) a.zoom = 0.5;
            if (a.zoom > 3.0) a.zoom = 3.0;
        } else if (!strcmp(argv[i], "--inline")) {
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--full")) {
            a.inline_mode = false;
        } else if (!strcmp(argv[i], "--rows") && i + 1 < argc) {
            a.want_rows = atoi(argv[++i]);
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--cols") && i + 1 < argc) {
            a.want_cols = atoi(argv[++i]);
            a.inline_mode = true;
        } else if (!strcmp(argv[i], "--clear")) {
            a.clear_exit = true;
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
        } else if (!strcmp(argv[i], "--list")) {
            list_only = true;
        } else if (!strcmp(argv[i], "--kill")) {
            kill_only = true;
        } else if (!strcmp(argv[i], "--mcp")) {
            mcp_only = true;
        } else if (!strcmp(argv[i], "--exec") && i + 1 < argc) {
            exec_cmd = argv[++i];
        } else if (!strcmp(argv[i], "--mute")) {
            a.mute = true;
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            a.shot_path = argv[++i];
        } else if (!strcmp(argv[i], "--shot-size") && i + 1 < argc) {
            // WxH, and an optional @S for the device pixel ratio: the page is
            // laid out as if the viewport were W css px and drawn at W*S.
            const char *v = argv[++i];
            int w = 0, hh = 0; double sc = 0; char sep = 0;
            int n = sscanf(v, "%d%*[xX]%d%c%lf", &w, &hh, &sep, &sc);
            if (n < 2 || w < 1 || hh < 1 || (n > 2 && sep != '@')) {
                fprintf(stderr, "web: --shot-size wants WxH or WxH@S, not \"%s\"\n", v);
                return 2;
            }
            if (n > 3) {
                if (sc < 0.1) sc = 0.1;
                if (sc > 4.0) sc = 4.0;
                a.shot_scale = sc;
            }
            a.shot_w = w; a.shot_h = hh;
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
        } else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            i++;                      // read above the loop
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            const char *n = argv[++i];
            // strspn past the digits: all digits is a pid, not a name
            if (!*n || n[strspn(n, "0123456789")] == 0) {
                fprintf(stderr, "web: --name wants a name, not a number\n");
                return 1;
            }
            snprintf(a.name, sizeof a.name, "%s", n);
        } else if (!strcmp(argv[i], "--freeze")) {
            a.freeze = true;
        } else if (!strcmp(argv[i], "--grid")) {
            a.grid_auto = true;
        } else if (!strcmp(argv[i], "--tmux-zoom")) {
            a.tmux_zoom = true;
        } else if (!strcmp(argv[i], "--no-graphics-check")) {
            a.no_gfx_check = true;
        } else if (!strcmp(argv[i], "--search") && i + 1 < argc) {
            snprintf(a.search, sizeof a.search, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            rec_path = argv[++i];
        } else if (!strcmp(argv[i], "--slowmo") && i + 1 < argc) {
            a.slowmo = atoi(argv[++i]);
            if (a.slowmo < 0) a.slowmo = 0;
            if (a.slowmo > 60000) a.slowmo = 60000;
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port < 1 || port > 65535) {
                fprintf(stderr, "web: --port wants a number from 1 to 65535\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--no-pause")) {
            a.pause_on_blur = false;
            a.hide_on_blur = false;
            a.no_pause_arg = true;
        } else if (!strcmp(argv[i], "--hide-on-blur")) {
            a.hide_on_blur = true;
        } else if (!strcmp(argv[i], "--no-media-pause")) {
            a.media_pause_on_blur = false;
            a.no_media_arg = true;
        } else if (!strcmp(argv[i], "--extension") && i + 1 < argc) {
            if (!ext_add(argv[++i])) return 1;
        } else if (!strcmp(argv[i], "--no-hover")) {
            a.hover = false;
        } else if (!strcmp(argv[i], "--login")) {
            login = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
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
    search_set(a.search);
    bool endpoint_bg = false, endpoint_win = false;
    if (endpoint_only) {
        if (print_sessions()) return 0;
        // a terminal to draw in gets a window in it; anything else, a caller
        // waiting on the address, gets one it cannot see
        if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) endpoint_win = true;
        else endpoint_bg = endpoint_detach();
    }
    if (mcp_only)      return mcp_serve();
    if (list_only)     return print_browsers();
    if (kill_only)     return kill_everything();
    if (hand_to_window) return hand_url(hand_to_window);

    a.status_open = !a.hide_status;

    if (a.extensions) ext_scan();

    if (eval_js) {
        script_push(&a, eval_js);
        a.script.drain_exit = true;
    } else if (!endpoint_bg && !isatty(STDIN_FILENO)) {
        script_load(&a, "-");
    }

    char welcome[1200] = "";
    if (a.show_start && !a.shot_path && !eval_js && isatty(STDIN_FILENO)) {
        char page[600];
        if (start_page_path(page, sizeof page))
            start_url(page, welcome, sizeof welcome);
    }

    char first[1200];
    start_url(start, first, sizeof first);
    if (welcome[0] && !nurls) snprintf(first, sizeof first, "%s", welcome);
    snprintf(a.url, sizeof a.url, "%s", first);

    term_log("%.3f start", now_sec());
    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_winch);
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_term);
    signal(SIGUSR1, on_hand);

    term_probe(&a.term);
    a.has_tty = isatty(a.term.fd);
    if (a.has_tty && !a.no_gfx_check && !login &&
        !term_graphics_ok(&a.term, getenv("TMUX") != NULL)) {
        if (a.shot_path) {
            a.has_tty = false;      // the picture still goes to the file
        } else {
            fprintf(stderr, "web: this terminal does not draw kitty graphics; "
                            "run it in ghostty or kitty\n");
            if (getenv("TMUX"))
                fprintf(stderr, "web: in tmux it also needs "
                                "`set -g allow-passthrough all`\n");
            fprintf(stderr, "web: --no-graphics-check starts anyway\n");
            return 1;
        }
    }
    a.stdout_tty = isatty(STDOUT_FILENO);
    a.shot_stdout = a.shot_path && !strcmp(a.shot_path, "-");
    if (a.shot_stdout && a.stdout_tty) {
        fprintf(stderr, "web: --screenshot - needs somewhere to write to; "
                        "redirect it or name a file\n");
        return 1;
    }
    if (a.shot_path && !a.has_tty) a.fit_width = false;
    int fw, fh;
    first_size(&a, &fw, &fh);

    if (login) {
        if (chrome_launch(&a.chrome, first, 1200, 900, true, a.mute, a.ua, false, 0) < 0)
            return 1;
        fprintf(stderr, "web: sign in, then press Enter here.\n");
        for (;;) {
            if (a.chrome.pid > 0 &&
                waitpid(a.chrome.pid, NULL, WNOHANG) == a.chrome.pid) {
                a.chrome.pid = 0;
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
            // the browser itself, not its process group
            kill(a.chrome.pid, SIGTERM);
            for (int i = 0; i < 200; i++) {
                if (waitpid(a.chrome.pid, NULL, WNOHANG) == a.chrome.pid) break;
                struct timespec ts = {0, 25 * 1000000};
                nanosleep(&ts, NULL);
            }
        }
        return 0;
    }

    // an extension is only there once the browser is up, and a page chrome
    // opened for itself at launch loads before any of them
    bool early_url = a.ua[0] != 0 && !ext_count();
    if (chrome_launch(&a.chrome, early_url ? first : "about:blank",
                      fw, fh, show, a.mute, a.ua, true, port) < 0)
        return 1;
    term_log("%.3f chrome up on port %d (%s)", now_sec(), a.chrome.port,
             a.chrome.adopted ? "adopted" : "launched");
    if (chrome_attach(&a.chrome) < 0) { chrome_kill(&a.chrome); return 1; }
    term_log("%.3f attached", now_sec());
    // an adopted browser is one an earlier run left up, extensions and all
    if (ext_count() && !a.chrome.adopted) {
        ext_load(&a.chrome);
        term_log("%.3f extensions in", now_sec());
    }
    tabs_init(&a);
    if (a.has_tty && !a.shot_path && !a.script.drain_exit &&
        chrome_watch(&a.chrome) < 0)
        term_log("no browser socket; pages that open windows will be missed");
    if (a.keep && !a.chrome.foreign) chrome_mark_kept(&a.chrome);
    {
        char ua[512];
        int rc = chrome_user_agent(&a.chrome, ua, sizeof ua);
        if (rc >= 0 && strcmp(ua, a.ua) != 0) {
            snprintf(a.ua, sizeof a.ua, "%s", ua);
            save_ua(&a);
        }
        a.ua_patch_req = rc == 1;
    }

    if (endpoint_win) endpoint_announce(&a);

    if (a.has_tty) term_enter(&a.term, a.inline_mode);
    if (a.has_tty && a.hover) term_hover(&a.term, true);
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
        int status = a.status_open ? 1 : 0;
        int rows = a.want_rows > 0 ? a.want_rows + status : a.term.rows / 2;
        if (rows > a.term.rows - 1) rows = a.term.rows - 1;
        if (rows < 4) rows = 4;
        term_reserve_inline(&a.term, rows);
        a.box_rows = rows - status;
        if (a.want_cols > 0) a.box_cols = a.want_cols;
    }
    g_app = &a;
    g_input_pump = pump_input;
    bool tmux = getenv("TMUX") != NULL;
    if (a.has_tty) kitty_init(&a.kitty, a.term.fd, tmux);

    session_init(&a);
    bool launched = !a.chrome.adopted && !a.chrome.foreign;
    if (a.chrome.foreign && !nurls) ask_where(&a);
    else if (early_url && launched) a.loading = true;
    else navigate(&a, first);
    term_log("%.3f navigate %s", now_sec(),
             early_url && launched ? "was on the command line" : "sent");

    if (nurls > 1 && !a.shot_path && !a.script.drain_exit) {
        for (int i = 1; i < nurls; i++) {
            char u[1200];
            start_url(urls[i], u, sizeof u);
            if (!tab_open_url(&a, u)) break;
        }
        tab_go(&a, 0);
    }
    if (welcome[0] && nurls && !a.shot_path && !a.script.drain_exit) {
        tab_open_url(&a, welcome);
        tab_go(&a, 0);
    }
    if (a.shot_path) {
        a.shot_state = SHOT_LOAD;
        a.shot_deadline = now_sec() + SHOT_LOAD_MAX;
    }
    relayout(&a);
    if (a.vim && a.vim_shadowed) {
        char m[96];
        snprintf(m, sizeof m, "vim: %d key%s kept by web.conf", a.vim_shadowed,
                 a.vim_shadowed == 1 ? "" : "s");
        notify(&a, m);
    }
    draw_panes(&a);

    if (rec_path) record_start(&a, rec_path);
    session_write(&a);
    if (exec_cmd) exec_start(&a, exec_cmd);

    while (!g_quit) {
        if (g_handed) {
            g_handed = 0;
            handoff_take(a.chrome.profile, getpid(), &a);
            merge_give(&a);
        }
        if (g_resized) {
            g_resized = 0;
            // The block sits a fixed distance up from the bottom of the pane,
            // and a resize scrolls it rather than moving it within the screen.
            // Its origin is a row number, so it has to be re-derived from that
            // distance - kept absolute, every later draw lands off by the
            // change in height.
            int gap = 0;
            if (a.inline_mode) {
                Term *t = &a.term;
                gap = t->rows - (t->inline_origin + t->inline_rows - 1);
                if (gap < 0) gap = 0;
                term_size(t);
                tmux_zoom_track(&a);
            }
            // The teardown waits for the frame that replaces it; doing it here
            // would leave the terminal showing its background for the whole
            // screenshot round trip. resize_at bounds that wait.
            a.resize_redraw = true;
            a.resize_at = now_sec();
            panes_forget(&a);
            a.last_hash = 0;
            relayout(&a);
            if (a.inline_mode) {
                Term *t = &a.term;
                t->inline_rows = a.img_rows + (a.tabs_open ? 1 : 0) +
                                 (a.status_open ? 1 : 0) + a.console_rows;
                if (gap + t->inline_rows > t->rows) gap = t->rows - t->inline_rows;
                if (gap < 0) gap = 0;
                t->inline_origin = t->rows - gap - t->inline_rows + 1;
                if (t->inline_origin < 1) t->inline_origin = 1;
                relayout(&a);          // the rect hangs off the new origin
                term_clear_below(t);
            }
            still_soon(&a);
        }

        struct pollfd fds[4] = {{0}};   // poll leaves revents alone on EINTR
        // poll ignores a negative fd
        fds[0].fd = a.has_tty ? a.term.fd : -1;
        fds[0].events = POLLIN;
        fds[1].fd = a.chrome.ws.fd;
        fds[1].events = POLLIN;
        fds[2].fd = a.exec_fd;
        fds[2].events = POLLIN;
        fds[3].fd = a.chrome.watch.fd > 0 ? a.chrome.watch.fd : -1;
        fds[3].events = POLLIN;

        bool draining = a.script.drain_exit && !script_busy(&a);
        int wait = (a.term.in.len || a.msg_until > now_sec() ||
                    a.expect_frame > 0 || a.hint_deadline > 0 ||
                    draining) ? 20 : -1;
        int sw = script_wait_ms(&a);
        if (sw >= 0 && (wait < 0 || sw < wait)) wait = sw;
        if (a.shot_path && (wait < 0 || wait > 100)) wait = 100;
        if (a.resize_redraw) {
            double left = a.resize_at + RESIZE_WAIT - now_sec();
            int ms = left > 0 ? (int)(left * 1000.0) + 1 : 0;
            if (wait < 0 || ms < wait) wait = ms;
        }
        if (a.grid_on && (wait < 0 || wait > 50)) wait = 50;
        if (a.in_motion) {
            double f = a.last_draw + a.settle_ms / 1000.0;
            double k = a.last_input + a.settle_ms / 1000.0;
            double left = (f > k ? f : k) - now_sec();
            double hold = a.last_input + MOTION_HOLD - now_sec();
            if (hold < left) left = hold;
            int ms = left > 0 ? (int)(left * 1000.0) + 1 : 0;
            if (wait < 0 || ms < wait) wait = ms;
        }
        for (int i = 0; i < 2; i++) {
            double at = i ? a.still_sent : a.still_at;
            if (at <= 0) continue;
            double left = at - now_sec();
            int ms = left > 0 ? (int)(left * 1000.0) + 1 : 0;
            if (wait < 0 || ms < wait) wait = ms;
        }
        if ((a.merge_until > 0 || a.giving_tabs) && (wait < 0 || wait > 100))
            wait = 100;
        if (a.freeze && !a.exec_paused && (a.exec_fd >= 0 || being_driven(&a)) &&
            (wait < 0 || wait > 200))
            wait = 200;
        int rc = poll(fds, 4, wait);
        if (rc < 0 && !g_resized) continue;

        if (fds[2].revents & (POLLIN | POLLHUP)) {
            exec_pump(&a);
            draw_panes(&a);
        }

        if (fds[0].revents & POLLIN) {
            term_read(&a.term);
            Event ev;
            bool owed = false;
            while (term_next(&a.term, &ev)) {
                if (ev.type == EV_MOUSE)
                    term_log("%.3f event mouse %s btn=%d cell %d,%d mods=%d",
                             now_sec(), ev.motion ? "move" :
                             ev.press ? "press" : "release",
                             ev.button, ev.mx, ev.my, ev.mods);
                else
                    term_log("%.3f event type=%d key=%d mods=%d text=%s", now_sec(),
                             ev.type, ev.key, ev.mods, ev.text[0] ? ev.text : "");
                if (ev.type != EV_MOUSE || ev.button != BTN_NONE) owed = true;
                if (ev.type == EV_KEY) handle_key(&a, &ev);
                else if (ev.type == EV_MOUSE) handle_mouse(&a, &ev);
                else if (ev.type == EV_FOCUS) handle_focus(&a, ev.press);
                else if (ev.type == EV_PASTE) {
                    if (!console_key(&a, &ev))
                        handle_paste(&a, a.term.paste.p, a.term.paste.len);
                }
                if (g_quit) break;
            }
            flush_pending(&a);
            draw_panes(&a);
            if (owed && a.expect_frame == 0) a.expect_frame = now_sec() + 2.0;
        }

        hint_tick(&a);
        check_driven(&a);
        claims_scan(&a);
        merge_collect(&a);
        give_tick(&a);
        if (a.grid_auto && a.has_tty) {
            if (!a.grid_on && a.ntabs > 1) grid_toggle(&a);
            else if (a.grid_on && a.ntabs < 2) grid_off(&a, NULL);
        }
        grid_tick(&a);
        exec_check_pause(&a);

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
            if (a.unwedge_run == 3)
                notify(&a, "page has stopped answering - ^R reloads, ^Q quits");
        }

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

        if (fds[3].revents & (POLLIN | POLLHUP)) {
            char *msg;
            size_t len;
            if (ws_fill(&a.chrome.watch) >= 0) {
                while (ws_next(&a.chrome.watch, &msg, &len) == 1) {
                    on_target_message(&a, msg);
                    a.chrome.watch.msg.len = 0;
                }
            }
            if (a.chrome.watch.closed) {
                chrome_unwatch(&a.chrome);
                term_log("%.3f browser socket closed; not watching for popups",
                         now_sec());
            }
            draw_panes(&a);
        }

        double quiet = now_sec() - a.last_input;
        if (a.in_motion &&
            ((now_sec() - a.last_draw > a.settle_ms / 1000.0 &&
              quiet > a.settle_ms / 1000.0) || quiet > MOTION_HOLD)) {
            a.in_motion = false;
            a.motion_run = 0;
            term_log("%.3f motion off", now_sec());
            a.last_hash = 0;
            relayout(&a);
            still_soon(&a);
        }

        if (a.resize_redraw && now_sec() - a.resize_at > RESIZE_WAIT) {
            resize_give_up(&a);
            draw_panes(&a);
        }

        if (a.still_at > 0 && now_sec() > a.still_at) still_request(&a);

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

        Event ev;
        if (a.has_tty && a.term.in.len && term_next(&a.term, &ev)) {
            if (ev.type == EV_KEY) handle_key(&a, &ev);
            else if (ev.type == EV_MOUSE) handle_mouse(&a, &ev);
            else if (ev.type == EV_FOCUS) handle_focus(&a, ev.press);
            else if (ev.type == EV_PASTE)
                handle_paste(&a, a.term.paste.p, a.term.paste.len);
            if (g_quit) break;
            flush_pending(&a);
            draw_panes(&a);
        }
    }

    if (a.has_tty) {
        g_write_force = 1;
        kitty_abort(&a.kitty);
        if (a.grid_on) grid_off(&a, NULL);
        if (!a.inline_mode || a.clear_exit) kitty_clear(&a.kitty);
        kitty_free(&a.kitty);
        term_restore(&a.term, a.clear_exit);
    }
    give_tick(&a);
    session_forget(&a);
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
    leave_browser(&a);
    term_log("%u frames drawn, %u duplicates skipped", a.frames, a.skipped);
    int rc = a.script.failures ? 1 : 0;
    if (a.shot_path && a.shot_state != SHOT_DONE) rc = 1;
    script_free(&a);
    return rc;
}

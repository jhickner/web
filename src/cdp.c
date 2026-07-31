#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "web.h"

static int http_get(int port, const char *path, Buf *out);

#define TRACE(...) do { \
    if (getenv("WEB_DEBUG")) { fprintf(stderr, "web: " __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static const char *CHROME_PATHS[] = {
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
    NULL,
};

static const char *find_chrome(void) {
    const char *env = getenv("WEB_CHROME");
    if (env && *env && access(env, X_OK) == 0) return env;
    for (int i = 0; CHROME_PATHS[i]; i++)
        if (access(CHROME_PATHS[i], X_OK) == 0) return CHROME_PATHS[i];
    return NULL;
}

// Chrome writes the chosen port here once the debug endpoint is listening, so
// we can pass --remote-debugging-port=0 and avoid racing for a free port.
static int read_devtools_port(const char *profile, double timeout) {
    char path[600];
    snprintf(path, sizeof path, "%s/DevToolsActivePort", profile);
    double deadline = now_sec() + timeout;
    while (now_sec() < deadline) {
        FILE *f = fopen(path, "r");
        if (f) {
            int port = 0;
            if (fscanf(f, "%d", &port) == 1 && port > 0) {
                fclose(f);
                return port;
            }
            fclose(f);
        }
        // Chrome takes roughly a quarter second to get here, and every
        // millisecond of poll granularity is a millisecond of startup.
        struct timespec ts = {0, 4 * 1000000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

// Anything at all listening there. A pinned port that belongs to some other
// process leaves Chrome with no debug endpoint, and the only sign of it would
// be the fifteen second wait for a port file that never arrives.
static bool port_taken(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    bool up = connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
    close(fd);
    return up;
}

int chrome_probe(int port) {
    if (port < 1 || port > 65535) return -1;
    Buf resp = {0};
    bool ok = http_get(port, "/json/version", &resp) == 0 && resp.len &&
              strstr(resp.p, "Browser") != NULL;
    buf_free(&resp);
    return ok ? 0 : -1;
}

// A previous run that died without cleaning up leaves a live Chrome holding
// the profile lock, which would make every later launch abort. If that browser
// still answers on its recorded port, take it over instead.
static int adopt_running(Chrome *c) {
    char path[600];
    snprintf(path, sizeof path, "%s/DevToolsActivePort", c->profile);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int port = 0;
    int ok = fscanf(f, "%d", &port);
    fclose(f);
    if (ok != 1 || port <= 0) return -1;

    if (chrome_probe(port) != 0) return -1;

    // Chrome records "<hostname>-<pid>" in the lock symlink, which is the only
    // place the pid of a browser we did not spawn is written down.
    snprintf(path, sizeof path, "%s/SingletonLock", c->profile);
    char link[256];
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        char *dash = strrchr(link, '-');
        if (dash) c->pid = (pid_t)atoi(dash + 1);
    }

    c->port = port;
    c->adopted = true;
    TRACE("adopted running chrome on port %d (pid %d)", port, (int)c->pid);
    return 0;
}

int chrome_launch(Chrome *c, const char *url, int w, int h, bool show_window,
                  bool mute, const char *user_agent, bool debug, int port) {
    const char *bin = find_chrome();
    if (!bin) {
        fprintf(stderr, "web: no Chrome found. Set WEB_CHROME=/path/to/chrome\n");
        return -1;
    }

    // The browser profile is bulk cache, not configuration, so it stays out of
    // ~/.config (which is often a synced dotfiles directory).
    const char *home = getenv("HOME");
    const char *cache = getenv("XDG_CACHE_HOME");
    if (cache && *cache)
        snprintf(c->profile, sizeof c->profile, "%s/web/profile", cache);
    else
        snprintf(c->profile, sizeof c->profile, "%s/.cache/web/profile",
                 home ? home : "/tmp");
    mkdirs(c->profile);

    // Only worth adopting a browser we could actually drive. A login window is
    // wanted for itself, and a headless browser already holding the profile
    // would swallow the request and show nothing.
    // A named port is a place, and a browser already answering there is the one
    // that was asked for - the same thing the attach command does, and what
    // makes --keep --port work: nothing writes that browser's port down
    // anywhere, so the profile check below cannot find it.
    if (debug && port > 0 && port_taken(port)) {
        if (chrome_probe(port) == 0) {
            c->port = port;
            c->adopted = c->foreign = true;    // not ours to shut down
            TRACE("took over the browser already on port %d", port);
            return 0;
        }
        fprintf(stderr, "web: something that is not a browser is already on "
                        "port %d\n", port);
        return -1;
    }

    if (debug && adopt_running(c) == 0) {
        // A pinned port is a promise to whatever is going to connect to it, and
        // the browser already holding this profile is listening somewhere else.
        if (port > 0 && c->port != port) {
            fprintf(stderr, "web: chrome from an earlier run is on port %d, not %d.\n"
                            "     quit it, or start with --port %d\n",
                    c->port, port, c->port);
            return -1;
        }
        return 0;                                   // caller navigates it to `url`
    }

    // A stale port file would be read as this run's port, and a lock left by a
    // killed run makes Chrome abort before it ever opens the port.
    char scratch[600];
    snprintf(scratch, sizeof scratch, "%s/DevToolsActivePort", c->profile);
    unlink(scratch);
    snprintf(scratch, sizeof scratch, "%s/SingletonLock", c->profile);
    unlink(scratch);
    snprintf(scratch, sizeof scratch, "%s/SingletonSocket", c->profile);
    unlink(scratch);
    snprintf(scratch, sizeof scratch, "%s/SingletonCookie", c->profile);
    unlink(scratch);

    char winsize[64], profarg[600], portarg[48];
    snprintf(winsize, sizeof winsize, "--window-size=%d,%d", w, h);
    snprintf(profarg, sizeof profarg, "--user-data-dir=%s", c->profile);
    snprintf(portarg, sizeof portarg, "--remote-debugging-port=%d",
             port > 0 ? port : 0);

    const char *argv[48];
    int a = 0;
    argv[a++] = bin;
    if (debug) argv[a++] = portarg;
    argv[a++] = profarg;
    argv[a++] = winsize;
    argv[a++] = "--no-first-run";
    argv[a++] = "--no-default-browser-check";
    argv[a++] = "--disable-background-networking";
    argv[a++] = "--disable-component-update";
    argv[a++] = "--disable-sync";
    if (mute) argv[a++] = "--mute-audio";
    argv[a++] = "--noerrdialogs";
    argv[a++] = "--disable-features=Translate,MediaRouter";

    // Subsystems that only cost startup time here: none of them can be seen
    // through a terminal, and the keychain one can block on a system prompt.
    argv[a++] = "--use-mock-keychain";
    argv[a++] = "--password-store=basic";
    argv[a++] = "--disable-extensions";
    argv[a++] = "--disable-default-apps";
    argv[a++] = "--disable-client-side-phishing-detection";
    argv[a++] = "--disable-domain-reliability";
    argv[a++] = "--disable-breakpad";
    argv[a++] = "--metrics-recording-only";
    argv[a++] = "--no-pings";
    argv[a++] = "--disable-hang-monitor";

    // A window nobody is looking at gets its timers throttled and its renderer
    // put to sleep, which shows up as a page that stops animating and a
    // screencast that goes quiet.
    argv[a++] = "--disable-background-timer-throttling";
    argv[a++] = "--disable-renderer-backgrounding";
    argv[a++] = "--disable-backgrounding-occluded-windows";

    // Set here rather than through Emulation.setUserAgentOverride, which
    // silences navigator.userAgentData entirely: a browser claiming to be
    // Chrome while reporting no brands at all is a stranger sight than the
    // headless marker this is removing. The flag leaves the hints intact.
    char uaarg[600];
    if (user_agent && *user_agent) {
        snprintf(uaarg, sizeof uaarg, "--user-agent=%s", user_agent);
        argv[a++] = uaarg;
    }

    if (!show_window) argv[a++] = "--headless=new";
    argv[a++] = url;
    argv[a] = NULL;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Detach from the terminal entirely: a browser holding our tty open
        // keeps it alive after we exit and lets its noise reach the screen.
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execv(bin, (char *const *)argv);
        _exit(127);
    }
    c->pid = pid;
    setpgid(pid, pid);

    if (!debug) return 0;      // nothing to attach to, and no port to wait for

    TRACE("spawned chrome pid %d, waiting for port", (int)pid);
    if (port > 0) {
        // Handed a port, Chrome does not write the port file at all, so the
        // endpoint answering is both the wait and the confirmation.
        c->port = -1;
        double deadline = now_sec() + 15.0;
        while (now_sec() < deadline) {
            if (chrome_probe(port) == 0) { c->port = port; break; }
            struct timespec ts = {0, 20 * 1000000};
            nanosleep(&ts, NULL);
        }
    } else {
        c->port = read_devtools_port(c->profile, 15.0);
    }
    if (c->port < 0) {
        fprintf(stderr, "web: chrome did not open a debug port\n");
        chrome_kill(c);
        return -1;
    }
    return 0;
}

// One blocking HTTP GET against the debug endpoint. The DevTools server holds
// the connection open regardless of what we ask for, so the body has to be
// measured by Content-Length rather than by waiting for EOF.
static int http_get(int port, const char *path, Buf *out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }

    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                     "Connection: close\r\n\r\n", path, port);
    if (writeall(fd, req, (size_t)n) < 0) { close(fd); return -1; }

    char tmp[8192];
    ssize_t r;
    out->len = 0;
    long content_len = -1;
    size_t header_end = 0;

    while ((r = read(fd, tmp, sizeof tmp)) > 0) {
        buf_add(out, tmp, (size_t)r);
        if (!header_end) {
            char *hdr = strstr(out->p, "\r\n\r\n");
            if (!hdr) continue;
            header_end = (size_t)(hdr - out->p) + 4;
            const char *cl = strcasestr(out->p, "content-length:");
            if (cl) content_len = strtol(cl + 15, NULL, 10);
        }
        if (content_len >= 0 && out->len >= header_end + (size_t)content_len) break;
    }
    close(fd);

    if (header_end) buf_consume(out, header_end);
    return out->len ? 0 : -1;
}

// Chrome's own user agent, with the one word that gives it away taken out.
// Nothing here is headless in any sense a site should care about - it lays out,
// paints and composites the same as any window - but the string says
// "HeadlessChrome", and Google refuses to sign anyone in from it.
//
// Read from the browser rather than written out here: a hardcoded version goes
// stale on the next Chrome update, and a user agent claiming a version the
// browser does not have is a worse tell than the one it replaces.
int chrome_user_agent(Chrome *c, char *out, size_t cap) {
    Buf resp = {0};
    if (http_get(c->port, "/json/version", &resp) != 0 || !resp.len) {
        buf_free(&resp);
        return -1;
    }
    size_t n = 0;
    const char *ua = json_str(resp.p, "User-Agent", &n);
    if (!ua || !n || n >= cap) { buf_free(&resp); return -1; }

    size_t o = 0;
    int patched = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ) {
        if (n - i >= 14 && !memcmp(ua + i, "HeadlessChrome", 14)) {
            if (o + 6 >= cap) break;
            memcpy(out + o, "Chrome", 6);
            o += 6;
            i += 14;
            patched = 1;
            continue;
        }
        out[o++] = ua[i++];
    }
    out[o] = 0;
    buf_free(&resp);
    return patched;      // 1 = this browser is still announcing itself headless
}

int chrome_attach(Chrome *c) {
    TRACE("attaching on port %d", c->port);
    Buf resp = {0};
    char ws_path[256] = {0};

    double deadline = now_sec() + 10.0;
    while (now_sec() < deadline && !ws_path[0]) {
        if (http_get(c->port, "/json/list", &resp) == 0 && resp.len) {
            // Chrome lists each target's keys alphabetically, so the debugger
            // URL of a "page" entry is the next ws:// after its type field.
            const char *p = strstr(resp.p, "\"type\": \"page\"");
            if (!p) p = strstr(resp.p, "\"type\":\"page\"");
            const char *ws = p ? strstr(p, "ws://") : NULL;
            if (ws) {
                const char *slash = strchr(ws + 5, '/');
                if (slash) {
                    const char *end = strchr(slash, '"');
                    size_t n = end ? (size_t)(end - slash) : strlen(slash);
                    if (n < sizeof ws_path) {
                        memcpy(ws_path, slash, n);
                        ws_path[n] = 0;
                    }
                }
            }
        }
        if (!ws_path[0]) {
            struct timespec ts = {0, 5 * 1000000};
            nanosleep(&ts, NULL);
        }
    }
    buf_free(&resp);
    if (!ws_path[0]) {
        fprintf(stderr, "web: could not find a page target\n");
        return -1;
    }
    TRACE("page target %s", ws_path);

    int fd = ws_connect("127.0.0.1", c->port, ws_path);
    if (fd < 0) {
        fprintf(stderr, "web: websocket connect failed\n");
        return -1;
    }
    TRACE("websocket connected");
    c->ws.fd = fd;
    c->next_id = 1;
    return 0;
}

// True until the browser is really gone. An adopted browser is not our child,
// so waitpid has nothing to reap and the signal probe is the only answer.
static bool chrome_alive(Chrome *c) {
    if (c->pid <= 0) return false;
    if (waitpid(c->pid, NULL, WNOHANG) == c->pid) return false;
    return kill(c->pid, 0) == 0 || errno != ESRCH;
}

void chrome_kill(Chrome *c) {
    // Ask the browser to close before signalling it. Chrome batches its cookie
    // writes and commits them on a clean shutdown; killed instead, it can lose
    // the session that was just established, which reads as being signed out
    // again on the next run.
    if (c->pid > 0 && c->ws.fd > 0 && !c->ws.closed) {
        // Nothing is going to draw these, and a page still being screencast is
        // work the browser has to finish before it can attend to shutting down.
        cdp_call(c, "Page.stopScreencast", "");
        cdp_call(c, "Browser.close", "");
        for (int i = 0; i < 120; i++) {          // up to three seconds
            if (!chrome_alive(c)) { c->pid = 0; break; }
            struct timespec ts = {0, 25 * 1000000};
            nanosleep(&ts, NULL);
        }
    }

    if (c->pid > 0) {
        // The browser, not its group: the group signal takes the helpers down
        // first and the orderly shutdown never runs, which is what loses the
        // cookie store. The group only gets involved as a last resort below.
        kill(c->pid, SIGTERM);
        for (int i = 0; i < 40; i++) {
            if (waitpid(c->pid, NULL, WNOHANG) == c->pid) { c->pid = 0; break; }
            struct timespec ts = {0, 25 * 1000000};
            nanosleep(&ts, NULL);
        }
        if (c->pid > 0) {
            kill(-c->pid, SIGKILL);
            waitpid(c->pid, NULL, 0);
            c->pid = 0;
        }
    }
    if (c->ws.fd > 0) ws_close(&c->ws);
}

int cdp_vcall(Chrome *c, const char *method, const char *params_fmt, va_list ap) {
    Buf b = {0};
    int id = c->next_id++;
    buf_addf(&b, "{\"id\":%d,\"method\":\"%s\",\"params\":{", id, method);
    if (params_fmt && *params_fmt) {
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(NULL, 0, params_fmt, copy);
        va_end(copy);
        if (n > 0) {
            buf_reserve(&b, b.len + (size_t)n + 4);
            vsnprintf(b.p + b.len, (size_t)n + 1, params_fmt, ap);
            b.len += (size_t)n;
        }
    }
    buf_add(&b, "}}", 2);
    term_log("cdp-> %.180s", b.p);
    int rc = ws_send_text(&c->ws, b.p, b.len);
    buf_free(&b);
    return rc < 0 ? -1 : id;
}

int cdp_call(Chrome *c, const char *method, const char *params_fmt, ...) {
    va_list ap;
    va_start(ap, params_fmt);
    int id = cdp_vcall(c, method, params_fmt, ap);
    va_end(ap);
    return id;
}

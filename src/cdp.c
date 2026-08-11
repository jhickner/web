#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
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

static int http_req(int port, const char *method, const char *path, Buf *out);
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
        struct timespec ts = {0, 4 * 1000000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

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

static char profile_name[64];
static bool profile_temp;

void web_cache_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    const char *cache = getenv("XDG_CACHE_HOME");
    if (cache && *cache) snprintf(out, cap, "%s/web", cache);
    else snprintf(out, cap, "%s/.cache/web", home ? home : "/tmp");
}

void chrome_profile_path(char *out, size_t cap) {
    char base[400];
    web_cache_path(base, sizeof base);
    if (profile_name[0]) snprintf(out, cap, "%s/profiles/%s", base, profile_name);
    else snprintf(out, cap, "%s/profile", base);
}

static bool pid_alive(pid_t p) {
    if (p <= 0) return false;
    return kill(p, 0) == 0 || errno != ESRCH;
}

void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1024];
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name)
            >= (int)sizeof child) continue;
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(child);
        else unlink(child);
    }
    closedir(d);
    rmdir(path);
}

static void sweep_temp_profiles(void) {
    char here[512], dir[520];
    chrome_profile_path(here, sizeof here);
    char *slash = strrchr(here, '/');
    if (!slash) return;
    *slash = 0;
    snprintf(dir, sizeof dir, "%s", here);

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "tmp-", 4)) continue;
        pid_t pid = (pid_t)atoi(e->d_name + 4);
        if (pid <= 0 || pid == getpid() || pid_alive(pid)) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        rm_tree(path);
    }
    closedir(d);
}

// "-" is a per-run temp profile, removed when the browser goes
int chrome_profile_set(const char *name) {
    if (!name || !*name) return -1;
    if (!strcmp(name, "-")) {
        snprintf(profile_name, sizeof profile_name, "tmp-%d", (int)getpid());
        profile_temp = true;
        sweep_temp_profiles();
        return 0;
    }
    if (strlen(name) >= sizeof profile_name) return -1;
    for (const char *p = name; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-';
        if (!ok) return -1;
    }
    if (name[0] == '.') return -1;
    snprintf(profile_name, sizeof profile_name, "%s", name);
    return 0;
}

bool chrome_profile_named(char *out, size_t cap) {
    if (out && cap) snprintf(out, cap, "%s", profile_name);
    return profile_name[0] != 0;
}

static void discard_temp_profile(Chrome *c) {
    if (!profile_temp || c->foreign) return;
    if (c->pid > 0 && pid_alive(c->pid)) return;
    char path[512];
    chrome_profile_path(path, sizeof path);
    rm_tree(path);
}

// the lock symlink reads "<hostname>-<pid>"
static pid_t singleton_pid(const char *profile) {
    char path[600];
    snprintf(path, sizeof path, "%s/SingletonLock", profile);
    char link[256];
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) return 0;
    link[n] = 0;
    char *dash = strrchr(link, '-');
    return dash ? (pid_t)atoi(dash + 1) : 0;
}

static void note_browser(Chrome *c) {
    char path[600];
    snprintf(path, sizeof path, "%s/web-port", c->profile);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d %d\n", c->port, (int)c->pid);
    fclose(f);
}

// noted port, or -1 if there is no note or that browser is gone
static int noted_browser(const char *profile, pid_t *pid) {
    char path[600];
    snprintf(path, sizeof path, "%s/web-port", profile);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int port = 0, holder = 0;
    int got = fscanf(f, "%d %d", &port, &holder);
    fclose(f);
    if (got < 1 || port <= 0) return -1;
    if (got >= 2 && holder > 0 && !pid_alive((pid_t)holder)) return -1;
    if (pid) *pid = (pid_t)(got >= 2 ? holder : 0);
    return port;
}

void chrome_mark_kept(Chrome *c) {
    char path[600];
    snprintf(path, sizeof path, "%s/web-keep", c->profile);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)c->pid);
    fclose(f);
}

bool chrome_is_kept(Chrome *c) {
    char path[600];
    snprintf(path, sizeof path, "%s/web-keep", c->profile);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    int marked = 0;
    int got = fscanf(f, "%d", &marked);
    fclose(f);
    if (got != 1) return false;
    return marked <= 0 || c->pid <= 0 || marked == (int)c->pid;
}

static void forget_browser(Chrome *c) {
    char path[600];
    snprintf(path, sizeof path, "%s/web-keep", c->profile);
    unlink(path);
    snprintf(path, sizeof path, "%s/web-port", c->profile);
    unlink(path);
    snprintf(path, sizeof path, "%s/DevToolsActivePort", c->profile);
    unlink(path);
}

int chrome_adoptable(const char *profile, pid_t *pid) {
    char path[600];
    snprintf(path, sizeof path, "%s/DevToolsActivePort", profile);
    int port = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d", &port) != 1) port = 0;
        fclose(f);
    }
    if (port <= 0 || chrome_probe(port) != 0) port = noted_browser(profile, NULL);
    if (port <= 0 || chrome_probe(port) != 0) return -1;
    if (pid) *pid = singleton_pid(profile);
    return port;
}

int chrome_running(const char *profile, ChromeProc *out, int cap) {
    char match[700];
    int mlen = snprintf(match, sizeof match, "--user-data-dir=%s", profile);
    if (mlen < 0 || mlen >= (int)sizeof match) return 0;

    FILE *p = popen("ps -axww -o pid=,etime=,command= 2>/dev/null", "r");
    if (!p) return 0;

    int n = 0;
    char line[8192];
    while (n < cap && fgets(line, sizeof line, p)) {
        const char *hit = strstr(line, match);
        if (!hit) continue;
        char after = hit[mlen];
        if (after && after != ' ' && after != '\n') continue;
        if (strstr(line, "--type=")) continue;      // a helper, not the browser

        int pid = 0;
        char age[24] = {0};
        if (sscanf(line, "%d %23s", &pid, age) != 2 || pid <= 0) continue;
        out[n].pid = (pid_t)pid;
        snprintf(out[n].age, sizeof out[n].age, "%s", age);
        n++;
    }
    pclose(p);
    return n;
}

static int adopt_running(Chrome *c) {
    pid_t pid = 0;
    int port = chrome_adoptable(c->profile, &pid);
    if (port < 0) return -1;

    c->pid = pid;
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

    chrome_profile_path(c->profile, sizeof c->profile);
    mkdirs(c->profile);

    if (debug && port > 0 && port_taken(port)) {
        if (chrome_probe(port) == 0) {
            c->port = port;
            pid_t holder = 0;
            if (noted_browser(c->profile, &holder) == port) {
                c->adopted = true;
                c->pid = holder;
            } else {
                c->foreign = true;
            }
            TRACE("took over the %s browser on port %d",
                  c->foreign ? "foreign" : "earlier run's", port);
            return 0;
        }
        fprintf(stderr, "web: something that is not a browser is already on "
                        "port %d\n", port);
        return -1;
    }

    if (debug) {
        double deadline = now_sec() + (pid_alive(singleton_pid(c->profile)) ? 3.0 : 0.0);
        for (;;) {
            if (adopt_running(c) == 0) {
                if (port > 0 && c->port != port) {
                    fprintf(stderr, "web: chrome from an earlier run is on port %d, not %d.\n"
                                    "     quit it, or start with --port %d\n",
                            c->port, port, c->port);
                    return -1;
                }
                return 0;                           // caller navigates it to `url`
            }
            if (now_sec() >= deadline) break;
            struct timespec ts = {0, 20 * 1000000};
            nanosleep(&ts, NULL);
        }
    }

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
    argv[a++] = "--disable-sync";
    if (mute) argv[a++] = "--mute-audio";
    argv[a++] = "--noerrdialogs";
    argv[a++] = "--disable-features=Translate,MediaRouter";

    argv[a++] = "--use-mock-keychain";
    argv[a++] = "--password-store=basic";
    argv[a++] = "--disable-default-apps";
    argv[a++] = "--disable-client-side-phishing-detection";
    argv[a++] = "--disable-domain-reliability";
    argv[a++] = "--disable-breakpad";
    argv[a++] = "--metrics-recording-only";
    argv[a++] = "--no-pings";
    argv[a++] = "--disable-hang-monitor";

    argv[a++] = "--disable-background-timer-throttling";
    argv[a++] = "--disable-renderer-backgrounding";
    argv[a++] = "--disable-backgrounding-occluded-windows";

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

    if (!debug) return 0;

    TRACE("spawned chrome pid %d, waiting for port", (int)pid);
    if (port > 0) {
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
    note_browser(c);
    return 0;
}

static int http_req(int port, const char *method, const char *path, Buf *out) {
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
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                     "Connection: close\r\n\r\n", method, path, port);
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

static int http_get(int port, const char *path, Buf *out) {
    return http_req(port, "GET", path, out);
}

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

// path part of the first ws:// url at or after p
static bool ws_path_at(const char *p, char *out, size_t cap) {
    const char *ws = p ? strstr(p, "ws://") : NULL;
    if (!ws) return false;
    const char *slash = strchr(ws + 5, '/');
    if (!slash) return false;
    const char *end = strchr(slash, '"');
    size_t n = end ? (size_t)(end - slash) : strlen(slash);
    if (n >= cap) return false;
    memcpy(out, slash, n);
    out[n] = 0;
    return true;
}

static int browser_call(Chrome *c, const char *method, const char *params,
                        Buf *reply) {
    Buf resp = {0};
    char path[256];
    bool have = http_get(c->port, "/json/version", &resp) == 0 &&
                ws_path_at(resp.p, path, sizeof path);
    buf_free(&resp);
    if (!have) return -1;

    int fd = ws_connect("127.0.0.1", c->port, path);
    if (fd < 0) return -1;
    WS ws = {0};
    ws.fd = fd;

    Buf req = {0};
    buf_addf(&req, "{\"id\":1,\"method\":\"%s\",\"params\":{%s}}", method, params);
    int rc = ws_send_text(&ws, req.p, req.len);
    buf_free(&req);

    double deadline = now_sec() + 5.0;
    while (rc == 0 && now_sec() < deadline) {
        struct pollfd p = {ws.fd, POLLIN, 0};
        if (poll(&p, 1, 50) > 0 && ws_fill(&ws) < 0) break;
        char *msg;
        size_t len;
        if (ws_next(&ws, &msg, &len) != 1) {
            if (ws.closed) break;
            continue;
        }
        if (json_num(msg, "id", 0) != 1) { ws.msg.len = 0; continue; }
        buf_add(reply, msg, len);
        buf_add(reply, "", 1);
        reply->len--;                       // nul-terminated
        ws_close(&ws);
        return 0;
    }
    ws_close(&ws);
    return -1;
}

int chrome_open_tab(Chrome *c, const char *url, char *out, size_t cap) {
    if (!url || !*url) url = "about:blank";
    char esc[2200], args[2400];
    json_escape(esc, sizeof esc, url);
    snprintf(args, sizeof args, "\"url\":\"%s\",\"newWindow\":true", esc);

    Buf reply = {0};
    int rc = browser_call(c, "Target.createTarget", args, &reply);
    size_t n = 0;
    const char *id = rc == 0 ? json_str(reply.p, "targetId", &n) : NULL;
    if (id && n && n < cap)
        snprintf(out, cap, "%.*s", (int)n, id);
    else
        id = NULL;
    buf_free(&reply);
    if (id) return 0;

    // everything after the ? is the address; /json/new takes PUT only
    Buf resp = {0};
    char path[256], req[1200];
    snprintf(req, sizeof req, "/json/new?%s", url);
    bool ok = http_req(c->port, "PUT", req, &resp) == 0 &&
              ws_path_at(resp.p, path, sizeof path);
    if (ok) {
        const char *slash = strrchr(path, '/');
        snprintf(out, cap, "%s", slash ? slash + 1 : path);
    }
    buf_free(&resp);
    return ok ? 0 : -1;
}

int chrome_switch_target(Chrome *c, const char *target) {
    if (c->port <= 0 || !target || !*target) return -1;
    char path[160];
    snprintf(path, sizeof path, "/devtools/page/%s", target);
    int fd = ws_connect("127.0.0.1", c->port, path);
    if (fd < 0) return -1;
    ws_close(&c->ws);
    c->ws = (WS){0};
    c->ws.fd = fd;
    c->next_id = 1;
    snprintf(c->target, sizeof c->target, "%s", target);
    return 0;
}

void chrome_close_id(Chrome *c, const char *target) {
    if (c->port <= 0 || !target || !*target) return;
    char path[160];
    snprintf(path, sizeof path, "/json/close/%s", target);
    Buf resp = {0};
    http_get(c->port, path, &resp);
    buf_free(&resp);
}

int chrome_attach(Chrome *c) {
    TRACE("attaching on port %d", c->port);
    Buf resp = {0};
    char ws_path[256] = {0};

    if (c->adopted) {
        char id[96];
        if (chrome_open_tab(c, NULL, id, sizeof id) == 0)
            snprintf(ws_path, sizeof ws_path, "/devtools/page/%s", id);
        else
            TRACE("no tab of our own; sharing the browser's");
    }

    double deadline = now_sec() + 10.0;
    while (now_sec() < deadline && !ws_path[0]) {
        if (http_get(c->port, "/json/list", &resp) == 0 && resp.len) {
            // target keys are alphabetical: the ws:// after the type field is that page's
            const char *p = strstr(resp.p, "\"type\": \"page\"");
            if (!p) p = strstr(resp.p, "\"type\":\"page\"");
            ws_path_at(p, ws_path, sizeof ws_path);
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
    const char *id = strrchr(ws_path, '/');
    snprintf(c->target, sizeof c->target, "%s", id ? id + 1 : "");

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

int chrome_watch(Chrome *c) {
    if (c->port <= 0) return -1;
    Buf resp = {0};
    char path[256];
    bool have = http_get(c->port, "/json/version", &resp) == 0 &&
                ws_path_at(resp.p, path, sizeof path);
    buf_free(&resp);
    if (!have) return -1;

    int fd = ws_connect("127.0.0.1", c->port, path);
    if (fd < 0) return -1;
    c->watch = (WS){0};
    c->watch.fd = fd;

    static const char ask[] = "{\"id\":1,\"method\":\"Target.setDiscoverTargets\","
                              "\"params\":{\"discover\":true}}";
    if (ws_send_text(&c->watch, ask, sizeof ask - 1) < 0) {
        chrome_unwatch(c);
        return -1;
    }
    TRACE("watching for pages on port %d", c->port);
    return 0;
}

int chrome_browser_ws(Chrome *c, WS *out) {
    if (c->port <= 0) return -1;
    Buf resp = {0};
    char path[256];
    bool have = http_get(c->port, "/json/version", &resp) == 0 &&
                ws_path_at(resp.p, path, sizeof path);
    buf_free(&resp);
    if (!have) return -1;
    int fd = ws_connect("127.0.0.1", c->port, path);
    if (fd < 0) return -1;
    *out = (WS){0};
    out->fd = fd;
    return 0;
}

void chrome_unwatch(Chrome *c) {
    if (c->watch.fd > 0) ws_close(&c->watch);
    c->watch.fd = -1;
}

// parked-tab marker address
#define PARKED_URL   "about:blank#web-parked"
#define PARKED_QUERY "/json/new?about:blank%23web-parked"

// page targets that are not ours or parked; 0 if the browser will not answer
int chrome_other_pages(Chrome *c) {
    if (c->port <= 0) return 0;
    Buf resp = {0};
    if (http_get(c->port, "/json/list", &resp) != 0) { buf_free(&resp); return 0; }

    int n = 0;
    for (const char *p = resp.p; (p = strstr(p, "\"type\"")) != NULL; p++) {
        const char *q = p + 6;
        while (*q == ' ' || *q == ':') q++;
        if (!strncmp(q, "\"page\"", 6)) n++;   // and not background_page
    }
    if (c->target[0] && strstr(resp.p, c->target)) n--;
    for (const char *p = resp.p; (p = strstr(p, PARKED_URL)) != NULL; p++) n--;
    buf_free(&resp);
    return n > 0 ? n : 0;
}

void chrome_park(Chrome *c) {
    if (c->port <= 0) return;
    Buf list = {0};
    bool have = http_get(c->port, "/json/list", &list) == 0 &&
                strstr(list.p, PARKED_URL) != NULL;
    buf_free(&list);
    if (have) return;

    Buf resp = {0};
    http_req(c->port, "PUT", PARKED_QUERY, &resp);
    buf_free(&resp);
}

void chrome_close_target(Chrome *c) {
    if (c->port <= 0 || !c->target[0]) return;
    chrome_close_id(c, c->target);
    c->target[0] = 0;
}

static bool chrome_alive(Chrome *c) {
    if (c->pid <= 0) return false;
    if (waitpid(c->pid, NULL, WNOHANG) == c->pid) return false;
    return kill(c->pid, 0) == 0 || errno != ESRCH;
}

void chrome_kill(Chrome *c) {
    bool asked = false;
    if (c->ws.fd > 0 && !c->ws.closed) {
        cdp_call(c, "Page.stopScreencast", "");
        asked = cdp_call(c, "Browser.close", "") > 0;
    }

    if (asked || c->pid > 0)
        forget_browser(c);

    if (asked) {
        for (int i = 0; i < 120; i++) {          // up to three seconds
            bool gone = c->pid > 0 ? !chrome_alive(c) : chrome_probe(c->port) != 0;
            if (gone) { c->pid = 0; break; }
            struct timespec ts = {0, 25 * 1000000};
            nanosleep(&ts, NULL);
        }
    }

    if (c->pid > 0) {
        kill(c->pid, SIGTERM);
        for (int i = 0; i < 40; i++) {
            if (!chrome_alive(c)) { c->pid = 0; break; }
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
    discard_temp_profile(c);
}

void chrome_kill_bg(Chrome *c) {
    if (c->pid <= 0 && (c->ws.fd <= 0 || c->ws.closed)) {
        chrome_kill(c);
        return;
    }
    if (c->pid > 0) forget_browser(c);

    pid_t pid = fork();
    if (pid < 0) { chrome_kill(c); return; }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        chrome_kill(c);
        _exit(0);
    }

    if (c->ws.fd > 0) ws_close(&c->ws);
    c->pid = 0;
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

int cdp_session_call(Chrome *c, const char *session, const char *method,
                     const char *params_fmt, ...) {
    if (c->watch.fd <= 0 || c->watch.closed) return -1;
    Buf b = {0};
    int id = c->next_id++;
    buf_addf(&b, "{\"id\":%d,\"method\":\"%s\"", id, method);
    if (session && *session) buf_addf(&b, ",\"sessionId\":\"%s\"", session);
    buf_addf(&b, ",\"params\":{");
    if (params_fmt && *params_fmt) {
        va_list ap;
        va_start(ap, params_fmt);
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(NULL, 0, params_fmt, copy);
        va_end(copy);
        if (n > 0) {
            buf_reserve(&b, b.len + (size_t)n + 4);
            vsnprintf(b.p + b.len, (size_t)n + 1, params_fmt, ap);
            b.len += (size_t)n;
        }
        va_end(ap);
    }
    buf_add(&b, "}}", 2);
    term_log("watch-> %.180s", b.p);
    int rc = ws_send_text(&c->watch, b.p, b.len);
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

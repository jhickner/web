#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "web.h"

#define MCP_PROTOCOL "2025-06-18"
#define REF_MAX      200
#define CALL_SECS    15.0

// ------------------------------------------------------------- the window

typedef struct {
    pid_t pid;
    int   port;
    char  target[128];
    char  drive[768];      // drive file path
    char  profile[512];
    WS    ws;
    int   next_id;
    char  marked[768];     // drive file currently held
} Win;

static Win W = {.ws = {.fd = -1}};

static bool cdp_do(const char *method, const char *params, Buf *reply,
                   char *err, size_t errcap);

static void field(const char *line, const char *key, char *out, size_t cap) {
    size_t n = 0;
    const char *v = json_str(line, key, &n);
    if (v && n) json_unescape(out, cap, v, n);
    else if (cap) out[0] = 0;
}

static bool win_read(void) {
    char dir[600];
    chrome_profile_path(W.profile, sizeof W.profile);
    snprintf(dir, sizeof dir, "%s/sessions", W.profile);
    DIR *d = opendir(dir);
    if (!d) return false;

    const char *pick = getenv("WEB_WINDOW");
    if (pick && !*pick) pick = NULL;
    // strspn past the digits: all digits is a pid, anything else a name
    bool by_pid = pick && pick[strspn(pick, "0123456789")] == 0;
    long want = by_pid ? atol(pick) : 0;
    char best[4096] = "";
    time_t best_t = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int pid = atoi(e->d_name);
        if (pid <= 0 || (want && pid != want)) continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) { unlink(path); continue; }
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (best[0] && st.st_mtime < best_t) continue;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[4096];
        if (fgets(line, sizeof line, f)) {
            char name[128];
            field(line, "name", name, sizeof name);
            if (by_pid || !pick || !strcmp(name, pick)) {
                snprintf(best, sizeof best, "%s", line);
                best_t = st.st_mtime;
            }
        }
        fclose(f);
    }
    closedir(d);
    if (!best[0]) return false;

    W.pid  = (pid_t)json_num(best, "pid", 0);
    W.port = (int)json_num(best, "port", 0);
    field(best, "target", W.target, sizeof W.target);
    field(best, "drive", W.drive, sizeof W.drive);
    return W.port > 0 && W.target[0];
}

static void drive_release(void) {
    if (!W.marked[0]) return;
    unlink(W.marked);
    W.marked[0] = 0;
    if (W.pid > 0) kill(W.pid, SIGUSR1);
}

static void drive_hold(void) {
    if (!W.drive[0] || W.pid <= 0) return;
    if (W.marked[0] && !strcmp(W.marked, W.drive)) return;
    drive_release();
    FILE *f = fopen(W.drive, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    snprintf(W.marked, sizeof W.marked, "%s", W.drive);
    kill(W.pid, SIGUSR1);
}

static void bye(int sig) {
    drive_release();
    _exit(sig == SIGINT ? 130 : 143);
}

static void win_drop(void) {
    if (W.ws.fd >= 0) ws_close(&W.ws);
    W.ws.fd = -1;
}

static bool win_ready(char *err, size_t cap) {
    char was_target[128];
    snprintf(was_target, sizeof was_target, "%s", W.target);
    int was_port = W.port;

    if (!win_read()) {
        win_drop();
        drive_release();
        snprintf(err, cap, "no web window is running: start one with `web`");
        return false;
    }
    if (W.ws.fd >= 0 &&
        (W.port != was_port || strcmp(W.target, was_target) != 0))
        win_drop();

    if (W.ws.fd < 0) {
        char path[256];
        snprintf(path, sizeof path, "/devtools/page/%s", W.target);
        int fd = ws_connect("127.0.0.1", W.port, path);
        if (fd < 0) {
            snprintf(err, cap, "cannot reach the window's browser on port %d",
                     W.port);
            return false;
        }
        W.ws = (WS){.fd = fd};
        char why[200];
        Buf reply = {0};
        cdp_do("Page.enable", "", &reply, why, sizeof why);
        buf_free(&reply);
    }
    drive_hold();
    return true;
}

// ---------------------------------------------------------------- devtools

// the page announced a navigation
static bool g_going;

// one devtools call, blocking; events arriving meanwhile are dropped
static bool cdp_do(const char *method, const char *params, Buf *reply,
                   char *err, size_t errcap) {
    int id = ++W.next_id;
    Buf req = {0};
    buf_addf(&req, "{\"id\":%d,\"method\":\"%s\",\"params\":{%s}}",
             id, method, params ? params : "");
    int rc = ws_send_text(&W.ws, req.p, req.len);
    buf_free(&req);
    if (rc != 0) {
        win_drop();
        snprintf(err, errcap, "the window's browser went away");
        return false;
    }

    double deadline = now_sec() + CALL_SECS;
    while (now_sec() < deadline) {
        struct pollfd p = {W.ws.fd, POLLIN, 0};
        if (poll(&p, 1, 50) > 0 && ws_fill(&W.ws) < 0) break;
        char *msg;
        size_t len;
        int got = ws_next(&W.ws, &msg, &len);
        if (got != 1) {
            if (W.ws.closed) break;
            continue;
        }
        if ((int)json_num(msg, "id", 0) != id) {
            if (strstr(msg, "Page.frameRequestedNavigation")) g_going = true;
            W.ws.msg.len = 0;
            continue;
        }
        buf_add(reply, msg, len);
        W.ws.msg.len = 0;
        return true;
    }
    win_drop();
    snprintf(err, errcap, "the page did not answer within %.0f seconds",
             CALL_SECS);
    return false;
}

static void events_drop(void) {
    g_going = false;
    if (W.ws.fd < 0) return;
    ws_fill(&W.ws);
    char *msg;
    size_t len;
    while (ws_next(&W.ws, &msg, &len) == 1) W.ws.msg.len = 0;
}

static bool event_wait(const char *method, double secs) {
    double deadline = now_sec() + secs;
    while (now_sec() < deadline) {
        struct pollfd p = {W.ws.fd, POLLIN, 0};
        if (poll(&p, 1, 50) > 0 && ws_fill(&W.ws) < 0) return false;
        char *msg;
        size_t len;
        if (ws_next(&W.ws, &msg, &len) != 1) {
            if (W.ws.closed) return false;
            continue;
        }
        bool hit = strstr(msg, method) != NULL;
        W.ws.msg.len = 0;
        if (hit) return true;
    }
    return false;
}

// javascript in the page, answered as text
static bool page_eval(const char *src, Buf *out, char *err, size_t errcap) {
    Buf expr = {0};
    buf_addf(&expr, "Promise.resolve(eval(\"");
    json_escape_buf(&expr, src);
    buf_addf(&expr, "\")).then(function(v){"
                    "if(typeof v==='string')return v;"
                    "try{var s=JSON.stringify(v);if(s!==undefined)return s}"
                    "catch(e){}"
                    "return String(v)})");

    Buf esc = {0}, params = {0}, reply = {0};
    json_escape_buf(&esc, expr.p);
    buf_addf(&params, "\"expression\":\"%s\",\"returnByValue\":true,"
                      "\"awaitPromise\":true", esc.p);
    bool ok = cdp_do("Runtime.evaluate", params.p, &reply, err, errcap);
    buf_free(&esc);
    buf_free(&expr);
    buf_free(&params);
    if (!ok) { buf_free(&reply); return false; }

    size_t n = 0;
    const char *v = json_eval_str(reply.p, &n);
    if (!v) {
        size_t dn = 0;
        const char *d = json_str(reply.p, "description", &dn);
        char raw[600] = "the page threw";
        if (d && dn) {
            json_unescape(raw, sizeof raw, d, dn);
            char *nl = strchr(raw, '\n');
            if (nl) *nl = 0;
        }
        snprintf(err, errcap, "%s", raw);
        buf_free(&reply);
        return false;
    }
    buf_reserve(out, out->len + n + 4);
    out->len += json_unescape(out->p + out->len, n + 4, v, n);
    buf_free(&reply);
    return true;
}

// ------------------------------------------------------------------- refs

// css selectors from the last snapshot, indexed by ref
static char g_ref[REF_MAX][512];
static int  g_refs;

static const char *ref_sel(const char *ref) {
    if (*ref == '#' && ref[1] >= '0' && ref[1] <= '9') ref++;
    else if (*ref < '0' || *ref > '9') return NULL;
    int i = atoi(ref);
    if (i < 1 || i > g_refs) return NULL;
    return g_ref[i - 1];
}

static bool want_target(const char *args, char *out, size_t cap,
                        char *err, size_t errcap) {
    char raw[512] = "";
    field(args, "ref", raw, sizeof raw);
    if (raw[0]) {
        const char *sel = ref_sel(raw);
        if (!sel) {
            snprintf(err, errcap,
                     "no such ref: %s. Take a snapshot for the refs on this page",
                     raw);
            return false;
        }
        snprintf(out, cap, "%s", sel);
        return true;
    }
    field(args, "selector", out, cap);
    if (out[0]) return true;
    snprintf(err, errcap, "say which element: ref, from a snapshot, or selector");
    return false;
}

// waits for readyState complete, answers with title and url
static bool settle(Buf *out, char *err, size_t errcap) {
    double deadline = now_sec() + 10.0;
    for (;;) {
        Buf state = {0};
        bool ok = page_eval("document.readyState", &state, err, errcap);
        bool done = ok && state.p && !strcmp(state.p, "complete");
        buf_free(&state);
        if (!ok) return false;
        if (done || now_sec() > deadline) break;
        struct timespec ts = {0, 200000000};
        nanosleep(&ts, NULL);
    }
    return page_eval(
        "document.title?document.title+' - '+location.href:location.href",
        out, err, errcap);
}

static void nav_after(Buf *out, char *err, size_t errcap) {
    if (!g_going && !event_wait("Page.frameRequestedNavigation", 0.5)) return;
    g_going = false;
    if (!event_wait("Page.frameNavigated", 15.0)) return;
    g_refs = 0;
    Buf where = {0};
    if (settle(&where, err, errcap) && where.len)
        buf_addf(out, ", now on %s", where.p);
    buf_free(&where);
}

// ------------------------------------------------------------------ tools

// snapshot script: one line per element, role/name/selector/value
static const char SNAP_JS[] =
    "(function(){"
    ROLE_NAME_FN
    SELECTOR_FN
    "function on(e){var r=e.getBoundingClientRect();"
    "if(!r.width||!r.height)return false;"
    "var s=getComputedStyle(e);"
    "return s.visibility!=='hidden'&&s.display!=='none'&&s.opacity!=='0'}"
    "function cut(s,n){return String(s||'').replace(/\\s+/g,' ').trim().slice(0,n)}"
    "var out=[],l=document.querySelectorAll("
    "'a,button,input,select,textarea,summary,[role],[onclick],[contenteditable]');"
    "for(var i=0;i<l.length&&out.length<200;i++){var e=l[i];"
    "if((e.type||'')==='hidden'||!on(e))continue;"
    "var r=rl(e)||e.tagName.toLowerCase(),n=nm(e),v='';"
    "if(!n)n=e.getAttribute('placeholder')||e.getAttribute('title')||'';"
    "if(e.type==='checkbox'||e.type==='radio')v=e.checked?'checked':'unchecked';"
    "else if(e.tagName==='INPUT'||e.tagName==='TEXTAREA'||e.tagName==='SELECT')"
    "v=e.value||'';"
    "out.push([r,cut(n,80),ws(e),cut(v,40)].join('\\t'))}"
    "return document.title+'\\t'+location.href+'\\n'+out.join('\\n')})()";

static bool tool_snapshot(const char *args, Buf *out, char *err, size_t errcap) {
    Buf raw = {0};
    if (!page_eval(SNAP_JS, &raw, err, errcap)) { buf_free(&raw); return false; }

    g_refs = 0;
    char *p = raw.p;
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    char *url = strchr(p, '\t');
    if (url) *url++ = 0;
    buf_addf(out, "%s\n%s\n\n", p, url ? url : "");
    if (!nl) {
        buf_addf(out, "nothing on this page can be acted on.\n");
        buf_free(&raw);
        return true;
    }

    for (p = nl + 1; p && *p && g_refs < REF_MAX; ) {
        char *end = strchr(p, '\n');
        if (end) *end = 0;
        char *name = strchr(p, '\t');
        if (!name) break;
        *name++ = 0;
        char *sel = strchr(name, '\t');
        if (!sel) break;
        *sel++ = 0;
        char *val = strchr(sel, '\t');
        if (val) *val++ = 0;

        snprintf(g_ref[g_refs], sizeof g_ref[0], "%s", sel);
        g_refs++;
        buf_addf(out, "#%-3d %s", g_refs, p);
        if (*name) buf_addf(out, " \"%s\"", name);
        if (val && *val) buf_addf(out, " [%s]", val);
        buf_addf(out, "\n");
        p = end ? end + 1 : NULL;
    }
    buf_free(&raw);
    if (!g_refs) buf_addf(out, "nothing on this page can be acted on.\n");
    return true;
}

static bool tool_click(const char *args, Buf *out, char *err, size_t errcap) {
    char sel[512];
    if (!want_target(args, sel, sizeof sel, err, errcap)) return false;

    Buf js = {0}, got = {0};
    buf_addf(&js, "(function(){var e=document.querySelector(");
    buf_addf(&js, "'");
    json_escape_buf(&js, sel);
    buf_addf(&js, "');if(!e)return 'gone';"
                  "e.scrollIntoView({block:'center'});"
                  "if(e.focus)e.focus();e.click();return 'ok'})()");
    events_drop();
    bool ok = page_eval(js.p, &got, err, errcap);
    buf_free(&js);
    if (ok && strcmp(got.p ? got.p : "", "ok") != 0) {
        snprintf(err, errcap, "no element matching %s any more; take a snapshot",
                 sel);
        ok = false;
    }
    if (ok) {
        buf_addf(out, "clicked %s", sel);
        nav_after(out, err, errcap);
    }
    buf_free(&got);
    return ok;
}

// key name, virtual key code, text
static const struct { const char *name; int code; const char *text; } KEYS[] = {
    {"Enter", 13, "\\r"}, {"Tab", 9, "\\t"}, {"Escape", 27, ""},
    {"Backspace", 8, ""}, {"Delete", 46, ""},
    {"ArrowUp", 38, ""}, {"ArrowDown", 40, ""},
    {"ArrowLeft", 37, ""}, {"ArrowRight", 39, ""},
    {"PageUp", 33, ""}, {"PageDown", 34, ""},
    {"Home", 36, ""}, {"End", 35, ""},
};

static bool send_key(const char *key, char *err, size_t errcap) {
    events_drop();
    int code = 0;
    const char *text = "";
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (!strcasecmp(KEYS[i].name, key)) { code = KEYS[i].code; text = KEYS[i].text; key = KEYS[i].name; }
    if (!code) {
        snprintf(err, errcap, "no key called %s", key);
        return false;
    }
    for (int up = 0; up < 2; up++) {
        Buf params = {0}, reply = {0};
        buf_addf(&params,
                 "\"type\":\"%s\",\"key\":\"%s\",\"code\":\"%s\","
                 "\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d",
                 up ? "keyUp" : "keyDown", key, key, code, code);
        if (!up && *text) buf_addf(&params, ",\"text\":\"%s\"", text);
        bool ok = cdp_do("Input.dispatchKeyEvent", params.p, &reply, err, errcap);
        buf_free(&params);
        buf_free(&reply);
        if (!ok) return false;
    }
    return true;
}

static bool tool_type(const char *args, Buf *out, char *err, size_t errcap) {
    char sel[512], text[4096] = "";
    if (!want_target(args, sel, sizeof sel, err, errcap)) return false;
    field(args, "text", text, sizeof text);

    Buf js = {0}, got = {0};
    buf_addf(&js, "(function(){var e=document.querySelector('");
    json_escape_buf(&js, sel);
    buf_addf(&js, "');if(!e)return 'gone';"
                  "e.scrollIntoView({block:'center'});e.focus();"
                  "if(e.select)e.select();"
                  "else if(e.isContentEditable)document.execCommand('selectAll');"
                  "return 'ok'})()");
    bool ok = page_eval(js.p, &got, err, errcap);
    buf_free(&js);
    if (ok && strcmp(got.p ? got.p : "", "ok") != 0) {
        snprintf(err, errcap, "no element matching %s any more; take a snapshot",
                 sel);
        ok = false;
    }
    buf_free(&got);
    if (!ok) return false;

    Buf params = {0}, reply = {0}, esc = {0};
    json_escape_buf(&esc, text);
    buf_addf(&params, "\"text\":\"%s\"", esc.p);
    ok = cdp_do("Input.insertText", params.p, &reply, err, errcap);
    buf_free(&params);
    buf_free(&reply);
    buf_free(&esc);
    if (!ok) return false;

    if (json_has(args, "submit") && !strstr(args, "\"submit\":false")) {
        if (!send_key("Enter", err, errcap)) return false;
        buf_addf(out, "typed into %s and pressed Enter", sel);
        nav_after(out, err, errcap);
        return true;
    }
    buf_addf(out, "typed into %s", sel);
    return true;
}

static bool tool_press(const char *args, Buf *out, char *err, size_t errcap) {
    char key[64] = "";
    field(args, "key", key, sizeof key);
    if (!key[0]) { snprintf(err, errcap, "say which key"); return false; }
    if (!send_key(key, err, errcap)) return false;
    buf_addf(out, "pressed %s", key);
    nav_after(out, err, errcap);
    return true;
}

static bool tool_navigate(const char *args, Buf *out, char *err, size_t errcap) {
    char raw[2048] = "", url[2200];
    field(args, "url", raw, sizeof raw);
    if (!raw[0]) { snprintf(err, errcap, "say where to go"); return false; }
    bar_url(raw, url, sizeof url);

    Buf params = {0}, reply = {0}, esc = {0};
    json_escape_buf(&esc, url);
    buf_addf(&params, "\"url\":\"%s\"", esc.p);
    bool ok = cdp_do("Page.navigate", params.p, &reply, err, errcap);
    buf_free(&params);
    buf_free(&reply);
    buf_free(&esc);
    if (!ok) return false;
    g_refs = 0;
    return settle(out, err, errcap);
}

static bool step_history(int delta, Buf *out, char *err, size_t errcap) {
    Buf reply = {0};
    if (!cdp_do("Page.getNavigationHistory", "", &reply, err, errcap)) {
        buf_free(&reply);
        return false;
    }
    const char *arr = strstr(reply.p, "\"entries\":");
    if (arr) { arr += 10; while (*arr == ' ') arr++; }
    int want = (int)json_num(reply.p, "currentIndex", -1) + delta;
    const char *e = (arr && want >= 0) ? json_array_at(arr, want) : NULL;
    int entry = e ? (int)json_num(e, "id", -1) : -1;
    buf_free(&reply);
    if (entry < 0) {
        snprintf(err, errcap, "nothing to go %s to", delta < 0 ? "back" : "forward");
        return false;
    }

    Buf params = {0}, sent = {0};
    buf_addf(&params, "\"entryId\":%d", entry);
    bool ok = cdp_do("Page.navigateToHistoryEntry", params.p, &sent, err, errcap);
    buf_free(&params);
    buf_free(&sent);
    if (!ok) return false;
    g_refs = 0;
    return settle(out, err, errcap);
}

static bool tool_back(const char *args, Buf *out, char *err, size_t errcap) {
    return step_history(-1, out, err, errcap);
}

static bool tool_forward(const char *args, Buf *out, char *err, size_t errcap) {
    return step_history(+1, out, err, errcap);
}

static bool tool_read(const char *args, Buf *out, char *err, size_t errcap) {
    return page_eval(
        "(document.body?document.body.innerText:'')"
        ".replace(/\\n{3,}/g,'\\n\\n').trim().slice(0,20000)",
        out, err, errcap);
}

static bool tool_eval(const char *args, Buf *out, char *err, size_t errcap) {
    char js[8192] = "";
    field(args, "js", js, sizeof js);
    if (!js[0]) { snprintf(err, errcap, "say what to run"); return false; }
    return page_eval(js, out, err, errcap);
}

static bool tool_wait(const char *args, Buf *out, char *err, size_t errcap) {
    char js[4096] = "";
    field(args, "js", js, sizeof js);
    if (!js[0]) { snprintf(err, errcap, "say what to wait for"); return false; }
    double secs = json_num(args, "timeout", 15);
    if (secs <= 0 || secs > 120) secs = 15;

    Buf test = {0};
    buf_addf(&test, "!!(");
    buf_add(&test, js, strlen(js));
    buf_addf(&test, ")");

    double deadline = now_sec() + secs;
    for (;;) {
        Buf got = {0};
        bool ok = page_eval(test.p, &got, err, errcap);
        bool yes = ok && got.p && !strcmp(got.p, "true");
        buf_free(&got);
        if (!ok) { buf_free(&test); return false; }
        if (yes) { buf_free(&test); buf_addf(out, "true"); return true; }
        if (now_sec() > deadline) {
            buf_free(&test);
            snprintf(err, errcap, "still not true after %.0f seconds", secs);
            return false;
        }
        struct timespec ts = {0, 250000000};
        nanosleep(&ts, NULL);
    }
}

static bool tool_new_tab(const char *args, Buf *out, char *err, size_t errcap) {
    char raw[2048] = "", url[2200];
    field(args, "url", raw, sizeof raw);
    if (!raw[0]) { snprintf(err, errcap, "say where to go"); return false; }
    bar_url(raw, url, sizeof url);

    char dir[600], path[800], tmp[820];
    snprintf(dir, sizeof dir, "%s/handoff", W.profile);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/%d-%d", dir, (int)W.pid, (int)getpid());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { snprintf(err, errcap, "cannot write %s", tmp); return false; }
    fprintf(f, "%s\n", url);
    fclose(f);
    if (rename(tmp, path) != 0 || kill(W.pid, SIGUSR1) != 0) {
        unlink(tmp);
        unlink(path);
        snprintf(err, errcap, "the window did not take the address");
        return false;
    }
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    win_drop();
    g_refs = 0;
    buf_addf(out, "opened %s in a new tab", url);
    return true;
}

// ------------------------------------------------------------------- stdio

// id copied out verbatim: JSON-RPC allows a string or a number
static void call_id(const char *msg, char *out, size_t cap) {
    snprintf(out, cap, "null");
    const char *p = strstr(msg, "\"id\"");
    if (!p) return;
    p = strchr(p + 4, ':');
    if (!p) return;
    p++;
    while (*p == ' ') p++;
    size_t n = 0;
    if (*p == '"') {
        const char *e = p + 1;
        while (*e && *e != '"') e += (*e == '\\' && e[1]) ? 2 : 1;
        n = (size_t)(e - p) + 1;
    } else {
        const char *e = p;
        while (*e && *e != ',' && *e != '}' && *e != ' ') e++;
        n = (size_t)(e - p);
    }
    if (n && n < cap) snprintf(out, n + 1, "%s", p);
}

static void say(const Buf *b) {
    writeall(STDOUT_FILENO, b->p, b->len);
    writeall(STDOUT_FILENO, "\n", 1);
}

static void reply_result(const char *id, const char *result) {
    Buf b = {0};
    buf_addf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
    say(&b);
    buf_free(&b);
}

static void reply_error(const char *id, int code, const char *message) {
    Buf b = {0};
    buf_addf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,"
                 "\"message\":\"", id, code);
    json_escape_buf(&b, message);
    buf_addf(&b, "\"}}");
    say(&b);
    buf_free(&b);
}

static void reply_tool(const char *id, const char *text, bool failed) {
    Buf b = {0};
    buf_addf(&b, "{\"content\":[{\"type\":\"text\",\"text\":\"");
    json_escape_buf(&b, text && *text ? text : "(nothing)");
    buf_addf(&b, "\"}]%s}", failed ? ",\"isError\":true" : "");
    reply_result(id, b.p);
    buf_free(&b);
}

static const char TOOLS[] =
"["
"{\"name\":\"snapshot\",\"description\":\"List everything on the page that can "
"be acted on, each with a ref (#1, #2) for click and type. Start here rather "
"than with a screenshot: it is far cheaper and it says what things are.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"

"{\"name\":\"click\",\"description\":\"Click an element, by ref from a snapshot "
"or by CSS selector.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
"\"ref\":{\"type\":\"string\",\"description\":\"a ref from the last snapshot\"},"
"\"selector\":{\"type\":\"string\",\"description\":\"a CSS selector\"}}}},"

"{\"name\":\"type\",\"description\":\"Type into a field, replacing what is in "
"it. Real key input, so the page's own listeners see it.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
"\"ref\":{\"type\":\"string\",\"description\":\"a ref from the last snapshot\"},"
"\"selector\":{\"type\":\"string\",\"description\":\"a CSS selector\"},"
"\"text\":{\"type\":\"string\"},"
"\"submit\":{\"type\":\"boolean\",\"description\":\"press Enter afterwards\"}},"
"\"required\":[\"text\"]}},"

"{\"name\":\"press\",\"description\":\"Press a key: Enter, Tab, Escape, "
"Backspace, Delete, ArrowUp/Down/Left/Right, PageUp, PageDown, Home, End.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
"\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}},"

"{\"name\":\"navigate\",\"description\":\"Go to an address in the tab on "
"screen. A bare host gets https, and a line that is plainly a search becomes "
"one.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
"\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}},"

"{\"name\":\"new_tab\",\"description\":\"Open an address in a new tab of the "
"window, leaving the current page where it is.\","

"{\"name\":\"back\",\"description\":\"Go back in the tab's history.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"

"{\"name\":\"forward\",\"description\":\"Go forward in the tab's history.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
"\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}},"

"{\"name\":\"read\",\"description\":\"The page as text, the way it reads on "
"screen.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"

"{\"name\":\"eval\",\"description\":\"Run JavaScript in the page and get the "
"value back. Promises are waited for. This is the way to ask a page anything "
"the other tools do not cover.\",\"inputSchema\":{\"type\":\"object\","
"\"properties\":{\"js\":{\"type\":\"string\"}},\"required\":[\"js\"]}},"

"{\"name\":\"wait\",\"description\":\"Wait until a JavaScript expression is "
"true, polling until it is or the timeout runs out.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
"\"js\":{\"type\":\"string\"},"
"\"timeout\":{\"type\":\"number\",\"description\":\"seconds, 15 by default\"}},"
"\"required\":[\"js\"]}},"

"{\"name\":\"screenshot\",\"description\":\"A picture of the page as it is on "
"screen. Use it to see layout or something that has no text; for finding what "
"to click, snapshot is cheaper.\",\"inputSchema\":{\"type\":\"object\","
"\"properties\":{}}}"
"]";

static const char INSTRUCTIONS[] =
    "These tools drive a `web` window the user has open, on their own browser "
    "and their own logins - not a browser of your own. Everything you do is on "
    "their screen while you do it. Take a snapshot before acting, act by ref, "
    "and prefer snapshot and read over screenshot.";

static void do_screenshot(const char *id) {
    char err[512] = "";
    Buf reply = {0};
    if (!cdp_do("Page.captureScreenshot", "\"format\":\"png\"", &reply,
                err, sizeof err)) {
        buf_free(&reply);
        reply_tool(id, err, true);
        return;
    }
    size_t n = 0;
    const char *data = json_str(reply.p, "data", &n);
    if (!data || !n) {
        buf_free(&reply);
        reply_tool(id, "the page sent no picture back", true);
        return;
    }
    Buf b = {0};
    buf_addf(&b, "{\"content\":[{\"type\":\"image\",\"data\":\"");
    buf_add(&b, data, n);              // base64, nothing to escape
    buf_addf(&b, "\",\"mimeType\":\"image/png\"}]}");
    reply_result(id, b.p);
    buf_free(&b);
    buf_free(&reply);
}

static void do_call(const char *id, const char *msg) {
    char name[64] = "";
    field(msg, "name", name, sizeof name);
    const char *args = strstr(msg, "\"arguments\"");
    if (!args) args = "{}";

    char err[600] = "";
    if (!win_ready(err, sizeof err)) { reply_tool(id, err, true); return; }

    if (!strcmp(name, "screenshot")) { do_screenshot(id); return; }

    bool (*fn)(const char *, Buf *, char *, size_t) = NULL;
    if      (!strcmp(name, "snapshot")) fn = tool_snapshot;
    else if (!strcmp(name, "click"))    fn = tool_click;
    else if (!strcmp(name, "type"))     fn = tool_type;
    else if (!strcmp(name, "press"))    fn = tool_press;
    else if (!strcmp(name, "navigate")) fn = tool_navigate;
    else if (!strcmp(name, "new_tab"))  fn = tool_new_tab;
    else if (!strcmp(name, "back"))     fn = tool_back;
    else if (!strcmp(name, "forward"))  fn = tool_forward;
    else if (!strcmp(name, "read"))     fn = tool_read;
    else if (!strcmp(name, "eval"))     fn = tool_eval;
    else if (!strcmp(name, "wait"))     fn = tool_wait;
    if (!fn) {
        snprintf(err, sizeof err, "no tool called %s", name);
        reply_tool(id, err, true);
        return;
    }

    Buf out = {0};
    bool ok = fn(args, &out, err, sizeof err);
    reply_tool(id, ok ? out.p : err, !ok);
    buf_free(&out);
}

int mcp_serve(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, bye);
    signal(SIGTERM, bye);
    atexit(drive_release);
    W.ws.fd = -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        if (n <= 2) continue;
        char method[64] = "";
        field(line, "method", method, sizeof method);
        char id[64];
        call_id(line, id, sizeof id);
        bool notice = !strstr(line, "\"id\"");

        if (!strcmp(method, "initialize")) {
            char ver[32] = "";
            field(line, "protocolVersion", ver, sizeof ver);
            Buf r = {0};
            buf_addf(&r, "{\"protocolVersion\":\"%s\",\"capabilities\":"
                         "{\"tools\":{}},\"serverInfo\":{\"name\":\"web\","
                         "\"version\":\"0.1.0\"},\"instructions\":\"",
                     ver[0] ? ver : MCP_PROTOCOL);
            json_escape_buf(&r, INSTRUCTIONS);
            buf_addf(&r, "\"}");
            reply_result(id, r.p);
            buf_free(&r);
        } else if (!strcmp(method, "tools/list")) {
            Buf r = {0};
            buf_addf(&r, "{\"tools\":%s}", TOOLS);
            reply_result(id, r.p);
            buf_free(&r);
        } else if (!strcmp(method, "tools/call")) {
            do_call(id, line);
        } else if (!strcmp(method, "ping")) {
            reply_result(id, "{}");
        } else if (!notice) {
            reply_error(id, -32601, method);
        }
    }
    free(line);
    drive_release();
    win_drop();
    return 0;
}

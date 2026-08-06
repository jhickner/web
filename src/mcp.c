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

// The window on screen, handed to an agent.
//
// `web --mcp` is not a window: it is a driver that speaks MCP on its standard
// input and output, and drives the window somebody already has up. That is the
// whole point of it. An agent with a browser of its own works in the dark, and
// the first anyone knows of what it did is the summary it writes afterwards.
// Here the page it is working is the page in the pane next door - the same
// tabs, the same logins, the same scroll position - and every step it takes is
// on screen as it happens. Take the keyboard back at any moment; it is your
// browser.
//
// Nothing is installed for this. The protocol is JSON-RPC by line on stdio,
// devtools is a websocket that takes JSON, and both of those are already here.
//
// The tools are the ones an agent cannot get any other way round: `snapshot`
// so it can act by name rather than by pixel, `eval` for everything a page can
// be asked in one line, and `screenshot` for when it has to see. What it does
// through them goes through the page's own listeners, so `--record` writes an
// agent's session down as a spec exactly as it writes down yours.

#define MCP_PROTOCOL "2025-06-18"
#define REF_MAX      200
#define CALL_SECS    15.0

// ------------------------------------------------------------- the window

typedef struct {
    pid_t pid;
    int   port;
    char  target[128];
    char  drive[768];      // where to say it is being driven, so it keeps drawing
    char  profile[512];
    WS    ws;
    int   next_id;
    char  marked[768];     // the drive file standing now, which window it is for
} Win;

static Win W = {.ws = {.fd = -1}};

static bool cdp_do(const char *method, const char *params, Buf *reply,
                   char *err, size_t errcap);

// A copy, so a value read out of a session line survives the next read.
static void field(const char *line, const char *key, char *out, size_t cap) {
    size_t n = 0;
    const char *v = json_str(line, key, &n);
    if (v && n) json_unescape(out, cap, v, n);
    else if (cap) out[0] = 0;
}

// Which window: the one most recently used, or the one WEB_WINDOW names when
// several are up. The same rule the javascript helpers follow, so an agent and
// a script started side by side land on the same page.
static bool win_read(void) {
    char dir[600];
    chrome_profile_path(W.profile, sizeof W.profile);
    snprintf(dir, sizeof dir, "%s/sessions", W.profile);
    DIR *d = opendir(dir);
    if (!d) return false;

    const char *pick = getenv("WEB_WINDOW");
    long want = pick ? atol(pick) : 0;
    char best[4096] = "";
    time_t best_t = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int pid = atoi(e->d_name);
        if (pid <= 0 || (want && pid != want)) continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        // A window that has gone leaves its file behind; this is the only pass
        // over the directory that would ever notice.
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) { unlink(path); continue; }
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (best[0] && st.st_mtime < best_t) continue;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[4096];
        if (fgets(line, sizeof line, f)) {
            snprintf(best, sizeof best, "%s", line);
            best_t = st.st_mtime;
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

// Say the window is being worked, and take it back when this process ends.
//
// A window stops drawing when its pane loses the focus, and an agent working it
// from a client somewhere else is exactly the case that looks unwatched and is
// not. Chrome tells nobody who else is attached, so the driver is the only one
// who can say.
static void drive_release(void) {
    if (!W.marked[0]) return;
    unlink(W.marked);
    W.marked[0] = 0;
    if (W.pid > 0) kill(W.pid, SIGUSR1);
}

// Held against the window it is for, not once and for all: a client outlives
// the windows it works, and one quit and opened again is a different window
// with a different file to say so in. Marking the one that has gone leaves the
// new one dark.
static void drive_hold(void) {
    if (!W.drive[0] || W.pid <= 0) return;
    if (W.marked[0] && !strcmp(W.marked, W.drive)) return;
    drive_release();
    FILE *f = fopen(W.drive, "w");
    if (!f) return;                        // an older window, with nowhere to say it
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    snprintf(W.marked, sizeof W.marked, "%s", W.drive);
    // The window is asleep in a poll: the signal is what makes it look now.
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

// Connected to the page the window is showing right now. Checked before every
// call rather than once at the start: the window is a browser somebody is
// using, and the tab in front of it is theirs to change. Following it is what
// makes "the page on screen" mean the same thing to both of us.
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
        // Nothing here subscribes to the page, but a navigation announcing
        // itself is what tells a click it is not finished - and Page has to be
        // enabled before it says anything at all.
        char why[200];
        Buf reply = {0};
        cdp_do("Page.enable", "", &reply, why, sizeof why);
        buf_free(&reply);
    }
    drive_hold();
    return true;
}

// ---------------------------------------------------------------- devtools

// The page saying it is about to go somewhere - a link, a form, a script - seen
// while waiting for something else. A click sets the navigation off inside the
// call that made it, so this news usually arrives before the answer to the
// click does, and dropping it would lose the only warning there is.
static bool g_going;

// One devtools call, answered before we return. Events arriving meanwhile are
// dropped, apart from the one above: this process asks questions and subscribes
// to nothing.
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

// What the page said while nobody was listening. Between calls this process
// sits in getline and the socket fills up, so an action that wants to hear the
// answer to itself starts by throwing away the answers to everything before.
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

// Javascript in the page, answered as text.
//
// Wrapped rather than sent as the expression itself, for the two reasons the
// console needs as well: eval gives the completion value, so a line that is a
// statement still says what it worked out, and a promise is waited for rather
// than stringified into "[object Promise]".
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
        // The page threw. Its own words are worth more than "it failed": the
        // description carries the message and the stack, and the first line of
        // it is the message.
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

// What the last snapshot found, so an agent can say `#4` instead of carrying a
// css path around. Kept here rather than in the page: a variable left on
// window is one more thing a page could trip over, and a css path is a plain
// string that survives being handed back to us.
static char g_ref[REF_MAX][512];
static int  g_refs;

static const char *ref_sel(const char *ref) {
    if (*ref == '#' && ref[1] >= '0' && ref[1] <= '9') ref++;
    else if (*ref < '0' || *ref > '9') return NULL;
    int i = atoi(ref);
    if (i < 1 || i > g_refs) return NULL;
    return g_ref[i - 1];
}

// A selector from the arguments, whichever way it was named.
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

// Where the page settles, not where it was sent. An address is only the
// request; what the agent needs back is what arrived, which after a redirect
// is somewhere else entirely.
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

// A click is not finished when the click returns. Whatever it set off commits
// after it, and an agent told "clicked" acts on the next line against the page
// it has just left - which is how a link is followed and then gone back from
// before the page it went to ever existed. So if the page said it was going
// somewhere, this waits until it has got there and says where that was.
static void nav_after(Buf *out, char *err, size_t errcap) {
    if (!g_going && !event_wait("Page.frameRequestedNavigation", 0.5)) return;
    g_going = false;
    if (!event_wait("Page.frameNavigated", 15.0)) return;
    g_refs = 0;                          // another page: the old refs are lies
    Buf where = {0};
    if (settle(&where, err, errcap) && where.len)
        buf_addf(out, ", now on %s", where.p);
    buf_free(&where);
}

// ------------------------------------------------------------------ tools

// Everything on the page worth acting on, one to a line: what it is, what a
// user would call it, and the css path that finds it again. Read here rather
// than shown as a picture because it is a hundredth of the size and says what
// the things are, which pixels never do.
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

// Clicked through the element rather than by dispatching a mouse event at its
// coordinates. The coordinate way is the better gesture - it carries hover and
// focus with it, and it is what the window itself does for a real click - but
// it is hit tested against the compositor's idea of where the page is scrolled
// to, which catches up only when a frame has gone out. Nothing guarantees a
// frame while an agent is working, and a click that silently lands on the wrong
// element is worse than a plain one that lands on the right one.
static bool tool_click(const char *args, Buf *out, char *err, size_t errcap) {
    char sel[512];
    if (!want_target(args, sel, sizeof sel, err, errcap)) return false;

    Buf js = {0}, got = {0};
    buf_addf(&js, "(function(){var e=document.querySelector(");
    buf_addf(&js, "'");
    json_escape_buf(&js, sel);          // it is about to be a javascript string
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

// A key, as the page hears one. The table is the keys worth pressing without a
// keyboard in front of you; anything else is a character, and typing is what
// `type` is for.
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

// Typed rather than assigned. Setting value is invisible to a page that
// listens for input, which is most of them now, so the field is focused, what
// is in it is selected, and the text is inserted over the top - the same
// events a keyboard would have produced.
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
    // The same reading the address bar gives a line, so an agent and a person
    // typing the same thing land in the same place.
    bar_url(raw, url, sizeof url);

    Buf params = {0}, reply = {0}, esc = {0};
    json_escape_buf(&esc, url);
    buf_addf(&params, "\"url\":\"%s\"", esc.p);
    bool ok = cdp_do("Page.navigate", params.p, &reply, err, errcap);
    buf_free(&params);
    buf_free(&reply);
    buf_free(&esc);
    if (!ok) return false;
    g_refs = 0;                          // another page: the old refs are lies
    return settle(out, err, errcap);
}

// Through the browser's own list rather than history.back(), for the reason
// the window's own back key uses it: the list says when there is nowhere to go,
// and a call into the page returns whether it moved or not.
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
    g_refs = 0;                          // another page: the old refs are lies
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

// Polled rather than slept through. A fixed wait is a guess about somebody
// else's server: too short and the agent reports the page it was on a moment
// ago, long enough to be safe and it is time nobody gets back.
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

// A tab of the window's own making, rather than one opened behind its back.
// The window is asked for it the way `web --open` asks, so the page arrives in
// its tab bar and on its screen instead of being a target nothing is drawing.
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
    // Renamed into place, which is what makes a file the window finds a whole
    // one; the signal is only the nudge.
    if (rename(tmp, path) != 0 || kill(W.pid, SIGUSR1) != 0) {
        unlink(tmp);
        unlink(path);
        snprintf(err, errcap, "the window did not take the address");
        return false;
    }
    // The window makes the tab, tells devtools about it and switches to it, and
    // the next call reads where that left it out of the session file.
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    win_drop();
    g_refs = 0;
    buf_addf(out, "opened %s in a new tab", url);
    return true;
}

// ------------------------------------------------------------------- stdio

// The id, copied out whole rather than read as a number: JSON-RPC lets it be a
// string, and an answer carrying a different id than the question is an answer
// the client will not match up.
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

// What a tool answered. A tool that failed answers in the result rather than as
// a protocol error, because the model is meant to read it and try something
// else - a page that has moved on is news, not a broken connection.
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

// A screenshot goes back as a picture rather than as text, so it does not have
// to pass through the text answer above.
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
    buf_add(&b, data, n);              // base64 already, and nothing in it to escape
    buf_addf(&b, "\",\"mimeType\":\"image/png\"}]}");
    reply_result(id, b.p);
    buf_free(&b);
    buf_free(&reply);
}

static void do_call(const char *id, const char *msg) {
    char name[64] = "";
    field(msg, "name", name, sizeof name);
    // The arguments object, left as the text it arrived as: every field wanted
    // out of it is a string or a number, and json_str finds those in place.
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
    // Nothing but protocol may go to stdout, and a client that hangs up must
    // not take this process out mid-write with the window still marked.
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
            // The client's version is echoed when it named one: everything here
            // is the unchanging part of the protocol, and arguing about a date
            // gains nothing.
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
        // Notifications - initialized, cancelled - are told, not asked.
    }
    free(line);
    drive_release();
    win_drop();
    return 0;
}

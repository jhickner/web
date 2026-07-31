#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

// The command language. One queue of lines, whatever put them there, and a
// state machine stepped from the main loop so the picture keeps arriving while
// a script drives the page - which is the whole point of running it here rather
// than in something headless.

// name, id, arity, help. The arity decides how the rest of the line is split;
// the help doubles as the REPL's dropdown text.
enum { A_NONE, A_ONE, A_TWO, A_REST, A_NUM };

#define VERBS(X)                                                              \
    X("goto",     VB_GOTO,    A_REST, "navigate to a url, file or search")     \
    X("back",     VB_BACK,    A_NONE, "history back")                          \
    X("forward",  VB_FWD,     A_NONE, "history forward")                       \
    X("reload",   VB_RELOAD,  A_NONE, "reload the page")                       \
    X("wait",     VB_WAIT,    A_NUM,  "pause for N ms")                        \
    X("wait-for", VB_WAITFOR, A_ONE,  "wait until a selector is visible")      \
    X("wait-gone",VB_WAITGONE,A_ONE,  "wait until it is gone or hidden")       \
    X("click",    VB_CLICK,   A_ONE,  "click the first visible match")         \
    X("type",     VB_TYPE,    A_REST, "type into whatever has focus")          \
    X("fill",     VB_FILL,    A_TWO,  "focus a field and replace its text")    \
    X("text",     VB_TEXT,    A_ONE,  "print an element's text")               \
    X("html",     VB_HTML,    A_ONE,  "print outerHTML")                       \
    X("attr",     VB_ATTR,    A_TWO,  "print an attribute")                    \
    X("count",    VB_COUNT,   A_ONE,  "print how many match")                  \
    X("press",    VB_PRESS,   A_ONE,  "send a key to the page")                \
    X("scroll",   VB_SCROLL,  A_ONE,  "scroll by N, or top/bottom")            \
    X("url",      VB_URL,     A_NONE, "print the current url")                 \
    X("title",    VB_TITLE,   A_NONE, "print the page title")                  \
    X("eval",     VB_EVAL,    A_REST, "print the result of a JS expression")   \
    X("echo",     VB_ECHO,    A_REST, "print a literal line")                  \
    X("help",     VB_HELP,    A_NONE, "show every command")                    \
    X("pick",     VB_PICK,    A_NONE, "toggle click-to-copy selectors")        \
    X("record",   VB_RECORD,  A_ONE,  "echo what you do as commands: on|off")  \
    X("attach",   VB_ATTACH,  A_NUM,  "drive the chrome on devtools port N")   \
    X("stop",     VB_STOP,    A_NONE, "clear the queue")

#define X_ENUM(n, id, ar, h) id,
enum { VERBS(X_ENUM) VB__COUNT };
#undef X_ENUM

typedef struct {
    const char *name;
    int         id, arity;
    const char *help;
} Verb;

#define X_ROW(n, id, ar, h) { n, id, ar, h },
static const Verb VTAB[] = { VERBS(X_ROW) };
#undef X_ROW

// The verb table, for the REPL's completion dropdown. Handed over a field at a
// time so the pane does not need this file's types nor this file the pane's.
int script_verb_count(void) { return (int)(sizeof VTAB / sizeof *VTAB); }
const char *script_verb_name(int i) { return VTAB[i].name; }
const char *script_verb_help(int i) { return VTAB[i].help; }

// ------------------------------------------------------------------ output

// Data goes to fd 1 - but only when fd 1 is not the terminal the page is drawn
// on. Writing there would scroll the picture out from under its placeholder
// cells, which nothing puts back.
static void out_line(App *a, const char *payload) {
    Script *s = &a->script;
    Buf b = {0};
    if (s->json) {
        buf_add(&b, "{\"cmd\":\"", 8);
        json_escape_buf(&b, VTAB[s->verb].name);
        buf_add(&b, "\",\"arg\":\"", 9);
        json_escape_buf(&b, s->sel);
        buf_add(&b, "\",\"value\":\"", 11);
        json_escape_buf(&b, payload);
        buf_add(&b, "\"}\n", 3);
    } else {
        buf_add(&b, payload, strlen(payload));
        buf_add(&b, "\n", 1);
    }
    if (!a->stdout_tty) writeall(STDOUT_FILENO, b.p, b.len);
    if (a->repl_open) repl_pane_log(a, payload);
    else if (a->stdout_tty) notify(a, payload);
    buf_free(&b);
}

static void fail(App *a, const char *why) {
    Script *s = &a->script;
    char msg[4608];
    snprintf(msg, sizeof msg, "error: %s: %s", s->line, why);
    if (a->repl_open) repl_pane_log(a, msg);
    else fprintf(stderr, "web: line %d: %s: %s\n", s->lineno, s->line, why);
    s->failures++;
    s->state = SC_IDLE;
    // A script that has lost its place cannot be trusted to carry on against a
    // page that is not where it thinks it is.
    s->queue.len = 0;
    if (s->queue.p) s->queue.p[0] = 0;
}

// ------------------------------------------------------------------ queue

void script_init(App *a) {
    Script *s = &a->script;
    s->timeout = 5.0;
    s->state = SC_IDLE;
}

void script_free(App *a) {
    buf_free(&a->script.queue);
}

// `acting` covers the moment in between: the line has left the queue and the
// state it will park in has not been set yet, which is exactly when a verb
// dispatches the input the recorder must not write down again.
bool script_busy(const App *a) {
    return a->script.acting || a->script.state != SC_IDLE ||
           a->script.queue.len > 0;
}

void script_push(App *a, const char *line) {
    Script *s = &a->script;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return;

    // stop is not queued: its whole job is to get rid of what is queued.
    if (!strcmp(line, "stop")) {
        s->queue.len = 0;
        if (s->queue.p) s->queue.p[0] = 0;
        s->state = SC_IDLE;
        s->stepping = false;
        notify(a, "queue cleared");
        return;
    }
    buf_add(&s->queue, line, strlen(line));
    buf_add(&s->queue, "\n", 1);
}

int script_load(App *a, const char *path) {
    int fd = STDIN_FILENO;
    if (path && strcmp(path, "-")) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "web: %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    Buf all = {0};
    char tmp[4096];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof tmp)) > 0) buf_add(&all, tmp, (size_t)n);
    if (fd != STDIN_FILENO) close(fd);

    for (char *p = all.p; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        script_push(a, p);
        if (!nl) break;
        p = nl + 1;
    }
    buf_free(&all);
    a->script.drain_exit = true;
    return 0;
}

// ------------------------------------------------------------------ parse

// Whitespace separated, with the last argument taking the rest of the line so
// `goto some search phrase` and `echo hello world` need no quoting. A quoted
// first argument keeps its spaces, which selectors need.
static bool parse(App *a, char *line) {
    Script *s = &a->script;
    s->sel[0] = s->txt[0] = 0;
    s->num = 0;

    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char *word = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    size_t wlen = (size_t)(p - word);

    s->verb = -1;
    for (size_t i = 0; i < sizeof VTAB / sizeof *VTAB; i++)
        if (strlen(VTAB[i].name) == wlen && !strncmp(VTAB[i].name, word, wlen)) {
            s->verb = (int)i;
            break;
        }
    if (s->verb < 0) return false;

    while (*p == ' ' || *p == '\t') p++;
    int arity = VTAB[s->verb].arity;

    if (arity == A_NONE) return true;
    if (arity == A_REST) {
        snprintf(s->sel, sizeof s->sel, "%s", p);
        return true;
    }
    if (arity == A_NUM) {
        s->num = atof(p);
        return true;
    }

    // A_ONE / A_TWO: pull one token, quoted or not.
    char *dst = s->sel;
    size_t cap = sizeof s->sel - 1, o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o < cap) dst[o++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && o < cap) dst[o++] = *p++;
    }
    dst[o] = 0;
    if (arity == A_TWO) {
        while (*p == ' ' || *p == '\t') p++;
        snprintf(s->txt, sizeof s->txt, "%s", p);
    }
    return true;
}

// ---------------------------------------------------------------- resolver

// Sent with every call rather than installed once at document start. A command
// issued between a navigation committing and that script running would find
// nothing there, and the runner has no way to tell those moments apart. Three
// kilobytes against frames of a hundred is not worth the class of bug.
//
// Everything comes back as one flat string, because the reply reader finds a
// key anywhere in the message and an object would give it several to choose
// from. OK carries the answer, MISS means try again, ERR means stop.
static const char RESOLVER_JS[] =
"(function(sel,act,arg){"
"function norm(s){return (s||'').replace(/\\s+/g,' ').trim();}"

// The accessible name, in the order the specification consults sources - far
// from all of it, but the part real pages actually use.
"function name(e){"
 "var a=e.getAttribute('aria-label');if(a)return norm(a);"
 "var lb=e.getAttribute('aria-labelledby');"
 "if(lb){var t='';lb.split(/\\s+/).forEach(function(id){"
   "var n=document.getElementById(id);if(n)t+=' '+n.textContent;});"
  "if(norm(t))return norm(t);}"
 "if(e.tagName==='INPUT'){"
  "if(e.type==='submit'||e.type==='button')return norm(e.value);"
  "var l=e.labels&&e.labels[0];if(l)return norm(l.textContent);"
  "return norm(e.getAttribute('placeholder')||e.getAttribute('title')||'');}"
 "if(e.tagName==='IMG')return norm(e.getAttribute('alt')||'');"
 "return norm(e.textContent)||norm(e.getAttribute('title')||'');}"

"var IMPLICIT={A:'link',BUTTON:'button',SELECT:'combobox',TEXTAREA:'textbox',"
 "H1:'heading',H2:'heading',H3:'heading',H4:'heading',H5:'heading',H6:'heading',"
 "IMG:'img',NAV:'navigation',FORM:'form',TABLE:'table',SUMMARY:'button',"
 "UL:'list',OL:'list',LI:'listitem'};"
"function role(e){"
 "var r=e.getAttribute('role');if(r)return r.split(/\\s+/)[0];"
 "if(e.tagName==='INPUT'){var t=(e.type||'text').toLowerCase();"
  "return t==='checkbox'?'checkbox':t==='radio'?'radio':"
   "(t==='submit'||t==='button'||t==='reset')?'button':"
   "t==='search'?'searchbox':'textbox';}"
 "if(e.tagName==='A'&&!e.hasAttribute('href'))return '';"
 "return IMPLICIT[e.tagName]||'';}"

"function visible(e){"
 "if(!e.getBoundingClientRect)return false;"
 "var r=e.getBoundingClientRect();if(r.width<1||r.height<1)return false;"
 "var c=getComputedStyle(e);"
 "return c.visibility!=='hidden'&&c.display!=='none'&&parseFloat(c.opacity)>=0.05;}"

"function step(list,s){var m;"
 "if((m=/^nth=(-?\\d+)$/.exec(s))){var i=+m[1];if(i<0)i+=list.length;"
  "return (i>=0&&i<list.length)?[list[i]]:[];}"
 "if(s==='visible=true')return list.filter(visible);"
 "if((m=/^text=(.*)$/.exec(s))){"
  "var raw=m[1],exact=/^\".*\"$/.test(raw);"
  "var q=norm(exact?raw.slice(1,-1):raw).toLowerCase(),out=[];"
  "list.forEach(function(root){var all=root.querySelectorAll('*');"
   "for(var i=0;i<all.length;i++){var e=all[i];"
    "var t=norm(e.textContent).toLowerCase();"
    "if(exact?t!==q:t.indexOf(q)<0)continue;"
    // The deepest element that still matches: every ancestor up to <body>
    // contains the text too, and none of them is what was meant.
    "var deeper=false;"
    "for(var j=0;j<e.children.length;j++){"
     "var ct=norm(e.children[j].textContent).toLowerCase();"
     "if(exact?ct===q:ct.indexOf(q)>=0)deeper=true;}"
    "if(!deeper&&out.indexOf(e)<0)out.push(e);}});"
  "return out;}"
 "if((m=/^role=([a-zA-Z]+)(?:\\[name=(?:\"([^\"]*)\"|([^\\]]*))\\])?$/.exec(s))){"
  "var want=m[1].toLowerCase();"
  "var wn=m[2]!==undefined?m[2]:m[3];"
  "wn=(wn===undefined||wn===null)?null:norm(wn).toLowerCase();"
  "var out=[];"
  "list.forEach(function(root){var all=root.querySelectorAll('*');"
   "for(var i=0;i<all.length;i++){var e=all[i];"
    "if(role(e)!==want)continue;"
    "if(wn!==null&&name(e).toLowerCase().indexOf(wn)<0)continue;"
    "out.push(e);}});"
  "return out;}"
 "var out=[];"
 "try{list.forEach(function(root){var q=root.querySelectorAll(s);"
  "for(var i=0;i<q.length;i++)out.push(q[i]);});}"
 "catch(e){return null;}"
 "return out;}"

"function resolve(sel){"
 "var parts=sel.split('>>').map(function(s){return s.trim();});"
 "var list=[document];"
 "for(var i=0;i<parts.length;i++){"
  "if(!parts[i])continue;"
  "list=step(list,parts[i]);"
  "if(list===null)return null;"
  "if(!list.length)return [];}"
 "return list;}"

"try{"
 "var list=resolve(sel);"
 "if(list===null)return 'ERR\\tbad selector';"
 "if(!list.length)return 'MISS\\tno match';"
 "var vis=list.filter(visible);"
 "if(act==='count')return 'OK\\t'+vis.length;"
 "if(!vis.length)return 'MISS\\thidden';"
 "var e=vis[0];"
 "if(act==='text')return 'OK\\t'+norm(e.innerText||e.textContent);"
 "if(act==='html')return 'OK\\t'+e.outerHTML;"
 "if(act==='attr'){var v=arg==='value'?e.value:e.getAttribute(arg);"
  "return (v===null||v===undefined)?'MISS\\tno attribute':'OK\\t'+v;}"
 "if(act==='focus'){e.scrollIntoView({block:'center',behavior:'instant'});"
  "e.focus();if(e.select)e.select();return 'OK\\t';}"
 // point: the centre, hit-tested. Something on top means the click would land
 // on that instead, which is a MISS worth retrying - banners come and go.
 "e.scrollIntoView({block:'center',inline:'center',behavior:'instant'});"
 "var r=e.getBoundingClientRect();"
 "var cx=Math.min(Math.max(r.left+r.width/2,1),innerWidth-1);"
 "var cy=Math.min(Math.max(r.top+r.height/2,1),innerHeight-1);"
 "var hit=document.elementFromPoint(cx,cy);"
 "while(hit&&hit.shadowRoot){var d=hit.shadowRoot.elementFromPoint(cx,cy);"
  "if(!d||d===hit)break;hit=d;}"
 "if(hit&&hit!==e&&!e.contains(hit)&&!hit.contains(e)){"
  "cx=Math.min(Math.max(r.left+4,1),innerWidth-1);"
  "cy=Math.min(Math.max(r.top+Math.min(r.height/2,8),1),innerHeight-1);"
  "hit=document.elementFromPoint(cx,cy);"
  "if(!hit||(hit!==e&&!e.contains(hit)&&!hit.contains(e)))"
   "return 'MISS\\tcovered';}"
 "return 'OK\\t'+Math.round(cx)+'\\t'+Math.round(cy);"
"}catch(x){return 'ERR\\t'+String((x&&x.message)||x);}"
"})";

// A JS string literal. Escaped once here for the JS source and once more by the
// caller for the JSON around it, which is what find_next has always done.
static void js_arg(Buf *out, const char *s) {
    buf_add(out, "\"", 1);
    json_escape_buf(out, s);
    buf_add(out, "\"", 1);
}

// ------------------------------------------------------------------ verbs

static const struct { const char *name; int key; } KEYNAMES[] = {
    {"Enter", KEY_ENTER}, {"Return", KEY_ENTER}, {"Tab", KEY_TAB},
    {"Backspace", KEY_BACKSPACE}, {"Delete", KEY_DELETE}, {"Escape", KEY_ESC},
    {"Esc", KEY_ESC}, {"ArrowUp", KEY_UP}, {"Up", KEY_UP},
    {"ArrowDown", KEY_DOWN}, {"Down", KEY_DOWN}, {"ArrowLeft", KEY_LEFT},
    {"Left", KEY_LEFT}, {"ArrowRight", KEY_RIGHT}, {"Right", KEY_RIGHT},
    {"Home", KEY_HOME}, {"End", KEY_END}, {"PageUp", KEY_PGUP},
    {"PageDown", KEY_PGDN},
};

// "Enter", "ctrl+a", "shift+Tab". Returns false if nothing there names a key.
static bool press_key(App *a, const char *spec) {
    int mods = 0;
    const char *p = spec;
    for (;;) {
        const char *plus = strchr(p, '+');
        if (!plus || plus == p) break;
        size_t n = (size_t)(plus - p);
        if      (n == 4 && !strncasecmp(p, "ctrl", 4))  mods |= MOD_CTRL;
        else if (n == 5 && !strncasecmp(p, "shift", 5)) mods |= MOD_SHIFT;
        else if (n == 3 && !strncasecmp(p, "alt", 3))   mods |= MOD_ALT;
        else break;
        p = plus + 1;
    }
    for (size_t i = 0; i < sizeof KEYNAMES / sizeof *KEYNAMES; i++)
        if (!strcasecmp(KEYNAMES[i].name, p))
            return special_key(a, KEYNAMES[i].key, mods);

    // A single character is itself. Anything longer is not a key we know.
    if (p[0] && !p[1]) {
        char t[2] = {p[0], 0};
        char esc[8];
        json_escape(esc, sizeof esc, t);
        app_cdp(a, "Input.dispatchKeyEvent", "\"type\":\"char\",\"text\":\"%s\"", esc);
        return true;
    }
    return false;
}

// Ask the page something and park until it answers.
static void probe(App *a, const char *expr) {
    Script *s = &a->script;
    Buf esc = {0};
    json_escape_buf(&esc, expr);
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"%s\",\"returnByValue\":true", esc.p), RQ_SCRIPT);
    buf_free(&esc);
    s->state = SC_WAIT;
}

static void probe_sel(App *a, const char *act) {
    Script *s = &a->script;
    snprintf(s->act, sizeof s->act, "%s", act);

    Buf e = {0};
    buf_add(&e, RESOLVER_JS, sizeof RESOLVER_JS - 1);
    buf_add(&e, "(", 1);
    js_arg(&e, s->sel[0] ? s->sel : "body");
    buf_add(&e, ",", 1);
    js_arg(&e, act);
    buf_add(&e, ",", 1);
    js_arg(&e, s->txt);
    buf_add(&e, ")", 1);
    probe(a, e.p);
    buf_free(&e);
}

// Done with the command in flight; the delay before the next one starts here.
static void done(App *a) {
    Script *s = &a->script;
    s->state = SC_IDLE;
    s->next_at = now_sec() + s->delay;
    if (s->step) s->stepping = true;
}

static void start(App *a) {
    Script *s = &a->script;
    s->deadline = now_sec() + s->timeout;

    switch (VTAB[s->verb].id) {
    case VB_HELP:
        repl_pane_help(a);
        if (!a->repl_open)
            for (int i = 0; i < script_verb_count(); i++) {
                char row[160];
                snprintf(row, sizeof row, "%-10s %s", VTAB[i].name, VTAB[i].help);
                out_line(a, row);
            }
        done(a);
        return;
    case VB_PICK:
        a->selector_pick = !a->selector_pick;
        out_line(a, a->selector_pick
                 ? "selector picker on - click the page; run pick again to stop"
                 : "selector picker off");
        done(a);
        return;
    case VB_ECHO:  out_line(a, s->sel); done(a); return;

    // A comment rather than a bare line: recorded output is a script, and a
    // redirected one should still run with this in the middle of it.
    case VB_RECORD: {
        int mode = -1;
        if (!strcasecmp(s->sel, "on"))       mode = 1;
        else if (!strcasecmp(s->sel, "off")) mode = 0;
        else if (s->sel[0]) { fail(a, "want on, off, or nothing"); return; }
        record_set(a, mode);
        out_line(a, a->recording ? "# recording on - what you do comes back here"
                                 : "# recording off");
        done(a);
        return;
    }

    case VB_ATTACH: {
        char m[160];
        if (app_attach(a, (int)s->num, m, sizeof m) < 0) { fail(a, m); return; }
        out_line(a, m);
        done(a);
        return;
    }

    // Both read from the page rather than from what we last cached: the cache
    // is filled by events that do not keep step with the one a navigation verb
    // finishes on, and a fragment or a route change never updates it at all.
    case VB_URL:   probe(a, "location.href");  return;
    case VB_TITLE: probe(a, "document.title"); return;

    case VB_WAIT:
        s->next_at = now_sec() + s->num / 1000.0;
        s->state = SC_SETTLE;
        return;

    case VB_GOTO:
        if (!s->sel[0]) { fail(a, "nothing to go to"); return; }
        s->nav_seq = a->load_seq;
        navigate(a, s->sel);
        a->expect_frame = now_sec() + 2.0;
        s->state = SC_NAV;
        return;

    case VB_RELOAD:
        s->nav_seq = a->load_seq;
        app_cdp(a, "Page.reload", "\"ignoreCache\":false");
        a->loading = true;
        a->expect_frame = now_sec() + 2.0;
        s->state = SC_NAV;
        return;

    case VB_BACK:
    case VB_FWD:
        s->nav_seq = a->load_seq;
        nav_history(a, VTAB[s->verb].id == VB_BACK ? -1 : +1);
        a->expect_frame = now_sec() + 2.0;
        // A single-page app answers history.back() by swapping the view without
        // ever firing load, so the deadline here is the ordinary way this ends
        // rather than a failure. Kept short for the same reason.
        s->deadline = now_sec() + 1.0;
        s->state = SC_NAV;
        return;

    case VB_SCROLL:
        if (!strcasecmp(s->sel, "top"))         scroll_page_end(a, false);
        else if (!strcasecmp(s->sel, "bottom")) scroll_page_end(a, true);
        else                                    scroll_by(a, atoi(s->sel));
        a->expect_frame = now_sec() + 2.0;
        s->next_at = now_sec() + 0.15;
        s->state = SC_SETTLE;
        return;

    case VB_PRESS:
        if (!press_key(a, s->sel)) { fail(a, "not a key"); return; }
        a->expect_frame = now_sec() + 2.0;
        s->next_at = now_sec() + 0.15;
        s->state = SC_SETTLE;
        return;

    case VB_WAITFOR:
    case VB_WAITGONE:
    case VB_CLICK:
        if (!s->sel[0]) { fail(a, "nothing to look for"); return; }
        probe_sel(a, VTAB[s->verb].id == VB_CLICK ? "point" : "count");
        return;

    case VB_TEXT:  probe_sel(a, "text");  return;
    case VB_HTML:  probe_sel(a, "html");  return;
    case VB_COUNT: probe_sel(a, "count"); return;

    case VB_ATTR:
        if (!s->sel[0] || !s->txt[0]) { fail(a, "want a selector and a name"); return; }
        probe_sel(a, "attr");
        return;

    case VB_FILL:
        if (!s->sel[0]) { fail(a, "nothing to fill"); return; }
        probe_sel(a, "focus");
        return;

    case VB_TYPE: {
        Buf esc = {0};
        json_escape_buf(&esc, s->sel);
        app_cdp(a, "Input.insertText", "\"text\":\"%s\"", esc.p);
        buf_free(&esc);
        a->expect_frame = now_sec() + 2.0;
        s->next_at = now_sec() + 0.15;
        s->state = SC_SETTLE;
        return;
    }

    case VB_EVAL: {
        // String() so the reply is always the flat kind the reader can take.
        Buf e = {0};
        buf_add(&e, "String(", 7);
        buf_add(&e, s->sel, strlen(s->sel));
        buf_add(&e, ")", 1);
        probe(a, e.p);
        buf_free(&e);
        return;
    }
    }
    fail(a, "not implemented");
}

// ------------------------------------------------------------------ replies

void script_reply(App *a, const char *msg) {
    Script *s = &a->script;
    if (s->state != SC_WAIT) return;

    size_t n = 0;
    const char *v = json_eval_str(msg, &n);
    if (!v) { fail(a, "the page threw"); return; }

    Buf out = {0};
    buf_reserve(&out, n + 4);
    json_unescape(out.p, n + 4, v, n);

    int id = VTAB[s->verb].id;

    // eval and the two page properties answer with the value itself; everything
    // that went through the resolver answers with the OK/MISS/ERR grammar.
    if (id == VB_EVAL || id == VB_URL || id == VB_TITLE) {
        out_line(a, out.p);
        done(a);
        buf_free(&out);
        return;
    }

    char *tab = strchr(out.p, '\t');
    char *body = tab ? tab + 1 : (char *)"";
    if (tab) *tab = 0;
    bool ok = !strcmp(out.p, "OK");
    bool miss = !strcmp(out.p, "MISS");

    if (!ok && !miss) { fail(a, body[0] ? body : "the page threw"); buf_free(&out); return; }

    // wait-gone reads the same answers the other way up: still there is the
    // thing worth waiting out.
    if (id == VB_WAITGONE) {
        long cnt = ok ? strtol(body, NULL, 10) : 0;
        if (miss || cnt == 0) done(a);
        else if (now_sec() > s->deadline) fail(a, "still there");
        else { s->next_at = now_sec() + 0.1; s->state = SC_PROBE; }
        buf_free(&out);
        return;
    }

    if (miss) {
        // The whole of auto-waiting: a page that has not caught up yet is asked
        // again rather than treated as an answer.
        if (now_sec() > s->deadline) fail(a, body[0] ? body : "no match");
        else { s->next_at = now_sec() + 0.1; s->state = SC_PROBE; }
        buf_free(&out);
        return;
    }

    switch (id) {
    case VB_WAITFOR:
        if (strtol(body, NULL, 10) > 0) done(a);
        else if (now_sec() > s->deadline) fail(a, "no match");
        else { s->next_at = now_sec() + 0.1; s->state = SC_PROBE; }
        break;

    case VB_TEXT:
    case VB_HTML:
    case VB_ATTR:
    case VB_COUNT:
        out_line(a, body);
        done(a);
        break;

    case VB_CLICK: {
        char *sp = strchr(body, '\t');
        if (!sp) { fail(a, "the page gave no point"); break; }
        *sp = 0;
        s->px = atoi(body);
        s->py = atoi(sp + 1);
        s->state = SC_ACT;
        break;
    }

    case VB_FILL: {
        // The field is focused and its old contents selected, so the insert
        // replaces them - and goes through the editing pipeline, which is what
        // fires the events a framework-controlled input listens for.
        Buf esc = {0};
        json_escape_buf(&esc, s->txt);
        app_cdp(a, "Input.insertText", "\"text\":\"%s\"", esc.p);
        buf_free(&esc);
        a->expect_frame = now_sec() + 2.0;
        s->next_at = now_sec() + 0.15;
        s->state = SC_SETTLE;
        break;
    }

    default:
        done(a);
        break;
    }
    buf_free(&out);
}

// ------------------------------------------------------------------ loop

void script_step(App *a) {
    Script *s = &a->script;
    double t = now_sec();

    switch (s->state) {
    case SC_IDLE:
        if (!s->queue.len || s->stepping || t < s->next_at) return;
        {
            char *nl = strchr(s->queue.p, '\n');
            size_t len = nl ? (size_t)(nl - s->queue.p) : s->queue.len;
            if (len >= sizeof s->line) len = sizeof s->line - 1;
            memcpy(s->line, s->queue.p, len);
            s->line[len] = 0;
            buf_consume(&s->queue, nl ? len + 1 : len);
        }
        s->lineno++;
        {
            char work[sizeof s->line];
            memcpy(work, s->line, sizeof work);
            if (!parse(a, work)) { fail(a, "unknown command"); return; }
        }
        s->acting = true;
        start(a);
        s->acting = false;
        return;

    case SC_PROBE:
        if (t < s->next_at) return;
        if (t > s->deadline) { fail(a, "timed out"); return; }
        probe_sel(a, s->act);
        return;

    case SC_WAIT:
        if (t > s->deadline) fail(a, "timed out");
        return;

    case SC_ACT: {
        // A real mouse event rather than element.click(): it is trusted, it
        // leaves the hover and focus states a click leaves, and those are what
        // make the run visible in the picture.
        int x = s->px, y = s->py;
        app_cdp(a, "Input.dispatchMouseEvent",
                "\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d,"
                "\"button\":\"none\",\"buttons\":0", x, y);
        app_cdp(a, "Input.dispatchMouseEvent",
                "\"type\":\"mousePressed\",\"x\":%d,\"y\":%d,"
                "\"button\":\"left\",\"buttons\":1,\"clickCount\":1", x, y);
        app_cdp(a, "Input.dispatchMouseEvent",
                "\"type\":\"mouseReleased\",\"x\":%d,\"y\":%d,"
                "\"button\":\"left\",\"buttons\":0,\"clickCount\":1", x, y);
        a->expect_frame = now_sec() + 2.0;
        s->next_at = now_sec() + 0.15;
        s->state = SC_SETTLE;
        return;
    }

    case SC_NAV:
        if (a->load_seq != s->nav_seq) { done(a); return; }
        if (t > s->deadline) {
            // goto and reload asked for a page, and not getting one is a
            // failure. Everything else only followed a load that started on its
            // own - a click on a link, an Enter that submitted a form - and a
            // page that never arrives there is not a failed command.
            int id = VTAB[s->verb].id;
            if (id == VB_GOTO || id == VB_RELOAD) fail(a, "the page did not load");
            else done(a);
        }
        return;

    case SC_SETTLE:
        if (t < s->next_at) return;
        // Input that set a page going: the command is not over until that page
        // has arrived. Without this the next command is sent to a document on
        // its way out, and a script whose last line submits a form exits before
        // the form has gone anywhere.
        if (a->loading) {
            s->nav_seq = a->load_seq;
            s->deadline = now_sec() + s->timeout;
            s->state = SC_NAV;
            return;
        }
        done(a);
        return;
    }
}

int script_wait_ms(const App *a) {
    const Script *s = &a->script;
    // Nothing queued and nothing running: the loop keeps blocking forever, the
    // way it did before any of this existed.
    if (s->state == SC_IDLE && !s->queue.len) return -1;
    if (s->stepping) return -1;

    double t = (s->state == SC_WAIT || s->state == SC_NAV) ? s->deadline : s->next_at;
    double ms = (t - now_sec()) * 1000.0;
    if (ms < 5) ms = 5;             // never spin
    if (ms > 100) ms = 100;         // never oversleep a poll cycle
    return (int)ms;
}

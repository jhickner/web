#include <stdio.h>
#include <string.h>
#include <time.h>
#include "web.h"

// Driving the page by hand and getting a Playwright spec out of it.
//
// `playwright codegen` opens a desktop window with an inspector beside it,
// which is no use down an ssh connection - and the window is the part that is
// hard to come by, not the recording. Here the window is already a terminal:
// the page is driven with the keyboard, the link labels reach anything
// clickable without a mouse, and what comes out is a file.
//
// What is recorded is real interaction, whatever caused it: a click dispatched
// at coordinates, a link label typed, a form filled in insert mode. The page's
// own listeners are what see them, so nothing here has to know how the window
// sent them - and the locator for what was hit is worked out in the page,
// where the DOM is, rather than guessed at from this side.

#define REC_MAX  400
#define LINE_MAX 400

static char g_lines[REC_MAX][LINE_MAX];
static int  g_n;

// The recorder, in the page. In an isolated world - it shares the DOM and sees
// every event, and defines nothing the page can trip over.
//
// The locator is the whole point. A CSS path is what a page happens to be built
// like today, and the run that reads it back is asking for the same button
// tomorrow: `getByRole('button', { name: 'Save' })` still finds it after the
// markup has been rewritten around it, and says what it is looking for. So the
// semantic ones are tried in the order Playwright itself recommends and a path
// is what is left when none of them is unique.
static const char REC_WATCHER[] =
    "(function(){if(window.__webrecOn)return;window.__webrecOn=1;"
    "function q(s){return String(s).replace(/\\\\/g,'\\\\\\\\').replace(/'/g,\"\\\\'\")}"
    "function vis(e){return e&&e.nodeType===1}"
    ROLE_NAME_FN
    // Whether one role-and-name picks out exactly one thing on the page.
    "function only(r,n){var c=0,l=document.querySelectorAll("
    "'a,button,input,select,textarea,img,h1,h2,h3,h4,h5,h6,[role]');"
    "for(var i=0;i<l.length;i++)if(rl(l[i])===r&&nm(l[i])===n)c++;return c===1}"
    "function onlyText(t){var c=0,l=document.querySelectorAll('*');"
    "for(var i=0;i<l.length;i++)if(!l[i].children.length&&"
    "(l[i].textContent||'').trim()===t)c++;return c===1}"
    SELECTOR_FN
    "function loc(e){if(!vis(e))return null;"
    "var d=e.getAttribute&&(e.getAttribute('data-testid')||e.getAttribute('data-test-id'));"
    "if(d)return \"getByTestId('\"+q(d)+\"')\";"
    "var r=rl(e),n=nm(e);"
    "if(r&&n&&n.length<60&&only(r,n))"
    "return \"getByRole('\"+q(r)+\"', { name: '\"+q(n)+\"' })\";"
    "var p=e.getAttribute&&e.getAttribute('placeholder');"
    "if(p)return \"getByPlaceholder('\"+q(p)+\"')\";"
    "if(e.labels&&e.labels[0])return \"getByLabel('\"+q(e.labels[0].textContent.trim())+\"')\";"
    "if(e.alt)return \"getByAltText('\"+q(e.alt.trim())+\"')\";"
    "if(n&&n.length<60&&onlyText(n))return \"getByText('\"+q(n)+\"')\";"
    "return \"locator('\"+q(ws(e))+\"')\"}"
    "function say(o){try{__webrec(JSON.stringify(o))}catch(_){}}"
    // Capture phase, so a page that swallows its own events is recorded anyway.
    // A tick box and a dropdown say what happened to them on change, and that
    // is the line worth having: .check() and .selectOption() ask for the state,
    // where a click on the same thing asks for whatever it toggles to.
    "function chg(e){var y=(e.type||'').toLowerCase();"
    "return e.tagName==='SELECT'||e.tagName==='OPTION'||"
    "(e.tagName==='INPUT'&&(y==='checkbox'||y==='radio'))}"
    "document.addEventListener('click',function(ev){"
    "if(chg(ev.target))return;"
    "var l=loc(ev.target);if(l)say({kind:'click',loc:l})},true);"
    // A field says what is in it only when it is done with, and for a keyboard
    // that is the enter key - which fires the change after the keydown it came
    // from. Taken at the keydown instead, so the value is written down before
    // the enter rather than after it; the change that follows says nothing new
    // and is dropped, which is what `seen` is for.
    "var seen=new WeakMap();"
    "function typed(e){var y=(e.type||'').toLowerCase();"
    "return e.tagName==='TEXTAREA'||(e.tagName==='INPUT'&&"
    "['checkbox','radio','submit','button','reset','file'].indexOf(y)<0)}"
    "function fill(e){var l=loc(e);if(!l||seen.get(e)===e.value)return;"
    "seen.set(e,e.value);say({kind:'fill',loc:l,value:e.value})}"
    "document.addEventListener('change',function(ev){"
    "var e=ev.target,l=loc(e);if(!l)return;"
    "if(e.tagName==='SELECT')say({kind:'select',loc:l,value:e.value});"
    "else if(e.type==='checkbox'||e.type==='radio')"
    "say({kind:e.checked?'check':'uncheck',loc:l});"
    "else if(typed(e))fill(e)},true);"
    "document.addEventListener('keydown',function(ev){"
    "if(ev.key!=='Enter')return;var e=ev.target;"
    "if(typed(e))fill(e);"
    "var l=loc(e);if(l)say({kind:'press',loc:l,value:'Enter'})},true);"
    "})()";

// ------------------------------------------------------------------- file

static void rec_write(App *a) {
    if (!a->rec_path[0]) return;
    FILE *f = fopen(a->rec_path, "w");
    if (!f) return;
    fprintf(f, "import { test, expect } from '@jhickner/web/test';\n\n"
               "test('recorded', async ({ page }) => {\n");
    for (int i = 0; i < g_n; i++) fprintf(f, "  %s\n", g_lines[i]);
    fputs("});\n", f);
    fclose(f);
}

// Kept whole rather than appended to, so the file on disk is a spec that runs
// at every moment of the recording rather than only at the end of it.
static void rec_add(App *a, const char *line) {
    if (g_n >= REC_MAX) return;
    snprintf(g_lines[g_n], LINE_MAX, "%s", line);
    g_n++;
    rec_write(a);
}

// Typing is one line, not one per keystroke: the page reports the field's value
// when it is done with, and a second report for the same field is the same
// field being finished with again.
static void rec_add_fill(App *a, const char *line, const char *loc) {
    char head[LINE_MAX];
    snprintf(head, sizeof head, "await page.%s.fill(", loc);
    if (g_n > 0 && !strncmp(g_lines[g_n - 1], head, strlen(head))) {
        snprintf(g_lines[g_n - 1], LINE_MAX, "%s", line);
        rec_write(a);
        return;
    }
    rec_add(a, line);
}

// ------------------------------------------------------------------ events

// One interaction, as the page saw it.
void record_event(App *a, const char *json) {
    if (!a->rec_on) return;
    size_t kn = 0, ln = 0, vn = 0;
    const char *k = json_str(json, "kind", &kn);
    const char *l = json_str(json, "loc", &ln);
    const char *v = json_str(json, "value", &vn);
    if (!k || !kn || !l || !ln) return;

    char kind[24], loc[LINE_MAX], val[LINE_MAX], line[LINE_MAX];
    json_unescape(kind, sizeof kind, k, kn);
    json_unescape(loc, sizeof loc, l, ln);
    json_unescape(val, sizeof val, v ? v : "", v ? vn : 0);
    // The page escaped these for javascript before it sent them; they came
    // through JSON on the way here and are quoted for javascript again below.
    for (char *p = val; *p; p++) if (*p == '\'' || *p == '\\') *p = ' ';

    if (!strcmp(kind, "click"))
        snprintf(line, sizeof line, "await page.%s.click();", loc);
    else if (!strcmp(kind, "fill")) {
        snprintf(line, sizeof line, "await page.%s.fill('%s');", loc, val);
        rec_add_fill(a, line, loc);
        return;
    } else if (!strcmp(kind, "select"))
        snprintf(line, sizeof line, "await page.%s.selectOption('%s');", loc, val);
    else if (!strcmp(kind, "check"))
        snprintf(line, sizeof line, "await page.%s.check();", loc);
    else if (!strcmp(kind, "uncheck"))
        snprintf(line, sizeof line, "await page.%s.uncheck();", loc);
    else if (!strcmp(kind, "press"))
        snprintf(line, sizeof line, "await page.%s.press('%s');", loc, val);
    else return;
    rec_add(a, line);
}

// Back and forward as the page's own history rather than as the address they
// land on: a spec that says goBack still does the right thing when the page it
// was recorded against changes what came before it.
void record_history(App *a, int delta) {
    if (!a->rec_on) return;
    rec_add(a, delta < 0 ? "await page.goBack();" : "await page.goForward();");
}

// An address the window was told to go to. A click that navigates is already
// recorded as the click, and the page arriving after it needs no line of its
// own - so this is only ever what somebody typed, or was handed in.
void record_goto(App *a, const char *url) {
    if (!a->rec_on || !url || !*url) return;
    char line[LINE_MAX], safe[LINE_MAX];
    snprintf(safe, sizeof safe, "%s", url);
    for (char *p = safe; *p; p++) if (*p == '\'' || *p == '\\') *p = ' ';
    snprintf(line, sizeof line, "await page.goto('%s');", safe);
    rec_add(a, line);
}

// ------------------------------------------------------------------ on/off

// The watcher goes in when recording starts and stays for the pages after it:
// a recording that stopped at the first navigation would record the first click
// of every flow and nothing else.
void record_install(App *a, bool fresh) {
    if (!a->rec_on) return;
    (void)fresh;
    char esc[16384];
    json_escape(esc, sizeof esc, REC_WATCHER);
    // runImmediately is what covers the document already on screen; the
    // registration alone would wait for the next one. Registered again when
    // recording starts mid-session, which the watcher's own guard makes
    // harmless - it defines itself once per document however often it is run.
    app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
             "\"source\":\"%s\",\"worldName\":\"%s\",\"runImmediately\":true",
             esc, WEB_WORLD);
}

void record_start(App *a, const char *path) {
    if (a->rec_on) return;
    if (path && *path) snprintf(a->rec_path, sizeof a->rec_path, "%s", path);
    if (!a->rec_path[0]) {
        // Named for when it was made, in the directory the window was started
        // from: a recording with nowhere to go is one nobody will find again.
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char stamp[32];
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        snprintf(a->rec_path, sizeof a->rec_path, "web-%s.spec.ts", stamp);
    }
    g_n = 0;
    a->rec_on = true;
    record_install(a, false);
    // Where it was when recording began, so the file opens on a page rather
    // than wherever the first click happened to be.
    record_goto(a, a->url);
    char m[600];
    snprintf(m, sizeof m, "recording to %.500s", a->rec_path);
    notify(a, m);
}

void record_stop(App *a) {
    if (!a->rec_on) return;
    a->rec_on = false;
    rec_write(a);
    char m[600];
    snprintf(m, sizeof m, "recorded %d step%s to %.480s", g_n,
             g_n == 1 ? "" : "s", a->rec_path);
    notify(a, m);
}

void record_toggle(App *a) {
    if (a->rec_on) record_stop(a);
    else record_start(a, NULL);
}

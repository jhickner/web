#include <stdio.h>
#include <string.h>
#include "web.h"

// Tabs. A tab is a page of the browser and nothing more: the CDP socket
// addresses one page at a time, so switching is moving that socket rather than
// asking anything to come forward. Each one is opened in a window of its own,
// for the reason chrome_attach already gives - Chrome paints only the tab in
// front of a window, and a background tab in a shared window screencasts an
// empty picture.
//
// The bar is one row above the page, drawn by the same rules as the status
// line: one buffered write, and nothing written at all when the row has not
// changed, so a repaint never lands in the middle of a frame going out.

#define SEP  "│"
#define FILL "─"
#define ELL  "…"

// Under this there is no room for a name beside the number, and the bar falls
// back to the numbers alone.
#define TAB_NAMED_MIN 5

// ------------------------------------------------------------------- text

static bool wide_cp(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115f) ||     // hangul jamo
           (cp >= 0x2e80 && cp <= 0xa4cf) ||     // cjk radicals through yi
           (cp >= 0xac00 && cp <= 0xd7a3) ||     // hangul syllables
           (cp >= 0xf900 && cp <= 0xfaff) ||     // cjk compatibility
           (cp >= 0xfe30 && cp <= 0xfe6f) ||
           (cp >= 0xff00 && cp <= 0xff60) ||     // fullwidth forms
           (cp >= 0xffe0 && cp <= 0xffe6) ||
           (cp >= 0x1f300 && cp <= 0x1f64f) ||   // emoji
           (cp >= 0x1f900 && cp <= 0x1f9ff) ||
           (cp >= 0x20000 && cp <= 0x3fffd);
}

// Bytes of the codepoint at s, and the columns it occupies. A byte sequence
// that stops short is taken as the single byte it starts with, so a title cut
// in half somewhere upstream cannot walk this past the end of it.
static size_t utf8_step(const char *s, int *cols) {
    unsigned char c = (unsigned char)s[0];
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xf0)      { cp = c & 0x07u; n = 4; }
    else if (c >= 0xe0) { cp = c & 0x0fu; n = 3; }
    else if (c >= 0xc0) { cp = c & 0x1fu; n = 2; }
    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)s[i] & 0xc0) != 0x80) { n = 1; cp = c; break; }
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3fu);
    }
    *cols = wide_cp(cp) ? 2 : 1;
    return n;
}

static int text_cols(const char *s) {
    int cols = 0;
    while (*s) {
        int w;
        s += utf8_step(s, &w);
        cols += w;
    }
    return cols;
}

// At most `cols` columns of src, cut on a codepoint boundary and closed with an
// ellipsis when anything was left behind. Control bytes are drawn as the space
// they would otherwise make a mess of the row with.
static int fit_cols(char *dst, size_t cap, const char *src, int cols) {
    if (cap < 8 || cols <= 0) { if (cap) dst[0] = 0; return 0; }
    bool cut = text_cols(src) > cols;
    int room = cut ? cols - 1 : cols;
    size_t o = 0;
    int used = 0;
    for (const char *s = src; *s; ) {
        int w;
        size_t n = utf8_step(s, &w);
        if (used + w > room || o + n + 8 >= cap) break;
        if ((unsigned char)*s < 0x20) dst[o++] = ' ';
        else { memcpy(dst + o, s, n); o += n; }
        used += w;
        s += n;
    }
    if (cut) {
        memcpy(dst + o, ELL, sizeof ELL - 1);
        o += sizeof ELL - 1;
        used++;
    }
    dst[o] = 0;
    return used;
}

// What to call a tab: its title, or the host out of its address until one
// arrives. The tab in front is asked of the window rather than of the list,
// because the list only learns where a tab is when the window looks away.
static void tab_label(const App *a, int i, char *out, size_t cap) {
    const Tab *t = &a->tabs[i];
    const char *title = i == a->tab ? a->title : t->title;
    const char *url   = i == a->tab ? a->url   : t->url;
    if (title[0]) { snprintf(out, cap, "%s", title); return; }

    const char *host = url;
    const char *sep = strstr(host, "://");
    if (sep) host = sep + 3;
    size_t n = strcspn(host, "/?#");
    if (!n || !strncmp(url, "about:", 6)) { snprintf(out, cap, "new tab"); return; }
    if (n >= cap) n = cap - 1;
    memcpy(out, host, n);
    out[n] = 0;
}

// ------------------------------------------------------------------- list

void tabs_init(App *a) {
    memset(a->tabs, 0, sizeof a->tabs);
    a->ntabs = 1;
    a->tab = 0;
    snprintf(a->tabs[0].target, sizeof a->tabs[0].target, "%s", a->chrome.target);
    snprintf(a->tabs[0].url, sizeof a->tabs[0].url, "%s", a->url);
    // A browser somebody else is driving keeps the page it is on: we are
    // looking at it, not moving into it, and it is not ours to close.
    a->tabs[0].ours = !a->chrome.foreign;
}

void tabs_free(App *a) {
    buf_free(&a->tabs_buf);
    buf_free(&a->tabs_last);
}

bool tabs_wanted(const App *a) { return a->ntabs > 1; }

// Where the window is now, kept against the tab it is on, so the bar can still
// name it once the socket has moved somewhere else.
static void tab_remember(App *a) {
    if (a->tab < 0 || a->tab >= a->ntabs) return;
    Tab *t = &a->tabs[a->tab];
    snprintf(t->url, sizeof t->url, "%s", a->url);
    snprintf(t->title, sizeof t->title, "%s", a->title);
}

static void tab_drop(App *a, int idx) {
    for (int i = idx; i + 1 < a->ntabs; i++) a->tabs[i] = a->tabs[i + 1];
    a->ntabs--;
    memset(&a->tabs[a->ntabs], 0, sizeof a->tabs[a->ntabs]);
    if (a->tab > idx) a->tab--;
}

// Move the socket onto another tab. Everything the window knows about a page
// belongs to the page it knew it about, so all of it is put back from the list
// and the rest is asked again from the browser.
static bool tab_show(App *a, int idx) {
    if (idx < 0 || idx >= a->ntabs || idx == a->tab) return true;
    tab_remember(a);
    // Nothing is going to draw the old page, and a screencast left running is
    // work the browser goes on doing for a picture nobody will collect.
    app_cdp(a, "Page.stopScreencast", "");

    if (chrome_switch_target(&a->chrome, a->tabs[idx].target) < 0) {
        notify(a, "that tab has gone");
        return false;
    }
    a->tab = idx;

    // The new session numbers its replies from one, so anything still
    // outstanding on the old one would be claimed by an unrelated reply.
    memset(a->reqs, 0, sizeof a->reqs);
    a->pend.kind = PEND_NONE;
    a->loading = false;
    a->insert = false;
    a->mouse_down = false;
    a->pdf = a->pdf_clicked = false;
    a->fit_w = 0;
    a->last_hash = 0;
    a->kitty.grid_dirty = true;
    snprintf(a->url, sizeof a->url, "%s", a->tabs[idx].url);
    snprintf(a->title, sizeof a->title, "%s", a->tabs[idx].title);

    session_init(a);
    ask_where(a);
    relayout(a);
    session_write(a);
    // A page that comes up with nothing to say leaves the tab we switched away
    // from still on screen, so the watchdog is told a frame is owed.
    a->expect_frame = now_sec() + 2.0;
    return true;
}

void tab_go(App *a, int idx) {
    if (idx < 0 || idx >= a->ntabs) return;
    tab_show(a, idx);
}

void tab_step(App *a, int delta) {
    if (a->ntabs < 2) return;
    int idx = (a->tab + delta) % a->ntabs;
    if (idx < 0) idx += a->ntabs;
    tab_show(a, idx);
}

void tab_new(App *a) {
    if (a->ntabs >= TAB_MAX) {
        notify(a, "that is as many tabs as the bar can name");
        return;
    }
    char target[96];
    if (chrome_open_tab(&a->chrome, target, sizeof target) < 0) {
        notify(a, "chrome would not open another page");
        return;
    }
    tab_remember(a);
    Tab *t = &a->tabs[a->ntabs];
    memset(t, 0, sizeof *t);
    snprintf(t->target, sizeof t->target, "%s", target);
    snprintf(t->url, sizeof t->url, "about:blank");
    t->ours = true;
    a->ntabs++;
    if (!tab_show(a, a->ntabs - 1)) {         // it never came up; forget it
        chrome_close_id(&a->chrome, target);
        a->ntabs--;
        return;
    }
    // A blank page is not somewhere to be, so the tab opens with the address
    // bar already waiting - the same key that would have been pressed next.
    a->editing = true;
    a->prompt = 1;
    a->edit_len = 0;
}

static void tab_close_at(App *a, int idx) {
    if (idx < 0 || idx >= a->ntabs) return;
    // The last tab closing is the window closing, which is what closing the
    // last tab means everywhere else.
    if (a->ntabs <= 1) { g_quit = 1; return; }

    char target[96];
    snprintf(target, sizeof target, "%s", a->tabs[idx].target);
    bool ours = a->tabs[idx].ours;
    if (idx == a->tab) {
        int next = idx + 1 < a->ntabs ? idx + 1 : idx - 1;
        if (!tab_show(a, next)) return;    // nowhere to go: leave it where it is
    }
    // A page we only attached to is not ours to close; it drops out of the bar
    // and goes on being whatever it was.
    if (ours) chrome_close_id(&a->chrome, target);
    tab_drop(a, idx);
}

void tab_close(App *a) { tab_close_at(a, a->tab); }

bool tab_lost(App *a) {
    if (a->ntabs <= 1) return false;
    int idx = a->tab;
    int next = idx + 1 < a->ntabs ? idx + 1 : idx - 1;
    if (!tab_show(a, next)) return false;
    tab_drop(a, idx);
    notify(a, "that page closed");
    return true;
}

void tabs_close_others(App *a) {
    for (int i = 0; i < a->ntabs; i++)
        if (i != a->tab && a->tabs[i].ours)
            chrome_close_id(&a->chrome, a->tabs[i].target);
}

bool tab_session_new(App *a) {
    if (a->tab < 0 || a->tab >= a->ntabs) return true;
    if (a->tabs[a->tab].inited) return false;
    a->tabs[a->tab].inited = true;
    return true;
}

// ------------------------------------------------------------------ paint

void tabs_paint(App *a) {
    if (!a->has_tty || !a->tabs_open || a->tabs_row < 1 || a->ntabs < 1) return;
    Term *t = &a->term;

    // The bar belongs to the window under it, not to the terminal: inline, the
    // box is narrower than the screen, and a bar running past its edge reads as
    // part of the shell rather than as part of the page.
    int sx = a->kitty.x > 0 ? a->kitty.x : 1;
    int sw = a->kitty.cols > 0 ? a->kitty.cols : t->cols;
    if (sx + sw - 1 > t->cols) sw = t->cols - sx + 1;
    if (sw < 8) sw = 8;

    int n = a->ntabs;
    // What each tab gets between its separator and the next one, with one
    // column left over for the separator that closes the row.
    int field = (sw - 1) / n - 1;
    bool named = field >= TAB_NAMED_MIN;
    if (!named) field = 1;

    Buf b = a->tabs_buf;
    b.len = 0;
    buf_addf(&b, "\x1b[%d;1H\x1b[2K\x1b[%d;%dH\x1b[0m",
             a->tabs_row, a->tabs_row, sx);

    int col = 0;
    for (int i = 0; i < n; i++) {
        Tab *tb = &a->tabs[i];
        tb->x0 = tb->x1 = 0;
        if (col + 1 + field > sw - 1) break;   // no room for this one and the end
        buf_addf(&b, "\x1b[0m\x1b[2m%s\x1b[0m", SEP);
        tb->x0 = sx + col;                     // the separator is part of the target
        col++;

        buf_add(&b, i == a->tab ? "\x1b[7m" : "\x1b[2m", 4);
        if (named) {
            char name[512], full[600], shown[600];
            tab_label(a, i, name, sizeof name);
            snprintf(full, sizeof full, "%d %s", i + 1, name);
            int used = fit_cols(shown, sizeof shown, full, field - 2);
            buf_addf(&b, " %s", shown);
            for (int p = used; p < field - 2; p++) buf_add(&b, " ", 1);
            buf_add(&b, " ", 1);
        } else {
            buf_addf(&b, "%d", i + 1);
        }
        buf_add(&b, "\x1b[0m", 4);
        col += field;
        tb->x1 = sx + col - 1;
    }

    // The row closes with a separator and runs on as a rule to the window's
    // edge, so the tabs read as sitting on something rather than floating.
    buf_addf(&b, "\x1b[2m%s", SEP);
    for (col++; col < sw; col++) buf_add(&b, FILL, sizeof FILL - 1);
    buf_add(&b, "\x1b[0m", 4);

    a->tabs_buf = b;
    if (b.len == a->tabs_last.len &&
        (b.len == 0 || memcmp(b.p, a->tabs_last.p, b.len) == 0))
        return;
    writeall(t->fd, b.p, b.len);
    a->tabs_last.len = 0;
    buf_add(&a->tabs_last, b.p, b.len);
}

// ------------------------------------------------------------------ input

bool tabs_mouse(App *a, Event *ev) {
    if (!a->tabs_open || ev->type != EV_MOUSE || ev->my != a->tabs_row)
        return false;
    // The whole row is the bar's, whatever the click lands on in it: a press
    // that reached the page from here would be a click at the top of it.
    if (!ev->press || ev->motion) return true;

    if (ev->button == 3 || ev->button == 4) {
        tab_step(a, ev->button == 3 ? -1 : +1);
        return true;
    }
    for (int i = 0; i < a->ntabs; i++) {
        Tab *t = &a->tabs[i];
        if (!t->x0 || ev->mx < t->x0 || ev->mx > t->x1) continue;
        if (ev->button == 1) tab_close_at(a, i);   // middle, as in any browser
        else if (ev->button == 0) tab_go(a, i);
        return true;
    }
    return true;
}

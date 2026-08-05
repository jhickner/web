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

// The widths at which the bar gives something up. With the number, a space
// either side and an ellipsis all taken out, a field of eight is the first that
// leaves enough letters of a title to tell it from another - below that the
// number is worth more than three dots. Narrower still, the number goes too and
// the title has the field to itself; narrower again, only the number fits.
#define TAB_NUMBERED_MIN 8
#define TAB_NAMED_MIN    5

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

// One column of a label, for a bar with nothing else left to give. The first
// character that a single cell can actually hold, so a title starting with
// something double-width is skipped past rather than drawn over its neighbour.
// A label with no such character at all - all CJK, say - falls back to the
// number's last digit, which at least differs from the tab beside it.
static void first_col(const char *src, int number, char *dst, size_t cap) {
    for (const char *s = src; *s; ) {
        int w;
        size_t k = utf8_step(s, &w);
        if (w == 1 && *s > ' ' && k + 1 <= cap) {
            memcpy(dst, s, k);
            dst[k] = 0;
            return;
        }
        s += k;
    }
    snprintf(dst, cap, "%d", number % 10);
}

// What to call a tab: its title, or the host out of its address until one
// arrives. The tab in front is asked of the window rather than of the list,
// because the list only learns where a tab is when the window looks away.
void tab_label(const App *a, int i, char *out, size_t cap) {
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
    for (int i = 0; i < TAB_MAX; i++) buf_free(&a->tabs[i].shot);
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
    // The picture kept for its tile goes with it: the shuffle below overwrites
    // the pointer, and nothing else knows where that buffer was.
    buf_free(&a->tabs[idx].shot);
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
    // Including a still: its id went with the table, and the page it was a
    // picture of is not the one being switched to.
    still_cancel(a);
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
    // from still on screen, so the watchdog is told a frame is owed - and the
    // picture is asked for outright rather than only waited for.
    a->expect_frame = now_sec() + 2.0;
    still_soon(a);
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

// A blank page of the browser, on the end of the list and with the window moved
// onto it. False leaves the list exactly as it was, and has already said why.
static bool tab_add(App *a) {
    // Not about the bar, which copes with whatever it is given: this is the
    // list itself running out, and every entry is a page of the browser.
    if (a->ntabs >= TAB_MAX) {
        char m[48];
        snprintf(m, sizeof m, "%d tabs is the limit", TAB_MAX);
        notify(a, m);
        return false;
    }
    char target[96];
    if (chrome_open_tab(&a->chrome, target, sizeof target) < 0) {
        notify(a, "chrome would not open another page");
        return false;
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
        return false;
    }
    return true;
}

// A page somebody else made, taken into the bar without the window moving onto
// it. A test runner working this browser opens a page per worker: each one is a
// page of its own, and one tile of the grid is where it is watched. Not ours,
// so whoever opened it is who closes it.
int tab_index_of(const App *a, const char *target) {
    if (!target || !*target) return -1;
    for (int i = 0; i < a->ntabs; i++)
        if (!strcmp(a->tabs[i].target, target)) return i;
    return -1;
}

bool tab_adopt(App *a, const char *target, const char *url) {
    if (!target || !*target || a->ntabs >= TAB_MAX) return false;
    for (int i = 0; i < a->ntabs; i++)
        if (!strcmp(a->tabs[i].target, target)) return false;   // already have it
    Tab *t = &a->tabs[a->ntabs];
    memset(t, 0, sizeof *t);
    snprintf(t->target, sizeof t->target, "%s", target);
    snprintf(t->url, sizeof t->url, "%s", url && *url ? url : "about:blank");
    t->ours = false;
    t->claimed = true;
    a->ntabs++;
    return true;
}

// One of those pages has gone. The tab in front is not this one's business:
// its socket closing is what says it went, and tab_lost answers that.
bool tab_forget(App *a, const char *target) {
    if (!target || !*target) return false;
    for (int i = 0; i < a->ntabs; i++) {
        if (i == a->tab || strcmp(a->tabs[i].target, target)) continue;
        tab_drop(a, i);
        return true;
    }
    return false;
}

void tab_new(App *a) {
    if (!tab_add(a)) return;
    // A blank page is not somewhere to be, so the tab opens with the address
    // bar already waiting - the same key that would have been pressed next.
    a->editing = true;
    a->prompt = 1;
    a->edit_len = 0;
}

bool tab_open_url(App *a, const char *url) {
    if (!tab_add(a)) return false;
    // Named before the load has said anything, for the reason tab_from_popup
    // gives: a tab called "new tab" for the length of a fetch is a tab that
    // looks like it was opened by mistake.
    snprintf(a->url, sizeof a->url, "%s", url);
    snprintf(a->tabs[a->tab].url, sizeof a->tabs[a->tab].url, "%s", url);
    navigate(a, url);
    return true;
}

// A page the browser opened on its own account, taken over. It is where a click
// was aimed, so the window moves onto it the way it would have moved onto a
// window opening anywhere else.
//
// Its address is taken rather than the page itself, because of the window a
// popup is put in: Chrome opens it inside the window of the page that asked, and
// paints only the tab in front of a window, so screencasting it gives a title
// and no picture under it. A tab of ours comes with a window of its own, which
// is the whole of what makes it drawable. What that costs is the opener - a
// popup that talks back to the page that made it now finds nobody there - and
// what it buys is a page that can be seen at all.
bool tab_from_popup(App *a, const char *target, const char *url) {
    if (!tab_add(a)) return false;
    // Closed only now: until the tab is up this is still the only copy of the
    // page, and a tab that never came up would have left nothing behind.
    chrome_close_id(&a->chrome, target);
    // Where it is going, said before it has got there: the page is still blank,
    // and a tab named "new tab" for the length of a load is a tab that looks
    // like it was opened by mistake.
    snprintf(a->url, sizeof a->url, "%s", url);
    snprintf(a->tabs[a->tab].url, sizeof a->tabs[a->tab].url, "%s", url);
    navigate(a, url);
    notify(a, "that page opened a window - it is this tab");
    return true;
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

    // What each tab gets, in the order the room runs out. First the width
    // between its own separator and the next one, with a column left over for
    // the separator closing the row. Under one column apiece the separators are
    // what goes: a tab one letter wide is still a tab, and a row of separators
    // is not. Nothing is ever dropped for being too narrow - a tab that cannot
    // be named can still be pointed at, and one that has quietly vanished
    // cannot.
    int field = (sw - 1) / n - 1;
    bool sep = field >= 1;
    if (!sep) field = sw / n;
    if (field < 1) field = 1;

    // Only when there are more tabs than the row has columns for, which takes
    // both a crowded bar and a narrow window. The ones around the tab in front
    // are shown rather than the first few: the tab you are on is the one that
    // has to be on the screen.
    int step = field + (sep ? 1 : 0);
    int fits = (sep ? sw - 1 : sw) / step;
    if (fits < 1) fits = 1;
    int first = 0, shown_n = n;
    if (fits < n) {
        shown_n = fits;
        first = a->tab - fits / 2;
        if (first < 0) first = 0;
        if (first > n - fits) first = n - fits;
    }

    Buf b = a->tabs_buf;
    b.len = 0;
    buf_addf(&b, "\x1b[%d;1H\x1b[2K\x1b[%d;%dH\x1b[0m",
             a->tabs_row, a->tabs_row, sx);

    // A tab that is not on the bar is not clickable either, and a stale cell
    // range would hand its clicks to whoever is standing there now.
    for (int i = 0; i < n; i++) a->tabs[i].x0 = a->tabs[i].x1 = 0;

    int col = 0;
    for (int i = first; i < first + shown_n; i++) {
        Tab *tb = &a->tabs[i];
        tb->x0 = sx + col;               // the separator is part of the target
        if (sep) {
            buf_addf(&b, "\x1b[0m\x1b[2m%s\x1b[0m", SEP);
            col++;
        }

        buf_add(&b, i == a->tab ? "\x1b[7m" : "\x1b[2m", 4);
        char name[512], shown[600];
        if (field >= TAB_NAMED_MIN) {
            // Room for a name, with a space either side. The number leads it
            // where there is room for both; where there is not, the title has
            // the field to itself and position is what says which tab it is.
            char full[600];
            tab_label(a, i, name, sizeof name);
            if (field >= TAB_NUMBERED_MIN) {
                char titled[600];
                snprintf(titled, sizeof titled, "%s", name);
                snprintf(full, sizeof full, "%d %s", i + 1, titled);
            } else {
                snprintf(full, sizeof full, "%s", name);
            }
            int used = fit_cols(shown, sizeof shown, full, field - 2);
            buf_addf(&b, " %s", shown);
            for (int p = used; p < field - 2; p++) buf_add(&b, " ", 1);
            buf_add(&b, " ", 1);
        } else if (field >= 2) {
            // Room for the number alone, which is still the thing you would
            // reach for: it is what alt+<n> takes.
            char num[16];
            snprintf(num, sizeof num, "%d", i + 1);
            int used = fit_cols(shown, sizeof shown, num, field);
            buf_addf(&b, "%s", shown);
            for (int p = used; p < field; p++) buf_add(&b, " ", 1);
        } else if (i < 9) {
            // One column, and a digit that means something: alt+<n> reaches
            // the first nine, so for those the number is worth more than a
            // letter of a title nobody can read anyway.
            buf_addf(&b, "%d", i + 1);
        } else {
            // Past those, a letter of its own name tells the tabs apart.
            tab_label(a, i, name, sizeof name);
            first_col(name, i + 1, shown, sizeof shown);
            buf_addf(&b, "%s", shown);
        }
        buf_add(&b, "\x1b[0m", 4);
        col += field;
        tb->x1 = sx + col - 1;
    }

    // The row closes with a separator and runs on as a rule to the window's
    // edge, so the tabs read as sitting on something rather than floating.
    if (sep) {
        buf_addf(&b, "\x1b[2m%s", SEP);
        col++;
    } else {
        buf_add(&b, "\x1b[2m", 4);
    }
    for (; col < sw; col++) buf_add(&b, FILL, sizeof FILL - 1);
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

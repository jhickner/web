#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "web.h"

// ~/.config/web/web.conf: the settings, and what every key does. The defaults
// are compiled in and the file is read over the top of them, so a line the
// file does not carry is the one it has always been, and the file is never
// written back - what it says is what the user said, and nothing here has an
// opinion it would want to save. Which is also why what a key changes while
// the window is up - the zoom, the size of the window - is only ever read out
// of here: it says where a new window opens, and the dozen that may be running
// each go their own way from there without any of it coming back.
//
// Nothing else in the program knows a key: main.c asks this what an event
// means and acts on the answer, and help.c asks it which key an action wears
// so the list it draws is the real one.

// A binding is one key or two: `key2` is 0 for the ones that are one, and the
// pair `g g` is a first key that does nothing on its own and a second that
// decides what the two of them were.
typedef struct { int mods, key, mods2, key2; Act act; } KeyBind;

// The tables below, where a pair costs two more numbers on the line and a
// single key should not have to carry them.
typedef struct { int mods, key; Act act; } KeyDef;
typedef struct { int mods, key, mods2, key2; Act act; } KeyPair;

// One entry per key, so a file line naming a key already bound replaces it
// rather than adding a second meaning. Two keys may share an action.
#define BINDS_MAX 192

static KeyBind g_binds[BINDS_MAX];
static int     g_nbinds;

// What the vim layer bound, so a file line landing on one of them can be
// counted: the file is read last and wins, and a vim map that quietly did
// nothing because web.conf already named `L` is worth a word on the status
// line rather than a puzzle.
static KeyBind g_vim[48];
static int     g_nvim;

// ------------------------------------------------------------------ names

// The order is the order the help list and the generated file walk, so it is
// also the order a reader meets the actions in.
static const struct { const char *name; Act act; const char *section; } ACTS[] = {
    {"scroll-down",    ACT_SCROLL_DOWN, "moving"},
    {"scroll-up",      ACT_SCROLL_UP, NULL},
    {"scroll-left",    ACT_SCROLL_LEFT, NULL},
    {"scroll-right",   ACT_SCROLL_RIGHT, NULL},
    {"line-down",      ACT_LINE_DOWN, NULL},
    {"line-up",        ACT_LINE_UP, NULL},
    {"half-down",      ACT_HALF_DOWN, NULL},
    {"half-up",        ACT_HALF_UP, NULL},
    {"page-down",      ACT_PAGE_DOWN, NULL},
    {"page-up",        ACT_PAGE_UP, NULL},
    {"top",            ACT_TOP, NULL},
    {"bottom",         ACT_BOTTOM, NULL},

    {"address",        ACT_ADDRESS, "the page"},
    {"address-blank",  ACT_ADDRESS_BLANK, NULL},
    {"address-tab",    ACT_ADDRESS_TAB, NULL},
    {"back",           ACT_BACK, NULL},
    {"forward",        ACT_FORWARD, NULL},
    {"reload",         ACT_RELOAD, NULL},
    {"reload-hard",    ACT_RELOAD_HARD, NULL},
    {"copy",           ACT_COPY, NULL},
    {"copy-url",       ACT_COPY_URL, NULL},
    {"copy-console",   ACT_COPY_CONSOLE, NULL},
    {"resume",         ACT_RESUME, NULL},
    {"grid",           ACT_GRID, NULL},
    {"record",         ACT_RECORD, NULL},
    {"external",       ACT_EXTERNAL, NULL},
    {"find",           ACT_FIND, NULL},
    {"find-next",      ACT_FIND_NEXT, NULL},
    {"find-prev",      ACT_FIND_PREV, NULL},
    {"hint",           ACT_HINT, "the links"},
    {"hint-tab",       ACT_HINT_TAB, NULL},
    {"hint-copy",      ACT_HINT_COPY, NULL},
    {"hint-all",       ACT_HINT_ALL, NULL},
    {"insert",         ACT_INSERT, NULL},
    {"insert-off",     ACT_INSERT_OFF, NULL},
    {"focus-input",    ACT_FOCUS_INPUT, NULL},
    {"pick-selector",  ACT_PICK, NULL},
    {"bookmark",       ACT_BOOKMARK, NULL},

    {"search-tabs",    ACT_SEARCH_TABS, "tabs"},
    {"search-history", ACT_SEARCH_HISTORY, NULL},
    {"search-bookmarks", ACT_SEARCH_BOOKMARKS, NULL},
    {"tab-new",        ACT_TAB_NEW, NULL},
    {"tab-close",      ACT_TAB_CLOSE, NULL},
    {"tab-next",       ACT_TAB_NEXT, NULL},
    {"tab-prev",       ACT_TAB_PREV, NULL},
    {"merge",          ACT_MERGE, NULL},
    {"tab-1",          ACT_TAB_1, NULL},
    {"tab-2",          ACT_TAB_2, NULL},
    {"tab-3",          ACT_TAB_3, NULL},
    {"tab-4",          ACT_TAB_4, NULL},
    {"tab-5",          ACT_TAB_5, NULL},
    {"tab-6",          ACT_TAB_6, NULL},
    {"tab-7",          ACT_TAB_7, NULL},
    {"tab-8",          ACT_TAB_8, NULL},
    {"tab-9",          ACT_TAB_9, NULL},

    {"zoom-in",        ACT_ZOOM_IN, "the window"},
    {"zoom-out",       ACT_ZOOM_OUT, NULL},
    {"zoom-reset",     ACT_ZOOM_RESET, NULL},
    {"smaller",        ACT_SMALLER, NULL},
    {"larger",         ACT_LARGER, NULL},
    {"fit-width",      ACT_FIT, NULL},
    {"page-wider",     ACT_PAGE_WIDER, NULL},
    {"page-narrower",  ACT_PAGE_NARROWER, NULL},
    {"scale",          ACT_SCALE, NULL},
    {"box-taller",     ACT_BOX_TALLER, NULL},
    {"box-shorter",    ACT_BOX_SHORTER, NULL},
    {"box-wider",      ACT_BOX_WIDER, NULL},
    {"box-narrower",   ACT_BOX_NARROWER, NULL},

    {"console",        ACT_CONSOLE, "the rest"},
    {"help",           ACT_HELP, NULL},
    {"stats",          ACT_STATS, NULL},
    {"status-line",    ACT_STATUS, NULL},
    {"trace",          ACT_TRACE, NULL},
    {"quit",           ACT_QUIT, NULL},
};
#define NACTS ((int)(sizeof ACTS / sizeof ACTS[0]))

// The first spelling of each is the one written back out and drawn in the help
// list; the rest are there so a file may say what its writer would have said.
static const struct { const char *name; int key; } KEYNAMES[] = {
    {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"up", KEY_UP}, {"down", KEY_DOWN},
    {"space", ' '}, {"esc", KEY_ESC}, {"escape", KEY_ESC},
    {"enter", KEY_ENTER}, {"return", KEY_ENTER}, {"tab", KEY_TAB},
    {"backspace", KEY_BACKSPACE}, {"bs", KEY_BACKSPACE},
    {"delete", KEY_DELETE}, {"del", KEY_DELETE},
    {"home", KEY_HOME}, {"end", KEY_END},
    {"pgup", KEY_PGUP}, {"pageup", KEY_PGUP},
    {"pgdn", KEY_PGDN}, {"pagedown", KEY_PGDN},
    {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4},
    {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8},
    {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
};
#define NKEYNAMES ((int)(sizeof KEYNAMES / sizeof KEYNAMES[0]))

static const struct { const char *name; int mod; } MODS[] = {
    {"ctrl", MOD_CTRL}, {"control", MOD_CTRL},
    {"alt", MOD_ALT}, {"meta", MOD_ALT}, {"opt", MOD_ALT}, {"option", MOD_ALT},
    {"shift", MOD_SHIFT},
    {"cmd", MOD_SUPER}, {"super", MOD_SUPER}, {"win", MOD_SUPER},
};
#define NMODS ((int)(sizeof MODS / sizeof MODS[0]))

const char *act_name(Act act) {
    for (int i = 0; i < NACTS; i++)
        if (ACTS[i].act == act) return ACTS[i].name;
    return "none";
}

static Act act_by_name(const char *s) {
    if (!strcmp(s, "none")) return ACT_NONE;
    for (int i = 0; i < NACTS; i++)
        if (!strcmp(ACTS[i].name, s)) return ACTS[i].act;
    return ACT_INVALID;
}

// ---------------------------------------------------------------- matching

// Shift is spelled by the character itself wherever there is a character to
// spell it with, so `G` and `shift+g` have to end up the same pair however
// either end reported them. Control is the other way round: a terminal sends ^Y
// as `y` with a flag, so the letter is folded down rather than up.
static void normalize(int *mods, int *key) {
    int m = *mods, k = *key;
    if (k > 0 && k < 128) {
        if (m & MOD_CTRL) {
            k = tolower(k);
            m &= ~MOD_SHIFT;
        } else if ((m & MOD_SHIFT) && isalpha(k)) {
            k = toupper(k);
            m &= ~MOD_SHIFT;
        }
    }
    *mods = m;
    *key = k;
}

static int find_bind(int mods, int key, int mods2, int key2) {
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].mods == mods && g_binds[i].key == key &&
            g_binds[i].mods2 == mods2 && g_binds[i].key2 == key2) return i;
    return -1;
}

// A terminal that reports the unshifted key sends `?` as `/` with shift held,
// and `:` as `;`. The shifted spelling is tried first, so a binding made on one
// may still differ from the binding made on the other.
static int find_either(int mods, int key, int mods2, int key2) {
    int i = find_bind(mods, key, mods2, key2);
    if (i < 0 && (mods & MOD_SHIFT) && key < 0x100)
        i = find_bind(mods & ~MOD_SHIFT, key, mods2, key2);
    return i;
}

// Whether anything longer starts here. Only asked of a key that is not a
// binding on its own, so a key that is both is simply that binding.
static bool starts_pair(int mods, int key) {
    for (int i = 0; i < g_nbinds; i++) {
        if (!g_binds[i].key2) continue;
        if (g_binds[i].key != key) continue;
        if (g_binds[i].mods == mods ||
            ((mods & MOD_SHIFT) && g_binds[i].mods == (mods & ~MOD_SHIFT)))
            return true;
    }
    return false;
}

Act keys_lookup(int mods, int key) {
    return keys_lookup_seq(0, 0, mods, key, NULL);
}

Act keys_lookup_seq(int pmods, int pkey, int mods, int key, bool *prefix) {
    if (prefix) *prefix = false;
    normalize(&mods, &key);
    if (pkey) {
        normalize(&pmods, &pkey);
        int i = find_either(pmods, pkey, mods, key);
        if (i >= 0) return g_binds[i].act;
        // The two of them are not a binding, so the first is dropped and this
        // key is asked about on its own - which is what it would have meant
        // had the other never been pressed.
    }
    int i = find_either(mods, key, 0, 0);
    if (i >= 0) return g_binds[i].act;
    if (prefix) *prefix = starts_pair(mods, key);
    return ACT_NONE;
}

static void bind_set(int mods, int key, int mods2, int key2, Act act) {
    normalize(&mods, &key);
    if (key2) normalize(&mods2, &key2);
    else      mods2 = 0;
    int i = find_bind(mods, key, mods2, key2);
    if (act == ACT_NONE) {
        if (i >= 0) g_binds[i] = g_binds[--g_nbinds];
        return;
    }
    if (i < 0) {
        if (g_nbinds >= BINDS_MAX) return;
        i = g_nbinds++;
        g_binds[i].mods = mods;
        g_binds[i].key = key;
        g_binds[i].mods2 = mods2;
        g_binds[i].key2 = key2;
    }
    g_binds[i].act = act;
}

// ---------------------------------------------------------------- spelling

// The left of a line: one key, or the two of a pair. Two are written with a
// space between them - `^X f` - or, where both are a bare character, run
// together the way they are pressed and said: `gg`, `yf`.
static bool parse_seq(char *s, int *m1, int *k1, int *m2, int *k2);

static bool parse_spec(const char *s, int *mods, int *key) {
    *mods = 0;
    *key = 0;
    if (s[0] == '^' && s[1]) {          // ^Y, the way the docs write it
        *mods |= MOD_CTRL;
        s++;
    }
    // A modifier is only a modifier when a separator and something to modify
    // follow it, which is what leaves `alt+-` and `alt++` meaning a key.
    for (bool again = true; again;) {
        again = false;
        for (int i = 0; i < NMODS && !again; i++) {
            size_t n = strlen(MODS[i].name);
            if (strncasecmp(s, MODS[i].name, n)) continue;
            if ((s[n] != '+' && s[n] != '-') || !s[n + 1]) continue;
            *mods |= MODS[i].mod;
            s += n + 1;
            again = true;
        }
    }
    if (!*s) return false;
    for (int i = 0; i < NKEYNAMES; i++)
        if (!strcasecmp(s, KEYNAMES[i].name)) { *key = KEYNAMES[i].key; return true; }
    // Anything left has to be the one character it stands for. Multibyte is
    // decoded far enough to be the codepoint the terminal will report.
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        if (s[1]) return false;
        *key = c;
    } else {
        int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if ((int)strlen(s) != n) return false;
        int k = c & (0xFF >> (n + 1));
        for (int i = 1; i < n; i++) k = (k << 6) | (s[i] & 0x3F);
        *key = k;
    }
    return true;
}

static bool parse_seq(char *s, int *m1, int *k1, int *m2, int *k2) {
    *m2 = *k2 = 0;
    char *sp = s;
    while (*sp && !isspace((unsigned char)*sp)) sp++;
    if (*sp) {
        *sp++ = 0;
        while (isspace((unsigned char)*sp)) sp++;
        if (!*sp || !parse_spec(sp, m2, k2)) return false;
    }
    if (parse_spec(s, m1, k1)) return true;
    if (*k2) return false;              // the first of a pair has to be a key
    if (s[0] && s[1] && !s[2] &&
        (unsigned char)s[0] >= 0x20 && (unsigned char)s[0] < 0x7f &&
        (unsigned char)s[1] >= 0x20 && (unsigned char)s[1] < 0x7f) {
        *m1 = 0;
        *k1 = (unsigned char)s[0];
        *m2 = 0;
        *k2 = (unsigned char)s[1];
        return true;
    }
    return false;
}

void key_text(int mods, int key, char *out, size_t cap) {
    normalize(&mods, &key);
    // ^Y rather than ctrl+y, since that is what a terminal's own documentation
    // calls it and what this program's did before the keys could be moved.
    if (mods == MOD_CTRL && key < 128 && isalpha(key)) {
        snprintf(out, cap, "^%c", toupper(key));
        return;
    }
    char name[16];
    name[0] = 0;
    for (int i = 0; i < NKEYNAMES; i++)
        if (KEYNAMES[i].key == key) { snprintf(name, sizeof name, "%s", KEYNAMES[i].name); break; }
    if (!name[0]) {
        if (key < 0x80) {
            snprintf(name, sizeof name, "%c", key);
        } else {                        // back to utf-8, for the list to draw
            int n = key < 0x800 ? 2 : key < 0x10000 ? 3 : 4;
            static const unsigned char lead[5] = {0, 0, 0xC0, 0xE0, 0xF0};
            for (int i = n - 1; i > 0; i--) { name[i] = (char)(0x80 | (key & 0x3F)); key >>= 6; }
            name[0] = (char)(lead[n] | key);
            name[n] = 0;
        }
    }
    snprintf(out, cap, "%s%s%s%s%s",
             mods & MOD_SUPER ? "cmd+" : "", mods & MOD_CTRL ? "ctrl+" : "",
             mods & MOD_ALT ? "alt+" : "", mods & MOD_SHIFT ? "shift+" : "", name);
}

// A whole binding, one key or two. A pair of bare characters is written the way
// it is pressed and said - `gg`, `yf` - and anything with a modifier or a named
// key in it takes the space it needs to be read back: `^X f`.
static void bind_text(const KeyBind *b, char *out, size_t cap) {
    char one[48];
    key_text(b->mods, b->key, one, sizeof one);
    if (!b->key2) {
        snprintf(out, cap, "%s", one);
        return;
    }
    char two[48];
    key_text(b->mods2, b->key2, two, sizeof two);
    if (strlen(one) == 1 && strlen(two) == 1) snprintf(out, cap, "%s%s", one, two);
    else                                      snprintf(out, cap, "%s %s", one, two);
}

// The shortest of the keys that do it, which is both the one worth learning
// and the one that keeps the help list narrow enough to hold two columns: `?`
// rather than `shift+/`, `D` rather than `shift+down`.
bool keys_text(Act act, char *out, size_t cap) {
    char best[48] = "";
    for (int i = 0; i < g_nbinds; i++) {
        if (g_binds[i].act != act) continue;
        char one[48];
        bind_text(&g_binds[i], one, sizeof one);
        if (!best[0] || strlen(one) < strlen(best)) memcpy(best, one, sizeof one);
    }
    snprintf(out, cap, "%s", best[0] ? best : "-");
    return best[0] != 0;
}

// ---------------------------------------------------------------- defaults

static const KeyDef DEFAULTS[] = {
    {MOD_CTRL, 'q', ACT_QUIT},
    {MOD_CTRL, 'c', ACT_QUIT},
    {MOD_CTRL, 'l', ACT_ADDRESS},
    {MOD_CTRL, 'o', ACT_BACK},
    {MOD_CTRL, 'p', ACT_FORWARD},
    {MOD_CTRL, 'r', ACT_RELOAD},
    {MOD_CTRL, 'y', ACT_COPY},
    {MOD_CTRL, 'e', ACT_EXTERNAL},
    {MOD_CTRL, 'x', ACT_CONSOLE},
    {MOD_CTRL, 'g', ACT_STATS},
    {MOD_CTRL, 's', ACT_STATUS},
    {MOD_CTRL, 'd', ACT_TRACE},
    {MOD_CTRL, 't', ACT_TAB_NEW},
    {MOD_CTRL, 'w', ACT_TAB_CLOSE},
    {MOD_CTRL, 'n', ACT_TAB_NEXT},
    {MOD_CTRL, 'b', ACT_TAB_PREV},
    {MOD_SUPER, 'c', ACT_COPY},
    // alt rather than ctrl: the console's own editor uses most of the control
    // keys, and this one has to work from inside it.
    {MOD_ALT, 'y', ACT_COPY_CONSOLE},
    {MOD_ALT, KEY_ENTER, ACT_RESUME},
    {MOD_ALT, 'g', ACT_GRID},
    {MOD_ALT, 'r', ACT_RECORD},
    {MOD_ALT, 'd', ACT_BOOKMARK},
    {MOD_ALT, 'b', ACT_SEARCH_BOOKMARKS},
    {MOD_ALT, 'm', ACT_MERGE},

    {MOD_ALT | MOD_SHIFT, KEY_RIGHT, ACT_TAB_NEXT},
    {MOD_ALT | MOD_SHIFT, KEY_LEFT, ACT_TAB_PREV},
    {MOD_ALT, '1', ACT_TAB_1},
    {MOD_ALT, '2', ACT_TAB_2},
    {MOD_ALT, '3', ACT_TAB_3},
    {MOD_ALT, '4', ACT_TAB_4},
    {MOD_ALT, '5', ACT_TAB_5},
    {MOD_ALT, '6', ACT_TAB_6},
    {MOD_ALT, '7', ACT_TAB_7},
    {MOD_ALT, '8', ACT_TAB_8},
    {MOD_ALT, '9', ACT_TAB_9},
    {MOD_ALT, 'f', ACT_FIT},
    {MOD_ALT, '=', ACT_ZOOM_IN},
    {MOD_ALT, '+', ACT_ZOOM_IN},
    {MOD_ALT, '-', ACT_ZOOM_OUT},
    {MOD_ALT, '_', ACT_ZOOM_OUT},
    {MOD_ALT, '0', ACT_ZOOM_RESET},

    {MOD_CTRL, 'f', ACT_FIND},

    {0, KEY_DOWN, ACT_LINE_DOWN},
    {0, KEY_UP, ACT_LINE_UP},
    {0, KEY_BACKSPACE, ACT_BACK},
    {MOD_SHIFT, KEY_BACKSPACE, ACT_FORWARD},
    {MOD_SHIFT, KEY_DOWN, ACT_BOX_TALLER},
    {MOD_SHIFT, KEY_UP, ACT_BOX_SHORTER},
    {MOD_SHIFT, KEY_RIGHT, ACT_BOX_WIDER},
    {MOD_SHIFT, KEY_LEFT, ACT_BOX_NARROWER},
    {0, KEY_PGDN, ACT_PAGE_DOWN},
    {0, KEY_PGUP, ACT_PAGE_UP},
    {0, ' ', ACT_PAGE_DOWN},
    // What every browser puts them on, and the way to walk a search without
    // the vi keys that used to be the only one.
    {0, KEY_F3, ACT_FIND_NEXT},
    {MOD_SHIFT, KEY_F3, ACT_FIND_PREV},
    {0, 'f', ACT_HINT},
    {0, 'F', ACT_HINT_TAB},
    {MOD_SHIFT, '/', ACT_HELP},
    {0, '?', ACT_HELP},
    {0, 'i', ACT_INSERT},
    {0, KEY_ESC, ACT_INSERT_OFF},
    {0, ':', ACT_CONSOLE},
    {0, 'P', ACT_PICK},
    {0, '[', ACT_SMALLER},
    {0, ']', ACT_LARGER},
    {0, 'w', ACT_PAGE_WIDER},
    {0, 'W', ACT_PAGE_NARROWER},
    {0, 's', ACT_SCALE},
    {0, 'D', ACT_BOX_TALLER},
    {0, 'U', ACT_BOX_SHORTER},
    {0, 'R', ACT_BOX_WIDER},
    {0, 'L', ACT_BOX_NARROWER},
};
#define NDEFAULTS ((int)(sizeof DEFAULTS / sizeof DEFAULTS[0]))

// ------------------------------------------------------------------- vim
//
// `vim = yes`, and the whole of the vi vocabulary is here rather than in the
// map above: with this off, `j` types a j into the page and the window is
// driven by the arrows, the chords and the handful of keys that are this
// program's own rather than vi's. With it on, a reader who knows vim already
// knows this - j k h l d u b gg G / n N for moving and searching, and the
// browser extensions' additions on top: H and L for the history, o for the
// address, f for the links, ZZ to leave.
//
// It sits under web.conf rather than over it, so a key the file names keeps
// what the file gave it. Six of these land on keys the plain map uses for
// something else, and those change meaning while vim is on:
//
//   L R    were the window's width; shift+left and shift+right still are
//   :      was the console, which keeps ^X
//   ^F     was find, which vi spells /
//   ^D ^B  were the trace and the previous tab; the tab is on alt+shift+left
static const KeyDef VIM[] = {
    {0, 'j', ACT_SCROLL_DOWN},
    {0, 'k', ACT_SCROLL_UP},
    {0, 'h', ACT_SCROLL_LEFT},
    {0, 'l', ACT_SCROLL_RIGHT},
    {0, 'd', ACT_HALF_DOWN},
    {0, 'u', ACT_HALF_UP},
    {0, 'b', ACT_PAGE_UP},
    {0, 'G', ACT_BOTTOM},
    {0, '/', ACT_FIND},
    {0, 'n', ACT_FIND_NEXT},
    {0, 'N', ACT_FIND_PREV},
    {0, 'H', ACT_BACK},
    {0, 'L', ACT_FORWARD},
    {0, 'o', ACT_ADDRESS},
    {0, 'O', ACT_ADDRESS_TAB},
    {0, ':', ACT_ADDRESS_BLANK},
    {0, 'r', ACT_RELOAD},
    {0, 'R', ACT_RELOAD_HARD},
    {0, 'T', ACT_SEARCH_TABS},
    {0, 'm', ACT_BOOKMARK},
    {MOD_CTRL, 'd', ACT_HALF_DOWN},
    {MOD_CTRL, 'u', ACT_HALF_UP},
    {MOD_CTRL, 'f', ACT_PAGE_DOWN},
    {MOD_CTRL, 'b', ACT_PAGE_UP},
};
#define NVIM ((int)(sizeof VIM / sizeof VIM[0]))

// `g` and `y` do nothing on their own here: each waits for the key that says
// what it was.
static const KeyPair VIM_PAIRS[] = {
    {0, 'g', 0, 'g', ACT_TOP},
    {0, 'g', 0, 'i', ACT_FOCUS_INPUT},
    {0, 'g', 0, 'f', ACT_HINT_ALL},
    {0, 'g', 0, 'h', ACT_SEARCH_HISTORY},
    {0, 'g', 0, 'b', ACT_SEARCH_BOOKMARKS},
    {0, 'y', 0, 'y', ACT_COPY_URL},
    {0, 'y', 0, 'f', ACT_HINT_COPY},
    {0, 'Z', 0, 'Z', ACT_QUIT},
};
#define NVIM_PAIRS ((int)(sizeof VIM_PAIRS / sizeof VIM_PAIRS[0]))

// --------------------------------------------------------------- settings

// The command line options worth having an opinion about for longer than one
// run. Each is a field of App and the flag that sets it stays what it always
// was: the file is read before the arguments are, so a flag still wins for the
// run it is given on.
//
// `zoom`, `rows` and `cols` are where a new window starts and nothing more. A
// key moves them afterwards and the file never hears about it, so the several
// windows usually up at once each keep their own - what is written here is an
// opening position, not a record of one.
typedef enum { S_BOOL, S_BOOL_NOT, S_SCALE, S_ZOOM, S_COUNT, S_MS } SetKind;

static const struct {
    const char *name;
    SetKind     kind;
    size_t      off;        // the field in App it is
} SETTINGS[] = {
    {"vim",           S_BOOL,     offsetof(App, vim)},
    {"pause-on-blur", S_BOOL,     offsetof(App, pause_on_blur)},
    {"status-line",   S_BOOL_NOT, offsetof(App, hide_status)},
    {"clear-on-exit", S_BOOL,     offsetof(App, clear_exit)},
    {"full",          S_BOOL_NOT, offsetof(App, inline_mode)},
    {"mute",          S_BOOL,     offsetof(App, mute)},
    {"raw-keys",      S_BOOL_NOT, offsetof(App, claim_keys)},
    {"keep",          S_BOOL,     offsetof(App, keep)},
    {"scale",         S_SCALE,    0},
    {"zoom",          S_ZOOM,     offsetof(App, zoom)},
    {"rows",          S_COUNT,    offsetof(App, want_rows)},
    {"cols",          S_COUNT,    offsetof(App, want_cols)},
    {"slowmo",        S_MS,       offsetof(App, slowmo)},
    {"freeze",        S_BOOL,     offsetof(App, freeze)},
    {"grid",          S_BOOL,     offsetof(App, grid_auto)},
    {"tmux-zoom",     S_BOOL,     offsetof(App, tmux_zoom)},
};
#define NSETTINGS ((int)(sizeof SETTINGS / sizeof SETTINGS[0]))

static bool   *bool_at(App *a, size_t off)   { return (bool *)((char *)a + off); }
static double *double_at(App *a, size_t off) { return (double *)((char *)a + off); }
static int    *int_at(App *a, size_t off)    { return (int *)((char *)a + off); }

static bool parse_bool(const char *v, bool *out) {
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
        !strcasecmp(v, "on")  || !strcmp(v, "1")) { *out = true;  return true; }
    if (!strcasecmp(v, "no")  || !strcasecmp(v, "false") ||
        !strcasecmp(v, "off") || !strcmp(v, "0")) { *out = false; return true; }
    return false;
}

static bool setting_set(App *a, int i, const char *v) {
    char *end;
    if (SETTINGS[i].kind == S_SCALE) {
        if (!strcasecmp(v, "auto")) { a->motion_auto = true; return true; }
        double d = strtod(v, &end);
        if (end == v || *end || d < 0.1 || d > 3.0) return false;
        a->motion_auto = false;
        a->want_scale = d;
        return true;
    }
    // The same range --zoom takes, refused rather than clamped: a file is
    // written once and read every run, so a number outside it is worth saying.
    if (SETTINGS[i].kind == S_ZOOM) {
        double d = strtod(v, &end);
        if (end == v || *end || d < 0.5 || d > 3.0) return false;
        *double_at(a, SETTINGS[i].off) = d;
        return true;
    }
    if (SETTINGS[i].kind == S_COUNT) {
        if (!strcasecmp(v, "auto")) { *int_at(a, SETTINGS[i].off) = 0; return true; }
        long n = strtol(v, &end, 10);
        if (end == v || *end || n < 1 || n > 1000) return false;
        *int_at(a, SETTINGS[i].off) = (int)n;
        return true;
    }
    // Milliseconds, where none is a number rather than a word: 0 is what a run
    // going as fast as it can already is.
    if (SETTINGS[i].kind == S_MS) {
        long n = strtol(v, &end, 10);
        if (end == v || *end || n < 0 || n > 60000) return false;
        *int_at(a, SETTINGS[i].off) = (int)n;
        return true;
    }
    bool on;
    if (!parse_bool(v, &on)) return false;
    *bool_at(a, SETTINGS[i].off) = SETTINGS[i].kind == S_BOOL_NOT ? !on : on;
    return true;
}

static void setting_text(const App *a, int i, char *out, size_t cap) {
    if (SETTINGS[i].kind == S_SCALE) {
        if (a->motion_auto) snprintf(out, cap, "auto");
        else                snprintf(out, cap, "%g", a->want_scale);
        return;
    }
    if (SETTINGS[i].kind == S_ZOOM) {
        snprintf(out, cap, "%g", *double_at((App *)a, SETTINGS[i].off));
        return;
    }
    if (SETTINGS[i].kind == S_COUNT) {
        int n = *int_at((App *)a, SETTINGS[i].off);
        if (n > 0) snprintf(out, cap, "%d", n);
        else       snprintf(out, cap, "auto");
        return;
    }
    if (SETTINGS[i].kind == S_MS) {
        snprintf(out, cap, "%d", *int_at((App *)a, SETTINGS[i].off));
        return;
    }
    bool on = *bool_at((App *)a, SETTINGS[i].off);
    snprintf(out, cap, "%s", (SETTINGS[i].kind == S_BOOL_NOT ? !on : on) ? "yes" : "no");
}

// ------------------------------------------------------------ site rules

// Which elements the labels land on, per site. `f` labels everything a click
// could be aimed at, which is the only honest answer on a page nothing is known
// about and the wrong one on a page that is a list: Hacker News is two hundred
// links of which thirty are the ones anyone came for, and the other hundred and
// seventy are what makes the thirty unreadable. `hint-only` says what the whole
// set is for a host and `hint-skip` takes a part of it away, both as ordinary
// CSS selectors, which is the same answer Vimium C arrived at.
//
//   hint-only news.ycombinator.com = .titleline > a
//   hint-skip github.com           = .Header, footer
//
// Neither is a way to lose a link for good: `hint-all` labels everything on any
// page, whatever the file said about it.
typedef struct { char host[96]; char sel[192]; bool skip; } HintRule;
#define HINTS_MAX 32

static HintRule g_hints[HINTS_MAX];
static int      g_nhints;

// One field of a line, trimmed at both ends into a fixed array. False when
// there was nothing there or more than there is room for, since a selector
// silently cut in half is a selector that quietly means something else.
static bool copy_field(char *out, size_t cap, const char *s, const char *end) {
    while (s < end && isspace((unsigned char)*s)) s++;
    while (end > s && isspace((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - s);
    if (!n || n >= cap) return false;
    memcpy(out, s, n);
    out[n] = 0;
    return true;
}

// The `host = selector` half of a rule line.
static bool hint_add(const char *s, bool skip) {
    // The first `=`, unlike every other line in the file: a selector may hold
    // one, as `a[href^=http]` does, and the host it belongs to may not.
    const char *eq = strchr(s, '=');
    if (!eq || g_nhints >= HINTS_MAX) return false;
    HintRule r = {{0}, {0}, skip};
    if (!copy_field(r.host, sizeof r.host, s, eq)) return false;
    if (!copy_field(r.sel, sizeof r.sel, eq + 1, eq + 1 + strlen(eq + 1))) return false;
    for (char *p = r.host; *p; p++) *p = (char)tolower((unsigned char)*p);
    g_hints[g_nhints++] = r;
    return true;
}

// The `hint-only` or `hint-skip` a line opens with, and what follows it. NULL
// for every other line, which is most of them.
static const char *hint_keyword(const char *s, bool *skip) {
    while (isspace((unsigned char)*s)) s++;
    if (!strncasecmp(s, "hint-only", 9))      *skip = false;
    else if (!strncasecmp(s, "hint-skip", 9)) *skip = true;
    else return NULL;
    s += 9;
    if (!isspace((unsigned char)*s)) return NULL;
    while (isspace((unsigned char)*s)) s++;
    return s;
}

// The host of a URL, lowercased, without the scheme, the port, the login or
// anything after it.
static void url_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *e = p;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    for (const char *at = p; at < e; at++)
        if (*at == '@') p = at + 1;
    for (const char *c = p; c < e; c++)
        if (*c == ':') { e = c; break; }
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)p[i]);
    out[n] = 0;
}

// A rule's host against the page's: the end of it, on a dot, so
// `ycombinator.com` is `news.ycombinator.com` as well and `combinator.com` is
// neither of them.
static bool host_match(const char *host, const char *pat) {
    size_t h = strlen(host), n = strlen(pat);
    if (!n || n > h) return false;
    if (strcasecmp(host + h - n, pat)) return false;
    return h == n || host[h - n - 1] == '.';
}

const char *hint_selector(const char *url, bool skip) {
    if (!url || !*url || !g_nhints) return NULL;
    char host[256];
    url_host(url, host, sizeof host);
    if (!host[0]) return NULL;
    // The first that matches rather than the longest: the file is short and in
    // an order its writer chose, and a second rule for a host already named is
    // a line that reads as if it were the one in force.
    for (int i = 0; i < g_nhints; i++)
        if (g_hints[i].skip == skip && host_match(host, g_hints[i].host))
            return g_hints[i].sel;
    return NULL;
}

// ------------------------------------------------------------------ file

// Written once, when there is nothing there, out of the values the program is
// about to run with - so the file always opens saying exactly what the
// defaults are rather than what they were when this was written. The lines
// and nothing else: what they mean is the readme's job.
static void write_config(const char *path, const App *a) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < NSETTINGS; i++) {
        char val[32];
        setting_text(a, i, val, sizeof val);
        fprintf(f, "%-16s = %s\n", SETTINGS[i].name, val);
    }
    fputs("\n# Which elements the link labels land on, for one site.\n"
          "#hint-only news.ycombinator.com = .titleline > a\n"
          "#hint-skip github.com = .Header, footer\n", f);
    // The keys are written with a `#` in front of them: each line says what the
    // key already does, and taking the `#` off is how it is changed. Live, they
    // would be the whole default map stated a second time - and anything laid
    // under this file, the vim map above all, would have nothing left to say.
    fputs("\n# Every key, as it stands. Take the `#` off a line to change it,\n"
          "# and press `?` in the window for the list as it really is.\n", f);
    // Walked in action order rather than binding order, so the file reads as a
    // list of what can be done rather than of what the keyboard happens to hold.
    for (int i = 0; i < NACTS; i++) {
        if (ACTS[i].section) fputc('\n', f);
        for (int j = 0; j < NDEFAULTS; j++) {
            if (DEFAULTS[j].act != ACTS[i].act) continue;
            KeyBind b = {DEFAULTS[j].mods, DEFAULTS[j].key, 0, 0, DEFAULTS[j].act};
            char spec[48];
            bind_text(&b, spec, sizeof spec);
            fprintf(f, "#%-15s = %s\n", spec, ACTS[i].name);
        }
    }
    fclose(f);
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

// Whether the vim layer had asked for this key, so a file line landing on it
// can be counted. A single key counts against every pair it would start: the
// file saying `y = copy-url` is the file taking `yy` and `yf` away, whether or
// not it meant to.
static bool vim_had(int m1, int k1, int m2, int k2) {
    normalize(&m1, &k1);
    if (k2) normalize(&m2, &k2);
    else    m2 = 0;
    for (int i = 0; i < g_nvim; i++) {
        if (g_vim[i].mods != m1 || g_vim[i].key != k1) continue;
        if (!k2 || (g_vim[i].mods2 == m2 && g_vim[i].key2 == k2)) return true;
    }
    return false;
}

// The file is read twice: once for the settings, and once for the keys, after
// whatever the settings asked for has been laid down underneath them. Each
// line belongs to exactly one of the two passes, so a mistake in it is
// reported once rather than on every trip through.
static int read_config(const char *path, App *a, bool keys) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int lineno = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        // A site rule, taken whole and before any of the reading below touches
        // it: a CSS selector may hold a `#` and it may hold an `=`, and neither
        // means there what it means on every other line of the file. Which
        // leaves such a line only able to be commented out from its very start.
        bool skip;
        trim(line);
        const char *rule = hint_keyword(line, &skip);
        if (rule) {
            if (!keys) {
                if (!hint_add(rule, skip)) {
                    fprintf(stderr, "web: %s:%d: `%s` is not a host and a selector\n",
                            path, lineno, rule);
                    bad++;
                }
            }
            continue;
        }
        // A `#` opening the line, or with a space in front of it, starts a
        // comment; one written up against the key is the key. Which leaves the
        // bare `#` key unreachable and every other spelling of it working.
        for (char *p = line; *p; p++) {
            if (*p != '#') continue;
            if (p == line || isspace((unsigned char)p[-1])) { *p = 0; break; }
        }
        // The last `=`, not the first: the key may be one, as alt+= is.
        char *eq = strrchr(line, '=');
        if (!eq) {
            trim(line);
            if (line[0] && keys) {
                fprintf(stderr, "web: %s:%d: no `=` in `%s`\n", path, lineno, line);
                bad++;
            }
            continue;
        }
        *eq = 0;
        char *spec = line, *name = eq + 1;
        while (isspace((unsigned char)*spec)) spec++;
        while (isspace((unsigned char)*name)) name++;
        trim(spec);
        trim(name);
        // A setting first, and a key only if the left of it is not the name of
        // one. No setting is named anything a key could be called, so the two
        // kinds of line cannot be confused for one another.
        int s = -1;
        for (int i = 0; i < NSETTINGS; i++)
            if (!strcasecmp(SETTINGS[i].name, spec)) { s = i; break; }
        if (s >= 0) {
            if (!keys && !setting_set(a, s, name)) {
                const char *want = "yes or no";
                if (SETTINGS[s].kind == S_SCALE)      want = "size";
                else if (SETTINGS[s].kind == S_ZOOM)  want = "magnification";
                else if (SETTINGS[s].kind == S_COUNT) want = "count";
                else if (SETTINGS[s].kind == S_MS)    want = "number of milliseconds";
                fprintf(stderr, "web: %s:%d: `%s` is not a %s for %s\n", path,
                        lineno, name, want, spec);
                bad++;
            }
            continue;
        }
        if (!keys) continue;
        int m1, k1, m2, k2;
        if (!parse_seq(spec, &m1, &k1, &m2, &k2)) {
            fprintf(stderr, "web: %s:%d: `%s` is not a key\n", path, lineno, spec);
            bad++;
            continue;
        }
        Act act = act_by_name(name);
        if (act == ACT_INVALID) {
            fprintf(stderr, "web: %s:%d: `%s` is not an action\n", path, lineno, name);
            bad++;
            continue;
        }
        if (vim_had(m1, k1, m2, k2)) a->vim_shadowed++;
        bind_set(m1, k1, m2, k2, act);
    }
    fclose(f);
    return bad;
}

// Where it lives. One browser, one terminal, one file - none of it is keyed to
// anything, because none of it says anything about a particular window.
void config_dir(char *out, size_t cap) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (cfg && *cfg) snprintf(out, cap, "%s/web", cfg);
    else             snprintf(out, cap, "%s/.config/web", home ? home : "/tmp");
}

// A key bound on its own happens the moment it is pressed, which leaves every
// pair starting with it unreachable. That is a fair thing to ask for and a
// terrible thing to arrive at by accident - and the commonest way in is a
// web.conf written by a version where `gg` was one binding pressed twice, whose
// `g = top` line now means a single press. Said once per key, and never for the
// defaults, which do not shadow anything.
static void warn_shadowed(const char *path) {
    for (int i = 0; i < g_nbinds; i++) {
        if (g_binds[i].key2) continue;
        for (int j = 0; j < g_nbinds; j++) {
            if (!g_binds[j].key2) continue;
            if (g_binds[j].mods != g_binds[i].mods) continue;
            if (g_binds[j].key != g_binds[i].key) continue;
            char one[48], pair[48];
            bind_text(&g_binds[i], one, sizeof one);
            bind_text(&g_binds[j], pair, sizeof pair);
            fprintf(stderr, "web: %s: `%s` acts on its own, so `%s` is out of reach\n",
                    path, one, pair);
            break;
        }
    }
}

// The vim layer, remembered as it goes on so the file can be seen taking any
// of it back again.
static void load_vim(void) {
    g_nvim = 0;
    for (int i = 0; i < NVIM && g_nvim < (int)(sizeof g_vim / sizeof g_vim[0]); i++) {
        g_vim[g_nvim++] = (KeyBind){VIM[i].mods, VIM[i].key, 0, 0, VIM[i].act};
        bind_set(VIM[i].mods, VIM[i].key, 0, 0, VIM[i].act);
    }
    for (int i = 0; i < NVIM_PAIRS && g_nvim < (int)(sizeof g_vim / sizeof g_vim[0]); i++) {
        g_vim[g_nvim++] = (KeyBind){VIM_PAIRS[i].mods, VIM_PAIRS[i].key,
                                    VIM_PAIRS[i].mods2, VIM_PAIRS[i].key2,
                                    VIM_PAIRS[i].act};
        bind_set(VIM_PAIRS[i].mods, VIM_PAIRS[i].key,
                 VIM_PAIRS[i].mods2, VIM_PAIRS[i].key2, VIM_PAIRS[i].act);
    }
}

int config_load(App *a) {
    g_nbinds = 0;
    g_nvim = 0;
    g_nhints = 0;
    for (int i = 0; i < NDEFAULTS; i++)
        bind_set(DEFAULTS[i].mods, DEFAULTS[i].key, 0, 0, DEFAULTS[i].act);

    char dir[512], path[600];
    config_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/web.conf", dir);
    if (access(path, F_OK) != 0) {
        mkdirs(dir);
        write_config(path, a);
        return 0;               // just written, so it says what is already loaded
    }
    // Settings first, because one of them decides what the keys are laid on
    // top of; then the layer; then the file's own keys, which win, since a key
    // named in a file is a key someone meant.
    int bad = read_config(path, a, false);
    if (a->vim) load_vim();
    bad += read_config(path, a, true);
    warn_shadowed(path);
    return bad;
}

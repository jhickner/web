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
// opinion it would want to save. Which is also why nothing that changes while
// the window is up is in it: the zoom and the size of the window belong to the
// one window, and there may be a dozen of them running.
//
// Nothing else in the program knows a key: main.c asks this what an event
// means and acts on the answer, and help.c asks it which key an action wears
// so the list it draws is the real one.

typedef struct { int mods, key; Act act; } KeyBind;

// One entry per key, so a file line naming a key already bound replaces it
// rather than adding a second meaning. Two keys may share an action.
#define BINDS_MAX 192

static KeyBind g_binds[BINDS_MAX];
static int     g_nbinds;

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
    {"back",           ACT_BACK, NULL},
    {"forward",        ACT_FORWARD, NULL},
    {"reload",         ACT_RELOAD, NULL},
    {"copy",           ACT_COPY, NULL},
    {"copy-url",       ACT_COPY_URL, NULL},
    {"external",       ACT_EXTERNAL, NULL},
    {"find",           ACT_FIND, NULL},
    {"find-next",      ACT_FIND_NEXT, NULL},
    {"find-prev",      ACT_FIND_PREV, NULL},
    {"insert",         ACT_INSERT, NULL},
    {"insert-off",     ACT_INSERT_OFF, NULL},
    {"pick-selector",  ACT_PICK, NULL},

    {"tab-new",        ACT_TAB_NEW, "tabs"},
    {"tab-close",      ACT_TAB_CLOSE, NULL},
    {"tab-next",       ACT_TAB_NEXT, NULL},
    {"tab-prev",       ACT_TAB_PREV, NULL},
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

static int find_bind(int mods, int key) {
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].mods == mods && g_binds[i].key == key) return i;
    return -1;
}

Act keys_lookup(int mods, int key) {
    normalize(&mods, &key);
    int i = find_bind(mods, key);
    // A terminal that reports the unshifted key sends `?` as `/` with shift
    // held, and `:` as `;`. The shifted spelling is tried first so a binding
    // made on one may still differ from the binding made on the other.
    if (i < 0 && (mods & MOD_SHIFT) && key < 0x100)
        i = find_bind(mods & ~MOD_SHIFT, key);
    return i < 0 ? ACT_NONE : g_binds[i].act;
}

static void bind_set(int mods, int key, Act act) {
    normalize(&mods, &key);
    int i = find_bind(mods, key);
    if (act == ACT_NONE) {
        if (i >= 0) g_binds[i] = g_binds[--g_nbinds];
        return;
    }
    if (i < 0) {
        if (g_nbinds >= BINDS_MAX) return;
        i = g_nbinds++;
        g_binds[i].mods = mods;
        g_binds[i].key = key;
    }
    g_binds[i].act = act;
}

// ---------------------------------------------------------------- spelling

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

// The shortest of the keys that do it, which is both the one worth learning
// and the one that keeps the help list narrow enough to hold two columns: `?`
// rather than `shift+/`, `D` rather than `shift+down`.
bool keys_text(Act act, char *out, size_t cap) {
    char best[48] = "";
    for (int i = 0; i < g_nbinds; i++) {
        if (g_binds[i].act != act) continue;
        char one[48];
        key_text(g_binds[i].mods, g_binds[i].key, one, sizeof one);
        if (!best[0] || strlen(one) < strlen(best)) memcpy(best, one, sizeof one);
    }
    snprintf(out, cap, "%s", best[0] ? best : "-");
    return best[0] != 0;
}

// ---------------------------------------------------------------- defaults

static const KeyBind DEFAULTS[] = {
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

    {0, KEY_DOWN, ACT_LINE_DOWN},
    {0, KEY_UP, ACT_LINE_UP},
    {0, KEY_BACKSPACE, ACT_BACK},
    {MOD_SHIFT, KEY_BACKSPACE, ACT_FORWARD},
    {MOD_SHIFT, KEY_DOWN, ACT_BOX_TALLER},
    {MOD_SHIFT, KEY_UP, ACT_BOX_SHORTER},
    {MOD_SHIFT, KEY_RIGHT, ACT_BOX_WIDER},
    {MOD_SHIFT, KEY_LEFT, ACT_BOX_NARROWER},
    {0, 'j', ACT_SCROLL_DOWN},
    {0, 'k', ACT_SCROLL_UP},
    {0, 'h', ACT_SCROLL_LEFT},
    {0, 'l', ACT_SCROLL_RIGHT},
    {0, 'd', ACT_HALF_DOWN},
    {0, 'u', ACT_HALF_UP},
    {0, ' ', ACT_PAGE_DOWN},
    {0, 'b', ACT_PAGE_UP},
    {0, 'g', ACT_TOP},
    {0, 'G', ACT_BOTTOM},
    {0, 'y', ACT_COPY_URL},
    {0, 'n', ACT_FIND_NEXT},
    {0, 'N', ACT_FIND_PREV},
    {0, '/', ACT_FIND},
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

// --------------------------------------------------------------- settings

// The command line options worth having an opinion about for longer than one
// run. Each is a field of App and the flag that sets it stays what it always
// was: the file is read before the arguments are, so a flag still wins for the
// run it is given on.
//
// The ones a key changes - the zoom, the width, the size of the window - are
// deliberately not here. They belong to the one window rather than to the
// person, and several windows are usually up at once.
typedef enum { S_BOOL, S_BOOL_NOT, S_SCALE } SetKind;

static const struct {
    const char *name;
    SetKind     kind;
    size_t      off;        // the field in App it is
} SETTINGS[] = {
    {"pause-on-blur", S_BOOL,     offsetof(App, pause_on_blur)},
    {"status-line",   S_BOOL_NOT, offsetof(App, hide_status)},
    {"clear-on-exit", S_BOOL,     offsetof(App, clear_exit)},
    {"full",          S_BOOL_NOT, offsetof(App, inline_mode)},
    {"mute",          S_BOOL,     offsetof(App, mute)},
    {"raw-keys",      S_BOOL_NOT, offsetof(App, claim_keys)},
    {"keep",          S_BOOL,     offsetof(App, keep)},
    {"scale",         S_SCALE,    0},
};
#define NSETTINGS ((int)(sizeof SETTINGS / sizeof SETTINGS[0]))

static bool *bool_at(App *a, size_t off) { return (bool *)((char *)a + off); }

static bool parse_bool(const char *v, bool *out) {
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
        !strcasecmp(v, "on")  || !strcmp(v, "1")) { *out = true;  return true; }
    if (!strcasecmp(v, "no")  || !strcasecmp(v, "false") ||
        !strcasecmp(v, "off") || !strcmp(v, "0")) { *out = false; return true; }
    return false;
}

static bool setting_set(App *a, int i, const char *v) {
    if (SETTINGS[i].kind == S_SCALE) {
        if (!strcasecmp(v, "auto")) { a->motion_auto = true; return true; }
        char *end;
        double d = strtod(v, &end);
        if (end == v || *end || d < 0.1 || d > 3.0) return false;
        a->motion_auto = false;
        a->want_scale = d;
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
    bool on = *bool_at((App *)a, SETTINGS[i].off);
    snprintf(out, cap, "%s", (SETTINGS[i].kind == S_BOOL_NOT ? !on : on) ? "yes" : "no");
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
    // Walked in action order rather than binding order, so the file reads as a
    // list of what can be done rather than of what the keyboard happens to hold.
    for (int i = 0; i < NACTS; i++) {
        if (ACTS[i].section) fputc('\n', f);
        for (int j = 0; j < NDEFAULTS; j++) {
            if (DEFAULTS[j].act != ACTS[i].act) continue;
            char spec[32];
            key_text(DEFAULTS[j].mods, DEFAULTS[j].key, spec, sizeof spec);
            fprintf(f, "%-16s = %s\n", spec, ACTS[i].name);
        }
    }
    fclose(f);
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int read_config(const char *path, App *a) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int lineno = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
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
            if (line[0]) {
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
            if (!setting_set(a, s, name)) {
                fprintf(stderr, "web: %s:%d: `%s` is not a %s for %s\n", path,
                        lineno, name,
                        SETTINGS[s].kind == S_SCALE ? "size" : "yes or no", spec);
                bad++;
            }
            continue;
        }
        int mods, key;
        if (!parse_spec(spec, &mods, &key)) {
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
        bind_set(mods, key, act);
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

int config_load(App *a) {
    g_nbinds = 0;
    for (int i = 0; i < NDEFAULTS; i++)
        bind_set(DEFAULTS[i].mods, DEFAULTS[i].key, DEFAULTS[i].act);

    char dir[512], path[600];
    config_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/web.conf", dir);
    if (access(path, F_OK) != 0) {
        mkdirs(dir);
        write_config(path, a);
        return 0;               // just written, so it says what is already loaded
    }
    return read_config(path, a);
}

# web: keybinding model, vim mode, and link hints — implementation design

Design for the TODO item:

> add robust keybinding model, so users can fully customize. add selectable vim
> mode out of the box, add keyboard-only navigation with link labels, etc. all
> optional.

Target: `/Users/jhickner/working/web`, C11, libc only, `-Wall -Wextra` clean.
Written to be executed without re-deriving decisions. Every open question is
answered with a recommendation.

---

## 0. What was checked first

- `~/working/libs/c` has **nothing reusable** for this: no config parser, no INI
  reader, no keybinding code. `tui/term/term.h` has its own incompatible key
  enum (`KEY_CHAR` + `ch`, no modifier bitset). Vendoring it would be a
  regression against `src/term.c`, which already decodes the kitty keyboard
  protocol and reports a real `mods` bitset. **Write the parser in this repo.**
- `src/util.c` already has `Buf`, `buf_addf`, `json_escape`, `json_unescape`,
  `mkdirs` — everything the config loader and the JS injection need.
- `src/main.c` already has the exact precedent for the hint machinery:
  `Runtime.addBinding` + `Page.addScriptToEvaluateOnNewDocument` +
  `Runtime.evaluate` (the `FOCUS_WATCHER` / `__webmode` trio at `main.c:619` and
  `main.c:1185`). Hints reuse that shape verbatim.

---

## 1. Current state

### 1.1 Every binding in `handle_key` (`src/main.c:690`)

Gate order is literal control flow; the "Gate" column is the condition that must
hold for the row to be reached, given everything above it did not return.

**Gate A — `a->editing` (address bar or find prompt has focus), `main.c:691`**

| Key | Action | Notes |
|---|---|---|
| `enter` | accept: `navigate(edit)` if `prompt==1`, else store `find` + `find_next` | clears `editing`, `prompt` |
| `esc` | cancel | |
| `ctrl-g` | cancel | |
| `backspace` | delete one byte | byte, not codepoint |
| `ctrl-u` | clear the line | |
| any `ev->text[0]` | append to `edit` | |
| everything else | swallowed | arrows, ctrl-a, etc. do nothing |

**Gate B — `ev->mods == MOD_SUPER` exactly, `main.c:726`**

| Key | Action |
|---|---|
| `cmd-c` | `copy_selection` |
| any other cmd chord | swallowed (never reaches the page) |

**Gate C — `ev->mods == MOD_CTRL` exactly, `main.c:731`** (no `default:`, so
unmatched ctrl chords fall through to Gate F/G)

| Key | Action |
|---|---|
| `ctrl-q`, `ctrl-c` | quit |
| `ctrl-l` | address bar, prefilled with the current url |
| `ctrl-g` | toggle `show_stats` |
| `ctrl-y` | `copy_selection` |
| `ctrl-r` | `Page.reload` |
| `ctrl-o` | history back |
| `ctrl-p` | history forward |

**Gate D — `!a->insert && !(ev->mods & (MOD_CTRL|MOD_ALT))`, `main.c:756`**
(i.e. reading mode, no modifier or shift-only)

| Key | Action |
|---|---|
| `down` / `up` | `scroll_by(±40)` CSS px |
| `left` / `right` | history back / forward |
| `j` / `k` | `scroll_by(±60)` |
| `d` / `u` | `scroll_by(±css_h/2)` |
| `space` / `b` | `scroll_by(±css_h*0.9)` |
| `g` | arm `pending_g`; second `g` → `scroll_page_end(false)` |
| `G` | `scroll_page_end(true)` |
| `[` | inline: `resize_box(-1)`; full: `zoom_by(1/1.25)` |
| `]` | inline: `resize_box(+1)`; full: `zoom_by(1.25)` |
| `w` / `W` | `cycle_width(±1)` |
| `s` | `cycle_scale()` |
| `n` / `N` | `find_next(false/true)` |
| `/` | find prompt (`editing=1, prompt=2`) |
| `i` | `insert = true` |

**Gate E — `ev->key == KEY_ESC && a->insert`, `main.c:812`**: blur the focused
element, `insert = false`.

**Gate F — `ev->mods == MOD_ALT`, `main.c:819`**

| Key | Action |
|---|---|
| `alt-f` | toggle `fit_width` |
| `alt-=` / `alt-+` | `zoom_by(1.25)` |
| `alt--` / `alt-_` | `zoom_by(1/1.25)` |
| `alt-0` | zoom to 100% |

**Gate G — fallthrough to the page, `main.c:842`**

| Key | Action |
|---|---|
| `enter tab backspace delete esc up down left right home end pgup pgdn` | `special_key` → `Input.dispatchKeyEvent` |
| any printable with no ctrl/alt | `send_char` → `Input.dispatchKeyEvent type:char` |

Plus one binding outside `handle_key` entirely: **`pump_input` (`main.c:134`)
scans raw bytes for `0x11`/`0x03` while a frame is being written** and quits.
That path never sees a keymap.

### 1.2 The modal state

| Field | Set by | Meaning |
|---|---|---|
| `editing` | `ctrl-l`, `/` | the status line is a text field |
| `prompt` | same | 1 = address, 2 = find; only meaningful while `editing` |
| `insert` | `i`, and the page's `__webmode` binding on focus change | the page owns the keyboard |
| `pending_g` | `g` | first half of `gg` |

### 1.3 What is structurally wrong with it as a customization substrate

1. **Actions have no names.** Every action is an expression at a call site.
   Nothing can be re-bound, listed, logged, or printed in a help screen because
   there is no noun for "scroll down half a page".
2. **The map is control flow, not data.** Precedence is source order. Whether a
   ctrl chord reaches the page depends on whether its `case` happens to sit in
   the switch at `main.c:732` — the ones that do not are *silently* passed down
   to Gate G.
3. **Arguments are baked into call sites.** `scroll_by(40)` for arrows and
   `scroll_by(60)` for `j` are two different literals in two `case`s. There is
   no way to express "scroll by N" as a bindable thing.
4. **`pending_g` is a one-off with a scoping bug.** It is cleared only inside
   Gate D (`main.c:760`). Press `g`, then `ctrl-l` (Gate C returns early),
   escape the url bar, press `g` — and the page jumps to the top, because
   `pending_g` survived. Any general chord engine fixes this by construction.
5. **Modal state is four fields where it is one.** `editing` and `prompt`
   always change together; `draw_status` (`main.c:301`) re-derives the prompt
   label from `prompt == 2`. Adding a fifth mode (hints, help) means another
   bool, another early return, and another branch in `draw_status`.
6. **`insert` conflates two facts**: "the page reported a typable element has
   focus" and "the user pressed `i`". That is defensible and should be kept,
   but it must be named as a policy rather than an accident.
7. **No introspection.** The status hint at `main.c:315` is a hardcoded string
   literal (`"^L url  ^O back  ^R reload  ^Q quit"`) that will silently lie the
   moment anything is rebindable.
8. **`run_js` caps injected JS at 2048 bytes** (`main.c:560`, `char esc[2048]`
   after escaping). The hint script is several kilobytes. This must be fixed
   before hints can exist.
9. **Two decoder inconsistencies that a keymap makes visible** (see §2.5):
   multi-byte UTF-8 keys report the *lead byte* as `ev->key`, and `ctrl-alt-x`
   decodes differently under the legacy encoding than under kitty CSI-u.

---

## 2. The binding model

### 2.1 Modes

```c
// src/web.h
enum {
    MODE_NORMAL = 0,   // reading; web owns the keyboard
    MODE_INSERT,       // the page owns it: a field has focus, or `i` was pressed
    MODE_PROMPT,       // the status line is a text field (address or find)
    MODE_HINT,         // link labels are up; typing filters them
    MODE_HELP,         // the key list is covering the page
    MODE_ANY,          // pseudo-mode: searched after the current one
    MODE_COUNT
};
```

The current mode is **derived, not stored**, so the page's focus reports cannot
clobber a prompt that is open:

```c
static int mode_now(const App *a) {
    if (a->editing)     return MODE_PROMPT;
    if (a->helping)     return MODE_HELP;
    if (a->hint.active) return MODE_HINT;
    if (a->insert)      return MODE_INSERT;
    return MODE_NORMAL;
}
```

`insert` keeps its exact current meaning and its two writers. `editing` +
`prompt` stay as they are (`prompt` is now only a label selector for
`draw_status` and a branch in `prompt-accept`).

**Mode chain** — which map is searched, and in what order. This encodes the
existing gate order exactly:

| Mode | Searched | Fallback for an unmatched key |
|---|---|---|
| `NORMAL` | NORMAL, then ANY | send to the page (`special_key` / `send_char`) |
| `INSERT` | INSERT, then ANY | send to the page |
| `PROMPT` | PROMPT only | append `ev->text` to the edit line |
| `HINT` | HINT only | feed the character to the label filter |
| `HELP` | HELP only | dismiss the help |

`MODE_ANY` is where the ctrl and alt chords live, which is why they work in
insert mode today (Gate C precedes Gate D) and do not work in the prompt (Gate A
returns first). No behavior change.

One rule that is not a binding: **an unmatched chord carrying `MOD_SUPER` is
swallowed, never forwarded to the page.** That is Gate B's `return` and it must
survive, because the terminal only forwards cmd chords it does not want, and
turning those into page keystrokes would be surprising.

### 2.2 Chords and bindings

```c
// src/web.h
#define MAX_CHORD 3

typedef struct {
    uint16_t key;    // KEY_* or an ASCII/unicode codepoint
    uint8_t  mods;   // MOD_* bitset, normalized (see 2.5)
} Chord;

typedef struct {
    uint8_t  mode;              // MODE_*
    uint8_t  nchord;            // 1..MAX_CHORD
    uint8_t  act;               // ACT_*
    Chord    seq[MAX_CHORD];
    int32_t  argn;              // numeric argument, action-defined
    int32_t  args;              // offset into the arg arena, -1 = none
} Binding;
```

`args` is an **offset**, not a pointer: the arena is a `Buf` that reallocs as
config lines are read, and stored pointers would dangle. `bind_arg(b)` resolves
it:

```c
static Buf g_argpool;
static const char *bind_arg(const Binding *b) {
    return b->args < 0 ? NULL : g_argpool.p + b->args;
}
```

`MAX_CHORD 3` costs 6 bytes and covers `g,g`, `y,f`, and anything a user
invents. Depth beyond 3 has no precedent worth supporting.

Storage is **one flat array**, defaults first, config appended:

```c
static Binding *g_binds;
static int      g_nbinds, g_bindcap;
```

Lookup scans **backward**, so a later entry wins. That single rule gives
override, rebind, and unbind for free with no removal logic — a config line
`bind normal j none` appends `{NORMAL, 1, ACT_NONE, {{'j',0}}}` and the earlier
default is unreachable. ~70 default bindings plus a handful of user ones means a
reverse linear scan is a few hundred comparisons per keypress, against a CDP
round trip measured in milliseconds. **Do not build a hash table.**

### 2.3 Actions: enum + table of handlers

**Recommendation: yes — an action enum with a table of
`static void (*)(App *a, const Binding *b)` handlers, generated from a single
X-macro list.**

The signature takes the whole `Binding` rather than `(App*, int)` because three
actions (`open`, `js`, `hint`) need a string argument as well as a number, and
splitting them into `void (*)(App*, int)` plus a parallel string table is how
the two drift apart.

Arguments against the alternatives, for the record:

- *Store the function pointer directly in the binding, skip the enum.* Loses the
  name. `--keys`, the `?` overlay and the generated status hint all need to go
  from a binding back to a printable action name; a reverse pointer lookup is
  worse than an index.
- *Dispatch by string name at keypress time.* Turns a config typo into a
  per-keystroke failure instead of a load-time error, and does a `strcmp` sweep
  on the hot path for nothing.
- *One giant `switch (act)` instead of a table.* Works, but then the name,
  the help text and the implementation live in three places. The X-macro keeps
  them on one line, which is the actual goal.

```c
// src/keys.c
//
// name in the config file, ACT_ suffix, handler, numeric-arg kind, help line.
// ARG_NONE actions ignore argn; the Makefile passes -Wno-unused-parameter, so
// handlers that want neither field can take both and use neither.
#define ACTIONS(X)                                                                     \
    X("none",         NONE,        act_none,       ARG_NONE, "do nothing (unbind)")    \
    X("quit",         QUIT,        act_quit,       ARG_NONE, "quit web")               \
    X("page-key",     PAGE_KEY,    act_page_key,   ARG_NONE, "send this key to the page") \
    X("url-bar",      URL_BAR,     act_url_bar,    ARG_NUM,  "address bar (1 = start empty)") \
    X("find",         FIND,        act_find,       ARG_NONE, "find in page")           \
    X("find-next",    FIND_NEXT,   act_find_next,  ARG_NUM,  "repeat find (1 = backwards)") \
    X("reload",       RELOAD,      act_reload,     ARG_NUM,  "reload (1 = ignore cache)") \
    X("history",      HISTORY,     act_history,    ARG_NUM,  "-1 back, 1 forward")     \
    X("copy",         COPY,        act_copy,       ARG_NONE, "copy the selection, else the url") \
    X("stats",        STATS,       act_stats,      ARG_NONE, "toggle the stats readout") \
    X("scroll",       SCROLL,      act_scroll,     ARG_NUM,  "scroll by N CSS pixels") \
    X("scroll-frac",  SCROLL_FRAC, act_scroll_frac,ARG_NUM,  "scroll by N/1000 of the viewport") \
    X("goto-top",     GOTO_TOP,    act_goto_top,   ARG_NONE, "top of the page")        \
    X("goto-bottom",  GOTO_BOT,    act_goto_bot,   ARG_NONE, "bottom of the page")     \
    X("zoom",         ZOOM,        act_zoom,       ARG_NUM,  "1 in, -1 out, 0 reset")  \
    X("width",        WIDTH,       act_width,      ARG_NUM,  "step the told-width")    \
    X("render-scale", SCALE,       act_scale,      ARG_NONE, "cycle 1x/2x/3x")         \
    X("fit-width",    FIT,         act_fit,        ARG_NONE, "toggle fit-to-width")    \
    X("resize-box",   RESIZE,      act_resize,     ARG_NUM,  "resize the inline window; zooms in --full") \
    X("insert",       INSERT,      act_insert,     ARG_NONE, "hand the keyboard to the page") \
    X("normal",       NORMAL,      act_normal,     ARG_NONE, "blur, dismiss hints, drop a pending chord") \
    X("focus-input",  FOCUS_INPUT, act_focus_input,ARG_NONE, "focus the first text field") \
    X("hint",         HINT,        act_hint,       ARG_NUM,  "link labels: 0 follow, 1 new tab, 2 copy url") \
    X("help",         HELP,        act_help,       ARG_NONE, "show the key list")      \
    X("open",         OPEN,        act_open,       ARG_STR,  "navigate to a url or search")  \
    X("js",           JS,          act_js,         ARG_STR,  "run a snippet in the page")    \
    /* prompt mode */                                                                  \
    X("prompt-accept",  P_ACCEPT,  act_p_accept,   ARG_NONE, "submit the prompt")      \
    X("prompt-cancel",  P_CANCEL,  act_p_cancel,   ARG_NONE, "close the prompt")       \
    X("prompt-erase",   P_ERASE,   act_p_erase,    ARG_NUM,  "1 char, 2 word, 3 line") \
    /* hint mode */                                                                    \
    X("hint-cancel",    H_CANCEL,  act_h_cancel,   ARG_NONE, "dismiss the labels")     \
    X("hint-erase",     H_ERASE,   act_h_erase,    ARG_NONE, "undo one typed character") \
    X("hint-type",      H_TYPE,    act_h_type,     ARG_NONE, "feed this key to the filter")

typedef enum {
#define X(nm, id, fn, ak, hp) ACT_##id,
    ACTIONS(X)
#undef X
    ACT_COUNT
} Action;

enum { ARG_NONE, ARG_NUM, ARG_STR };

#define X(nm, id, fn, ak, hp) static void fn(App *a, const Binding *b);
ACTIONS(X)
#undef X

static const struct {
    const char *name;
    void      (*fn)(App *, const Binding *);
    int         argkind;
    const char *help;
} ACTION_TAB[ACT_COUNT] = {
#define X(nm, id, fn, ak, hp) { nm, fn, ak, hp },
    ACTIONS(X)
#undef X
};
```

Handlers are thin; the existing helpers do the work and keep their comments:

```c
static void act_scroll(App *a, const Binding *b)      { scroll_by(a, b->argn); }
static void act_scroll_frac(App *a, const Binding *b) { scroll_by(a, a->css_h * b->argn / 1000); }
static void act_history(App *a, const Binding *b)     { nav_history(a, b->argn); }
static void act_goto_top(App *a, const Binding *b)    { scroll_page_end(a, false); }
static void act_zoom(App *a, const Binding *b) {
    if (b->argn > 0)      zoom_by(a, 1.25);
    else if (b->argn < 0) zoom_by(a, 1.0 / 1.25);
    else { a->zoom = 1.0; a->fit_w = 0; relayout(a); save_state(a); notify(a, "zoom 100%"); request_fit(a); }
}
// [ and ] mean the window inline and the zoom in --full. Keeping that fork
// inside the action is what lets the default map bind them on one line.
static void act_resize(App *a, const Binding *b) {
    if (a->inline_mode) resize_box(a, b->argn);
    else                zoom_by(a, b->argn > 0 ? 1.25 : 1.0 / 1.25);
}
```

`act_page_key` is the fallback made explicit, so a user can *force* a key
through to the page (`bind any ctrl-l page-key`):

```c
static void act_page_key(App *a, const Binding *b) {
    Event *ev = a->cur_ev;              // the event being dispatched
    if (special_key(a, ev->key, ev->mods)) return;
    if (ev->text[0] && !(ev->mods & (MOD_CTRL | MOD_ALT))) send_char(a, ev->text);
}
```

`App` gains `Event *cur_ev` for exactly this — one pointer, set by
`keys_dispatch` for the duration of the call, so handlers that need the raw
event (`page-key`, `hint-type`, `prompt` fallback) can reach it without
threading it through every signature.

### 2.4 Chord dispatch

```c
// src/main.c App:
    Chord   pending[MAX_CHORD];
    int     npending;
    double  pending_at;
    Event  *cur_ev;
```

```c
// src/keys.c
//
// Highest priority exact match wins; a longer binding that starts with what has
// been typed so far sets *prefix, which is what makes the sequence wait.
static const Binding *bind_find(int mode, const Chord *seq, int n, bool *prefix) {
    const Binding *hit = NULL;
    *prefix = false;
    for (int i = g_nbinds - 1; i >= 0; i--) {
        const Binding *b = &g_binds[i];
        if (b->mode != mode && b->mode != MODE_ANY) continue;
        if (b->mode == MODE_ANY && !mode_falls_back_to_any(mode)) continue;
        if (b->nchord < n) continue;
        if (memcmp(b->seq, seq, (size_t)n * sizeof *seq) != 0) continue;
        if (b->nchord == n) { if (!hit) hit = b; }
        else *prefix = true;
    }
    return hit;
}

void keys_dispatch(App *a, Event *ev) {
    Chord c = { (uint16_t)ev->key, (uint8_t)ev->mods };
    chord_normalize(&c);

    // A pending prefix that has gone stale is not part of the sequence.
    if (a->npending && a->pending_at > 0 && now_sec() > a->pending_at) a->npending = 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (a->npending < MAX_CHORD) a->pending[a->npending++] = c;

        bool prefix = false;
        const Binding *b = bind_find(mode_now(a), a->pending, a->npending, &prefix);

        if (prefix && !b) {                        // wait for the next key
            a->pending_at = g_chord_timeout > 0 ? now_sec() + g_chord_timeout : 0;
            return;
        }
        if (b) {
            a->npending = 0;
            a->cur_ev = ev;
            ACTION_TAB[b->act].fn(a, b);
            a->cur_ev = NULL;
            return;
        }
        // No match. Drop the prefix and try this key on its own - which is what
        // the old code did when `gj` fell out of the gg branch straight into the
        // scroll switch. One retry, so it cannot loop.
        if (a->npending <= 1) break;
        a->npending = 0;
    }

    a->npending = 0;
    mode_fallback(a, ev);
}
```

`mode_fallback` is the per-mode default from the table in §2.1: `page-key` for
NORMAL/INSERT, append-to-`edit` for PROMPT, `hint-type` for HINT, dismiss for
HELP — plus the `MOD_SUPER` swallow.

A prefix that is waiting must be visible and must expire, so:

- `draw_status` prints the pending sequence at the left of the status line
  (`g-`), next to the `INSERT` badge.
- The main loop's poll wait (`main.c:1221`) gains `|| a->npending` to its
  20 ms-poll condition, so the timeout can actually fire while nothing else is
  happening.
- Default `chord-timeout` is **900 ms**. `0` disables expiry.

`gg` becomes two ordinary table rows and `pending_g` is deleted along with its
scoping bug.

### 2.5 Key normalization (the part that decides whether any of this works)

`src/term.c` produces the same physical chord in different shapes depending on
the encoding, so **one function normalizes both the parsed config and the
incoming event** and lookups are guaranteed to agree:

```c
// A shifted letter is its own key here, never a shift bit: `G` is a binding and
// shift+g is not. term.c already does this for the CSI-u path (term.c:440); the
// legacy path produces it naturally. Doing it once more here is what makes
// `shift-g` in a config file mean the same thing.
static void chord_normalize(Chord *c) {
    if ((c->mods & MOD_SHIFT) && c->key >= 'a' && c->key <= 'z') {
        c->key = (uint16_t)(c->key - 32);
        c->mods &= (uint8_t)~MOD_SHIFT;
    }
    if (c->key >= 'A' && c->key <= 'Z' && (c->mods & MOD_SHIFT))
        c->mods &= (uint8_t)~MOD_SHIFT;
    if ((c->mods & MOD_CTRL) && c->key >= 'A' && c->key <= 'Z')
        c->key = (uint16_t)(c->key + 32);   // ctrl-shift-l is ctrl-L, not ctrl-l
}
```

Note the last rule carefully: under the kitty protocol `ctrl-shift-l` arrives as
`key='l', mods=CTRL|SHIFT`, and term.c uppercases it to `key='L', mods=CTRL`.
So in this app **`ctrl-L` and `ctrl-l` are different bindings**, and the config
spellings `ctrl-shift-l` and `ctrl-L` are the same thing. This must be in the
documentation, because it is genuinely surprising.

**What the terminal can actually deliver** — verified against `term_next`:

| Chord class | Legacy encoding | kitty CSI-u (`\x1b[>1u`, set at `term.c:106`) | Bindable? |
|---|---|---|---|
| plain printable ASCII | byte → `key = c` | text keys stay as text | yes |
| shifted letters | byte `'G'` | `p1='g'` + shift → folded to `'G'` | yes |
| `ctrl` + letter | `0x01..0x1a` → `'a'+c-1` (`term.c:310`) | `p1` + CTRL | yes |
| `ctrl` + digit/punct | **not delivered** (no legacy encoding) | `p1` + CTRL | yes on kitty/Ghostty, **no under tmux** |
| `ctrl-shift` + letter | not delivered | folded to uppercase + CTRL | kitty only |
| `alt` + any | `ESC x` → `key=x`, `MOD_ALT` (`term.c:357`) | `p1` + ALT | yes |
| `cmd` (`MOD_SUPER`) | **impossible** — the legacy encoding has no super bit (`term.c:26`) | `p1` + SUPER, *and only if the terminal is configured to release the chord* | kitty/Ghostty only, per-chord terminal config |
| `shift-tab`, `shift-enter` | `CSI Z` → **dropped**, `term_next` returns `KEY_NONE` | CSI-u with SHIFT | kitty only |
| F1–F4 | `ESC O P..S` | same | yes |
| F5–F12 | `CSI n ~` | same | yes |
| key release / held keys | never | not requested (flag `1` only) | **no** |
| anything at all under tmux | legacy only | tmux does not forward CSI-u | ctrl-letter, alt, plain keys only |

Consequences for the design, all mandatory:

- **No default binding may require `MOD_SUPER`.** The one that exists today
  (`cmd-c`) already needs a line in the user's Ghostty config and is a bonus,
  not a path. Keep it that way; `cmd` is bindable, and `--keys` prints a note
  next to any cmd binding saying it needs terminal cooperation.
- **No default binding may require ctrl+non-letter or shift+special**, because
  those vanish under tmux, which is a first-class target for this app.
- **Key releases do not exist**, so no binding may be "hold to do X".

Two decoder fixes to land in step 1, both small and both making `ev->key`
honest enough to bind against:

```c
// term.c:322 - a multi-byte character reports its lead byte as the key, so
// `é` currently binds as 0xc3. Decode the codepoint instead.
    ev->key = utf8_decode(b, clen);

// term.c:357 - ESC followed by a control byte is alt+ctrl+letter under the
// legacy encoding, and must land on the same (key, mods) the CSI-u path gives.
    if (b[1] < 0x20 && b[1] != 0x1b) {
        ev->key = 'a' + b[1] - 1;
        ev->mods = MOD_ALT | MOD_CTRL;
    } else {
        ev->key = b[1];
        ev->mods = MOD_ALT;
    }
```

Config parsing rejects non-ASCII key names regardless; the fix just stops the
lead byte from *silently* matching a binding it should not.

---

## 3. Config format and location

### 3.1 Location

`~/.config/web/config` — sibling of the existing `~/.config/web/state`, built
with the existing `state_path()` (`main.c:434`), which already honours
`XDG_CONFIG_HOME`. Overridable with `--config PATH`. `state` stays exactly as it
is: it is *state* (zoom, cached user agent), written by the program; `config` is
*configuration*, written by the user, never rewritten by the program.

### 3.2 Format

**Recommendation: line-oriented directives, whitespace-separated, `#` comments.
No sections.**

Rejected: INI with `[normal]` sections (a stray section header silently moves
every following line to the wrong mode — the worst failure mode for a file
people copy fragments out of); JSON (the vendored reader in `util.c` is a
CDP-field scraper, not a parser, and a real one is 400 lines); TOML (same, more).

A flat `bind` line is self-describing, so every line in the file can be pasted
anywhere else in it, and an error is always confined to one line.

```ebnf
file       = { line } ;
line       = [ ws ] [ directive ] [ ws ] [ comment ] EOL ;
comment    = "#" { any-but-EOL } ;
directive  = bind | unbind | set ;

bind       = "bind" ws modes ws keyseq ws action [ ws arg ] ;
unbind     = "unbind" ws modes ws keyseq ;          (* sugar for `... none` *)
set        = "set" ws option ws value ;

modes      = mode { "," mode } ;
mode       = "normal" | "insert" | "prompt" | "hint" | "help" | "any" | "all" ;
             (* "all" expands to every real mode, "any" is the fallback map *)

keyseq     = chord { "," chord } ;                  (* max 3 *)
chord      = { modifier "-" } keyname ;
modifier   = "ctrl" | "alt" | "shift" | "cmd" | "super" ;   (* super = cmd *)
keyname    = printable-ascii | named ;
named      = "space" | "comma" | "minus" | "enter" | "esc" | "tab"
           | "backspace" | "delete" | "up" | "down" | "left" | "right"
           | "home" | "end" | "pgup" | "pgdn" | "f1" ... "f12" ;

action     = <a name from ACTION_TAB> ;
arg        = integer | rest-of-line ;               (* per the action's argkind *)
option     = "vim" | "chord-timeout" | "hint-chars" | "hint-selector"
           | "status" | "autofocus-blur" | "search-url" ;
value      = "on" | "off" | integer | rest-of-line ;
```

**Spelling a key.** Any printable ASCII character stands for itself, so `j`,
`/`, `[`, `?`, `;` are written literally. Case is significant and is how shift
is expressed for letters: `G` is shift-g. Three characters cannot be literal
because the grammar uses them, and get names: `space`, `comma` (`,` separates
chords), `minus` (`-` separates modifiers). Modifiers are long words only —
`c-x`-style abbreviations look like emacs but collide with `c` the key.

Examples: `ctrl-l`, `alt-=`, `g,g`, `y,f`, `cmd-c`, `ctrl-shift-l` (≡ `ctrl-L`),
`space`, `alt-minus`, `f1`.

**Rebinding and unbinding.** Both are appends; the backward scan does the rest.

```
bind normal j       scroll 100        # j now scrolls further
bind normal x       none              # x is unbound: it types an "x" into the page
unbind normal x                       # identical
bind normal ctrl-r  none              # ctrl-r stops reloading; falls to the page
```

There is deliberately **no "clear all defaults" directive**. It would let a
config from an old version silently strip a binding a new version added. Users
who want a blank slate write `bind all <key> none` for the ones they dislike, or
copy `web --keys --config /dev/null` output and edit it.

**Parse errors never prevent startup.** A bad line is skipped and recorded:

- The line number, the text and the reason go to `term_log` (the `WEB_DEBUG`
  trace).
- The count and the first error are shown with `notify()` on startup:
  `config: 2 errors, first at line 14: unknown action "scrol"`.
- `web --check-config` prints every error to stderr and exits non-zero — the
  thing to run after editing.

Rationale: this is a browser, not a compiler. A typo in a keybinding must not
cost you the page you were about to read.

**No file is generated on first run.** A generated file is a snapshot of one
version's defaults that then silently pins them forever, and users do not delete
what a program wrote. Instead:

```
web --keys           # the effective map, mode by mode, in `bind` syntax
web --keys > ~/.config/web/config    # explicit opt-in to a frozen copy
```

`--keys` output is valid config input, which is the property that matters.

### 3.3 Worked example

```sh
# ~/.config/web/config
#
# One directive per line. `web --keys` prints the effective map in this syntax;
# `web --check-config` reports errors without starting a browser.

set vim            on          # the vim map on top of the defaults
set chord-timeout  900         # ms to wait for the rest of a sequence; 0 = forever
set hint-chars     asdfghjkl   # label alphabet, home row
set autofocus-blur on          # a page that steals focus on load does not get it
set search-url     https://duckduckgo.com/?q=%s

# ---- reading -------------------------------------------------------------
bind normal e            scroll -60       # e/n instead of k/j, dvorak-ish
bind normal n            scroll 60
bind normal j            none             # ...and j types a j again
bind normal k            none
bind normal ctrl-d       scroll-frac 500
bind normal ctrl-u       scroll-frac -500
bind normal ctrl-f       scroll-frac 900
bind normal ctrl-b       scroll-frac -900

# ---- links ---------------------------------------------------------------
bind normal f            hint 0           # follow
bind normal F            hint 1           # open in a new tab
bind normal y,f          hint 2           # copy a link's url

# ---- bookmarks: `open` takes anything the address bar takes ---------------
bind normal g,h          open news.ycombinator.com
bind normal g,m          open https://mail.google.com
bind normal g,s          open lobste.rs
bind normal g,?          open "why is the sky blue"    # no dot, no scheme: a search

# ---- a snippet, for a site that needs one --------------------------------
bind normal g,r          js document.querySelectorAll('[data-ad]').forEach(e=>e.remove())

# ---- prompt line ---------------------------------------------------------
bind prompt ctrl-w       prompt-erase 2   # delete a word
bind prompt ctrl-a       none             # not bound; nothing happens

# ---- everywhere ----------------------------------------------------------
bind any    ctrl-q       quit
bind any    alt-f        fit-width
bind any    f1           help
bind any    cmd-c        copy             # needs the Ghostty keybind; see README

# ---- send a key the app would otherwise eat, straight to the page --------
bind insert ctrl-l       page-key
```

### 3.4 Parser shape

Roughly 240 lines in a new `src/keys.c`. No allocation beyond the two `Buf`s.

```c
static int key_name_lookup(const char *s) {   // "pgup" -> KEY_PGDN etc, -1 = no
    static const struct { const char *n; int k; } NAMES[] = {
        {"space", ' '}, {"comma", ','}, {"minus", '-'},
        {"esc", KEY_ESC}, {"enter", KEY_ENTER}, {"tab", KEY_TAB},
        {"backspace", KEY_BACKSPACE}, {"delete", KEY_DELETE},
        {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
        {"home", KEY_HOME}, {"end", KEY_END}, {"pgup", KEY_PGUP}, {"pgdn", KEY_PGDN},
        {"f1", KEY_F1},  /* ... f12 */
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
        if (!strcmp(NAMES[i].n, s)) return NAMES[i].k;
    return -1;
}

// "ctrl-shift-l" -> {'L', MOD_CTRL}. Returns 0 on success, -1 with *why set.
static int chord_parse(const char *s, Chord *out, const char **why) {
    int mods = 0;
    for (;;) {
        const char *dash = strchr(s, '-');
        if (!dash || dash == s) break;              // a bare "-" is the key
        size_t n = (size_t)(dash - s);
        if      (!strncmp(s, "ctrl", n)  && n == 4) mods |= MOD_CTRL;
        else if (!strncmp(s, "alt", n)   && n == 3) mods |= MOD_ALT;
        else if (!strncmp(s, "shift", n) && n == 5) mods |= MOD_SHIFT;
        else if ((!strncmp(s, "cmd", n)  && n == 3) ||
                 (!strncmp(s, "super", n)&& n == 5)) mods |= MOD_SUPER;
        else break;                                 // not a modifier: it's the key
        s = dash + 1;
    }
    int k = key_name_lookup(s);
    if (k < 0) {
        if (s[0] && !s[1] && (unsigned char)s[0] >= 0x20 && (unsigned char)s[0] < 0x7f)
            k = (unsigned char)s[0];
        else { *why = "unknown key name"; return -1; }
    }
    out->key  = (uint16_t)k;
    out->mods = (uint8_t)mods;
    chord_normalize(out);
    return 0;
}

static int seq_parse(char *s, Chord *out, int *n, const char **why);   // splits on ','
static int mode_parse(char *s, uint8_t *mask, const char **why);       // splits on ','
static int action_parse(const char *s);                                // index in ACTION_TAB

// The whole loader. Defaults are already in g_binds; this appends.
int keys_load(const char *path, Buf *errs) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;                       // no config is not an error
    char line[1024];
    int lineno = 0, nerr = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        char *hash = strchr(p, '#');
        if (hash) *hash = 0;
        /* tokenize on whitespace with strsep-ish walking, then: */
        /*   "bind"   -> mode_parse, seq_parse, action_parse, arg by argkind */
        /*   "unbind" -> same, action = ACT_NONE */
        /*   "set"    -> opt_set */
        /* every failure: buf_addf(errs, "line %d: %s\n", lineno, why); nerr++; */
    }
    fclose(f);
    return nerr;
}
```

A `bind` line with a mode list expands to one `Binding` per mode — the array is
the only representation, and `--keys` re-collapses nothing. That keeps lookup
trivial at the cost of a few extra rows.

---

## 4. Vim mode

### 4.1 What turns it on

`set vim on` in the config, or `--vim` on the command line (which wins). The
implementation is one more default table appended after `DEFAULTS` and before
the user's config, so the precedence chain is:

```
DEFAULTS  <  VIM_DEFAULTS (if enabled)  <  ~/.config/web/config  <  --bind on argv
```

This means a vim-mode user can still unbind anything vim mode added, and a
plain user is bit-for-bit unaffected. `--bind "normal f hint 0"` as a one-shot
argv override is four lines of code once the parser exists; include it.

### 4.2 The vim keymap

The existing default map is already substantially vim-flavoured (`j k d u gg G
/ n N i esc`), so vim mode is mostly *additions* plus the vimium vocabulary that
people actually have in their fingers. Additions only — nothing in the default
map is removed, so a user who turns vim mode on does not lose a key.

| Key | Action | Note |
|---|---|---|
| `f` | `hint 0` | follow a link |
| `F` | `hint 1` | open a link in a new tab |
| `y,f` | `hint 2` | copy a link's url |
| `y,y` | `copy` | copy selection or url |
| `o` | `url-bar 0` | address bar, prefilled (= `ctrl-l`) |
| `O` | `url-bar 1` | address bar, empty |
| `:` | `url-bar 1` | the command line people reach for |
| `H` | `history -1` | back |
| `L` | `history 1` | forward |
| `ctrl-o` / `ctrl-i` | `history -1` / `history 1` | the vim jumplist reflex |
| `r` | `reload 0` | |
| `R` | `reload 1` | ignore cache |
| `ctrl-d` / `ctrl-u` | `scroll-frac ±500` | as well as `d`/`u` |
| `ctrl-f` / `ctrl-b` | `scroll-frac ±900` | as well as `space`/`b` |
| `g,i` | `focus-input` | focus the first text field, entering insert |
| `g,g` / `G` | top / bottom | unchanged |
| `?` | `help` | as well as `f1` |
| `esc` | `normal` | blur, dismiss hints, drop a pending chord |
| `Z,Z` | `quit` | |

`h`/`l` are deliberately **left unbound**, against vim habit: in a browser they
are far more useful as horizontal scroll (a future action) than as
character-motion, and vimium's `H`/`L` is the convention people already carry
for history. Documented in `--keys` help text.

Conflicts to know about, all in vim mode only, all listed in the README:
`r` and `o` and `f` currently type themselves into the page; a site with its own
single-key shortcuts (Gmail, GitHub) will stop receiving them in normal mode.
That is the whole point of the mode, and `i` is one keystroke away.

### 4.3 Coexistence with `insert`

**Nothing changes.** Vim mode edits `MODE_NORMAL` and nothing else. The moment
the page reports a typable element has focus, `__webmode` sets `a->insert`,
`mode_now()` returns `MODE_INSERT`, and the only bindings searched are
`MODE_INSERT` (empty by default) and `MODE_ANY` (the ctrl/alt chords). So `f`
types an `f` into the search box, exactly as today.

### 4.4 Escaping back out

Three exits, in order of how often they are wanted:

1. `esc` → `act_normal`, which is the existing blur plus the new housekeeping:

```c
static void act_normal(App *a, const Binding *b) {
    a->npending = 0;
    if (a->hint.active) { hint_cancel(a); return; }
    if (a->helping)     { help_close(a);  return; }
    if (a->insert) {
        run_js(a, "document.activeElement&&document.activeElement.blur()");
        a->insert = false;
        return;
    }
    // Not in any mode: esc belongs to the page (closing its own dialogs).
    special_key(a, KEY_ESC, 0);
}
```

   The last line preserves today's Gate G behavior: `esc` outside insert mode is
   forwarded to the page.

2. `set autofocus-blur on` (default **on** in vim mode, **off** otherwise) adds
   a blur on `Page.loadEventFired`, so a search-box-focusing homepage does not
   drop you into insert mode before you have pressed anything. This is the
   single most common complaint about keyboard browsers and it is four lines.

3. `ctrl-q` still quits from any mode, from `MODE_ANY`, and `pump_input`'s raw
   `0x03`/`0x11` scan still quits mid-frame regardless of the keymap (see §6.3).

---

## 5. Link labels / keyboard-only navigation

### 5.1 Where the labels are drawn — the decision

Two candidates, and the terminal one is more viable than it first looks, so it
gets a fair hearing.

**Option A — draw the labels in the terminal, over the image.**
The page occupies placeholder cells (`kitty.c:117`); writing ordinary text into
one replaces the placeholder, so the terminal stops substituting pixels there
and shows the character. Contrary to the obvious worry, *this is stable*: later
frames only re-send image data and re-assert the placement (`kitty.c:174`), and
`draw_grid` runs only when `grid_dirty`, so an overwritten cell stays overwritten
until we choose to restore it. Restoring is cheap and needs **no new frame at
all** — the image is still in the terminal's cache under the same id, so
re-emitting just those placeholder cells brings the pixels back.

- Pros: zero page mutation, immune to CSP and to hostile page CSS, no CDP round
  trip to draw, no extra screencast frames, works identically under tmux.
- Cons: labels land on the cell grid, so they are up to 8×17 CSS px off-centre
  from their link; a 2-character label needs two adjacent cells and collides
  with its neighbours on a dense page (a Hacker News comment page has 30 links
  inside 20 rows); the label is drawn at terminal font size, which on a 20-row
  inline window is enormous next to the page text; and it punches a hole in the
  picture rather than sitting over it, so you lose the thing you are aiming at.

**Option B — paint the labels into the DOM, so they arrive inside the
screencast frame.**

- Pros: labels are positioned in the page's own coordinate space, scale with the
  page, can dim non-matching labels and highlight the live one, and can never
  collide with the cell grid. All the fiddly work happens in JS, which is where
  everything else fiddly in this app already happens (`scroll_at`, `find_next`,
  `FOCUS_WATCHER`).
- Cons: mutates the page; ~one CDP round trip plus one screencast frame of
  latency to show, one more to hide, one per filtering keystroke; must survive
  the page's own CSS and CSP; must be cleaned up on navigation and on crash.

**Recommendation: Option B.** The dense-page collision problem is fatal for
Option A — link labels are for pages with many links, which is exactly where
cell-grid labels stop being legible. Every one of Option B's costs has a
concrete mitigation below, and each is a mitigation this codebase already uses
somewhere.

(Keep Option A's insight though: the "restore placeholder cells without a new
frame" trick is exactly how the `?` help overlay is drawn in §6.2.)

### 5.2 The three costs, worked through

**Page mutation.** One container element appended to `document.documentElement`
(not `body` — a React root mounted on `body` reconciles children away). It
carries `position:fixed`, `pointer-events:none`, `z-index:2147483647`, and holds
an **open shadow root**, so no page selector, however `!important`, can reach
the labels and no page `MutationObserver` that reads `.children` sees anything
but one anonymous div. Removal is `container.remove()`. Nothing else in the
document is touched — no classes added to links, no attributes, no scroll
position changes.

**CSS isolation and CSP.** Two rules make this safe:

- Never insert a `<style>` element and never assign `element.setAttribute
  ("style", ...)`; both are subject to CSP `style-src` / `style-src-attr` on a
  strict site. Set every property through `el.style.setProperty(...)`, which is
  CSSOM and is not CSP-checked.
- Never load an external resource (no fonts, no images, no data URLs).
  `font-family: monospace` is the system default and costs nothing.

`Runtime.evaluate` itself is not blocked by `script-src`: the debugger's
evaluation bypasses page CSP, which is already how `FOCUS_WATCHER` works on
CSP'd sites today.

*Hardening follow-up (step 7):* move the script into a named isolated world so
the page cannot see `window.__webHint` at all and CSP is out of the picture
entirely:

```c
app_cdp(a, "Runtime.addBinding",
        "\"name\":\"__webhint\",\"executionContextName\":\"web-hints\"");
app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
        "\"source\":\"%s\",\"worldName\":\"web-hints\",\"runImmediately\":true", esc);
```

then track `Runtime.executionContextCreated` events whose `name` is `web-hints`
and pass that `contextId` to every `Runtime.evaluate`. `Runtime.enable` is
already on (`main.c:1167`), so the events already arrive. Worth doing, but it is
not needed for the feature to work and it adds a piece of state that must be
re-acquired on every navigation — hence a separate step.

**Frames.** A hint session costs: 1 frame to show, 1 per filtering keystroke
that changes what is dimmed, 1 to hide. Three or four full-page PNGs for a
navigation that replaces a mouse hunt. Acceptable, and the existing ack-based
backpressure (`main.c:891`) means they cannot pile up. Do not try to save the
dimming frame; the dimming is what makes a 2-character label usable.

### 5.3 Getting the script in

`run_js`'s `char esc[2048]` (`main.c:559`) cannot carry this script. Add a
heap-based path to `util.c` / `main.c`:

```c
// util.c
size_t json_escape_buf(Buf *dst, const char *src);   // same rules, grows

// main.c
static void run_js_long(App *a, const char *js) {
    Buf b = {0};
    json_escape_buf(&b, js);
    app_cdp(a, "Runtime.evaluate", "\"expression\":\"%s\"", b.p);
    buf_free(&b);
}
```

`cdp_vcall` already sizes its output with `vsnprintf(NULL, 0, ...)`
(`cdp.c:400`), so an arbitrarily long params string is fine.

The script is installed exactly like `FOCUS_WATCHER` (`main.c:1185`): once via
`Page.addScriptToEvaluateOnNewDocument` so every future document has it, and
once via `Runtime.evaluate` for the document already loaded. Per-invocation
calls are then tiny and fit `run_js` unchanged:

```c
static void act_hint(App *a, const Binding *b) {
    char js[128];
    snprintf(js, sizeof js, "__webHint.show(%d,'%s')", b->argn, g_hint_chars);
    run_js(a, js);
    a->hint.active = true;
    a->hint.kind   = b->argn;
    a->hint.deadline = now_sec() + 1.5;   // no answer by then: drop back to normal
}
```

### 5.4 The injected script

```js
// HINT_JS - installed once per document, like FOCUS_WATCHER.
(function () {
    if (window.__webHint) return;

    var SEL = 'a[href],button,input:not([type=hidden]),select,textarea,summary,' +
              'details,label[for],[role=button],[role=link],[role=checkbox],' +
              '[role=radio],[role=tab],[role=menuitem],[onclick],[contenteditable=""],' +
              '[contenteditable=true],[tabindex]:not([tabindex="-1"])';

    var box = null, labels = [], typed = '', kind = 0;

    function px(el, k, v) { el.style.setProperty(k, v, 'important'); }

    // Every candidate in this document and in any open shadow root under it.
    // Closed roots and cross-origin iframes are not reachable and are the two
    // known blind spots; same-origin iframes are recursed with their rect added.
    function collect(root, dx, dy, out) {
        var all = root.querySelectorAll(SEL);
        for (var i = 0; i < all.length; i++) out.push([all[i], dx, dy]);
        var hosts = root.querySelectorAll('*');
        for (var j = 0; j < hosts.length; j++) {
            if (hosts[j].shadowRoot) collect(hosts[j].shadowRoot, dx, dy, out);
        }
        var frames = root.querySelectorAll('iframe');
        for (var f = 0; f < frames.length; f++) {
            try {
                var d = frames[f].contentDocument;
                if (!d) continue;
                var r = frames[f].getBoundingClientRect();
                collect(d, dx + r.left, dy + r.top, out);
            } catch (e) { /* cross-origin: nothing to do */ }
        }
    }

    // Visible means: inside the viewport, big enough to aim at, not painted
    // over. The hit test is what handles cookie banners, sticky headers and
    // modal overlays - a rect alone says nothing about what is on top of it.
    function pickable(el, dx, dy) {
        var r = el.getBoundingClientRect();
        var x = r.left + dx, y = r.top + dy;
        if (r.width < 3 || r.height < 3) return null;
        if (x + r.width <= 0 || y + r.height <= 0) return null;
        if (x >= innerWidth || y >= innerHeight) return null;

        var cs = getComputedStyle(el);
        if (cs.visibility === 'hidden' || cs.display === 'none') return null;
        if (parseFloat(cs.opacity) < 0.05) return null;

        var cx = Math.min(Math.max(x + r.width / 2, 1), innerWidth - 1);
        var cy = Math.min(Math.max(y + r.height / 2, 1), innerHeight - 1);
        var hit = document.elementFromPoint(cx, cy);
        while (hit && hit.shadowRoot) {              // descend into shadow roots
            var deeper = hit.shadowRoot.elementFromPoint(cx, cy);
            if (!deeper || deeper === hit) break;
            hit = deeper;
        }
        if (!hit) return null;
        if (hit !== el && !el.contains(hit) && !hit.contains(el)) {
            // Fall back to the top-left corner: a wide link whose middle is
            // covered by an image overlay is still clickable at its edge.
            cx = Math.min(Math.max(x + 4, 1), innerWidth - 1);
            cy = Math.min(Math.max(y + Math.min(r.height / 2, 8), 1), innerHeight - 1);
            hit = document.elementFromPoint(cx, cy);
            if (!hit || (hit !== el && !el.contains(hit) && !hit.contains(el))) return null;
        }
        return { el: el, x: cx, y: cy, top: y, left: x };
    }

    // Prefix-free labels, short ones first. With k characters and n targets,
    // reserve t = ceil((n-k)/(k-1)) characters as prefixes: the remaining k-t
    // stand alone and the t prefixes carry k labels each, for k + t(k-1) >= n.
    // No standalone label is then a prefix of any two-character one. Recurses
    // for a third character on pages with more than k*k links.
    function makeLabels(n, chars) {
        var k = chars.length;
        if (n <= k) {
            var one = [];
            for (var i = 0; i < n; i++) one.push(chars[i]);
            return one;
        }
        var t = Math.ceil((n - k) / (k - 1));
        if (t > k) t = k;
        var out = [], i2;
        for (i2 = t; i2 < k; i2++) out.push(chars[i2]);          // standalone
        var need = n - out.length;
        var per = makeLabels(Math.ceil(need / t), chars);         // tails
        for (var p = 0; p < t && out.length < n; p++) {
            for (var q = 0; q < per.length && out.length < n; q++)
                out.push(chars[p] + per[q]);
        }
        out.sort(function (a, b) { return a.length - b.length; });
        return out;
    }

    function build(k, chars) {
        clear();
        kind = k;
        typed = '';

        var raw = [];
        collect(document, 0, 0, raw);
        var hits = [];
        for (var i = 0; i < raw.length; i++) {
            var h = pickable(raw[i][0], raw[i][1], raw[i][2]);
            if (h) hits.push(h);
        }
        // Reading order, so the same link keeps roughly the same label between
        // invocations and the short labels land near the top of the page.
        hits.sort(function (a, b) { return (a.top - b.top) || (a.left - b.left); });

        var names = makeLabels(hits.length, chars.split(''));
        box = document.createElement('div');
        px(box, 'position', 'fixed');
        px(box, 'inset', '0');
        px(box, 'pointer-events', 'none');
        px(box, 'z-index', '2147483647');
        var sh = box.attachShadow({ mode: 'open' });
        document.documentElement.appendChild(box);

        labels = [];
        for (var j = 0; j < hits.length; j++) {
            var t = document.createElement('span');
            t.textContent = names[j];
            px(t, 'position', 'fixed');
            px(t, 'left', Math.max(0, hits[j].left - 4) + 'px');
            px(t, 'top', Math.max(0, hits[j].top - 4) + 'px');
            px(t, 'font', 'bold 11px/1.1 monospace');
            px(t, 'padding', '1px 3px');
            px(t, 'color', '#000');
            px(t, 'background', '#fbe14d');
            px(t, 'border', '1px solid #a08000');
            px(t, 'border-radius', '3px');
            px(t, 'text-transform', 'uppercase');
            px(t, 'letter-spacing', '1px');
            sh.appendChild(t);
            labels.push({ name: names[j], node: t, hit: hits[j] });
        }
        report('ready\t' + labels.length);
    }

    function type(ch) {
        typed += ch;
        var live = 0, exact = null;
        for (var i = 0; i < labels.length; i++) {
            var m = labels[i].name.indexOf(typed) === 0;
            px(labels[i].node, 'display', m ? 'block' : 'none');
            if (m) { live++; if (labels[i].name === typed) exact = labels[i]; }
        }
        if (!live) { cancel(); return; }
        if (exact && live === 1) activate(exact);
    }

    function activate(l) {
        var href = l.hit.el.getAttribute('href');
        var abs = '';
        try { abs = href ? new URL(href, location.href).href : ''; } catch (e) {}
        var x = Math.round(l.hit.x), y = Math.round(l.hit.y);
        clear();                       // off the page before the click lands
        report('click\t' + x + '\t' + y + '\t' + kind + '\t' + abs);
    }

    function erase() {
        if (!typed) { cancel(); return; }
        typed = typed.slice(0, -1);
        for (var i = 0; i < labels.length; i++)
            px(labels[i].node, 'display',
               labels[i].name.indexOf(typed) === 0 ? 'block' : 'none');
    }

    function clear() { if (box) box.remove(); box = null; labels = []; typed = ''; }
    function cancel() { clear(); report('cancel'); }
    function report(s) { try { __webhint(s); } catch (e) {} }

    window.__webHint = { show: build, type: type, back: erase, cancel: cancel };
})();
```

Label alphabet default: `asdfghjkl` — nine home-row characters, no `;` (awkward
to type on non-US layouts), no `g`/`h` collision worries since hints swallow
every key while they are up. Configurable with `set hint-chars`. Nine characters
gives 9 single-character labels, 81 at two, 729 at three — a viewport never
needs the third.

### 5.5 Activation: a real click, dispatched from C

The script does **not** call `el.click()`. `el.click()` skips hover state, skips
focus side effects, does not count as a user gesture for popup blocking, and is
a well-known source of "works on some sites" bugs with React and with anchors
whose handlers listen for `pointerdown`.

Instead the script reports the target's viewport centre and C dispatches a real
mouse event through the same path `handle_mouse` uses — Chrome does its own hit
testing, and everything downstream (focus, `:active`, gesture-gated
`window.open`) behaves exactly as if the user had clicked:

```c
// on_cdp_message, next to the existing __webmode branch (main.c:927)
if (strstr(msg, "Runtime.bindingCalled") && strstr(msg, "__webhint")) {
    size_t n;
    const char *p = json_str(msg, "payload", &n);
    char pl[2048];
    json_unescape(pl, sizeof pl, p ? p : "", p ? n : 0);
    hint_reply(a, pl);
    return;
}

// Tab-separated rather than JSON: one unescape and a walk, against two rounds
// of unescaping to get a url back out of a nested JSON string.
static void hint_reply(App *a, char *pl) {
    char *tab = strchr(pl, '\t');
    if (tab) *tab++ = 0;

    if (!strcmp(pl, "ready")) {
        int n = tab ? atoi(tab) : 0;
        a->hint.deadline = 0;
        if (n == 0) { a->hint.active = false; notify(a, "no links in view"); }
        return;
    }
    if (!strcmp(pl, "cancel")) { a->hint.active = false; return; }
    if (strcmp(pl, "click") != 0) return;

    int x = 0, y = 0, kind = 0;
    char href[1024] = {0};
    char *f[4] = {0};
    for (int i = 0; i < 4 && tab; i++) { f[i] = tab; tab = strchr(tab, '\t'); if (tab) *tab++ = 0; }
    if (f[0]) x = atoi(f[0]);
    if (f[1]) y = atoi(f[1]);
    if (f[2]) kind = atoi(f[2]);
    if (f[3]) snprintf(href, sizeof href, "%s", f[3]);
    a->hint.active = false;

    if (kind == 2) {                       // copy the link
        clipboard_put(href[0] ? href : a->url);
        notify(a, href[0] ? "copied link" : "copied url");
        return;
    }
    if (kind == 1 && href[0]) { open_new_tab(a, href); return; }

    // Same tab: a hover first, then press and release, exactly as handle_mouse
    // would have produced them.
    app_cdp(a, "Input.dispatchMouseEvent",
            "\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d,\"button\":\"none\",\"buttons\":0", x, y);
    app_cdp(a, "Input.dispatchMouseEvent",
            "\"type\":\"mousePressed\",\"x\":%d,\"y\":%d,\"button\":\"left\","
            "\"buttons\":1,\"clickCount\":1", x, y);
    app_cdp(a, "Input.dispatchMouseEvent",
            "\"type\":\"mouseReleased\",\"x\":%d,\"y\":%d,\"button\":\"left\","
            "\"buttons\":0,\"clickCount\":1", x, y);
    a->expect_frame = now_sec() + 2.0;
}
```

A click on an `<input>` or `<textarea>` focuses it, `__webmode` fires, and the
app is in insert mode — **no special case needed**, which is the payoff for
dispatching a real click.

### 5.6 New tab vs same tab

This app attaches to exactly one page target (`chrome_attach`, `cdp.c:304`) and
screencasts it. "Open in a new tab" therefore has three honest shapes:

1. **`window.open(url)` from the page.** Creates a target the app is not
   attached to and can never show. From the user's seat, `F` does nothing. Also
   needs `Runtime.evaluate` with `"userGesture":true` to get past the popup
   blocker. **Reject** — a tab you cannot reach is a bug, not a feature.
2. **`Target.createTarget` + tab switching.** Keep a list of target ids, add
   `gt` / `gT` / `x` to cycle and close, and re-attach the websocket to the
   selected target's debugger url. Re-attaching means a second `ws_connect` and
   re-running the per-session setup (`Page.enable`, `Runtime.enable`,
   `Runtime.addBinding`, both `addScriptToEvaluateOnNewDocument` calls,
   `setScrollbarsHidden`, `relayout`). That is a genuine feature — call it
   `chrome_attach_target(Chrome*, const char *targetId)` and factor the session
   setup out of `main()` into `session_setup(App*)` so it can be re-run.
3. **`F` copies the link instead**, and tabs come later.

**Recommendation: ship 3 first, then 2 as its own step.** In the first hint
release, `hint 1` is documented as "open in a new tab (requires tabs, see
`--keys`)" and, until step 8 lands, behaves as `hint 2` with a
`notify(a, "no tabs yet - link copied")`. When step 8 lands, `open_new_tab`
becomes `Target.createTarget` plus a switch to it, and the default vim binding
`F` starts doing what it says. Nothing in the keymap changes at that point,
which is the property that makes it a clean split.

### 5.7 Hint mode lifecycle

| Event | Handling |
|---|---|
| `hint N` pressed | `__webHint.show(N, chars)`; `hint.active = true`; deadline `now+1.5s` |
| `ready 0` | not active; `notify("no links in view")` |
| any character | `MODE_HINT` fallback → `__webHint.type('c')` |
| `backspace` | `hint-erase` → `__webHint.back()` |
| `esc` | `hint-cancel` → `__webHint.cancel()`, mode drops immediately in C |
| unique match | JS removes the overlay, reports `click`, C dispatches the mouse |
| no match left | JS cancels itself |
| navigation (`Page.frameNavigated`) | C clears `hint.active`; the overlay went with the document |
| deadline passes with no `ready` | C clears `hint.active`, `notify("hints unavailable")` |

The deadline matters: a page whose main world is wedged (an infinite loop in a
page script) would otherwise leave the app in a mode that swallows every key.
The main loop already wakes on `expect_frame`; add `hint.deadline` to the same
`wait` condition at `main.c:1221`.

Every character typed while hints are up is swallowed — none reach the page.
That is deliberate: half-typed label characters leaking into a search box is
the single most annoying failure mode of this feature elsewhere.

---

## 6. Migration and defaults

### 6.1 The default keymap is today's keymap

`DEFAULTS[]` is written to reproduce §1.1 line for line. With no config file it
is the entire map, so a user who never writes one sees no change whatsoever.

```c
// keys.c - one macro so -Wextra's missing-field-initializers has nothing to say
#define B1(m, k, md, act, n)      { MODE_##m, 1, ACT_##act, {{ (k), (md) }}, (n), -1 }
#define B2(m, k1, k2, act, n)     { MODE_##m, 2, ACT_##act, {{ (k1), 0 }, { (k2), 0 }}, (n), -1 }

static const Binding DEFAULTS[] = {
    // --- everywhere (was Gate B and Gate C) -------------------------------
    B1(ANY,    'q',           MOD_CTRL,  QUIT,        0),
    B1(ANY,    'c',           MOD_CTRL,  QUIT,        0),
    B1(ANY,    'l',           MOD_CTRL,  URL_BAR,     0),
    B1(ANY,    'g',           MOD_CTRL,  STATS,       0),
    B1(ANY,    'y',           MOD_CTRL,  COPY,        0),
    B1(ANY,    'r',           MOD_CTRL,  RELOAD,      0),
    B1(ANY,    'o',           MOD_CTRL,  HISTORY,    -1),
    B1(ANY,    'p',           MOD_CTRL,  HISTORY,     1),
    B1(ANY,    'c',           MOD_SUPER, COPY,        0),
    B1(ANY,    'f',           MOD_ALT,   FIT,         0),
    B1(ANY,    '=',           MOD_ALT,   ZOOM,        1),
    B1(ANY,    '+',           MOD_ALT,   ZOOM,        1),
    B1(ANY,    '-',           MOD_ALT,   ZOOM,       -1),
    B1(ANY,    '_',           MOD_ALT,   ZOOM,       -1),
    B1(ANY,    '0',           MOD_ALT,   ZOOM,        0),
    B1(ANY,    KEY_F1,        0,         HELP,        0),

    // --- reading (was Gate D) ---------------------------------------------
    B1(NORMAL, KEY_DOWN,      0,         SCROLL,      40),
    B1(NORMAL, KEY_UP,        0,         SCROLL,     -40),
    B1(NORMAL, KEY_LEFT,      0,         HISTORY,     -1),
    B1(NORMAL, KEY_RIGHT,     0,         HISTORY,      1),
    B1(NORMAL, 'j',           0,         SCROLL,      60),
    B1(NORMAL, 'k',           0,         SCROLL,     -60),
    B1(NORMAL, 'd',           0,         SCROLL_FRAC, 500),
    B1(NORMAL, 'u',           0,         SCROLL_FRAC,-500),
    B1(NORMAL, ' ',           0,         SCROLL_FRAC, 900),
    B1(NORMAL, 'b',           0,         SCROLL_FRAC,-900),
    B2(NORMAL, 'g', 'g',                 GOTO_TOP,    0),
    B1(NORMAL, 'G',           0,         GOTO_BOT,    0),
    B1(NORMAL, '[',           0,         RESIZE,     -1),
    B1(NORMAL, ']',           0,         RESIZE,      1),
    B1(NORMAL, 'w',           0,         WIDTH,       1),
    B1(NORMAL, 'W',           0,         WIDTH,      -1),
    B1(NORMAL, 's',           0,         SCALE,       0),
    B1(NORMAL, 'n',           0,         FIND_NEXT,   0),
    B1(NORMAL, 'N',           0,         FIND_NEXT,   1),
    B1(NORMAL, '/',           0,         FIND,        0),
    B1(NORMAL, 'i',           0,         INSERT,      0),

    // --- leaving insert (was Gate E) ---------------------------------------
    B1(INSERT, KEY_ESC,       0,         NORMAL,      0),

    // --- the prompt line (was Gate A) --------------------------------------
    B1(PROMPT, KEY_ENTER,     0,         P_ACCEPT,    0),
    B1(PROMPT, KEY_ESC,       0,         P_CANCEL,    0),
    B1(PROMPT, 'g',           MOD_CTRL,  P_CANCEL,    0),
    B1(PROMPT, KEY_BACKSPACE, 0,         P_ERASE,     1),
    B1(PROMPT, 'u',           MOD_CTRL,  P_ERASE,     3),

    // --- hints (new mode, harmless until `hint` is bound) ------------------
    B1(HINT,   KEY_ESC,       0,         H_CANCEL,    0),
    B1(HINT,   KEY_BACKSPACE, 0,         H_ERASE,     0),
};
```

Two deliberate deviations from the letter of today's code, both improvements
that cannot be observed as regressions:

- `d`/`u`/`space`/`b` become `SCROLL_FRAC 500 / -500 / 900 / -900` instead of
  `css_h/2` and `css_h*0.9`. `css_h * 500 / 1000` and `css_h / 2` differ only by
  integer rounding on odd heights.
- `esc` in normal mode reaches the page through `act_normal`'s tail rather than
  through Gate G's `special_key`. Same call, same result.

Everything else, including which keys fall through to the page, is preserved by
the fallback table in §2.1.

### 6.2 Telling the user what is bound

**Status line** (`draw_status`, `main.c:285`) — three changes:

1. The hint string stops being a literal and is generated once at startup from
   the live map: look up the chord bound to `URL_BAR`, `HISTORY -1`, `RELOAD`,
   `QUIT` and format them with the same `key_spell()` the config parser is the
   inverse of. A user who rebinds `ctrl-l` sees their own key in the corner.
2. A pending chord prefix renders at the left, next to the `INSERT` badge:
   `g-`. Without it, an armed prefix is invisible state.
3. `HINT` renders as a badge like `INSERT` does, in a different colour.

**Help overlay** — yes, `?` (vim mode) / `f1` (always), and it has the same
rendering question as link labels but **the opposite answer**: the help text is
*ours*, not the page's, and it does not need to align with anything on the page.
So draw it in the terminal, using the Option A insight from §5.1:

```c
// The picture stays in the terminal's cache under the same image id, so writing
// text over the placeholder cells hides it and rewriting those cells brings it
// straight back - no new frame, no round trip to Chrome.
void kitty_repaint_cells(Kitty *k);     // draw_grid + flush, exposed in web.h

static void help_open(App *a) {
    a->helping = true;
    a->help_top = 0;
    help_paint(a);            // plain text rows inside the kitty rect
}

static void help_close(App *a) {
    a->helping = false;
    kitty_repaint_cells(&a->kitty);
    a->status_last.len = 0;
}
```

`help_paint` writes `\x1b[row;colH` + a cleared line + a formatted binding per
row, inside `kitty.x/y/cols/rows`, iterating `g_binds` grouped by mode and
skipping shadowed entries. In `MODE_HELP` the bindings are `j`/`k`/`space` to
scroll `help_top` and anything else to close. When the box has fewer than ~10
rows, `help_open` instead does `notify(a, "window too small - run: web --keys")`.

`web --keys` prints the same content to stdout in `bind` syntax and exits — the
surface that works regardless of window size, and doubles as the starting point
for a config file.

### 6.3 What stays outside the keymap

`pump_input` (`main.c:134`) scans raw bytes for `0x11`/`0x03` while a frame is
being written and quits. It cannot consult the keymap without running the whole
dispatcher re-entrantly in the middle of a tty write, which is exactly the
reentrancy that function's comment exists to avoid. **Keep it hardcoded, and
document it**: `ctrl-c` and `ctrl-q` always quit, even if rebound. It is a
safety hatch on a program that can be occupied for a quarter of a second at a
time by a frame, and users of terminal programs expect `ctrl-c` to work.

---

## 7. Implementation plan

Eight steps. Each is independently shippable and leaves the app working.

### Step 1 — Actions, modes, and the dispatcher (no config file yet)
**~400 lines net. The whole behavioural risk of the project is here.**
- New `src/keys.c`, new declarations in `src/web.h`.
- `handle_key` shrinks to `keys_dispatch(a, ev)` plus `mode_fallback`.
- Every `case` becomes an `act_*` handler; existing helpers (`scroll_by`,
  `cycle_width`, `zoom_by`, `find_next`, `copy_selection`, `nav_history`,
  `resize_box`, `cycle_scale`) are called unchanged.
- `App` gains `Chord pending[3]; int npending; double pending_at; Event *cur_ev;`
  and loses `pending_g`.
- `term.c` fixes from §2.5 (utf-8 key, alt+ctrl).
- Touches: `handle_key`, `App`, `main()` (nothing yet), `term_next`.
- **Verification is the whole job**: this must be a pure refactor. Drive it with
  `tools/ptycap` — inject each key from §1.1, diff the `WEB_DEBUG` CDP trace
  against the same run on the previous commit. A byte-identical CDP trace is the
  acceptance criterion.
- Risk: the fallthrough cases. `ctrl-a` (unbound ctrl chord → page), `pgup`
  (→ page), an unbound printable (→ `send_char`), `cmd-x` (swallowed). Those
  four are the ones a naive rewrite breaks.

### Step 2 — The chord engine and its surface
**~90 lines.**
- `bind_find` prefix detection, the timeout, the retry-on-failure rule.
- `main()` poll wait gains `|| a.npending`.
- `draw_status` renders the pending prefix.
- Touches: `keys.c`, `draw_status`, the main loop's `wait` expression.
- Ships `gg` as data. Delete `pending_g` and its bug.

### Step 3 — The config file
**~260 lines, all new, plus ~40 in `main()`.**
- `chord_parse`, `seq_parse`, `mode_parse`, `keys_load`, `key_spell` (the
  inverse, for `--keys`).
- `--config PATH`, `--check-config`, `--keys`, `--bind "..."`.
- Error collection into a `Buf`, first error through `notify()`.
- Touches: `main()` argv loop, `usage()`, `state_path` (reused for the dir).
- Risk: low. Self-contained, and a broken parse cannot break the defaults.

### Step 4 — Vim mode
**~70 lines, nearly all table.**
- `VIM_DEFAULTS[]`, `set vim on`, `--vim`, `autofocus-blur`.
- Touches: `keys.c` table, `main()` argv, `Page.loadEventFired` handler.
- Depends on step 8 for `F` to do anything useful; ships with the copy-url
  fallback from §5.6.

### Step 5 — Help
**~130 lines.**
- `MODE_HELP`, `help_paint`, `kitty_repaint_cells` exported from `kitty.c`,
  the generated status hint.
- Touches: `kitty.c` (`draw_grid` gets a public wrapper), `draw_status`,
  `keys.c`.
- Risk: the restore path. Verify under tmux, in `--full`, and after a resize
  while help is up (a `SIGWINCH` during `MODE_HELP` must close it — `relayout`
  moves the rect out from under the painted rows).

### Step 6 — Link hints
**~120 lines of C, ~200 lines of JS. The riskiest step.**
- `json_escape_buf` in `util.c`, `run_js_long` in `main.c`.
- `HINT_JS` installed alongside `FOCUS_WATCHER`; `Runtime.addBinding __webhint`.
- `MODE_HINT`, `act_hint`, `hint_reply`, the deadline in the poll wait.
- Clear `hint.active` in the `Page.frameNavigated` handler.
- Touches: `main()` setup block (`main.c:1185`), `on_cdp_message`, `keys.c`,
  `util.c`, `web.h`.
- Risks, in order:
  1. **Site coverage.** The selector and the occlusion test are where this
     feature is won or lost. Test against: Hacker News (dense text links),
     GitHub (shadow-DOM-free but heavy ARIA), Google results (overlay-heavy),
     a page with a cookie banner (the hit test's whole purpose), Gmail (nested
     scrollers), and any site with a `position:sticky` header.
  2. **Escaping.** The script contains `'`, `"`, `\` and newlines and goes
     through `json_escape` into a JSON string inside a `Runtime.evaluate`. A
     single missed escape produces a silent no-op. Keep the JS free of
     backslashes (no regex literals — the version above has none, deliberately).
  3. **Cleanup on the abnormal paths.** Navigation mid-hint, a crash mid-hint
     (nothing to do — the document goes), a resize mid-hint (the labels are
     fixed-positioned in the *old* viewport; `relayout` must cancel hints).
  4. **The frame cost** on a huge page. Measure with `ctrl-g`.

### Step 7 — Isolated world hardening
**~50 lines.**
- `worldName` on `addScriptToEvaluateOnNewDocument`, `executionContextName` on
  `addBinding`, track `Runtime.executionContextCreated`, pass `contextId` to
  every hint `Runtime.evaluate`.
- Touches: `main()` setup, `on_cdp_message`, `act_hint`.
- Risk: the contextId must be re-acquired on every navigation, and a hint fired
  before it arrives must degrade rather than fail. Guard with the existing
  deadline.

### Step 8 — Tabs, and `hint 1` for real
**~180 lines.**
- `session_setup(App*)` factored out of `main()`; `chrome_attach_target()` in
  `cdp.c`; a target list; `Target.createTarget`, `Target.closeTarget`,
  `Target.setDiscoverTargets`; actions `tab-new`, `tab-next`, `tab-prev`,
  `tab-close`; vim bindings `gt`, `gT`, `x`.
- Touches: `cdp.c` (`chrome_attach` splits in two), `main()`, `keys.c`,
  `draw_status` (a tab count).
- Risk: the websocket swap. `ws_close` + `ws_connect` mid-session while frames
  are in flight; the screencast must be restarted and `kitty.grid_dirty` set.
  The existing unwedge watchdog (`main.c:1251`) is a useful safety net here.

**Suggested order to ship in:** 1 → 2 → 3 → 5 → 6 → 4 → 7 → 8. Help before hints
because it is easy and it makes the config file discoverable; vim mode after
hints because half its value is `f`/`F`.

---

## 8. Decision summary

| Question | Decision | Why |
|---|---|---|
| Action representation | X-macro → enum + `{name, fn, argkind, help}` table | one line per action keeps name, handler and help from drifting |
| Handler signature | `void (*)(App *, const Binding *)` | three actions need a string *and* a number |
| Binding storage | flat array, backward scan, later wins | override/unbind for free; 70 entries is not a hash table problem |
| Chord depth | 3, fixed array | covers `g,g` and `y,f` with room; 6 bytes |
| Failed chord | drop the prefix, retry the last key alone | preserves today's `gj` behavior; cannot loop |
| Chord timeout | 900 ms default, `0` = never | a prefix that never expires is invisible stuck state |
| Modes | derived from existing fields, not stored | the page's focus reports must not clobber an open prompt |
| Mode composition | current mode, then `MODE_ANY`, except PROMPT/HINT/HELP | reproduces today's gate order exactly |
| Config format | flat `bind`/`unbind`/`set` lines, `#` comments | one line = one fact; no section can silently swallow the rest of the file |
| Config location | `~/.config/web/config` | user convention; `state` stays program-owned |
| Key spelling | literal ASCII, case-significant, `ctrl-`/`alt-`/`shift-`/`cmd-`, `,` joins chords | matches what `term.c` can actually deliver |
| Parse errors | skip the line, keep going, report via `notify` + `--check-config` | it is a browser, not a compiler |
| Generated config | **no** — `--keys` prints it on demand | a generated file silently pins one version's defaults |
| Vim mode | additive overlay table, `--vim` / `set vim on` | nothing is removed, so nobody loses a key |
| `h`/`l` in vim mode | unbound; `H`/`L` are history | vimium convention, and h/l are wanted for horizontal scroll |
| Hint rendering | DOM injection, shadow root, `style.setProperty` only | cell-grid labels collide on exactly the pages hints are for |
| Hint isolation | main world first, isolated world as step 7 | main world matches `FOCUS_WATCHER` and works today |
| Hint activation | JS reports coordinates, C dispatches a real mouse event | trusted click: focus, hover, gesture gating all behave |
| Hint labels | prefix-free mixed length, `asdfghjkl`, reading order | short labels where they are needed, stable between invocations |
| New tab | copy-url stand-in now, `Target.createTarget` + tab switching in step 8 | a tab you cannot reach is a bug |
| Help overlay | terminal text over the placeholder cells, restored by rewriting them | the image is cached in the terminal; restore needs no frame at all |
| `ctrl-c` / `ctrl-q` | always quit, not rebindable, documented | `pump_input` runs mid-frame and must stay re-entrancy-free |

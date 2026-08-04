/**
 * repl.h - Full-featured UTF-8 line editor with completion dropdown
 *
 * Self-contained input widget modeled on a modern shell prompt:
 * - Line editing: insert, codepoint/word cursor motion, emacs kill bindings
 * - Command history (up/down when the dropdown is closed)
 * - A completion dropdown filtered by the typed command name, navigated with
 *   the arrow keys and accepted with Right/Enter
 *
 * SINGLE-HEADER, stb-style. Depends on libc only. In exactly ONE .c file:
 *
 *     #define REPL_IMPLEMENTATION
 *     #include "repl.h"
 *
 * and include "repl.h" plainly wherever else you need the declarations.
 *
 * The widget owns no terminal/screen/color state: the host translates its own
 * key events into ReplEvent and supplies a ReplDraw callback that paints cells
 * however it likes (see repl_handle_input / repl_render). It owns no
 * application state either; the embedding UI feeds it ReplEvents and reads back
 * submitted lines.
 */

#ifndef REPL_H
#define REPL_H

#include <stdbool.h>
#include <stdint.h>

#define REPL_HISTORY_MAX  200
#define REPL_MAX_COMMANDS 64
#define REPL_MAX_VISIBLE  6     // max dropdown rows shown at once
#define REPL_UNDO_MAX     64    // bounded undo snapshot stack
#define REPL_CAND_TEXT    192   // max bytes of a completion's insert-text
#define REPL_CAND_DESC    96    // max bytes of a completion's description

// Static command-registry entry. `name` includes the leading slash (e.g.
// "/spawn"); `args` is an optional usage hint shown as ghost text once the
// command name is fully typed (e.g. "<secs> <cmd>"); either desc/args may be NULL.
typedef struct {
    const char *name;
    const char *desc;
    const char *args;
} ReplCommand;

// One dropdown candidate: the text inserted in place of the active token, plus a
// one-line description. Slash commands fill these from the registry; an @-file
// completer (see ReplCompleter) fills them from the host.
typedef struct {
    char text[REPL_CAND_TEXT];
    char desc[REPL_CAND_DESC];
} ReplCandidate;

// Host callback supplying @-file (or other token) completions. `token` is the
// active word with its leading '@' stripped. Fill up to `max` candidates and
// return the count. NULL when the host offers no file completion.
typedef int (*ReplCompleter)(void *ctx, const char *token,
                             ReplCandidate *out, int max);

// ----------------------------------------------------------------------------
// Input contract — the host maps its own terminal keys onto these. Control
// codes (Ctrl-A/E/W/U/K/C, etc.) arrive as REPL_KEY_CHAR with a codepoint < 0x20
// and are interpreted as emacs bindings inside the widget, so the keymap stays
// with the widget rather than the host.
// ----------------------------------------------------------------------------
typedef enum {
    REPL_KEY_CHAR,        // insert/​interpret ev.codepoint
    REPL_KEY_TEXT,        // insert ev.text (a UTF-8 run, e.g. a paste)
    REPL_KEY_ENTER,
    REPL_KEY_NEWLINE,     // insert a literal '\n' (Shift+Enter / Ctrl-J)
    REPL_KEY_BACKSPACE,
    REPL_KEY_LEFT,
    REPL_KEY_RIGHT,
    REPL_KEY_UP,
    REPL_KEY_DOWN,
    REPL_KEY_WORD_LEFT,
    REPL_KEY_WORD_RIGHT,
    REPL_KEY_ESCAPE,
} ReplKey;

typedef struct {
    ReplKey     key;
    uint32_t    codepoint;   // for REPL_KEY_CHAR (a full Unicode codepoint)
    const char *text;        // for REPL_KEY_TEXT (borrowed; the widget copies)
} ReplEvent;

// Result of feeding one event to the editor.
typedef enum {
    REPL_CONSUMED,   // handled; caller should redraw
    REPL_SUBMIT,     // Enter on a complete line; read via repl_line() then repl_reset()
    REPL_UNFOCUS,    // ESC with dropdown closed; caller should return focus to the map
} ReplResult;

// ----------------------------------------------------------------------------
// Render contract — repl_render emits one cell at a time through this callback.
// `style` is a semantic role the host maps onto its own colors; REPL_STYLE_CURSOR
// marks the single cell under the cursor (the host typically swaps fg/bg).
// ----------------------------------------------------------------------------
typedef enum {
    REPL_STYLE_PROMPT,    // the "> " prompt prefix
    REPL_STYLE_TYPED,     // typed input text (focused)
    REPL_STYLE_DIM,       // unfocused text, dropdown descriptions, ghost hints
    REPL_STYLE_CURSOR,    // the cell under the cursor
    REPL_STYLE_SELECTED,  // the highlighted dropdown row
} ReplStyle;

typedef void (*ReplDraw)(void *ctx, int x, int y, uint32_t cp, ReplStyle style);

typedef struct {
    char *buf;                   // heap, NUL-terminated UTF-8; grows on demand
    int   len;                   // bytes used (excl. NUL)
    int   cap;                   // bytes allocated
    int   cursor;                // 0..len byte index, always on a codepoint boundary

    // History ring (oldest..newest), browsed when the dropdown is closed.
    char *history[REPL_HISTORY_MAX];   // heap strings
    int   hist_count;
    int   hist_pos;              // -1 = editing live buffer; else 0=newest, 1=older, ...
    char *stash;                // live buffer saved while browsing history

    // Completion dropdown. Candidates are recomputed from the token under the
    // cursor: a leading '/' (at column 0) filters the command registry; a leading
    // '@' calls the host completer. accept replaces [tok_start,tok_end).
    const ReplCommand *commands; // borrowed pointer to a static table (may be NULL)
    int   command_count;
    ReplCompleter completer;     // borrowed; @-file completions (may be NULL)
    void *completer_ctx;
    ReplCandidate cands[REPL_MAX_COMMANDS];
    int   cand_count;
    int   tok_start, tok_end;    // byte range of the active token being completed
    bool  cand_is_command;       // true: slash command (append a space on accept)
    int   sel;                   // index into cands[]; -1 = nothing highlighted
    bool  dropdown_open;

    // Reverse-incremental history search (Ctrl-R). While active, input edits the
    // query and the matched history entry is previewed; Enter/motion accepts it,
    // Esc cancels back to the live line saved in `stash`.
    bool  searching;
    char *search_query;          // heap, NUL-terminated
    int   sq_len, sq_cap;
    int   search_idx;            // history index of the current match, -1 = none

    // Kill register (Ctrl-W/U/K fill it; Ctrl-Y yanks it back).
    char *kill;

    // Undo: bounded stack of pre-edit snapshots. Consecutive same-kind edits at
    // the same spot coalesce into one step (typing a word undoes at once).
    struct { char *text; int cursor; } undo[REPL_UNDO_MAX];
    int   undo_count;
    int   undo_anchor;           // cursor of the current edit-run; -1 = none
    int   undo_kind;             // 0 structural, 1 insert, 2 delete
} Repl;

void        repl_init(Repl *r, const ReplCommand *commands, int command_count);
// Register an @-file completer (borrowed callback + ctx). Optional.
void        repl_set_completer(Repl *r, ReplCompleter fn, void *ctx);
void        repl_free(Repl *r);               // release heap (buf/stash/history)

// The usage hint to show as ghost text after the input, or NULL. Non-NULL when
// the first token is a fully-typed command name followed by a space.
const char *repl_arg_hint(const Repl *r);

// The inline history autosuggestion (the unmatched tail of a prior entry), or
// NULL. repl_render draws it as ghost text; Right-arrow at end-of-line accepts it.
const char *repl_suggestion(const Repl *r);
void        repl_reset(Repl *r);              // clear line/cursor/dropdown, keep history

// Append a line to the command history (as if submitted) — for restoring a saved
// per-input history. No-op on NULL/empty; dedups against the most recent entry.
void        repl_history_add(Repl *r, const char *line);
ReplResult  repl_handle_input(Repl *r, const ReplEvent *ev);
const char *repl_line(const Repl *r);

// Insert a block of text at the cursor (e.g. a paste). Newlines are kept (the
// input is word-wrapped on render); tabs become spaces; other control bytes are
// dropped. The buffer grows to fit.
void        repl_insert_text(Repl *r, const char *s);

// Number of dropdown rows currently shown (0 when closed) — for panel sizing.
int  repl_dropdown_rows(const Repl *r);

// Number of visual input rows once the buffer is word-wrapped to `width`
// columns (the same width passed to repl_render) — for sizing the input block.
int  repl_input_rows(const Repl *r, int width);

// Zero-based visual row containing the cursor at the same wrap width.
int  repl_cursor_row(const Repl *r, int width);

// Draw the input block starting at (x,y): the input line(s) at y .. and, when
// focused & open, the completion dropdown directly below them. The total height
// is repl_input_rows() + (focused ? repl_dropdown_rows() : 0). Cells are emitted
// through `draw(ctx, ...)`; the host clips/paints as it sees fit.
void repl_render(const Repl *r, int x, int y, int width, bool focused,
                 ReplDraw draw, void *ctx);

#endif // REPL_H

// ============================================================================
//   IMPLEMENTATION
//   Define REPL_IMPLEMENTATION in exactly one .c file before including this.
// ============================================================================
#ifdef REPL_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <limits.h>

// ----------------------------------------------------------------------------
// UTF-8 helpers (inlined from utf8.h). The UTF8_H guard is kept so this compiles
// cleanly even if the host also vendors the original utf8.h alongside it.
// ----------------------------------------------------------------------------
#ifndef UTF8_H
#define UTF8_H

// Decode one UTF-8 codepoint from text[*pos .. len), advancing *pos past it.
// Returns 0 at end of input and '?' for a malformed lead/continuation byte
// (advancing past the offending byte so callers always make progress).
static inline uint32_t utf8_decode(const char *text, int len, int *pos) {
    if (*pos >= len) return 0;
    uint8_t b = (uint8_t)text[*pos];
    uint32_t cp; int extra;
    if (b < 0x80)      { cp = b; extra = 0; }
    else if (b < 0xC0) { (*pos)++; return '?'; }   // stray continuation byte
    else if (b < 0xE0) { cp = b & 0x1F; extra = 1; }
    else if (b < 0xF0) { cp = b & 0x0F; extra = 2; }
    else               { cp = b & 0x07; extra = 3; }
    (*pos)++;
    for (int i = 0; i < extra && *pos < len; i++, (*pos)++) {
        if (((uint8_t)text[*pos] & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | ((uint8_t)text[*pos] & 0x3F);
    }
    return cp;
}

// Byte offset of the next codepoint boundary at/after `pos` (clamped to len).
static inline int utf8_next(const char *text, int len, int pos) {
    if (pos >= len) return len;
    int p = pos;
    utf8_decode(text, len, &p);
    return p;
}

// Byte offset of the previous codepoint boundary before `pos` (clamped to 0).
static inline int utf8_prev(const char *text, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((uint8_t)text[pos] & 0xC0) == 0x80) pos--;  // skip continuation bytes
    return pos;
}

// Encode `cp` as UTF-8 into `out` (needs <= 4 bytes, not NUL-terminated).
// Returns the number of bytes written.
static inline int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

#endif // UTF8_H

// ----------------------------------------------------------
// Buffer growth
// ----------------------------------------------------------

// Ensure buf can hold at least `need` bytes plus a NUL. Grows geometrically.
// Returns false (leaving buf/cap unchanged) on overflow or allocation failure.
static bool buf_ensure(Repl *r, int need) {
    if (need < 0 || need > INT_MAX - 1) return false;
    need += 1;  // NUL
    if (r->cap >= need) return true;
    int cap = r->cap ? r->cap : 64;
    while (cap < need) {
        if (cap > INT_MAX / 2) { cap = need; break; }   // avoid signed-int overflow
        cap *= 2;
    }
    char *nb = realloc(r->buf, (size_t)cap);
    if (!nb) return false;                               // old buf/cap stay valid
    r->buf = nb;
    r->cap = cap;
    return true;
}

static void undo_clear(Repl *r);   // (defined in the Undo section)

// ----------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------

void repl_init(Repl *r, const ReplCommand *commands, int command_count) {
    memset(r, 0, sizeof(*r));
    r->commands = commands;
    r->command_count = command_count;
    r->hist_pos = -1;
    r->sel = -1;
    r->undo_anchor = -1;
    if (buf_ensure(r, 0)) r->buf[0] = '\0';   // on OOM buf stays NULL; guarded elsewhere
}

void repl_set_completer(Repl *r, ReplCompleter fn, void *ctx) {
    r->completer = fn;
    r->completer_ctx = ctx;
}

void repl_free(Repl *r) {
    free(r->buf);
    free(r->stash);
    free(r->search_query);
    free(r->kill);
    for (int i = 0; i < r->hist_count; i++) free(r->history[i]);
    for (int i = 0; i < r->undo_count; i++) free(r->undo[i].text);
    memset(r, 0, sizeof(*r));
}

void repl_reset(Repl *r) {
    if (r->buf) r->buf[0] = '\0';
    r->len = 0;
    r->cursor = 0;
    r->hist_pos = -1;
    r->sel = -1;
    r->dropdown_open = false;
    r->cand_count = 0;
    r->searching = false;
    r->sq_len = 0;
    if (r->search_query) r->search_query[0] = '\0';
    undo_clear(r);   // a fresh line starts a fresh undo history (kill persists)
}

const char *repl_line(const Repl *r) {
    return r->buf;
}

static char *dupstr(const char *s);        // (defined in the History section)
static void  recompute_candidates(Repl *r);
static void  load_buf(Repl *r, const char *line);

// ----------------------------------------------------------
// Undo + kill register
// ----------------------------------------------------------

static void undo_push_state(Repl *r) {
    if (r->undo_count == REPL_UNDO_MAX) {
        free(r->undo[0].text);
        memmove(&r->undo[0], &r->undo[1], sizeof(r->undo[0]) * (REPL_UNDO_MAX - 1));
        r->undo_count--;
    }
    char *snap = dupstr(r->buf);
    if (!snap) return;                  // can't snapshot on OOM -> skip this undo step
    r->undo[r->undo_count].text = snap;
    r->undo[r->undo_count].cursor = r->cursor;
    r->undo_count++;
}

// Call BEFORE a mutation. kind: 1 insert, 2 delete, 0 structural (always a new
// step). A snapshot is pushed unless this continues a same-kind run at the same
// cursor (so contiguous typing/deleting collapses to one undo).
static void undo_mark(Repl *r, int kind) {
    bool contiguous = kind != 0 && kind == r->undo_kind && r->cursor == r->undo_anchor;
    if (!contiguous) undo_push_state(r);
    r->undo_kind = kind;
}

// Call AFTER the mutation so the run's anchor tracks the new cursor.
static void undo_anchor_here(Repl *r) { r->undo_anchor = r->cursor; }

static void undo_clear(Repl *r) {
    for (int i = 0; i < r->undo_count; i++) free(r->undo[i].text);
    r->undo_count = 0;
    r->undo_anchor = -1;
    r->undo_kind = 0;
}

static void set_kill(Repl *r, int from, int to) {
    if (to <= from) return;
    int n = to - from;
    char *nk = malloc((size_t)n + 1);
    if (!nk) return;                    // keep the previous kill register on OOM
    free(r->kill);
    r->kill = nk;
    memcpy(r->kill, r->buf + from, (size_t)n);
    r->kill[n] = '\0';
}

// ----------------------------------------------------------
// Completion filter
// ----------------------------------------------------------

static bool is_sep(char c) { return c == ' ' || c == '\n'; }

// Length of the first whitespace-delimited token in buf.
static int first_token_len(const Repl *r) {
    int i = 0;
    while (i < r->len && !is_sep(r->buf[i])) i++;
    return i;
}

// The whitespace-delimited token under the cursor, as a byte range [*ts,*te).
static void active_token(const Repl *r, int *ts, int *te) {
    int s = r->cursor;
    while (s > 0 && !is_sep(r->buf[s - 1])) s--;
    int e = r->cursor;
    while (e < r->len && !is_sep(r->buf[e])) e++;
    *ts = s; *te = e;
}

// Case-insensitive subsequence score (higher = better), or -1 if `q` is not a
// subsequence of `name`. Rewards contiguous runs and a match at the start.
static int fuzzy_score(const char *name, const char *q, int qlen) {
    int score = 0, ni = 0, streak = 0;
    for (int qi = 0; qi < qlen; qi++) {
        char qc = (char)tolower((unsigned char)q[qi]);
        bool found = false;
        while (name[ni]) {
            char nc = (char)tolower((unsigned char)name[ni]);
            ni++;
            if (nc == qc) {
                streak++;
                score += 1 + streak;
                if (ni == 1) score += 5;   // matched the very first char
                found = true;
                break;
            }
            streak = 0;
        }
        if (!found) return -1;
    }
    return score;
}

// Recompute the dropdown candidates from the token under the cursor: a leading
// '/' at column 0 fuzzy-filters the command registry; a leading '@' asks the
// host completer; anything else closes the dropdown.
static void recompute_candidates(Repl *r) {
    r->cand_count = 0;
    r->sel = -1;
    r->dropdown_open = false;
    r->cand_is_command = false;

    int ts, te;
    active_token(r, &ts, &te);
    r->tok_start = ts;
    r->tok_end = te;
    if (te <= ts) return;

    char first = r->buf[ts];
    if (first == '/' && ts == 0 && r->commands) {
        int qlen = te - ts;
        // Score every command, then insertion-sort indices by descending score.
        int idx[REPL_MAX_COMMANDS], score[REPL_MAX_COMMANDS], n = 0;
        for (int i = 0; i < r->command_count && n < REPL_MAX_COMMANDS; i++) {
            int sc = fuzzy_score(r->commands[i].name, r->buf + ts, qlen);
            if (sc < 0) continue;
            int j = n++;
            while (j > 0 && score[j - 1] < sc) {
                score[j] = score[j - 1]; idx[j] = idx[j - 1]; j--;
            }
            score[j] = sc; idx[j] = i;
        }
        for (int k = 0; k < n; k++) {
            const ReplCommand *c = &r->commands[idx[k]];
            snprintf(r->cands[k].text, REPL_CAND_TEXT, "%s", c->name);
            snprintf(r->cands[k].desc, REPL_CAND_DESC, "%s", c->desc ? c->desc : "");
        }
        r->cand_count = n;
        r->cand_is_command = true;
    } else if (first == '@' && r->completer) {
        char partial[REPL_CAND_TEXT];
        int pl = te - ts - 1;
        if (pl < 0) pl = 0;
        if (pl > REPL_CAND_TEXT - 1) pl = REPL_CAND_TEXT - 1;
        memcpy(partial, r->buf + ts + 1, (size_t)pl);
        partial[pl] = '\0';
        int n = r->completer(r->completer_ctx, partial, r->cands, REPL_MAX_COMMANDS);
        if (n < 0) n = 0;
        if (n > REPL_MAX_COMMANDS) n = REPL_MAX_COMMANDS;
        r->cand_count = n;
    } else {
        return;
    }
    if (r->cand_count > 0) r->dropdown_open = true;
}

// The usage hint shown as ghost text, or NULL: non-NULL only when the first
// token exactly matches a command name, a space follows, and no argument has
// been typed yet.
const char *repl_arg_hint(const Repl *r) {
    if (!r->commands || r->searching) return NULL;
    int tl = first_token_len(r);
    if (tl == 0 || tl >= r->len || r->buf[tl] != ' ') return NULL;
    for (int i = tl; i < r->len; i++)
        if (r->buf[i] != ' ') return NULL;     // an argument was started
    for (int i = 0; i < r->command_count; i++)
        if ((int)strlen(r->commands[i].name) == tl &&
            strncmp(r->commands[i].name, r->buf, (size_t)tl) == 0)
            return r->commands[i].args;
    return NULL;
}

// The inline autosuggestion: the tail of the most recent history entry that the
// current buffer is a prefix of, or NULL. Only offered with the cursor at the
// end of a non-empty live line and no dropdown/search in the way.
const char *repl_suggestion(const Repl *r) {
    if (r->searching || r->dropdown_open) return NULL;
    if (r->len == 0 || r->cursor != r->len || r->hist_pos != -1) return NULL;
    for (int i = r->hist_count - 1; i >= 0; i--) {
        const char *h = r->history[i];
        if ((int)strlen(h) > r->len && strncmp(h, r->buf, (size_t)r->len) == 0)
            return h + r->len;
    }
    return NULL;
}

// ----------------------------------------------------------
// Editing primitives — every mutation re-derives the filter and drops out of
// history-browse mode.
// ----------------------------------------------------------

static void after_edit(Repl *r) {
    r->hist_pos = -1;
    recompute_candidates(r);
}

// Insert `n` bytes at the cursor (assumed a valid UTF-8 run / single codepoint).
static void insert_bytes(Repl *r, const char *bytes, int n) {
    if (n <= 0) return;
    if (r->len > INT_MAX - n || !buf_ensure(r, r->len + n)) return;
    memmove(r->buf + r->cursor + n, r->buf + r->cursor, (size_t)(r->len - r->cursor));
    memcpy(r->buf + r->cursor, bytes, (size_t)n);
    r->cursor += n;
    r->len += n;
    r->buf[r->len] = '\0';
}

static void insert_codepoint(Repl *r, uint32_t cp) {
    char enc[4];
    int n = utf8_encode(cp, enc);
    undo_mark(r, 1);
    insert_bytes(r, enc, n);
    undo_anchor_here(r);
    after_edit(r);
}

void repl_insert_text(Repl *r, const char *s) {
    if (!s) return;
    undo_mark(r, 0);   // a paste is one structural undo step
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char ch = *p;
        if (ch == '\r') {
            if (p[1] == '\n') continue;   // CRLF -> let the '\n' insert the break
            ch = '\n';                    // lone CR -> newline
        } else if (ch == '\t') {
            ch = ' ';
        }
        // Keep newlines and printable bytes (incl. UTF-8 continuation bytes);
        // drop other control characters.
        if (ch != '\n' && ch < 32) continue;
        char c = (char)ch;
        insert_bytes(r, &c, 1);
    }
    undo_anchor_here(r);
    after_edit(r);
}

static void delete_range(Repl *r, int from, int to) {
    if (from < 0) from = 0;
    if (to > r->len) to = r->len;
    if (from >= to) return;
    undo_mark(r, 2);
    int gap = to - from;
    memmove(r->buf + from, r->buf + to, (size_t)(r->len - to));
    r->len -= gap;
    r->cursor = from;
    r->buf[r->len] = '\0';
    undo_anchor_here(r);
    after_edit(r);
}

// Scan left from cursor over trailing separators then a run of non-separators.
static int word_left(const Repl *r) {
    int c = r->cursor;
    while (c > 0 && is_sep(r->buf[c - 1])) c--;
    while (c > 0 && !is_sep(r->buf[c - 1])) c--;
    return c;
}

// Scan right from cursor over a run of non-separators then trailing separators.
static int word_right(const Repl *r) {
    int c = r->cursor;
    while (c < r->len && !is_sep(r->buf[c])) c++;
    while (c < r->len && is_sep(r->buf[c])) c++;
    return c;
}

// ----------------------------------------------------------
// Multi-line cursor helpers (lines are separated by '\n')
// ----------------------------------------------------------

// Index of the first char of the line containing `pos`.
static int line_start(const Repl *r, int pos) {
    while (pos > 0 && r->buf[pos - 1] != '\n') pos--;
    return pos;
}

// Index of the line terminator ('\n') at/after `pos`, or len if last line.
static int line_end(const Repl *r, int pos) {
    while (pos < r->len && r->buf[pos] != '\n') pos++;
    return pos;
}

// Move the cursor up/down one logical line, preserving column where possible.
// Returns false when already on the first/last line (caller falls back to history).
static bool cursor_line_move(Repl *r, int dir) {  // dir: -1 up, +1 down
    int ls = line_start(r, r->cursor);
    int col = r->cursor - ls;
    if (dir < 0) {
        if (ls == 0) return false;                  // already on first line
        int prev_ls = line_start(r, ls - 1);
        int prev_len = (ls - 1) - prev_ls;          // bytes before the '\n'
        r->cursor = prev_ls + (col < prev_len ? col : prev_len);
    } else {
        int le = line_end(r, r->cursor);
        if (le >= r->len) return false;             // already on last line
        int next_ls = le + 1;
        int next_len = line_end(r, next_ls) - next_ls;
        r->cursor = next_ls + (col < next_len ? col : next_len);
    }
    // Snap onto a codepoint boundary (col counts bytes; a multi-line edit can
    // land mid-sequence on the destination line).
    while (r->cursor > 0 && ((unsigned char)r->buf[r->cursor] & 0xC0) == 0x80)
        r->cursor = utf8_prev(r->buf, r->cursor);
    return true;
}

// ----------------------------------------------------------
// Display width / word wrap
// ----------------------------------------------------------

// Display columns a codepoint occupies (control/unknown -> 1; we never store
// control bytes other than '\n', which the wrapper handles separately).
static int glyph_cols(uint32_t cp) {
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

// Display width of buf[from..to).
static int disp_width(const char *buf, int from, int to) {
    int w = 0, i = from;
    while (i < to) {
        uint32_t cp = utf8_decode(buf, to, &i);
        w += glyph_cols(cp);
    }
    return w;
}

// Wrap the whole buffer into visual rows for a text area `text_cols` wide (the
// columns left after the 2-char prefix). When `seg` is non-NULL, fills seg[k]
// with the byte offset where visual row k begins (seg must hold `seg_cap`
// entries). Returns the row count (always >= 1). A '\n' forces a new row; an
// over-long line breaks after the last space, or mid-word when a single word
// exceeds the width.
static int wrap_segments(const Repl *r, int text_cols, int *seg, int seg_cap) {
    if (text_cols < 1) text_cols = 1;
    int n = 0;
    if (seg && seg_cap > 0) seg[n] = 0;
    n = 1;
    int row_begin = 0;      // byte where the current visual row starts
    int col = 0;            // display column within the current row
    int last_break = -1;    // byte just after the last space on this row, or -1
    int i = 0;
    while (i < r->len) {
        int start = i;
        uint32_t cp = utf8_decode(r->buf, r->len, &i);
        if (cp == '\n') {
            if (seg && n < seg_cap) seg[n] = i;    // next row starts after the newline
            n++;
            row_begin = i; col = 0; last_break = -1;
            continue;
        }
        int w = glyph_cols(cp);
        if (col + w > text_cols && col > 0) {      // placing this glyph would overflow
            int brk = (last_break > row_begin) ? last_break : start;
            if (seg && n < seg_cap) seg[n] = brk;
            n++;
            row_begin = brk;
            col = disp_width(r->buf, brk, start);  // chars carried onto the new row
            last_break = -1;
        }
        col += w;
        if (cp == ' ') last_break = i;             // break point is after the space
    }
    return n;
}

// ----------------------------------------------------------
// History
// ----------------------------------------------------------

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void history_push(Repl *r, const char *line) {
    if (line[0] == '\0') return;
    // Skip if identical to the most recent entry.
    if (r->hist_count > 0 && strcmp(r->history[r->hist_count - 1], line) == 0)
        return;
    if (r->hist_count == REPL_HISTORY_MAX) {
        free(r->history[0]);
        memmove(&r->history[0], &r->history[1],
                sizeof(r->history[0]) * (REPL_HISTORY_MAX - 1));
        r->hist_count--;
    }
    char *d = dupstr(line);
    if (!d) return;                     // drop this history entry on OOM
    r->history[r->hist_count++] = d;
}

void repl_history_add(Repl *r, const char *line) {
    if (line && line[0]) history_push(r, line);
}

static void load_buf(Repl *r, const char *line) {
    int n = (int)strlen(line);
    if (!buf_ensure(r, n)) return;
    memcpy(r->buf, line, (size_t)n + 1);
    r->len = n;
    r->cursor = n;
    recompute_candidates(r);
}

// hist_pos: -1 live buffer, 0 newest, increasing = older.
static void history_browse(Repl *r, int dir) {  // dir: +1 older (Up), -1 newer (Down)
    if (r->hist_count == 0) return;

    if (dir > 0) {  // Up
        if (r->hist_pos == -1) {
            free(r->stash);
            r->stash = dupstr(r->buf);   // save the live line
            r->hist_pos = 0;
        } else if (r->hist_pos < r->hist_count - 1) {
            r->hist_pos++;
        } else {
            return;
        }
    } else {  // Down
        if (r->hist_pos <= -1) return;
        r->hist_pos--;
        if (r->hist_pos == -1) {
            load_buf(r, r->stash ? r->stash : "");
            r->hist_pos = -1;       // back to the live line
            r->dropdown_open = false;  // keep history browsing free of the dropdown
            r->sel = -1;
            return;
        }
    }
    int pos = r->hist_pos;  // hist_pos>=0 here
    load_buf(r, r->history[r->hist_count - 1 - pos]);
    // load_buf re-derives the filter; suppress the dropdown so repeated Up/Down
    // keep walking history instead of being captured by the completion list.
    r->dropdown_open = false;
    r->sel = -1;
    r->hist_pos = pos;
}

// ----------------------------------------------------------
// Completion acceptance
// ----------------------------------------------------------

// Replace the active token [tok_start,tok_end) with the selected candidate.
// Command completions append a space (ready for arguments); file completions do
// not. The cursor lands just after the inserted text.
static void accept_completion(Repl *r, int idx) {
    if (idx < 0 || idx >= r->cand_count) return;
    undo_mark(r, 0);
    const char *text = r->cands[idx].text;
    int ts = r->tok_start, te = r->tok_end;
    if (ts < 0) ts = 0;
    if (te > r->len) te = r->len;
    if (te < ts) te = ts;

    char *suffix = dupstr(r->buf + te);   // text after the token (incl. any space)
    if (!suffix) return;
    int suffix_len = (int)strlen(suffix);
    int tlen = (int)strlen(text);
    bool add_space = r->cand_is_command && suffix[0] != ' ';

    if (!buf_ensure(r, ts + tlen + (add_space ? 1 : 0) + suffix_len)) { free(suffix); return; }
    int n = ts;
    memcpy(r->buf + n, text, (size_t)tlen); n += tlen;
    if (add_space) r->buf[n++] = ' ';
    int cur = n;
    memcpy(r->buf + n, suffix, (size_t)suffix_len); n += suffix_len;
    r->buf[n] = '\0';
    r->len = n;
    r->cursor = cur;
    free(suffix);

    r->hist_pos = -1;
    undo_anchor_here(r);
    recompute_candidates(r);  // re-derive from the new cursor position
}

// Yank the kill register at the cursor (one structural undo step).
static void yank(Repl *r) {
    if (!r->kill || !r->kill[0]) return;
    undo_mark(r, 0);
    insert_bytes(r, r->kill, (int)strlen(r->kill));
    undo_anchor_here(r);
    after_edit(r);
}

// Restore the most recent pre-edit snapshot.
static void do_undo(Repl *r) {
    if (r->undo_count == 0) return;
    r->undo_count--;
    char *t = r->undo[r->undo_count].text;
    int cur = r->undo[r->undo_count].cursor;
    load_buf(r, t);                       // sets buf/len, cursor=len, recomputes
    r->cursor = (cur <= r->len) ? cur : r->len;
    free(t);
    r->undo_kind = 0;
    r->undo_anchor = -1;
}

// ----------------------------------------------------------
// Reverse-incremental history search (Ctrl-R)
// ----------------------------------------------------------

// Case-insensitive substring test.
static bool contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (const char *p = hay; *p; p++) {
        int i = 0;
        while (needle[i] && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (!needle[i]) return true;
    }
    return false;
}

// Newest-to-oldest scan for an entry containing the query, starting at history
// index `from` and moving older. Returns the index, or -1 if none.
static int history_find(Repl *r, int from) {
    if (!r->search_query || !r->search_query[0]) return from;  // empty query: stay put
    for (int i = from; i >= 0; i--)
        if (contains_ci(r->history[i], r->search_query)) return i;
    return -1;
}

// Returns false (query unchanged) on overflow or allocation failure.
static bool search_query_set_len(Repl *r, int n) {
    if (n < 0 || n > INT_MAX - 1) return false;
    if (r->sq_cap < n + 1) {
        int cap = r->sq_cap ? r->sq_cap : 32;
        while (cap < n + 1) {
            if (cap > INT_MAX / 2) { cap = n + 1; break; }
            cap *= 2;
        }
        char *nq = realloc(r->search_query, (size_t)cap);
        if (!nq) return false;                          // old query stays valid
        r->search_query = nq;
        r->sq_cap = cap;
    }
    r->sq_len = n;
    r->search_query[n] = '\0';
    return true;
}

static void start_search(Repl *r) {
    free(r->stash);
    r->stash = dupstr(r->buf);     // remember the live line to restore on cancel
    r->searching = true;
    r->dropdown_open = false;
    r->sel = -1;
    search_query_set_len(r, 0);
    r->search_idx = -1;
}

// Apply the current match (or the saved live line) to the buffer and leave search.
static void search_finish(Repl *r, bool accept) {
    r->searching = false;
    if (accept && r->search_idx >= 0)
        load_buf(r, r->history[r->search_idx]);
    else if (!accept)
        load_buf(r, r->stash ? r->stash : "");
    // accept with no match: keep whatever the query produced? No — keep live line.
    r->dropdown_open = false;
    r->sel = -1;
    r->hist_pos = -1;
}

// Re-run the search from the newest entry (after the query changed).
static void search_research(Repl *r) {
    r->search_idx = history_find(r, r->hist_count - 1);
}

static ReplResult handle_search(Repl *r, const ReplEvent *ev) {
    switch (ev->key) {
        case REPL_KEY_CHAR: {
            uint32_t cp = ev->codepoint;
            if (cp == 18) {                         // Ctrl-R: step to an older match
                int from = (r->search_idx > 0) ? r->search_idx - 1 : -1;
                int hit = (from >= 0) ? history_find(r, from) : -1;
                if (hit >= 0) r->search_idx = hit;  // else keep the current match
                return REPL_CONSUMED;
            }
            if (cp == 7 || cp == 3) { search_finish(r, false); return REPL_CONSUMED; } // ^G/^C cancel
            if (cp >= 0x20 && cp != 0x7f) {         // extend the query
                char enc[4];
                int n = utf8_encode(cp, enc);
                int old = r->sq_len;
                if (!search_query_set_len(r, old + n)) return REPL_CONSUMED;
                memcpy(r->search_query + old, enc, (size_t)n);
                search_research(r);
            }
            return REPL_CONSUMED;
        }
        case REPL_KEY_TEXT:
            if (ev->text) {
                for (const char *p = ev->text; *p; p++) {
                    if ((unsigned char)*p < 0x20) continue;
                    if (!search_query_set_len(r, r->sq_len + 1)) break;
                    r->search_query[r->sq_len - 1] = *p;
                }
                search_research(r);
            }
            return REPL_CONSUMED;
        case REPL_KEY_BACKSPACE:
            if (r->sq_len > 0) {
                int p = utf8_prev(r->search_query, r->sq_len);
                search_query_set_len(r, p);
                search_research(r);
            }
            return REPL_CONSUMED;
        case REPL_KEY_ENTER:
            search_finish(r, true);     // accept into the buffer; user reviews then submits
            return REPL_CONSUMED;
        case REPL_KEY_ESCAPE:
            search_finish(r, false);
            return REPL_CONSUMED;
        default:
            // Any cursor motion accepts the match, then applies normally.
            search_finish(r, true);
            return repl_handle_input(r, ev);
    }
}

// ----------------------------------------------------------
// Input handling
// ----------------------------------------------------------

// Move the cursor one codepoint left/right.
static void cursor_left(Repl *r)  { r->cursor = utf8_prev(r->buf, r->cursor); }
static void cursor_right(Repl *r) { r->cursor = utf8_next(r->buf, r->len, r->cursor); }

static ReplResult handle_char(Repl *r, uint32_t cp) {
    // Emacs control bindings arrive as low control codes.
    switch (cp) {
        case 1:  r->cursor = line_start(r, r->cursor); return REPL_CONSUMED; // ctrl-a
        case 5:  r->cursor = line_end(r, r->cursor);   return REPL_CONSUMED; // ctrl-e
        case 23: set_kill(r, word_left(r), r->cursor);             // ctrl-w
                 delete_range(r, word_left(r), r->cursor); return REPL_CONSUMED;
        case 21: set_kill(r, 0, r->cursor);                        // ctrl-u
                 delete_range(r, 0, r->cursor);            return REPL_CONSUMED;
        case 11: set_kill(r, r->cursor, r->len);                   // ctrl-k
                 delete_range(r, r->cursor, r->len);       return REPL_CONSUMED;
        case 25: yank(r);             return REPL_CONSUMED;  // ctrl-y: yank the kill register
        case 31: do_undo(r);          return REPL_CONSUMED;  // ctrl-_ / ctrl-/: undo
        case 18: start_search(r);     return REPL_CONSUMED;  // ctrl-r: reverse history search
        case 3:  repl_reset(r);       return REPL_CONSUMED;  // ctrl-c: clear line
        default: break;
    }
    if (cp >= 0x20 && cp != 0x7f) {
        insert_codepoint(r, cp);
        return REPL_CONSUMED;
    }
    return REPL_CONSUMED;  // swallow other control chars while focused
}

ReplResult repl_handle_input(Repl *r, const ReplEvent *ev) {
    if (r->searching) return handle_search(r, ev);
    switch (ev->key) {
        case REPL_KEY_CHAR:
            return handle_char(r, ev->codepoint);

        case REPL_KEY_TEXT:
            repl_insert_text(r, ev->text);
            return REPL_CONSUMED;

        case REPL_KEY_NEWLINE:
            insert_codepoint(r, '\n');
            return REPL_CONSUMED;

        case REPL_KEY_BACKSPACE:
            if (r->cursor > 0) delete_range(r, utf8_prev(r->buf, r->cursor), r->cursor);
            return REPL_CONSUMED;

        case REPL_KEY_LEFT:
            if (r->cursor > 0) cursor_left(r);
            return REPL_CONSUMED;

        case REPL_KEY_RIGHT:
            // At end-of-line: accept the open completion, else accept the inline
            // history suggestion; otherwise just move right.
            if (r->dropdown_open && r->cursor == r->len) {
                accept_completion(r, r->sel >= 0 ? r->sel : 0);
            } else if (r->cursor < r->len) {
                cursor_right(r);
            } else {
                const char *s = repl_suggestion(r);
                if (s && s[0]) {
                    undo_mark(r, 0);
                    insert_bytes(r, s, (int)strlen(s));
                    undo_anchor_here(r);
                    after_edit(r);
                }
            }
            return REPL_CONSUMED;

        case REPL_KEY_WORD_LEFT:
            r->cursor = word_left(r);
            return REPL_CONSUMED;

        case REPL_KEY_WORD_RIGHT:
            r->cursor = word_right(r);
            return REPL_CONSUMED;

        case REPL_KEY_UP:
            // Dropdown nav > multi-line cursor nav > history browse.
            if (r->dropdown_open) {
                r->sel = (r->sel <= 0) ? r->cand_count - 1 : r->sel - 1;
            } else if (!cursor_line_move(r, -1)) {
                history_browse(r, +1);
            }
            return REPL_CONSUMED;

        case REPL_KEY_DOWN:
            if (r->dropdown_open) {
                r->sel = (r->sel + 1) % r->cand_count;
            } else if (!cursor_line_move(r, +1)) {
                history_browse(r, -1);
            }
            return REPL_CONSUMED;

        case REPL_KEY_ENTER:
            // Highlighted completion -> accept (don't submit yet).
            if (r->dropdown_open && r->sel >= 0) {
                accept_completion(r, r->sel);
                return REPL_CONSUMED;
            }
            if (r->len > 0) {
                history_push(r, r->buf);
                return REPL_SUBMIT;
            }
            return REPL_CONSUMED;

        case REPL_KEY_ESCAPE:
            if (r->dropdown_open) {
                r->dropdown_open = false;
                r->sel = -1;
                return REPL_CONSUMED;
            }
            return REPL_UNFOCUS;

        default:
            return REPL_CONSUMED;  // focused REPL captures all other keys
    }
}

// ----------------------------------------------------------
// Rendering
// ----------------------------------------------------------

int repl_dropdown_rows(const Repl *r) {
    if (r->searching || !r->dropdown_open) return 0;
    int n = r->cand_count;
    return n < REPL_MAX_VISIBLE ? n : REPL_MAX_VISIBLE;
}

// First visible candidate so the selection stays on screen.
static int window_start(const Repl *r, int rows) {
    if (r->sel < 0 || r->sel < rows) return 0;
    int start = r->sel - rows + 1;
    int max_start = r->cand_count - rows;
    if (start > max_start) start = max_start;
    return start < 0 ? 0 : start;
}

static void put_str(ReplDraw draw, void *ctx, int x, int y, int max_x,
                    const char *s, ReplStyle style) {
    int i = 0;
    while (s[i] && x < max_x) {
        uint32_t cp = utf8_decode(s, (int)strlen(s), &i);
        draw(ctx, x, y, cp, style);
        x += glyph_cols(cp);
    }
}

int repl_input_rows(const Repl *r, int width) {
    if (r->searching) return 1;                    // the single search prompt row
    return wrap_segments(r, width - 2, NULL, 0);   // width minus the 2-char prefix
}

int repl_cursor_row(const Repl *r, int width) {
    int cap = r->len + 2;
    int *seg = malloc(sizeof *seg * (size_t)cap);
    if (!seg) return 0;
    int rows = wrap_segments(r, width - 2, seg, cap);
    int row = 0;
    for (int i = 1; i < rows; i++) {
        if (seg[i] > r->cursor) break;
        row = i;
    }
    free(seg);
    return row;
}

// Render the reverse-search prompt: (reverse-i-search)`query`: <match>
static void render_search(const Repl *r, ReplDraw draw, void *ctx,
                          int x, int y, int width) {
    int max_x = x + width;
    int px = x;
    const char *label = "(reverse-i-search)`";
    for (int i = 0; label[i] && px < max_x; i++)
        draw(ctx, px++, y, (unsigned char)label[i], REPL_STYLE_PROMPT);
    const char *q = r->search_query ? r->search_query : "";
    int qi = 0;
    while (q[qi] && px < max_x) {
        uint32_t cp = utf8_decode(q, r->sq_len, &qi);
        draw(ctx, px, y, cp, REPL_STYLE_TYPED);
        px += glyph_cols(cp);
    }
    // cursor caret sits right after the query
    if (px < max_x) draw(ctx, px, y, '_', REPL_STYLE_CURSOR);
    px++;
    const char *sep = "': ";
    for (int i = 0; sep[i] && px < max_x; i++)
        draw(ctx, px++, y, (unsigned char)sep[i], REPL_STYLE_PROMPT);
    const char *match = (r->search_idx >= 0) ? r->history[r->search_idx] : "";
    int mi = 0, mlen = (int)strlen(match);
    while (match[mi] && px < max_x) {
        uint32_t cp = utf8_decode(match, mlen, &mi);
        draw(ctx, px, y, cp, REPL_STYLE_DIM);
        px += glyph_cols(cp);
    }
}

// Render one input row [ls,le) (le excludes any trailing '\n') at row_y with a
// 2-char prefix. When the cursor is on this row, highlight the glyph under it
// (or a trailing caret at end-of-content).
static void render_row(const Repl *r, ReplDraw draw, void *ctx, int x, int row_y,
                       int width, const char *prefix, bool focused,
                       bool cursor_here, int ls, int le) {
    int max_x = x + width;
    int px = x;
    if (px < max_x) draw(ctx, px++, row_y, (unsigned char)prefix[0], REPL_STYLE_PROMPT);
    if (px < max_x) draw(ctx, px++, row_y, (unsigned char)prefix[1], REPL_STYLE_PROMPT);

    int text_cols = max_x - px;
    if (text_cols < 1) return;

    // Horizontal scroll so the cursor column stays visible (cursor row only).
    int cursor_col = cursor_here ? disp_width(r->buf, ls, r->cursor) : 0;
    int scroll = (cursor_here && cursor_col > text_cols - 1)
                     ? cursor_col - (text_cols - 1) : 0;

    ReplStyle text_style = focused ? REPL_STYLE_TYPED : REPL_STYLE_DIM;
    int col = 0;
    int i = ls;
    while (i < le) {
        int start = i;
        uint32_t cp = utf8_decode(r->buf, le, &i);
        int w = glyph_cols(cp);
        int scol = col - scroll;
        if (scol >= 0 && px + scol < max_x) {
            bool at_cursor = focused && cursor_here && start == r->cursor;
            draw(ctx, px + scol, row_y, cp, at_cursor ? REPL_STYLE_CURSOR : text_style);
        }
        col += w;
    }
    // Trailing caret when the cursor sits at end-of-content on this row.
    if (focused && cursor_here && r->cursor >= le) {
        int scol = col - scroll;
        if (scol >= 0 && px + scol < max_x)
            draw(ctx, px + scol, row_y, '_', REPL_STYLE_CURSOR);
    }
}

void repl_render(const Repl *r, int x, int y, int width, bool focused,
                 ReplDraw draw, void *ctx) {
    int max_x = x + width;

    if (r->searching) { render_search(r, draw, ctx, x, y, width); return; }

    // --- Input rows (word-wrapped) ---
    int row_count = wrap_segments(r, width - 2, NULL, 0);
    int *seg = malloc(sizeof(int) * (size_t)(row_count + 1));
    if (!seg) return;
    wrap_segments(r, width - 2, seg, row_count + 1);

    for (int li = 0; li < row_count; li++) {
        int ls  = seg[li];
        int end = (li + 1 < row_count) ? seg[li + 1] : r->len;
        // A newline-terminated row: don't render the trailing '\n' itself.
        if (li + 1 < row_count && end > ls && r->buf[end - 1] == '\n') end--;
        // The cursor belongs to this row up to (but not including) the next
        // row's start; the last row also owns the end-of-buffer caret position.
        int next = (li + 1 < row_count) ? seg[li + 1] : r->len + 1;
        bool cursor_here = focused && r->cursor >= ls && r->cursor < next;
        render_row(r, draw, ctx, x, y + li, width,
                   li == 0 ? "> " : "  ", focused, cursor_here, ls, end);
    }

    // --- Ghost text after the prompt: history autosuggestion wins over the
    // command usage hint. The suggestion's first cell shows the cursor (so the
    // tail reads as a continuation of the word); the usage hint trails the caret.
    if (focused && row_count > 0) {
        const char *sugg = repl_suggestion(r);
        int li = row_count - 1;
        int col = disp_width(r->buf, seg[li], r->len);   // cursor sits at end
        if (sugg && sugg[0]) {
            int px = x + 2 + col;
            int slen = (int)strlen(sugg), si = 0;
            bool first = true;
            while (sugg[si] && px < max_x) {
                uint32_t cp = utf8_decode(sugg, slen, &si);
                draw(ctx, px, y + li, cp, first ? REPL_STYLE_CURSOR : REPL_STYLE_DIM);
                px += glyph_cols(cp);
                first = false;
            }
        } else {
            const char *hint = repl_arg_hint(r);
            if (hint && hint[0]) {
                int px = x + 2 + col + 1;                 // +1 to clear the caret
                int hlen = (int)strlen(hint), hi = 0;
                while (hint[hi] && px < max_x) {
                    uint32_t cp = utf8_decode(hint, hlen, &hi);
                    draw(ctx, px, y + li, cp, REPL_STYLE_DIM);
                    px += glyph_cols(cp);
                }
            }
        }
    }

    // --- Completion dropdown, directly below the input block ---
    int rows = focused ? repl_dropdown_rows(r) : 0;
    if (rows == 0) { free(seg); return; }

    int start = window_start(r, rows);
    for (int i = 0; i < rows; i++) {
        int ci = start + i;
        int row_y = y + row_count + i;
        const ReplCandidate *cand = &r->cands[ci];
        bool selected = (ci == r->sel);
        ReplStyle name_style = selected ? REPL_STYLE_SELECTED : REPL_STYLE_TYPED;

        for (int c = x; c < max_x; c++)
            draw(ctx, c, row_y, ' ', name_style);   // clear the row

        if (selected && x < max_x)
            draw(ctx, x, row_y, 0x2192, REPL_STYLE_SELECTED);  // →

        int cx = x + 2;
        put_str(draw, ctx, cx, row_y, max_x, cand->text, name_style);
        int desc_x = cx + (int)strlen(cand->text) + 2;
        if (cand->desc[0] && desc_x < max_x)
            put_str(draw, ctx, desc_x, row_y, max_x, cand->desc, REPL_STYLE_DIM);
    }
    free(seg);
}

#endif // REPL_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define REPL_IMPLEMENTATION
#include "../vendor/repl.h"

// The command pane: repl.h's editor drawn under the page, with what it emits
// turned into the same kind of single buffered write draw_status does. The
// editor owns no terminal state of its own, which is why it fits here at all -
// it hands back cells and this decides what a cell is.

#define PANE_ROWS 12         // stable height: typing never restarts the screencast
#define LOG_MAX 200
#define LOG_COLS 512
#define INPUT_ROWS 4

typedef struct { uint32_t cp; uint8_t style; } RCell;

static Repl        g_repl;
static ReplCommand g_cmds[64];
static int         g_ncmds;
static bool        g_ready;

static char   g_log[LOG_MAX][LOG_COLS];
static int    g_log_n;              // how many are filled
static int    g_log_scroll;         // lines above the newest transcript entry

static RCell *g_grid;
static int    g_gw, g_gh;
static int    g_clip_y0, g_clip_y1;

// ------------------------------------------------------------------- setup

void repl_pane_init(App *a) {
    if (g_ready) return;
    // repl.h opens its dropdown for a token starting with '/', so the names go
    // in with one. A bare `goto x` still runs - the slash is stripped on the way
    // to the queue - so '/' simply means "show me what there is".
    static char names[64][24];
    int n = script_verb_count();
    if (n > (int)(sizeof g_cmds / sizeof *g_cmds)) n = (int)(sizeof g_cmds / sizeof *g_cmds);
    for (int i = 0; i < n; i++) {
        snprintf(names[i], sizeof names[i], "/%s", script_verb_name(i));
        g_cmds[i].name = names[i];
        g_cmds[i].desc = script_verb_help(i);
        g_cmds[i].args = NULL;
    }
    g_ncmds = n;
    repl_init(&g_repl, g_cmds, g_ncmds);
    g_ready = true;
}

void repl_pane_free(App *a) {
    if (g_ready) repl_free(&g_repl);
    g_ready = false;
    free(g_grid);
    g_grid = NULL;
    buf_free(&a->repl_buf);
    buf_free(&a->repl_last);
}

static int pane_width(App *a) {
    int w = a->kitty.cols > 0 ? a->kitty.cols : a->term.cols;
    if (w > a->term.cols) w = a->term.cols;
    if (w < 4) w = 4;
    return w;
}

// ------------------------------------------------------------------- rows

// Fixed on purpose. Changing height as output arrives or completion filters
// would restart the screencast on almost every keystroke.
int repl_pane_rows(App *a) {
    if (!a->repl_open) return 0;
    repl_pane_init(a);
    int rows = PANE_ROWS;
    int max = a->term.rows - (a->status_open ? 1 : 0) - 2;
    if (rows > max) rows = max;
    if (rows < 3) rows = 3;
    return rows;
}

// Open and focused, or gone: the key that opens the pane is the key that puts
// it away, whether or not it currently holds the keyboard. `esc` is the
// narrower move, handing the keyboard back with the transcript still up.
void repl_pane_toggle(App *a) {
    repl_pane_init(a);
    if (a->repl_open) {
        a->repl_open = false;
        a->repl_focus = false;
    } else {
        a->repl_open = true;
        a->repl_focus = true;
    }
}

// ---------------------------------------------------------------- transcript

// A line the pane did not run, but that belongs in what it can recall: a
// recorded command is one you often want back to edit or run again, and it has
// to be there whether or not the pane was ever opened while it was written.
void repl_pane_history_add(App *a, const char *line) {
    if (!line || !*line) return;
    repl_pane_init(a);
    repl_history_add(&g_repl, line);
}

void repl_pane_log(App *a, const char *line) {
    const char *p = line ? line : "";
    do {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (g_log_n == LOG_MAX) {
            memmove(g_log[0], g_log[1], (LOG_MAX - 1) * LOG_COLS);
            g_log_n--;
        }
        if (n >= LOG_COLS) n = LOG_COLS - 1;
        memcpy(g_log[g_log_n], p, n);
        g_log[g_log_n][n] = 0;
        g_log_n++;
        if (!nl) break;
        p = nl + 1;
    } while (1);
    g_log_scroll = 0;                // new output follows the tail, like a shell
}

void repl_pane_help(App *a) {
    for (int i = 0; i < script_verb_count(); i++) {
        char row[160];
        snprintf(row, sizeof row, "%-10s %s",
                 script_verb_name(i), script_verb_help(i));
        repl_pane_log(a, row);
    }
}

// ------------------------------------------------------------------- paint

static void cell(void *ctx, int x, int y, uint32_t cp, ReplStyle st) {
    (void)ctx;
    if (x < 0 || y < g_clip_y0 || x >= g_gw || y >= g_clip_y1) return;
    g_grid[y * g_gw + x].cp = cp;
    g_grid[y * g_gw + x].style = (uint8_t)st;
}

static const char *sgr_for(uint8_t st) {
    switch (st) {
    case REPL_STYLE_PROMPT:   return "\x1b[36m";
    case REPL_STYLE_DIM:      return "\x1b[2m";
    case REPL_STYLE_CURSOR:   return "\x1b[7m";
    case REPL_STYLE_SELECTED: return "\x1b[1;33m";
    default:                  return "\x1b[0m";
    }
}

static size_t utf8_put(uint32_t cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xc0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xe0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
                        out[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

void repl_pane_paint(App *a) {
    if (!a->has_tty || !a->repl_open || a->repl_rows < 1) return;
    repl_pane_init(a);

    int w = pane_width(a), h = a->repl_rows;
    if (w != g_gw || h != g_gh || !g_grid) {
        RCell *ng = realloc(g_grid, sizeof(RCell) * (size_t)w * (size_t)h);
        if (!ng) return;
        g_grid = ng;
        g_gw = w;
        g_gh = h;
    }
    for (int i = 0; i < g_gw * g_gh; i++) {
        g_grid[i].cp = ' ';
        g_grid[i].style = REPL_STYLE_TYPED;
    }

    int iw = w - 2, ih = h - 2;
    if (iw < 1 || ih < 1) return;
    int in_all = repl_input_rows(&g_repl, iw);
    int in_show = in_all > INPUT_ROWS ? INPUT_ROWS : in_all;
    int drop = a->repl_focus ? repl_dropdown_rows(&g_repl) : 0;
    if (drop > ih - in_show) drop = ih - in_show;
    if (drop < 0) drop = 0;
    int log_rows = ih - in_show - drop;

    g_clip_y0 = 0; g_clip_y1 = h;
    cell(a, 0, 0, 0x250c, REPL_STYLE_DIM);
    cell(a, w - 1, 0, 0x2510, REPL_STYLE_DIM);
    cell(a, 0, h - 1, 0x2514, REPL_STYLE_DIM);
    cell(a, w - 1, h - 1, 0x2518, REPL_STYLE_DIM);
    for (int x = 1; x < w - 1; x++) {
        cell(a, x, 0, 0x2500, REPL_STYLE_DIM);
        cell(a, x, h - 1, 0x2500, REPL_STYLE_DIM);
    }
    for (int y = 1; y < h - 1; y++) {
        cell(a, 0, y, 0x2502, REPL_STYLE_DIM);
        cell(a, w - 1, y, 0x2502, REPL_STYLE_DIM);
    }
    const char *title = a->selector_pick ? " REPL - PICKING " : " REPL ";
    for (int x = 0; title[x] && x + 2 < w - 1; x++)
        cell(a, x + 2, 0, (unsigned char)title[x], REPL_STYLE_PROMPT);

    int end = g_log_n - g_log_scroll;
    if (end < 0) end = 0;
    int start = end - log_rows;
    if (start < 0) start = 0;
    int first_y = 1 + (log_rows - (end - start));
    for (int i = start, y = first_y; i < end && y < 1 + log_rows; i++, y++)
        for (int c = 0, o = 0; g_log[i][o] && c < iw; c++) {
            unsigned char ch = (unsigned char)g_log[i][o++];
            cell(a, c + 1, y, ch, REPL_STYLE_DIM);
        }

    int cur_row = repl_cursor_row(&g_repl, iw);
    int first_input = cur_row - in_show + 1;
    if (first_input < 0) first_input = 0;
    if (first_input > in_all - in_show) first_input = in_all - in_show;
    int input_y = 1 + log_rows;
    g_clip_y0 = input_y;
    g_clip_y1 = h - 1;
    repl_render(&g_repl, 1, input_y - first_input, iw, a->repl_focus, cell, a);
    g_clip_y0 = 0; g_clip_y1 = h;

    Buf b = a->repl_buf;
    b.len = 0;
    int sx = a->kitty.x > 0 ? a->kitty.x : 1;
    for (int r = 0; r < g_gh; r++) {
        buf_addf(&b, "\x1b[%d;%dH\x1b[0m", a->repl_row + r, sx);
        int cur = -1;
        for (int c = 0; c < g_gw; c++) {
            RCell *k = &g_grid[r * g_gw + c];
            if (k->style != cur) {
                // The cursor cell is a reverse of whatever it sits on, so the
                // reset has to come first or the previous run leaks into it.
                buf_add(&b, "\x1b[0m", 4);
                const char *sgr = sgr_for(k->style);
                buf_add(&b, sgr, strlen(sgr));
                cur = k->style;
            }
            char u[4];
            buf_add(&b, u, utf8_put(k->cp, u));
        }
        buf_add(&b, "\x1b[0m", 4);
    }
    a->repl_buf = b;

    // The same guard draw_status uses: an unchanged pane writes nothing, so a
    // repaint never lands in the middle of a frame going out.
    if (b.len == a->repl_last.len &&
        (b.len == 0 || memcmp(b.p, a->repl_last.p, b.len) == 0))
        return;
    writeall(a->term.fd, b.p, b.len);
    a->repl_last.len = 0;
    buf_add(&a->repl_last, b.p, b.len);
}

// ------------------------------------------------------------------- input

// term_next reports a control byte as its letter plus MOD_CTRL; repl.h wants
// the raw codepoint, because that is where its emacs bindings live.
bool repl_pane_key(App *a, Event *ev) {
    if (!a->repl_focus) return false;
    repl_pane_init(a);

    if (ev->type == EV_PASTE) {
        repl_insert_text(&g_repl, a->term.paste.p);
        return true;
    }
    if (ev->type != EV_KEY) return false;

    if (ev->key == KEY_PGUP || ev->key == KEY_PGDN) {
        int page = a->repl_rows > 5 ? a->repl_rows - 4 : 1;
        g_log_scroll += ev->key == KEY_PGUP ? page : -page;
        if (g_log_scroll < 0) g_log_scroll = 0;
        if (g_log_scroll > g_log_n) g_log_scroll = g_log_n;
        return true;
    }

    ReplEvent re = {0};
    switch (ev->key) {
    case KEY_ENTER:
        re.key = (ev->mods & MOD_SHIFT) ? REPL_KEY_NEWLINE : REPL_KEY_ENTER;
        break;
    case KEY_BACKSPACE: re.key = REPL_KEY_BACKSPACE; break;
    case KEY_ESC:       re.key = REPL_KEY_ESCAPE;    break;
    case KEY_UP:        re.key = REPL_KEY_UP;        break;
    case KEY_DOWN:      re.key = REPL_KEY_DOWN;      break;
    case KEY_LEFT:
        re.key = (ev->mods & MOD_ALT) ? REPL_KEY_WORD_LEFT : REPL_KEY_LEFT;
        break;
    case KEY_RIGHT:
        re.key = (ev->mods & MOD_ALT) ? REPL_KEY_WORD_RIGHT : REPL_KEY_RIGHT;
        break;
    // Right is what accepts a completion, so tab means the same thing here.
    case KEY_TAB:  re.key = REPL_KEY_RIGHT; break;
    case KEY_HOME: re.key = REPL_KEY_CHAR; re.codepoint = 1; break;   // ^A
    case KEY_END:  re.key = REPL_KEY_CHAR; re.codepoint = 5; break;   // ^E
    default:
        if (ev->mods == MOD_CTRL && ev->key >= 'a' && ev->key <= 'z') {
            re.key = REPL_KEY_CHAR;
            re.codepoint = (uint32_t)(ev->key - 'a' + 1);
        } else if (ev->text[0] && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
            re.key = REPL_KEY_TEXT;
            re.text = ev->text;          // one shot, so UTF-8 stays whole
        } else {
            return true;                 // swallowed rather than sent to the page
        }
    }

    ReplResult rc = repl_handle_input(&g_repl, &re);
    if (rc == REPL_UNFOCUS) {
        a->repl_focus = false;
    } else if (rc == REPL_SUBMIT) {
        const char *line = repl_line(&g_repl);
        if (line && *line) {
            repl_history_add(&g_repl, line);
            char shown[LOG_COLS];
            snprintf(shown, sizeof shown, "> %s", line);
            repl_pane_log(a, shown);
            char command[4096];
            snprintf(command, sizeof command, "%s", line[0] == '/' ? line + 1 : line);
            // Shift+Enter is an editing newline. The command language treats
            // it as whitespace when the whole input is finally submitted.
            for (char *p = command; *p; p++)
                if (*p == '\r' || *p == '\n') *p = ' ';
            script_push(a, command);
        }
        repl_reset(&g_repl);
    }
    return true;
}

bool repl_pane_mouse(App *a, Event *ev) {
    if (!a->repl_open || ev->type != EV_MOUSE ||
        ev->my < a->repl_row || ev->my >= a->repl_row + a->repl_rows)
        return false;
    if (ev->button == 3 || ev->button == 4) {
        g_log_scroll += ev->button == 3 ? 3 : -3;
        if (g_log_scroll < 0) g_log_scroll = 0;
        if (g_log_scroll > g_log_n) g_log_scroll = g_log_n;
    } else if (ev->button == 0 && ev->press) {
        a->repl_focus = true;
    }
    return true;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define REPL_IMPLEMENTATION
#include "../vendor/repl.h"

// The console: repl.h's editor drawn under the page, with what it emits
// turned into the same kind of single buffered write draw_status does. The
// editor owns no terminal state of its own, which is why it fits here at all -
// it hands back cells and this decides what a cell is.

#define CONSOLE_ROWS 12         // the height it opens at
#define CONSOLE_MIN 3
#define LOG_MAX 200
#define LOG_COLS 512
#define INPUT_ROWS 4

typedef struct { uint32_t cp; uint8_t style; } RCell;

static Repl        g_repl;
static bool        g_ready;

static char   g_log[LOG_MAX][LOG_COLS];
static int    g_log_n;              // how many are filled
static int    g_log_scroll;         // lines above the newest transcript entry

static RCell *g_grid;
static int    g_gw, g_gh;
static int    g_clip_y0, g_clip_y1;

static bool   g_dragging;           // the top border is being carried
static int    g_drag_y;             // the row the pointer was on last

// ------------------------------------------------------------------- setup

void console_init(App *a) {
    if (g_ready) return;
    // Nothing to complete: what is typed here is javascript, and the page's own
    // names are not ours to know without asking it about every keystroke.
    repl_init(&g_repl, NULL, 0);
    g_ready = true;
}

void console_free(App *a) {
    if (g_ready) repl_free(&g_repl);
    g_ready = false;
    free(g_grid);
    g_grid = NULL;
    buf_free(&a->console_buf);
    buf_free(&a->console_last);
}

static int console_width(App *a) {
    int w = a->kitty.cols > 0 ? a->kitty.cols : a->term.cols;
    if (w > a->term.cols) w = a->term.cols;
    if (w < 4) w = 4;
    return w;
}

// ------------------------------------------------------------------- rows

static int fit_rows(App *a, int rows) {
    int max = a->term.rows - (a->status_open ? 1 : 0) - 2;
    if (rows > max) rows = max;
    if (rows < CONSOLE_MIN) rows = CONSOLE_MIN;
    return rows;
}

// Whatever the border was last dragged to, and nothing else: a height that
// moved with output or completions would restart the screencast on almost every
// keystroke.
int console_rows(App *a) {
    if (!a->console_open) return 0;
    console_init(a);
    return fit_rows(a, a->console_want > 0 ? a->console_want : CONSOLE_ROWS);
}

// Open and focused, or gone: the key that opens the console is the key that puts
// it away, whether or not it currently holds the keyboard. `esc` is the
// narrower move, handing the keyboard back with the transcript still up.
void console_toggle(App *a) {
    console_init(a);
    if (a->console_open) {
        a->console_open = false;
        a->console_focus = false;
    } else {
        a->console_open = true;
        a->console_focus = true;
    }
}

// ---------------------------------------------------------------- transcript

// A line the console did not run, but that belongs in what it can recall: a
// recorded command is one you often want back to edit or run again, and it has
// to be there whether or not the console was ever opened while it was written.
void console_history_add(App *a, const char *line) {
    if (!line || !*line) return;
    console_init(a);
    repl_history_add(&g_repl, line);
}

void console_log(App *a, const char *line) {
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

// The whole transcript, joined, into the clipboard. A page drawn in cells is
// out of the terminal's own selection's reach, and the transcript is the part
// of the window most often wanted somewhere else - a value to paste into a
// script, an error to send on. Answers how many lines went, for the notice.
int console_copy(App *a) {
    console_init(a);
    if (!g_log_n) return 0;
    Buf b = {0};
    for (int i = 0; i < g_log_n; i++) {
        buf_add(&b, g_log[i], strlen(g_log[i]));
        buf_add(&b, "\n", 1);
    }
    if (b.p) clipboard_put(b.p);
    buf_free(&b);
    return g_log_n;
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

void console_paint(App *a) {
    if (!a->has_tty || !a->console_open || a->console_rows < 1) return;
    console_init(a);

    int w = console_width(a), h = a->console_rows;
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
    int drop = a->console_focus ? repl_dropdown_rows(&g_repl) : 0;
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
    static const char title[] = " CONSOLE ";
    for (int x = 0; title[x] && x + 2 < w - 1; x++)
        cell(a, x + 2, 0, (unsigned char)title[x], REPL_STYLE_PROMPT);

    int end = g_log_n - g_log_scroll;
    if (end < 0) end = 0;
    int start = end - log_rows;
    if (start < 0) start = 0;
    int first_y = 1 + (log_rows - (end - start));
    for (int i = start, y = first_y; i < end && y < 1 + log_rows; i++, y++) {
        int len = (int)strlen(g_log[i]), o = 0, col = 0;
        while (g_log[i][o]) {
            uint32_t cp = utf8_decode(g_log[i], len, &o);
            int w = glyph_cols(cp);
            if (col + w > iw) break;
            cell(a, col + 1, y, cp < 0x20 ? ' ' : cp, REPL_STYLE_DIM);
            col += w;
        }
    }

    int cur_row = repl_cursor_row(&g_repl, iw);
    int first_input = cur_row - in_show + 1;
    if (first_input < 0) first_input = 0;
    if (first_input > in_all - in_show) first_input = in_all - in_show;
    int input_y = 1 + log_rows;
    g_clip_y0 = input_y;
    g_clip_y1 = h - 1;
    repl_render(&g_repl, 1, input_y - first_input, iw, a->console_focus, cell, a);
    g_clip_y0 = 0; g_clip_y1 = h;

    Buf b = a->console_buf;
    b.len = 0;
    int sx = a->kitty.x > 0 ? a->kitty.x : 1;
    for (int r = 0; r < g_gh; r++) {
        buf_addf(&b, "\x1b[%d;%dH\x1b[0m", a->console_row + r, sx);
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
            // repl.h's contract, and the transcript follows it: a double-width
            // glyph is emitted once and the cell it covers is left alone.
            int w = glyph_cols(k->cp);
            if (w > 1) c += w - 1;
        }
        buf_add(&b, "\x1b[0m", 4);
    }
    a->console_buf = b;

    // The same guard draw_status uses: an unchanged console writes nothing, so a
    // repaint never lands in the middle of a frame going out.
    if (b.len == a->console_last.len &&
        (b.len == 0 || memcmp(b.p, a->console_last.p, b.len) == 0))
        return;
    writeall(a->term.fd, b.p, b.len);
    a->console_last.len = 0;
    buf_add(&a->console_last, b.p, b.len);
}

// ------------------------------------------------------------------- input

// term_next reports a control byte as its letter plus MOD_CTRL; repl.h wants
// the raw codepoint, because that is where its emacs bindings live.
bool console_key(App *a, Event *ev) {
    if (!a->console_focus) return false;
    console_init(a);

    if (ev->type == EV_PASTE) {
        repl_insert_text(&g_repl, a->term.paste.p);
        return true;
    }
    if (ev->type != EV_KEY) return false;

    if (ev->key == KEY_PGUP || ev->key == KEY_PGDN) {
        int page = a->console_rows > 5 ? a->console_rows - 4 : 1;
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
        a->console_focus = false;
    } else if (rc == REPL_SUBMIT) {
        const char *line = repl_line(&g_repl);
        if (line && *line) {
            repl_history_add(&g_repl, line);
            char shown[LOG_COLS];
            snprintf(shown, sizeof shown, "> %s", line);
            console_log(a, shown);
            // Javascript, whatever it is. There is nothing else it could be.
            char command[4096];
            snprintf(command, sizeof command, "%s", line);
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

bool console_mouse(App *a, Event *ev) {
    if (ev->type != EV_MOUSE) return false;

    if (g_dragging) {
        if (!ev->press) g_dragging = false;
        else if (ev->motion) {
            int have = a->console_want > 0 ? a->console_want : a->console_rows;
            int delta = fit_rows(a, have + (g_drag_y - ev->my)) - have;
            // Inline, the block is anchored at its top, so the rows have to be
            // traded with the picture to hold the last row still.
            if (a->inline_mode) {
                if (a->box_rows - delta < BOX_MIN_ROWS)
                    delta = a->box_rows - BOX_MIN_ROWS;
                a->box_rows -= delta;
            }
            a->console_want = have + delta;
            g_drag_y = ev->my;
        }
        return true;
    }

    if (!a->console_open ||
        ev->my < a->console_row || ev->my >= a->console_row + a->console_rows)
        return false;

    if (ev->my == a->console_row && ev->button == 0 && ev->press && !ev->motion) {
        g_dragging = true;
        g_drag_y = ev->my;
        a->console_focus = true;
        return true;
    }
    if (ev->button == 3 || ev->button == 4) {
        g_log_scroll += ev->button == 3 ? 3 : -3;
        if (g_log_scroll < 0) g_log_scroll = 0;
        if (g_log_scroll > g_log_n) g_log_scroll = g_log_n;
    } else if (ev->button == 0 && ev->press) {
        a->console_focus = true;
    }
    return true;
}

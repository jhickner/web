#ifndef WEB_H
#define WEB_H

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// ---------------------------------------------------------------- buffers

typedef struct {
    char  *p;
    size_t len, cap;
} Buf;

int  buf_reserve(Buf *b, size_t need);
int  buf_add(Buf *b, const void *src, size_t n);
int  buf_addf(Buf *b, const char *fmt, ...);
void buf_consume(Buf *b, size_t n);
void buf_free(Buf *b);

double now_sec(void);
int    writeall(int fd, const char *p, size_t n);
void   mkdirs(const char *path);

// Bytes written. dst needs room for three per four of src.
size_t base64_decode(const char *src, size_t n, char *dst);

// ---------------------------------------------------------------- json

// Minimal readers for the handful of CDP fields we consume. The buffer must be
// NUL-terminated; values are returned as pointers into it.
const char *json_str(const char *js, const char *key, size_t *len);
double      json_num(const char *js, const char *key, double def);
bool        json_has(const char *js, const char *key);
size_t      json_escape(char *dst, size_t cap, const char *src);
void        json_escape_buf(Buf *out, const char *src);
size_t      json_unescape(char *dst, size_t cap, const char *src, size_t n);

// The by-value string of a Runtime.evaluate reply, or NULL if it threw or
// answered with anything else.
const char *json_eval_str(const char *msg, size_t *len);

// ---------------------------------------------------------------- websocket

typedef struct {
    int  fd;
    Buf  in;        // raw bytes off the socket
    Buf  msg;       // reassembled payload, always NUL-terminated
    bool closed;
} WS;

int  ws_connect(const char *host, int port, const char *path);
int  ws_send_text(WS *ws, const char *p, size_t n);
int  ws_fill(WS *ws);                        // read what's available
int  ws_next(WS *ws, char **msg, size_t *len); // 1 = message ready
void ws_close(WS *ws);

// ---------------------------------------------------------------- chrome

typedef struct {
    pid_t pid;
    int   port;
    bool  adopted;      // an earlier run's, so we open a window of our own in it
    bool  foreign;      // attached to one we never started: leave it running
    char  profile[512];
    char  target[96];   // the page we are driving, so the others stay theirs
    WS    ws;
    int   next_id;
} Chrome;

// debug=false leaves out the remote-debugging port entirely: a browser that
// can be driven over CDP can also have its credentials read that way, and
// Google refuses to sign anyone in to one. port 0 lets Chrome pick a free one.
int  chrome_launch(Chrome *c, const char *url, int w, int h, bool show_window,
                   bool mute, const char *user_agent, bool debug, int port);
int  chrome_attach(Chrome *c);
void chrome_profile_path(char *out, size_t cap);

// 0 when a devtools endpoint answers there, so a port can be checked before
// anything is given up for it.
int  chrome_probe(int port);
int  chrome_user_agent(Chrome *c, char *out, size_t cap);
void chrome_kill(Chrome *c);
void chrome_kill_bg(Chrome *c);

// How many page targets belong to somebody else - another `web` sharing this
// browser, or a window the user opened in it.
int  chrome_other_pages(Chrome *c);
void chrome_close_target(Chrome *c);

// Leave a tab behind that holds the browser open for the next run to adopt.
void chrome_park(Chrome *c);

// --keep, remembered against the browser instead of the run, so that one
// window asking for it keeps the browser for every window.
void chrome_mark_kept(Chrome *c);
bool chrome_is_kept(Chrome *c);

// Fire-and-forget CDP call; params is a JSON object body without braces.
int  cdp_call(Chrome *c, const char *method, const char *params_fmt, ...);
int  cdp_vcall(Chrome *c, const char *method, const char *params_fmt, va_list ap);

// ---------------------------------------------------------------- graphics

typedef struct {
    int  ttyfd;
    bool tmux;
    int  x, y, cols, rows;   // 1-based cell rect the page occupies
    bool grid_dirty;
    unsigned gen;            // which name the picture is going up under
    Buf  out;
} Kitty;

void kitty_init(Kitty *k, int ttyfd, bool tmux);
void kitty_set_rect(Kitty *k, int x, int y, int cols, int rows);
int  kitty_draw_png(Kitty *k, const char *b64, size_t len);
void kitty_clear(Kitty *k);

// Take the picture down and come back under a new name. For when the cells it
// was drawn into are still somewhere on the screen but no longer somewhere we
// can address - after a resize, where the terminal has moved them and only it
// knows to where. They go on naming the old image, which is now gone.
void kitty_renew(Kitty *k);
void kitty_abort(Kitty *k);    // close an escape a dropped frame left open
void kitty_free(Kitty *k);

// ---------------------------------------------------------------- terminal

enum {
    KEY_NONE = 0,
    KEY_ESC = 0x100, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_DELETE,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};

enum { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4, MOD_SUPER = 8 };

typedef enum { EV_NONE, EV_KEY, EV_MOUSE, EV_PASTE, EV_FOCUS, EV_EOF } EvType;

typedef struct {
    EvType type;
    int    key;          // KEY_* or a unicode codepoint
    int    mods;
    char   text[8];      // UTF-8 for printable keys, empty otherwise
    int    mx, my;       // 1-based cell coords
    int    px, py;       // pixel coords when the terminal reports them
    bool   have_pixels;
    int    button;       // 0 left, 1 middle, 2 right, 3 wheel-up, 4 wheel-down,
                         // 5 wheel-left, 6 wheel-right
    bool   press;
    bool   motion;
} Event;

typedef struct {
    int    fd;
    Buf    in;
    Buf    paste;         // text of the EV_PASTE just decoded
    bool   raw;
    bool   inline_mode;   // draw in the normal flow, not the alternate screen
    int    inline_rows;   // rows reserved for the block, status line included
    int    inline_origin; // 1-based row the block starts on
    int    cols, rows;
    int    cell_w, cell_h;
    double esc_at;      // when a lone ESC showed up, to tell it from a sequence
} Term;

int  term_probe(Term *t);                 // open the tty and measure it
void term_enter(Term *t, bool inline_mode); // raw mode, alt screen, mouse
void term_reserve_inline(Term *t, int rows);
void term_resize_inline(Term *t, int rows);   // caller drops the image first
void term_clear_inline(Term *t);              // blank the rows the block owns
void term_restore(Term *t, bool clear_inline);  // erase the block on the way out
void term_size(Term *t);
int  term_read(Term *t);                  // pull available bytes
void term_log(const char *fmt, ...);      // WEB_DEBUG input trace
int  term_next(Term *t, Event *ev);       // 1 = event decoded

extern volatile sig_atomic_t g_resized;
extern volatile sig_atomic_t g_quit;

// Set once the shutdown is writing rather than drawing: from then on a write
// waits for a terminal that is not draining instead of giving up on it.
extern volatile sig_atomic_t g_write_force;

// Called from inside long tty writes so a keypress is still noticed while a
// frame is being pushed out. A full frame can occupy the terminal for a while,
// and a quit that waits for it to finish reads as a hang.
extern void (*g_input_pump)(void);

// ---------------------------------------------------------------- app

enum { PEND_NONE, PEND_MOVE };

// One pointer move held back so the next one replaces it: a drag that arrives
// while a frame is being written then costs one dispatch and one frame instead
// of one of each per step.
typedef struct {
    int         kind;
    int         x, y;
    int         mods;
    int         buttons;
    const char *btn;
} Pending;

// What an outstanding CDP call was for. Replies carry only the id they were
// issued with, so the kind has to be remembered alongside it.
enum {
    RQ_NONE, RQ_TITLE, RQ_URL, RQ_COPY, RQ_FIT, RQ_SCRIPT,
    RQ_SELECTOR, RQ_RECORD, RQ_PDF, RQ_SHOT_READY, RQ_SHOT
};

#define REQ_MAX 8

typedef struct { int id, kind; } Req;

// Where --screenshot is between being asked for and being on disk.
enum {
    SHOT_NONE,     // none was asked for
    SHOT_LOAD,     // waiting for the page, and for any script run against it
    SHOT_SETTLE,   // waiting for the fonts and the paint after them
    SHOT_SENT,     // the capture is out at the browser
    SHOT_DONE,
    SHOT_FAIL,
};

// ---------------------------------------------------------------- script

// Where a command is between being popped and being finished. The runner holds
// one command at a time and at most one CDP call, so this is the whole of it.
enum {
    SC_IDLE,      // nothing running; the queue may still have lines in it
    SC_WAIT,      // a line is out at the page; its reply is the value
    SC_NAV,       // that line started a load, and the next one waits for it
};

typedef struct {
    Buf      queue;         // pending lines of javascript, '\n' separated, FIFO
    char     line[4096];    // the one running now, for errors and --json
    int      lineno;

    int      state;
    double   deadline;      // give up on the line in flight here
    double   next_at;       // when the delay after the last one expires
    unsigned nav_seq;       // load_seq when the line that is loading started

    double   timeout;       // --timeout, seconds
    double   delay;         // --delay between commands, seconds
    bool     step;          // --step: a key between commands
    bool     stepping;      // parked on that key now
    bool     json;          // --json output
    bool     from_file;     // --script, so a failure stops the rest of it
    bool     drain_exit;    // a script was the only source: exit when it runs out
    int      failures;
} Script;

typedef struct {
    Term    term;
    Kitty   kitty;
    Chrome  chrome;
    Pending pend;

    Req     reqs[REQ_MAX];

    int     css_w, css_h;      // page viewport in CSS pixels
    int     cast_w, cast_h;    // pixel size the screencast was last asked for
    int     scale;             // device pixel ratio actually in use
    int     want_scale;        // what was asked for on the command line
    int     img_rows;          // cell rows the page occupies

    char    url[1024];
    char    title[256];
    bool    loading;
    unsigned load_seq;         // bumped on every load event, for script waits

    // Chrome draws a PDF with the viewer extension, in a frame of its own in a
    // process of its own. Nothing this document can be asked reaches it, so the
    // page is moved with real input instead of with script. Keys reach it only
    // once it has been clicked in - a frame in another process cannot be given
    // the keyboard any other way - and that is what the second flag remembers.
    bool    pdf;
    bool    pdf_clicked;

    bool    editing;           // URL bar has focus
    char    edit[1024];
    size_t  edit_len;

    uint64_t last_hash;
    unsigned frames, skipped;
    double  last_draw;
    double  last_metrics_fix;  // when the viewport override was last restored
    double  expect_frame;      // something was done that should redraw; deadline
    double  last_unwedge;      // when the screencast was last restarted for it

    bool    show_stats;        // ^G
    double  zoom;              // page magnification, alt+= / alt+-

    bool    inline_mode;       // a block in the shell's flow, like rom
    int     want_rows;         // rows for that block, 0 = pick one
    int     status_row;
    bool    hide_status;       // the status line is not wanted, ^S
    bool    status_open;       // whether it is on screen right now

    bool    pause_on_blur;     // stop drawing while the terminal is not focused
    bool    pause_cfg;         // what the config file said, which is what it keeps
    bool    paused;            // and whether that has happened
    int     blur_cpu_rate;     // how hard the renderer is throttled while it is

    bool    insert;            // a text field has focus: keys belong to the page
    bool    mouse_down;        // a button went down on the page and is still held
    bool    pending_g;         // first half of gg
    int     prompt;            // 0 none, 1 address, 2 find
    char    find[256];         // last search, for n and N

    int     box_rows;          // inline: cell rows the window occupies
    int     box_cols;          // inline: cell columns, 0 = from the proportion
    int     want_cols;         // columns remembered from an earlier run
    int     width_idx;         // cursor into WIDTHS, 0 = derived from the cells
    bool    scale_locked;      // the render scale was chosen by hand

    bool    fit_width;         // widen the viewport so no page is cut off
    int     fit_w;             // width the page says it needs
    size_t  last_bytes;        // base64 size of the frame just drawn
    double  last_write_ms;     // how long it took to reach the terminal
    double  fps;               // smoothed, so the number is readable
    double  bytes_per_sec;

    char    msg[256];
    double  msg_until;

    char    ua[512];           // chrome's own user agent, headless marker gone
    bool    ua_patch_req;      // this browser predates the flag and needs fixing
    bool    mute;              // start chrome with its audio switched off
    bool    clear_exit;        // erase the inline block on the way out (default)
    bool    keep;              // leave chrome running so the next start adopts it

    // Where a picture can go, and where data can. They are not the same fd, and
    // in a pipeline they are not both usable.
    bool    has_tty;           // a real terminal to draw into
    bool    stdout_tty;        // fd 1 is that terminal, so data must not go there

    int     exec_fd;           // --exec: the child's output, -1 when none
    pid_t   exec_pid;
    Buf     exec_buf;          // what has arrived of the line being read

    // --screenshot. The picture is the whole point of such a run, so it is
    // what the exit waits for rather than something taken on the way past.
    const char *shot_path;     // where the png goes; "-" is stdout
    bool    shot_stdout;       // and when it does, fd 1 is the picture's
    int     shot_state;        // SHOT_*
    double  shot_deadline;     // give up on the step in flight here

    Buf     status, status_last;

    Script  script;

    // The command pane. The editor itself lives in replpane.c: it is a
    // singleton, and keeping it there saves including its header everywhere.
    bool    console_open;         // drawn below the page
    bool    console_focus;        // and holding the keyboard
    bool    selector_pick;        // clicks report a selector instead of reaching the page
    int     console_rows;         // how many rows it occupies
    int     console_row;          // 1-based row it starts on
    Buf     console_buf, console_last;
} App;

// Note an outstanding call so its reply can be recognised, and claim the reply.
void app_req_note(App *a, int id, int kind);
int  app_req_take(App *a, int id);

// Everything that is not a coalesced pointer event goes through here, so a held
// back scroll or move always reaches the page ahead of whatever follows it.
int  app_cdp(App *a, const char *method, const char *fmt, ...);

void notify(App *a, const char *s);

// Move the session onto a browser listening on `port`, whoever started it, and
// write what happened into msg. 0 = attached.
int  app_attach(App *a, int port, char *msg, size_t cap);

void navigate(App *a, const char *raw);
void run_js(App *a, const char *js);
void relayout(App *a);
void scroll_at(App *a, int x, int y, int dy);
void scroll_by(App *a, int dy);
void scroll_page_end(App *a, bool bottom);
void nav_history(App *a, int delta);
bool special_key(App *a, int key, int mods);

// ---------------------------------------------------------------- script

void script_init(App *a);
void script_free(App *a);

// Queue one line of javascript. The source does not matter: a file, a pipe and
// the console all arrive here, which is what lets a typed line fall in behind a
// running script without either end knowing about the other.
void script_push(App *a, const char *line);
int  script_load(App *a, const char *path);   // "-" or NULL reads stdin
bool script_busy(const App *a);

// Called once per pass of the main loop; drives the line in flight.
void script_step(App *a);

// How long the loop may sleep, in ms, or -1 when the runner wants nothing.
int  script_wait_ms(const App *a);

// A reply the runner asked for.
void script_reply(App *a, const char *msg);

// ---------------------------------------------------------------- console

void console_init(App *a);
void console_free(App *a);
void console_toggle(App *a);          // open and focus, or unfocus
int  console_rows(App *a);            // rows it wants right now
bool console_key(App *a, Event *ev);  // true when the pane consumed it
bool console_mouse(App *a, Event *ev);// transcript wheel/click handling
void console_paint(App *a);
void console_log(App *a, const char *line);
void console_history_add(App *a, const char *line);

#endif

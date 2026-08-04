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

// One browser process running on the profile. The age is however ps spelled
// it, which is a thing to read rather than a number to work with.
typedef struct { pid_t pid; char age[24]; } ChromeProc;

// All of them, whoever started them. The note in the profile only ever names
// one, and a browser that outlived the run that started it is exactly the one
// worth finding - so this asks the process table rather than the note.
int  chrome_running(const char *profile, ChromeProc *out, int cap);

// The browser a new window would adopt: the port it answers on, or -1 when
// there is nothing there to adopt. *pid is its process where that is knowable,
// which it is not for one whose lock has been cleared away.
int  chrome_adoptable(const char *profile, pid_t *pid);
int  chrome_user_agent(Chrome *c, char *out, size_t cap);
void chrome_kill(Chrome *c);
void chrome_kill_bg(Chrome *c);

// How many page targets belong to somebody else - another `web` sharing this
// browser, or a window the user opened in it.
int  chrome_other_pages(Chrome *c);
void chrome_close_target(Chrome *c);

// A further page of our own, in a window of its own, and the id it answers to.
int  chrome_open_tab(Chrome *c, char *out, size_t cap);

// Move the session onto another page of this browser. The new socket is up
// before the old one goes, so a failure leaves the window where it was.
int  chrome_switch_target(Chrome *c, const char *target);
void chrome_close_id(Chrome *c, const char *target);

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
void term_clear_below(Term *t);               // and anything left under them
void term_restore(Term *t, bool clear_inline);  // erase the block on the way out
void term_size(Term *t);
int  term_read(Term *t);                  // pull available bytes
void term_log(const char *fmt, ...);      // WEB_DEBUG input trace

// The same trace, switched on while the window is up rather than for the whole
// run: ^D before doing the thing worth recording, ^D again after. It appends to
// /tmp/web_input.log. -1 toggles; the answer is whether it is now on.
bool term_trace(int on);
bool term_tracing(void);
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
    RQ_SELECTOR, RQ_RECORD, RQ_PDF, RQ_SHOT_READY, RQ_SHOT, RQ_FRAME,
    RQ_MODE
};

// The isolated world everything we inject for our own use lives in. It shares
// the page's DOM and nothing else: our globals are not on the page's window,
// and a page redefining one of the things we call cannot reach ours.
#define WEB_WORLD "web"

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

// ---------------------------------------------------------------- tabs

// One page of the browser, drawn as one tab of the bar. The window drives
// exactly one of them at a time: the CDP socket is per page, so switching is
// moving that socket rather than telling anything to come forward.
typedef struct {
    char target[96];    // the devtools page id, which is what a tab really is
    char url[1024];     // where it was when the window last looked away
    char title[256];
    bool ours;          // we opened it, so it is ours to close on the way out
    bool inited;        // the once-per-page CDP setup has been done
    int  x0, x1;        // cells the bar last drew it on, for clicks
} Tab;

// Well past what a bar can name comfortably, because the bar's job is to cope
// rather than to refuse: it gives up the titles, then the numbers, then the
// separators, and a tab is still a tab when it is one letter wide. This is only
// the point where the list itself stops, and each entry is a page of the
// browser - a real cost - so it is not unbounded either.
#define TAB_MAX 32

typedef struct {
    Term    term;
    Kitty   kitty;
    Chrome  chrome;
    Pending pend;

    Req     reqs[REQ_MAX];

    int     css_w, css_h;      // page viewport in CSS pixels
    int     cast_w, cast_h;    // pixel size the screencast was last asked for
    double  scale;             // device pixel ratio actually in use
    double  want_scale;        // what was asked for on the command line
    int     img_rows;          // cell rows the page occupies

    char    url[1024];
    char    title[256];
    bool    loading;
    unsigned load_seq;         // bumped on every load event, for script waits

    // The page's own frame, so an address change can be told from one an
    // advert in a corner made. Only the top frame is the window's address; a
    // frame inside it moves the same way and means nothing here.
    char    frame[64];

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
    int     unwedge_run;       // restarts since the last frame that answered one

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

    // Cancel the browser's own action for a navigation key the page did not
    // want, so it is never handed back to the window system. The page still
    // gets the key; this only stops the part that happens after nobody claimed
    // it, which on macOS is a walk of the whole menu bar and, on some pages,
    // seconds of the one thread that answers everything.
    bool    claim_keys;

    bool    insert;            // a text field has focus: keys belong to the page
    bool    mouse_down;        // a button went down on the page and is still held
    bool    pending_g;         // first half of gg
    int     prompt;            // 0 none, 1 address, 2 find
    char    find[256];         // last search, for n and N

    int     box_rows;          // inline: cell rows the window occupies
    int     box_cols;          // inline: cell columns, 0 = from the proportion
    int     want_cols;         // columns remembered from an earlier run
    int     want_width;        // width pinned by w/W, 0 = derived from the cells
    bool    scale_locked;      // the render scale was chosen by hand

    // --scale auto: a picture that is moving is drawn at fewer pixels, which
    // are the same pixels Chrome encodes, we write and the terminal decodes.
    bool    motion_auto;
    bool    in_motion;         // whether it is doing so right now
    int     motion_run;        // quick frames in a row seen so far
    double  motion_scale;      // how far down, harder over ssh than locally

    // What was asked for, and what to do when it does not turn up. Asking is
    // all the size change ever was: a frame captured before the resize landed
    // arrives after it, and one that is dropped on Chrome's side is never
    // re-sent, so the picture has to be measured rather than assumed.
    int     frame_w;           // pixel width the next frame should arrive at
    int     resize_tries;      // asks since the last frame that came back right
    double  resize_at;         // when to ask again, 0 = nothing owed

    bool    fit_width;         // widen the viewport so no page is cut off
    int     fit_w;             // width the page says it needs
    size_t  last_bytes;        // base64 size of the frame just drawn
    double  last_write_ms;     // how long it took to reach the terminal
    size_t  total_bytes;       // and every frame's, for what a trace adds up to
    double  worst_write_ms;    // the slowest single frame of the run
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

    // The tab bar. It takes a row above the page, and only once there is more
    // than one page to name: a window showing the only tab it has is a window
    // with no tabs, and the row is worth more to the picture.
    Tab     tabs[TAB_MAX];
    int     ntabs;
    int     tab;                  // the one the socket is on
    bool    tabs_open;            // whether the bar has a row right now
    int     tabs_row;             // 1-based row it is drawn on
    Buf     tabs_buf, tabs_last;
} App;

// Note an outstanding call so its reply can be recognised, and claim the reply.
void app_req_note(App *a, int id, int kind);
int  app_req_take(App *a, int id);

// Everything that is not a coalesced pointer event goes through here, so a held
// back scroll or move always reaches the page ahead of whatever follows it.
int  app_cdp(App *a, const char *method, const char *fmt, ...);

void notify(App *a, const char *s);

// The CDP setup a page needs before it is worth drawing, where the window is
// now, and the note on disk that says how to reach it. Shared with the tab
// code, which moves the socket between pages and has to do all three again.
void session_init(App *a);
void session_write(App *a);
void ask_where(App *a);

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

// ---------------------------------------------------------------- tabs

// Start the list over on whatever page the socket is on now: one tab, the one
// in front. Called after every attach, since none of the old browser's pages
// survive a change of browser.
void tabs_init(App *a);
void tabs_free(App *a);

bool tabs_wanted(const App *a);        // whether the bar has earned its row
void tabs_paint(App *a);
bool tabs_mouse(App *a, Event *ev);    // true when the bar consumed the click

void tab_new(App *a);
void tab_close(App *a);
void tab_go(App *a, int idx);          // 0-based
void tab_step(App *a, int delta);      // wraps

// The page the socket was on has gone. Fall back to another tab if there is
// one; false means there is nothing left to draw and the window is over.
bool tab_lost(App *a);

// Close every tab of ours but the one in front, which the caller deals with.
void tabs_close_others(App *a);

// Whether the page in front is one this window has not set up before. The
// per-session half of that setup is done every time the socket moves; the half
// that sticks to the page itself must not be done twice, or a tab switched
// back to a dozen times carries a dozen copies of the watcher script.
bool tab_session_new(App *a);

#endif

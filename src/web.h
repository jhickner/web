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

uint64_t fnv1a(const char *p, size_t n);
int    writeall(int fd, const char *p, size_t n);
void   mkdirs(const char *path);

// Bytes written. dst needs room for three per four of src.
size_t base64_decode(const char *src, size_t n, char *dst);

// ---------------------------------------------------------------- json

// buffer must be NUL-terminated; values point into it
const char *json_str(const char *js, const char *key, size_t *len);
double      json_num(const char *js, const char *key, double def);
bool        json_has(const char *js, const char *key);
size_t      json_escape(char *dst, size_t cap, const char *src);
void        json_escape_buf(Buf *out, const char *src);
size_t      json_unescape(char *dst, size_t cap, const char *src, size_t n);

// the by-value string of a Runtime.evaluate reply, or NULL
const char *json_eval_str(const char *msg, size_t *len);

// the `n`th element of a JSON array, or NULL when the array ends first
const char *json_array_at(const char *arr, int n);

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
    bool  adopted;      // an earlier run's
    bool  foreign;      // attached to one we never started
    char  profile[512];
    char  target[96];   // the page we are driving
    WS    ws;
    WS    watch;        // the browser's own socket: pages appearing and going
    int   next_id;
} Chrome;

// debug=false leaves out the remote-debugging port; port 0 lets chrome pick one
int  chrome_launch(Chrome *c, const char *url, int w, int h, bool show_window,
                   bool mute, const char *user_agent, bool debug, int port);
int  chrome_attach(Chrome *c);
void chrome_profile_path(char *out, size_t cap);

// call before anything asks where the profile is. a name is letters, digits,
// dot, underscore and dash; "-" is a throwaway one. -1 when the name is not one.
int  chrome_profile_set(const char *name);
bool chrome_profile_named(char *out, size_t cap);   // false when it is the shared one

// 0 when a devtools endpoint answers there
int  chrome_probe(int port);

// age is however ps spelled it: text, not a number
typedef struct { pid_t pid; char age[24]; } ChromeProc;

// all of them, whoever started them
int  chrome_running(const char *profile, ChromeProc *out, int cap);

// the port to adopt, or -1 when there is nothing to adopt. *pid where knowable.
int  chrome_adoptable(const char *profile, pid_t *pid);
int  chrome_user_agent(Chrome *c, char *out, size_t cap);
void chrome_kill(Chrome *c);
void chrome_kill_bg(Chrome *c);

// how many page targets belong to somebody else
int  chrome_other_pages(Chrome *c);
void chrome_close_target(Chrome *c);

// a page of our own in a window of its own; out gets its target id
int  chrome_open_tab(Chrome *c, const char *url, char *out, size_t cap);

// a socket of its own, told when a page appears in this browser or goes away
int  chrome_watch(Chrome *c);
void chrome_unwatch(Chrome *c);

// a socket on the browser endpoint, for what belongs to no page. the caller
// sends, reads and closes it.
int  chrome_browser_ws(Chrome *c, WS *out);

// move the session onto another page; a failure leaves it where it was
int  chrome_switch_target(Chrome *c, const char *target);
void chrome_close_id(Chrome *c, const char *target);

// leave a tab behind that holds the browser open
void chrome_park(Chrome *c);

// --keep, remembered against the browser instead of the run
void chrome_mark_kept(Chrome *c);
bool chrome_is_kept(Chrome *c);

// Fire-and-forget CDP call; params is a JSON object body without braces.
int  cdp_call(Chrome *c, const char *method, const char *params_fmt, ...);

// over the watch socket, through a flattened session. the request id, or -1
// when there is no socket.
int  cdp_session_call(Chrome *c, const char *session, const char *method,
                      const char *params_fmt, ...);
int  cdp_vcall(Chrome *c, const char *method, const char *params_fmt, va_list ap);

// ---------------------------------------------------------------- graphics

#define GRID_MAX 9

typedef struct {
    int  x, y, cols, rows;
    unsigned gen;
    bool grid_dirty;
    bool live;               // something has been sent under this name
} KittyTile;

typedef struct {
    int  ttyfd;
    bool tmux;
    int  x, y, cols, rows;   // 1-based cell rect the page occupies
    bool grid_dirty;
    unsigned grid_draws;     // grids laid down
    unsigned gen;            // which name the picture is going up under
    int  slot;               // which tile the fields above are describing
    KittyTile tiles[GRID_MAX + 1];
    Buf  out;
} Kitty;

void kitty_init(Kitty *k, int ttyfd, bool tmux);
// Which tile the calls below are about; 0 is the whole picture.
void kitty_use(Kitty *k, int slot);
bool kitty_tile_live(const Kitty *k, int slot);
void kitty_area(const Kitty *k, int *x, int *y, int *cols, int *rows);
void kitty_set_rect(Kitty *k, int x, int y, int cols, int rows);
int  kitty_draw_png(Kitty *k, const char *b64, size_t len);
void kitty_clear(Kitty *k);

// take the picture down and come back under a new name
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

// a move with nothing held down
#define BTN_NONE (-1)

typedef enum { EV_NONE, EV_KEY, EV_MOUSE, EV_PASTE, EV_FOCUS, EV_EOF } EvType;

// ------------------------------------------------------------------- keys

// ACT_TAB_1 through ACT_TAB_9 must stay consecutive
typedef enum {
    ACT_NONE = 0,
    ACT_SCROLL_DOWN, ACT_SCROLL_UP, ACT_SCROLL_LEFT, ACT_SCROLL_RIGHT,
    ACT_LINE_DOWN, ACT_LINE_UP, ACT_HALF_DOWN, ACT_HALF_UP,
    ACT_PAGE_DOWN, ACT_PAGE_UP, ACT_TOP, ACT_BOTTOM,
    ACT_ADDRESS, ACT_ADDRESS_BLANK, ACT_ADDRESS_TAB, ACT_BACK, ACT_FORWARD,
    ACT_RELOAD, ACT_RELOAD_HARD,
    ACT_COPY, ACT_COPY_URL, ACT_COPY_CONSOLE, ACT_EXTERNAL, ACT_RESUME,
    ACT_GRID, ACT_RECORD, ACT_PLAY_PAUSE,
    ACT_FIND, ACT_FIND_NEXT, ACT_FIND_PREV,
    ACT_HINT, ACT_HINT_TAB, ACT_HINT_COPY, ACT_HINT_ALL,
    ACT_INSERT, ACT_INSERT_OFF, ACT_FOCUS_INPUT, ACT_PICK,
    ACT_TAB_NEW, ACT_TAB_CLOSE, ACT_TAB_NEXT, ACT_TAB_PREV, ACT_MERGE,
    ACT_SEARCH_TABS, ACT_SEARCH_HISTORY, ACT_SEARCH_BOOKMARKS, ACT_BOOKMARK,
    ACT_TAB_1, ACT_TAB_2, ACT_TAB_3, ACT_TAB_4, ACT_TAB_5,
    ACT_TAB_6, ACT_TAB_7, ACT_TAB_8, ACT_TAB_9,
    ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_ZOOM_RESET, ACT_SMALLER, ACT_LARGER,
    ACT_FIT, ACT_PAGE_WIDER, ACT_PAGE_NARROWER, ACT_SCALE,
    ACT_BOX_TALLER, ACT_BOX_SHORTER, ACT_BOX_WIDER, ACT_BOX_NARROWER,
    ACT_CONSOLE, ACT_HELP, ACT_STATUS, ACT_TRACE, ACT_QUIT,
    ACT_INVALID,          // a name the file gave that is not one of these
} Act;

// ACT_NONE when nothing is bound to it
Act  keys_lookup(int mods, int key);

// `pkey` is the key already waiting, or 0 when none is. *prefix true means this
// key begins a pair: the action is ACT_NONE and the caller asks again with the
// next key.
Act  keys_lookup_seq(int pmods, int pkey, int mods, int key, bool *prefix);

// false when nothing is bound to it; `out` is then a dash
bool keys_text(Act act, char *out, size_t cap);

void key_text(int mods, int key, char *out, size_t cap);
const char *act_name(Act act);

typedef struct {
    EvType type;
    int    key;          // KEY_* or a unicode codepoint
    int    mods;
    char   text[8];      // UTF-8 for printable keys, empty otherwise
    int    mx, my;       // 1-based cell coords
    int    px, py;       // pixel coords when the terminal reports them
    bool   have_pixels;
    int    button;       // 0 left, 1 middle, 2 right, 3 wheel-up, 4 wheel-down,
                         // 5 wheel-left, 6 wheel-right, BTN_NONE on a hover
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
    double esc_at;      // when a lone ESC showed up
} Term;

int  term_probe(Term *t);                 // open the tty and measure it
void term_enter(Term *t, bool inline_mode); // raw mode, alt screen, mouse
void term_hover(Term *t, bool on);        // report moves with no button down
void term_reserve_inline(Term *t, int rows);
void term_resize_inline(Term *t, int rows);   // caller drops the image first
void term_clear_inline(Term *t);              // blank the rows the block owns
void term_clear_below(Term *t);               // and anything left under them
void term_restore(Term *t, bool clear_inline);  // erase the block on the way out

// key and mouse modes back to what they were, and nothing else. safe to call
// from a signal handler.
void term_panic(int fd);
void term_size(Term *t);
int  term_read(Term *t);                  // pull available bytes
void term_log(const char *fmt, ...);      // WEB_DEBUG input trace

// appends to /tmp/web_input.log. -1 toggles; returns whether it is now on.
bool term_trace(int on);
bool term_tracing(void);
int  term_next(Term *t, Event *ev);       // 1 = event decoded

extern volatile sig_atomic_t g_resized;
extern volatile sig_atomic_t g_quit;

// set while shutting down: a write waits for the terminal instead of giving up
extern volatile sig_atomic_t g_write_force;

// called from inside long tty writes
extern void (*g_input_pump)(void);

// ---------------------------------------------------------------- app

enum { PEND_NONE, PEND_MOVE };

// one pointer move held back, replaced by the next one
typedef struct {
    int         kind;
    int         x, y;
    int         mods;
    int         buttons;
    const char *btn;
} Pending;

// what an outstanding CDP call was for
enum {
    RQ_NONE, RQ_TITLE, RQ_URL, RQ_COPY, RQ_FIT, RQ_SCRIPT,
    RQ_SELECTOR, RQ_RECORD, RQ_PDF, RQ_SHOT_READY, RQ_SHOT, RQ_FRAME,
    RQ_MODE, RQ_HISTORY, RQ_STILL, RQ_LINK, RQ_PLAY, RQ_HELD
};

// the isolated world our injected script lives in
#define WEB_WORLD "web"

// js: shortest CSS path that picks out one element
#define SELECTOR_FN \
    "function ws(e){if(!e||e.nodeType!==1)return '';" \
    "if(e.id)return '#'+CSS.escape(e.id);var p=[];" \
    "while(e&&e.nodeType===1&&e!==document.body){" \
    "var s=e.tagName.toLowerCase(),i=1,q=e;while((q=q.previousElementSibling))" \
    "if(q.tagName===e.tagName)i++;if(i>1)s+=':nth-of-type('+i+')';p.unshift(s);" \
    "var z=p.join(' > ');try{if(document.querySelectorAll(z).length===1)return z}catch(_){}" \
    "e=e.parentElement}return p.join(' > ')}"

// js: `rl` is the role, `nm` the accessible name
#define ROLE_NAME_FN \
    "function nm(e){" \
    "var a=e.getAttribute&&(e.getAttribute('aria-label')||" \
    "(e.getAttribute('aria-labelledby')&&(document.getElementById(" \
    "e.getAttribute('aria-labelledby'))||{}).textContent));" \
    "if(a)return a.trim();" \
    "if(e.labels&&e.labels[0])return e.labels[0].textContent.trim();" \
    "if(e.alt)return e.alt.trim();" \
    "if(e.tagName==='INPUT'&&(e.type==='submit'||e.type==='button'))" \
    "return (e.value||'').trim();" \
    "if(['INPUT','SELECT','TEXTAREA'].indexOf(e.tagName)>=0)return '';" \
    "return (e.textContent||'').trim().replace(/\\s+/g,' ').slice(0,80)}" \
    "function rl(e){var r=e.getAttribute&&e.getAttribute('role');if(r)return r;" \
    "var t=e.tagName,y=(e.type||'').toLowerCase();" \
    "if(t==='A'&&e.href)return 'link';" \
    "if(t==='BUTTON'||(t==='INPUT'&&(y==='submit'||y==='button'||y==='reset')))" \
    "return 'button';" \
    "if(t==='INPUT'&&y==='checkbox')return 'checkbox';" \
    "if(t==='INPUT'&&y==='radio')return 'radio';" \
    "if(t==='SELECT')return 'combobox';" \
    "if(t==='TEXTAREA'||(t==='INPUT'&&['text','search','email','url','tel'," \
    "'password',''].indexOf(y)>=0))return 'textbox';" \
    "if(/^H[1-6]$/.test(t))return 'heading';" \
    "if(t==='IMG')return 'img';return ''}"

#define REQ_MAX 8

typedef struct { int id, kind; } Req;

// --screenshot state
enum {
    SHOT_NONE,     // none was asked for
    SHOT_LOAD,     // waiting for the page, and for any script run against it
    SHOT_SETTLE,   // waiting for the fonts and the paint after them
    SHOT_SENT,     // the capture is out at the browser
    SHOT_DONE,
    SHOT_FAIL,
};

// ---------------------------------------------------------------- script

// script runner state
enum {
    SC_IDLE,      // nothing running; the queue may still have lines in it
    SC_WAIT,      // a line is out at the page; its reply is the value
    SC_NAV,       // that line started a load, and the next one waits for it
};

typedef struct {
    Buf      queue;         // pending lines of javascript, '\n' separated, FIFO
    char     line[4096];    // the one running now
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
    bool     from_file;     // --script: a failure stops the rest of it
    bool     drain_exit;    // exit when the queue runs out
    int      failures;
} Script;

// ---------------------------------------------------------------- tabs

// one page of the browser, drawn as one tab of the bar
typedef struct {
    char target[96];    // the devtools page id
    char url[1024];     // where it was when the window last looked away
    char title[256];
    bool ours;          // we opened it: ours to close on the way out
    bool claimed;       // a driver opened this one, and will say when it goes
    bool inited;        // the once-per-page CDP setup has been done
    int  x0, x1;        // cells the bar last drew it on
    Buf  shot;          // the last picture taken of it
    uint64_t shot_hash; // and what it hashed to
    int  shot_w, shot_h;// the size it was last told to lay itself out for
    char session[64];   // the flattened session its tile is captured through
} Tab;

#define TAB_MAX 32

// a page one of ours opened, seen appearing and not yet worth a tab
typedef struct {
    char   target[96];
    double at;
} Popup;

#define POPUP_MAX   8
#define POPUP_GRACE 15.0   // seconds a popup has to say where it is going

#define BOX_MIN_ROWS 2     // least rows an inline window can be left with

// longest a setting written as text may be
#define SETTING_TEXT_MAX 512

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
    unsigned load_seq;         // bumped on every load event
    int     hist_delta;        // which way the outstanding history call is going

    // the page's top frame
    char    frame[64];

    // the pdf viewer is in front; keys reach it only once it has been clicked in
    bool    pdf;
    bool    pdf_clicked;

    bool    editing;           // URL bar has focus
    char    edit[1024];
    size_t  edit_len;

    uint64_t last_hash;
    unsigned frames, skipped, stills;
    double  last_draw;
    double  last_metrics_fix;  // when the viewport override was last restored
    double  expect_frame;      // something was done that should redraw; deadline
    double  last_unwedge;      // when the screencast was last restarted for it
    int     unwedge_run;       // restarts since the last frame that answered one

    double  zoom;              // page magnification, alt+= / alt+-
    // what the last relayout actually put on screen
    double  zoom_eff;

    bool    inline_mode;       // a block in the shell's flow
    int     want_rows;         // rows for that block, 0 = pick one
    int     status_row;
    bool    hide_status;       // the status line is not wanted, ^G
    bool    status_open;       // whether it is on screen right now

    bool    extensions;        // load what ~/.config/web/extensions holds

    bool    pause_on_blur;     // stop drawing while the terminal is not focused
    bool    hide_on_blur;      // and take the window off the screen with it
    bool    media_pause_on_blur;  // and stop whatever is playing in the page
    bool    media_held;        // something was stopped that way, and owes a play
    bool    no_pause_arg;      // --no-pause: no site rule turns it on
    bool    no_media_arg;      // --no-media-pause, the same for the playing
    bool    paused;            // and whether that has happened
    bool    hidden;            // and whether the screen was cleared with it
    double  pause_wait;        // 0 none, >0 the deadline, -1 waited and gave up
    bool    blurred;           // what the terminal last said

    // cancel the browser's own action for a navigation key the page did not want
    bool    claim_keys;

    bool    insert;            // a text field has focus: keys belong to the page
    bool    player;            // a media player has focus: the arrows are its own
    bool    mouse_down;        // a button went down on the page and is still held
    bool    hover;             // pass moves with no button down to the page
    bool    hovering;          // the page believes the pointer is over it
    bool    click_newtab;      // and it was the one that opens a link in a tab
    bool    show_start;        // this run wrote the start page
    bool    vim;               // the vim layer, under the keys web.conf names
    int     vim_shadowed;      // and how many of it that file took back
    int     pend_mods;         // the first key of a pair, still waiting for its
    int     pend_key;          // second; key 0 when nothing is waiting
    int     prompt;            // 0 none, 1 address, 2 find
    char    find[256];         // last search, for n and N
    char    search[SETTING_TEXT_MAX];   // where a phrase goes, %s for the words

    int     box_rows;          // inline: cell rows the window occupies, BOX_MIN_ROWS up
    int     box_cols;          // inline: cell columns, 0 = from the proportion
    bool    tmux_zoom;         // fill the pane while tmux has it zoomed
    bool    zoomed;            // whether the pane is zoomed right now
    int     unzoom_rows;       // the window's size before it was zoomed
    int     unzoom_cols;
    int     want_cols;         // cols for that block, 0 = from the proportion
    int     want_width;        // width pinned by w/W, 0 = derived from the cells
    bool    scale_locked;      // the render scale was chosen by hand

    // --scale auto: a picture that is moving is drawn at fewer pixels
    bool    motion_auto;
    bool    in_motion;         // whether it is doing so right now
    int     motion_run;        // quick frames in a row seen so far
    double  motion_scale;      // how far down
    int     settle_ms;         // nothing happening for this long ends the motion

    int     frame_w;           // pixel width the next screencast frame should be
    int     still_w;           // and what a screenshot comes back at
    double  still_at;          // when to ask for one, 0 = nothing owed
    double  still_sent;        // deadline on the reply, 0 = none outstanding
    int     still_tries;       // asks since the last one that answered
    double  last_still;
    double  last_input;

    bool    fit_width;         // widen the viewport so no page is cut off
    int     fit_w;             // width the page says it needs
    unsigned nav_seq;          // documents this window has been shown
    unsigned fit_seq;          // which of them the outstanding measure was for
    size_t  last_bytes;        // base64 size of the frame just drawn
    double  last_write_ms;     // how long it took to reach the terminal
    size_t  total_bytes;       // and every frame's
    double  worst_write_ms;    // the slowest single frame of the run
    double  fps;               // smoothed
    double  bytes_per_sec;

    char    msg[256];
    double  msg_until;

    char    ua[512];           // chrome's own user agent, headless marker gone
    bool    ua_patch_req;      // this browser predates the flag
    bool    mute;              // start chrome with its audio switched off
    bool    clear_exit;        // erase the inline block on the way out (default)
    bool    keep;              // leave chrome running for the next start to adopt

    bool    has_tty;           // a real terminal to draw into
    bool    stdout_tty;        // fd 1 is that terminal

    int     exec_fd;           // --exec: the child's output, -1 when none
    pid_t   exec_pid;
    Buf     exec_buf;          // what has arrived of the line being read
    int     slowmo;            // ms between a driver's actions
    bool    rec_on;            // driving the page is being written down as a spec
    char    rec_path[512];     // where it is being written
    double  nav_asked;         // when the page last said it was going somewhere
    bool    nav_ours;          // and whether this window is what sent it
    char    name[64];          // --name
    bool    freeze;            // a driver that fails holds the page where it failed
    bool    exec_paused;       // and is waiting to be let go of
    char    exec_note[160];    // what it said it was waiting about

    // --screenshot
    const char *shot_path;     // where the png goes; "-" is stdout
    bool    shot_stdout;       // and when it does, fd 1 is the picture's
    int     shot_state;        // SHOT_*
    double  shot_deadline;     // give up on the step in flight here

    Buf     status, status_last;

    Script  script;

    // the command pane; the editor itself lives in replpane.c
    bool    console_open;         // drawn below the page
    bool    console_focus;        // and holding the keyboard
    bool    selector_pick;        // clicks report a selector instead of reaching the page
    int     console_rows;         // how many rows it occupies
    int     console_want;         // rows asked for by dragging its top border
    int     console_row;          // 1-based row it starts on
    Buf     console_buf, console_last;

    // the tab bar; a row above the page, only once there is more than one page
    Tab     tabs[TAB_MAX];
    int     ntabs;
    int     tab;                  // the one the socket is on
    bool    grid_on;              // every tab at once, each in a tile
    bool    grid_auto;            // and open it by itself once there is more than one
    int     grid_shown_tab;       // which tile the labels were last drawn for
    int     grid_turn;            // whose turn it is to be photographed
    int     grid_req;             // the capture outstanding, 0 when none
    int     grid_req_tab;         // and which tile it is for
    double  grid_req_at;          // when it went out
    double  grid_next_at;         // when the next one may go
    int     grid_attach_req[GRID_MAX];  // the session each tile is waiting for
    Popup   popups[POPUP_MAX];    // pages ours opened, waiting for an address
    int     npopups;
    // windows folded into this one, and this one folded into another
    double  merge_until;          // collecting replies until here, 0 when not
    int     merge_want;           // windows still owing one
    int     merge_got;            // tabs taken so far
    int     merge_lost;           // and ones the bar had no room for
    bool    giving_tabs;          // ours are offered to another window
    bool    gave_tabs;            // and taken: theirs to close, not ours
    double  giving_until;         // give the offer up here if nobody has taken it
    char    giving_path[820];     // the offer, removed when it is taken

    bool    tabs_open;            // whether the bar has a row right now
    int     tabs_row;             // 1-based row it is drawn on
    Buf     tabs_buf, tabs_last;

    // the key list, `?`, drawn into the cells the picture occupies
    bool     help_open;
    int      help_page;           // the page of the list on show, from zero
    unsigned help_grid;           // the grid count it was last drawn over
    Buf      help_buf, help_last;

    bool     omni_open;
    int      omni_mode;           // OMNI_TABS, OMNI_HIST or OMNI_MARKS
    char     omni_q[128];         // what has been typed at the list
    size_t   omni_qlen;
    int      omni_sel;            // the row picked, in the filtered order
    int      omni_scroll;         // rows past the top of the ones on screen
    unsigned omni_grid;
    Buf      omni_buf, omni_last;

    // link labels, `f`, drawn by the page; while they are up every key is theirs
    bool     hint_on;
    int      hint_kind;           // 0 follow, 1 new tab, 2 copy the address
    int      hint_n;              // labels the page reported putting up
    char     hint_typed[16];      // what has been typed at them, for the line
    double   hint_deadline;       // a page that never answers gives them back
} App;

// ----------------------------------------------------------------- config

// defaults, then ~/.config/web/web.conf over them, writing the file if it is
// not there. call before the command line is read. returns how many lines made
// no sense, each already reported on stderr.
int  config_load(App *a);
void config_dir(char *out, size_t cap);

// the start page beside the config; false when there is none
bool start_page_path(char *out, size_t cap);

// the selector a `hint-only` or `hint-skip` line gave this page's host, or NULL.
// `skip` picks which of the two kinds.
const char *hint_selector(const char *url, bool skip);

// false when no line names this page's host
bool pause_rule(const char *url, bool *out);

// the media-pause-on-blur rule for this page's host
bool media_rule(const char *url, bool *out);

// -------------------------------------------------------------- extensions

// an unpacked folder; false when it holds no manifest.json, already reported
bool ext_add(const char *path);

// every folder under ~/.config/web/extensions holding a manifest.json
void ext_scan(void);
void ext_dir(char *out, size_t cap);
int  ext_count(void);

// hands them to the browser; returns how many loaded
int  ext_load(Chrome *c);

// a tab going out of the front, and one arriving there
void media_tab_leave(App *a);
void media_tab_enter(App *a);

// note an outstanding call, and claim its reply
void app_req_note(App *a, int id, int kind);
int  app_req_take(App *a, int id);

// everything that is not a coalesced pointer event goes through here
int  app_cdp(App *a, const char *method, const char *fmt, ...);

void notify(App *a, const char *s);

// into the terminal's clipboard
void clipboard_put(const char *text);

// the per-page CDP setup, the note on disk, and where the window is now
void session_init(App *a);
void session_write(App *a);
void ask_where(App *a);

// move the session onto a browser on `port`; msg gets what happened. 0 = attached.
int  app_attach(App *a, int port, char *msg, size_t cap);

void navigate(App *a, const char *raw);

// the address `navigate` would use for a line typed at the bar, without going
void bar_url(const char *raw, char *url, size_t cap);

// the template a phrase becomes an address through; bar_url works from this
void search_set(const char *tpl);
void run_js(App *a, const char *js);
void relayout(App *a);

// drop any outstanding or owed screenshot of the page
void still_cancel(App *a);

// ask for one shortly, unless a frame turns up first
void still_soon(App *a);
void scroll_at(App *a, int x, int y, int dy);
void scroll_by(App *a, int dy);
void scroll_page_end(App *a, bool bottom);
void nav_history(App *a, int delta);
bool special_key(App *a, int key, int mods);

// ---------------------------------------------------------------- script

void script_init(App *a);
void script_free(App *a);

// queue one line of javascript
void script_push(App *a, const char *line);
int  script_load(App *a, const char *path);   // "-" or NULL reads stdin
bool script_busy(const App *a);

// call once per pass of the main loop
void script_step(App *a);

// how long the loop may sleep, in ms, or -1 when the runner wants nothing
int  script_wait_ms(const App *a);

// a reply the runner asked for
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
int  console_copy(App *a);           // the transcript to the clipboard; lines sent
void console_history_add(App *a, const char *line);

// ---------------------------------------------------------------- help

void help_toggle(App *a);            // `?`, and the key that puts it away again
bool help_key(App *a, Event *ev);    // true when the list consumed it
void help_paint(App *a);
void help_free(App *a);

// ---------------------------------------------------------------- the lists

// the tabs already open, the pages the profile has been to, or the bookmarks
#define OMNI_TABS  0
#define OMNI_HIST  1
#define OMNI_MARKS 2

// false when no bookmark holds every word of the phrase
bool omni_best_bookmark(const char *query, char *url, size_t cap);

void omni_show(App *a, int mode);
void omni_close(App *a);             // safe when it is not open
bool omni_key(App *a, Event *ev);    // true when the list consumed it
void omni_paste(App *a, const char *text, size_t len);
void omni_paint(App *a);
void omni_free(App *a);

// ---------------------------------------------------------------- bookmarks

// chrome's own, read and written in `Bookmarks` in the profile
#define BOOKMARK_MAX 500

typedef struct {
    char   url[1024];
    char   title[256];
    double added;                // unix seconds, out of Chrome's own count
} Bookmark;

// newest added first; the array is good until the next call or toggle
const Bookmark *bookmarks_all(int *n);

// the page in front into the bookmark bar, or out of whatever folder holds it
void bookmark_toggle(App *a);

// whether the page in front is bookmarked; held between address changes
bool bookmark_current(App *a);

// ---------------------------------------------------------------- hints

// `fresh` is tab_session_new()
void hint_install(App *a, bool fresh);

// `kind` 0 follow, 1 new tab, 2 copy the address. `all` ignores the site rules.
void hint_show(App *a, int kind, bool all);
void hint_cancel(App *a);             // labels away, mode over; safe when off
bool hint_key(App *a, Event *ev);     // true when the labels consumed it

// call for every `__webhint` binding message
void hint_reply(App *a, char *payload);

// call once round the main loop
void hint_tick(App *a);

// ---------------------------------------------------------------- tabs

// start the list over on the page the socket is on. call after every attach.
void tabs_init(App *a);
void tabs_free(App *a);

bool tabs_wanted(const App *a);        // whether the bar has earned its row
void tabs_paint(App *a);
void tab_label(const App *a, int i, char *out, size_t cap);
bool tabs_mouse(App *a, Event *ev);    // true when the bar consumed the click

void tab_new(App *a);

// a tab opened on an address; the window is left on it
bool tab_open_url(App *a, const char *url);
bool tab_open_bg(App *a, const char *url); // a tab behind this one, not switched to

void tab_close(App *a);
// a driver's page taken into the bar; the window does not move off the tab it
// is on
bool tab_adopt(App *a, const char *target, const char *url);
int  tab_index_of(const App *a, const char *target);   // -1 when it is not one
bool tab_forget(App *a, const char *target);

// a page handed over by another window; it becomes ours, and the window does
// not move onto it
bool tab_take(App *a, const char *target, const char *url, const char *title);

void tab_go(App *a, int idx);          // 0-based
void tab_step(App *a, int delta);      // wraps

// the page the socket was on has gone; false when no tab is left
bool tab_lost(App *a);

// close every tab of ours but the one in front
void tabs_close_others(App *a);

// ---------------------------------------------------------------- the grid
void grid_toggle(App *a);
void grid_off(App *a, const char *why);   // safe when it is not on
void grid_paint(App *a);
void grid_tick(App *a);                   // whose turn it is, and asking
bool grid_reply(App *a, const char *msg); // true when the message was the grid's
bool grid_mouse(App *a, Event *ev);       // true when a tile took the click
bool grid_key(App *a, Event *ev);         // arrows pick a tile, enter opens it
bool grid_tile_rect(const App *a, int i, int *x, int *y, int *cols, int *rows);
int  grid_tile_at(const App *a, int col, int row);
int  grid_count(const App *a);

// ---------------------------------------------------------------- recording
//
// driving the page by hand, written down as a playwright spec
void record_start(App *a, const char *path);
void record_stop(App *a);
void record_toggle(App *a);
void record_event(App *a, const char *json);   // one interaction, from the page
void record_goto(App *a, const char *url);
void record_history(App *a, int delta);        // back, or forward
void record_install(App *a, bool fresh);       // the watcher, into a new document

// -------------------------------------------------------------------- mcp
//
// speaks the protocol on stdio and drives a window already up
int mcp_serve(void);

// the address moves into a tab of our own and the page it came from is closed
bool tab_from_popup(App *a, const char *target, const char *url);

// whether the page in front is one this window has not set up before
bool tab_session_new(App *a);

#endif

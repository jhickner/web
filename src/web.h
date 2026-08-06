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

// The hash a picture is deduped by. Chrome's encoder is deterministic, so two
// frames of an unchanged page are the same bytes and hash the same.
uint64_t fnv1a(const char *p, size_t n);
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

// The `n`th element of a JSON array, or NULL when the array ends first.
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
    bool  adopted;      // an earlier run's, so we open a window of our own in it
    bool  foreign;      // attached to one we never started: leave it running
    char  profile[512];
    char  target[96];   // the page we are driving, so the others stay theirs
    WS    ws;
    WS    watch;        // the browser's own socket: pages appearing and going
    int   next_id;
} Chrome;

// debug=false leaves out the remote-debugging port entirely: a browser that
// can be driven over CDP can also have its credentials read that way, and
// Google refuses to sign anyone in to one. port 0 lets Chrome pick a free one.
int  chrome_launch(Chrome *c, const char *url, int w, int h, bool show_window,
                   bool mute, const char *user_agent, bool debug, int port);
int  chrome_attach(Chrome *c);
void chrome_profile_path(char *out, size_t cap);

// Which profile this run works in, before anything asks where it is. A name is
// letters, digits, dot, underscore and dash; "-" is a throwaway one, made for
// this run and removed when its browser goes. -1 when the name is not one.
int  chrome_profile_set(const char *name);
bool chrome_profile_named(char *out, size_t cap);   // false when it is the shared one

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
int  chrome_open_tab(Chrome *c, const char *url, char *out, size_t cap);

// Ask to be told when a page appears in this browser or goes away, on a socket
// of its own. A page is not the only thing that opens a page - a link asking for
// a window, or a script calling open(), makes one nothing here asked for - and
// the browser endpoint is the only place that news arrives. Nothing is sent on
// this socket afterwards; every call the window makes still goes to its page.
int  chrome_watch(Chrome *c);
void chrome_unwatch(Chrome *c);

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

// Against another of the browser's pages, over the watch socket, through a
// flattened session. Answers the request id, or -1 when there is no socket.
int  cdp_session_call(Chrome *c, const char *session, const char *method,
                      const char *params_fmt, ...);
int  cdp_vcall(Chrome *c, const char *method, const char *params_fmt, va_list ap);

// ---------------------------------------------------------------- graphics

// How many pages the grid draws at once. Nine because a tile smaller than a
// third of the window is a colour, not a page.
#define GRID_MAX 9

// Everything one tile has to remember while another is being drawn. The rect
// and the name go together: a tile put up under one name and moved is a tile
// whose old cells still name the picture where it used to be.
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
    unsigned grid_draws;     // grids laid down, for anything drawn over them
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

// A move with nothing held down. Below zero rather than another button number,
// so the wheel tests - which are all "the button is 3 or more" - keep meaning
// what they meant before there was anything else for a move to be.
#define BTN_NONE (-1)

typedef enum { EV_NONE, EV_KEY, EV_MOUSE, EV_PASTE, EV_FOCUS, EV_EOF } EvType;

// ------------------------------------------------------------------- keys

// Everything a key can be asked to do. The keys themselves are in config.c,
// where ~/.config/web/web.conf is read over the defaults; main.c only ever
// sees the action a key came out as. tab-1 through tab-9 are consecutive on
// purpose: the one handler subtracts to get the number.
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

// The config file is read into the App, so loading it is declared with the
// rest of what takes one, below.

// What this key means now, or ACT_NONE for one that means nothing.
Act  keys_lookup(int mods, int key);

// The same question, asked by a keyboard that may be halfway through a pair.
// `pkey` is the key already waiting, or 0 when none is; a pair whose two keys
// are these is what the answer comes from. *prefix comes back true when this
// key begins a pair instead of being one: the action is then ACT_NONE and
// nothing has happened yet, so the caller holds the key and asks again with
// the next one. A key that is bound on its own is that binding at once, and
// the pairs it could have started are unreachable - which is why the file that
// adds `y f` unbinds `y` first.
Act  keys_lookup_seq(int pmods, int pkey, int mods, int key, bool *prefix);

// A key that does this, for the help list; false when nothing is bound to it,
// and `out` is a dash so the list still has something to draw.
bool keys_text(Act act, char *out, size_t cap);

// The spellings, both ways round, for the generated file and for messages.
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
    double esc_at;      // when a lone ESC showed up, to tell it from a sequence
} Term;

int  term_probe(Term *t);                 // open the tty and measure it
void term_enter(Term *t, bool inline_mode); // raw mode, alt screen, mouse
void term_hover(Term *t, bool on);        // report moves with no button down
void term_reserve_inline(Term *t, int rows);
void term_resize_inline(Term *t, int rows);   // caller drops the image first
void term_clear_inline(Term *t);              // blank the rows the block owns
void term_clear_below(Term *t);               // and anything left under them
void term_restore(Term *t, bool clear_inline);  // erase the block on the way out

// The key and mouse modes back to what they were, and nothing else: the one
// thing that has to happen even when the window is being killed rather than
// closed. Safe to call from a signal handler.
void term_panic(int fd);
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
    RQ_MODE, RQ_HISTORY, RQ_STILL, RQ_LINK, RQ_PLAY
};

// The isolated world everything we inject for our own use lives in. It shares
// the page's DOM and nothing else: our globals are not on the page's window,
// and a page redefining one of the things we call cannot reach ours.
#define WEB_WORLD "web"

// The shortest CSS path that picks out one element, walked up from it until a
// query matches only that one. The last resort of anything naming an element:
// the picker reports one, and the recorder writes one down when nothing
// semantic is unique.
#define SELECTOR_FN \
    "function ws(e){if(!e||e.nodeType!==1)return '';" \
    "if(e.id)return '#'+CSS.escape(e.id);var p=[];" \
    "while(e&&e.nodeType===1&&e!==document.body){" \
    "var s=e.tagName.toLowerCase(),i=1,q=e;while((q=q.previousElementSibling))" \
    "if(q.tagName===e.tagName)i++;if(i>1)s+=':nth-of-type('+i+')';p.unshift(s);" \
    "var z=p.join(' > ');try{if(document.querySelectorAll(z).length===1)return z}catch(_){}" \
    "e=e.parentElement}return p.join(' > ')}"

// What an element is and what a user would call it: `rl` the role, explicit or
// the one the tag implies, and `nm` the accessible name, near enough. Shared,
// so a locator written down by the recorder names the same thing an agent was
// shown by the snapshot.
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
    /* A field is not named by what is inside it: a select's text is its own \
       options, and calling one 'alphabeta' names a thing nobody looks for. */ \
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
    bool claimed;       // a driver said it opened this one, and will say when it goes
    bool inited;        // the once-per-page CDP setup has been done
    int  x0, x1;        // cells the bar last drew it on, for clicks
    Buf  shot;          // the last picture taken of it, for its tile in the grid
    uint64_t shot_hash; // and what it hashed to, so an unchanged page is not redrawn
    int  shot_w, shot_h;// the size it was last told to lay itself out for
    char session[64];   // the flattened session its tile is captured through
} Tab;

// Well past what a bar can name comfortably, because the bar's job is to cope
// rather than to refuse: it gives up the titles, then the numbers, then the
// separators, and a tab is still a tab when it is one letter wide. This is only
// the point where the list itself stops, and each entry is a page of the
// browser - a real cost - so it is not unbounded either.
#define TAB_MAX 32

// A page one of ours opened, seen appearing and not yet worth a tab. A popup is
// made before it is sent anywhere, so the address it is created with is usually
// still blank and the real one arrives a message later; this is the note kept in
// between. Only a page seen appearing is ever taken - a popup that has been
// sitting there for a while is somewhere the user has been, not somewhere a
// click was just aimed - so the time it was first seen is half of what is kept.
typedef struct {
    char   target[96];
    double at;
} Popup;

// Small on purpose: these are the popups of one moment, and a page opening more
// than a handful at once is a page opening them at us.
#define POPUP_MAX   8
#define POPUP_GRACE 15.0   // seconds a popup has to say where it is going

#define BOX_MIN_ROWS 2     // the least picture an inline window can be left with

// The longest a setting written as text may be, which is the search template
// and nothing else so far. Long enough for the query strings the engines with
// something to say about themselves ask for.
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
    unsigned load_seq;         // bumped on every load event, for script waits
    int     hist_delta;        // which way the outstanding history call is going

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
    unsigned frames, skipped, stills;
    double  last_draw;
    double  last_metrics_fix;  // when the viewport override was last restored
    double  expect_frame;      // something was done that should redraw; deadline
    double  last_unwedge;      // when the screencast was last restarted for it
    int     unwedge_run;       // restarts since the last frame that answered one

    double  zoom;              // page magnification, alt+= / alt+-
    // What the last relayout actually put on screen. A page held wider than it
    // was asked to be is magnified less than `zoom` says, and the difference is
    // that page's alone: `zoom` is what was typed and survives it.
    double  zoom_eff;

    bool    inline_mode;       // a block in the shell's flow, like rom
    int     want_rows;         // rows for that block, 0 = pick one
    int     status_row;
    bool    hide_status;       // the status line is not wanted, ^G
    bool    status_open;       // whether it is on screen right now

    bool    pause_on_blur;     // stop drawing while the terminal is not focused
    bool    no_pause_arg;      // --no-pause was given, so no site rule turns it on
    bool    paused;            // and whether that has happened
    bool    blurred;           // what the terminal last said, which is not the
                               // same: a window being driven draws through it

    // Cancel the browser's own action for a navigation key the page did not
    // want, so it is never handed back to the window system. The page still
    // gets the key; this only stops the part that happens after nobody claimed
    // it, which on macOS is a walk of the whole menu bar and, on some pages,
    // seconds of the one thread that answers everything.
    bool    claim_keys;

    bool    insert;            // a text field has focus: keys belong to the page
    bool    player;            // a media player has focus: the arrows are its own
    bool    mouse_down;        // a button went down on the page and is still held
    bool    hover;             // pass moves with no button down to the page
    bool    hovering;          // the page believes the pointer is over it
    bool    click_newtab;      // and it was the one that opens a link in a tab
    bool    show_start;        // this run wrote the start page, so it opens on it
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
    int     unzoom_rows;       // the window's size before it was, to come back to
    int     unzoom_cols;
    int     want_cols;         // cols for that block, 0 = from the proportion
    int     want_width;        // width pinned by w/W, 0 = derived from the cells
    bool    scale_locked;      // the render scale was chosen by hand

    // --scale auto: a picture that is moving is drawn at fewer pixels, which
    // are the same pixels Chrome encodes, we write and the terminal decodes.
    bool    motion_auto;
    bool    in_motion;         // whether it is doing so right now
    int     motion_run;        // quick frames in a row seen so far
    double  motion_scale;      // how far down, harder over ssh than locally
    int     settle_ms;         // nothing happening for this long ends the motion

    // The sharp picture, fetched rather than waited for. Restarting the
    // screencast is an ask nothing answers: a page with nothing moving on it
    // gives Chrome no reason to draw, a frame captured before the size changed
    // arrives after it, and a frame dropped for being one too many in flight is
    // never re-sent. A screenshot is a call with a reply, so a still that does
    // not arrive is something this can see and ask for again.
    int     frame_w;           // pixel width the next screencast frame should be
    int     still_w;           // and what a screenshot comes back at
    double  still_at;          // when to ask for one, 0 = nothing owed
    double  still_sent;        // deadline on the reply, 0 = none outstanding
    int     still_tries;       // asks since the last one that answered
    double  last_still;        // so a capture cannot chase the frame it provokes
    double  last_input;        // the other half of "the page has stopped"

    bool    fit_width;         // widen the viewport so no page is cut off
    int     fit_w;             // width the page says it needs
    unsigned nav_seq;          // documents this window has been shown
    unsigned fit_seq;          // which of them the outstanding measure was for
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
    int     slowmo;            // ms between a driver's actions, so it can be watched
    bool    rec_on;            // driving the page is being written down as a spec
    char    rec_path[512];     // where it is being written
    double  nav_asked;         // when the page last said it was going somewhere
    bool    nav_ours;          // and whether this window is what sent it
    bool    freeze;            // a driver that fails holds the page where it failed
    bool    exec_paused;       // and is waiting to be let go of
    char    exec_note[160];    // what it said it was waiting about

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
    int     console_want;         // rows asked for by dragging its top border
    int     console_row;          // 1-based row it starts on
    Buf     console_buf, console_last;

    // The tab bar. It takes a row above the page, and only once there is more
    // than one page to name: a window showing the only tab it has is a window
    // with no tabs, and the row is worth more to the picture.
    Tab     tabs[TAB_MAX];
    int     ntabs;
    int     tab;                  // the one the socket is on
    bool    grid_on;              // every tab at once, each in a tile
    bool    grid_auto;            // and open it by itself once there is more than one
    int     grid_shown_tab;       // which tile the labels were last drawn for
    int     grid_turn;            // whose turn it is to be photographed
    int     grid_req;             // the capture outstanding, 0 when none
    int     grid_req_tab;         // and which tile it is for
    double  grid_req_at;          // when it went out, so a silent page is dropped
    double  grid_next_at;         // when the next one may go
    int     grid_attach_req[GRID_MAX];  // the session each tile is waiting for
    Popup   popups[POPUP_MAX];    // pages ours opened, waiting for an address
    int     npopups;
    // Windows folded into this one, and this one folded into another. A window
    // is on one side or the other of it, never both: the tabs move in a
    // direction, and a collector that also gave its own away would lose them.
    double  merge_until;          // collecting replies until here, 0 when not
    int     merge_want;           // windows still owing one
    int     merge_got;            // tabs taken so far
    int     merge_lost;           // and ones the bar had no room for
    bool    giving_tabs;          // ours are offered to another window
    bool    gave_tabs;            // and taken, so they are its to close, not ours
    double  giving_until;         // give the offer up here if nobody has taken it
    char    giving_path[820];     // the offer, removed when it is taken

    bool    tabs_open;            // whether the bar has a row right now
    int     tabs_row;             // 1-based row it is drawn on
    Buf     tabs_buf, tabs_last;

    // The key list, `?`. It is drawn into the cells the picture occupies, so
    // there is no row of its own to account for anywhere.
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

    // Link labels, `f`. They are drawn by the page, in the picture rather than
    // over it, so there is nothing here but what the keyboard needs: while they
    // are up every key belongs to them.
    bool     hint_on;
    int      hint_kind;           // 0 follow, 1 new tab, 2 copy the address
    int      hint_n;              // labels the page reported putting up
    char     hint_typed[16];      // what has been typed at them, for the line
    double   hint_deadline;       // a page that never answers gives them back
} App;

// ----------------------------------------------------------------- config

// The defaults, then ~/.config/web/web.conf over them - the settings as well
// as the keys, which is why this takes the App it is about to fill in. Writes
// the file first if it is not there yet, so what can be changed is visible
// without reading the manual. Called before the command line is read, so a
// flag still wins for the run it is on. Returns how many lines of the file
// made no sense, each already reported on stderr.
int  config_load(App *a);
void config_dir(char *out, size_t cap);

// The start page beside the config, and whether there is one there to open.
bool start_page_path(char *out, size_t cap);

// The selector a `hint-only` or `hint-skip` line in the file gave this page's
// host, or NULL where the file said nothing about it. `skip` picks which of the
// two kinds is being asked for.
const char *hint_selector(const char *url, bool skip);

// The pause-on-blur rule this page's host is named by, if any. False when no
// line names it, leaving the file-wide setting to say.
bool pause_rule(const char *url, bool *out);

// Note an outstanding call so its reply can be recognised, and claim the reply.
void app_req_note(App *a, int id, int kind);
int  app_req_take(App *a, int id);

// Everything that is not a coalesced pointer event goes through here, so a held
// back scroll or move always reaches the page ahead of whatever follows it.
int  app_cdp(App *a, const char *method, const char *fmt, ...);

void notify(App *a, const char *s);

// Into the terminal's clipboard, by the escape the terminal reads.
void clipboard_put(const char *text);

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

// What a line typed at the address bar means, without going anywhere: the same
// address `navigate` would have used, for the caller that wants it in a tab of
// its own instead.
void bar_url(const char *raw, char *url, size_t cap);

// The template a phrase becomes an address through, once the config file has
// been read. bar_url works from this rather than from the App: the MCP tools
// resolve a line without one, and both have to land in the same place.
void search_set(const char *tpl);
void run_js(App *a, const char *js);
void relayout(App *a);

// Drop any outstanding or owed screenshot of the page. What is on screen is
// about to stop being the page this was asked about.
void still_cancel(App *a);

// Ask for one shortly, unless a frame turns up first and makes it unnecessary.
void still_soon(App *a);
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
int  console_copy(App *a);           // the transcript to the clipboard; lines sent
void console_history_add(App *a, const char *line);

// ---------------------------------------------------------------- help

void help_toggle(App *a);            // `?`, and the key that puts it away again
bool help_key(App *a, Event *ev);    // true when the list consumed it
void help_paint(App *a);
void help_free(App *a);

// ---------------------------------------------------------------- the lists

// Going somewhere by typing a few letters of it: the tabs already open, the
// pages the profile has been to, or Chrome's bookmarks.
#define OMNI_TABS  0
#define OMNI_HIST  1
#define OMNI_MARKS 2

// The bookmark a phrase names, or false when none holds every word of it.
bool omni_best_bookmark(const char *query, char *url, size_t cap);

void omni_show(App *a, int mode);
void omni_close(App *a);             // safe when it is not open
bool omni_key(App *a, Event *ev);    // true when the list consumed it
void omni_paste(App *a, const char *text, size_t len);
void omni_paint(App *a);
void omni_free(App *a);

// ---------------------------------------------------------------- bookmarks

// Chrome's own, read and written in `Bookmarks` in the profile.
#define BOOKMARK_MAX 500

typedef struct {
    char   url[1024];
    char   title[256];
    double added;                // unix seconds, out of Chrome's own count
} Bookmark;

// Every bookmark, newest added first, in an array good until the next call and
// until the next toggle - both read the file again over it.
const Bookmark *bookmarks_all(int *n);

// The page in front into the bookmark bar, or out of whatever folder holds it.
void bookmark_toggle(App *a);

// Whether the page in front is bookmarked, for the star on the status line.
// Cheap to ask on every draw: the answer is held between address changes.
bool bookmark_current(App *a);

// ---------------------------------------------------------------- hints

// Keyboard-only clicking: a label on everything in view worth clicking, and
// the label typed to click it. The labels are the page's own elements, put
// there from the world in web.h's WEB_WORLD, so they arrive in the picture
// like the rest of the page and cost nothing to draw.

// Registered with the rest of the session's bindings and scripts; `fresh` is
// tab_session_new, since the script sticks to the page and the binding does not.
void hint_install(App *a, bool fresh);

// `kind` is 0 follow, 1 new tab, 2 copy the address. `all` labels everything
// clickable whatever the file's site rules say, which is the way back to the
// links a `hint-only` line leaves out.
void hint_show(App *a, int kind, bool all);
void hint_cancel(App *a);             // labels away, mode over; safe when off
bool hint_key(App *a, Event *ev);     // true when the labels consumed it

// The page answering: a count when the labels are up, or the label that was
// typed and where it landed. Called for every `__webhint` binding message.
void hint_reply(App *a, char *payload);

// Once round the main loop, for the page that never answered at all.
void hint_tick(App *a);

// ---------------------------------------------------------------- tabs

// Start the list over on whatever page the socket is on now: one tab, the one
// in front. Called after every attach, since none of the old browser's pages
// survive a change of browser.
void tabs_init(App *a);
void tabs_free(App *a);

bool tabs_wanted(const App *a);        // whether the bar has earned its row
void tabs_paint(App *a);
void tab_label(const App *a, int i, char *out, size_t cap);
bool tabs_mouse(App *a, Event *ev);    // true when the bar consumed the click

void tab_new(App *a);

// A tab opened on an address instead of on the address bar, for the addresses
// after the first on the command line. The window is left on it.
bool tab_open_url(App *a, const char *url);
bool tab_open_bg(App *a, const char *url); // a tab behind this one, not switched to

void tab_close(App *a);
// A page opened by whatever is driving this browser, taken into the bar as it
// is - and dropped again when it goes. Neither moves the window off the tab it
// is on.
bool tab_adopt(App *a, const char *target, const char *url);
int  tab_index_of(const App *a, const char *target);   // -1 when it is not one
bool tab_forget(App *a, const char *target);

// A page handed over by another window that is about to go. Unlike an adopted
// one it becomes ours: the window that opened it is leaving, so this is the
// only one left to close it. The window does not move onto it.
bool tab_take(App *a, const char *target, const char *url, const char *title);

void tab_go(App *a, int idx);          // 0-based
void tab_step(App *a, int delta);      // wraps

// The page the socket was on has gone. Fall back to another tab if there is
// one; false means there is nothing left to draw and the window is over.
bool tab_lost(App *a);

// Close every tab of ours but the one in front, which the caller deals with.
void tabs_close_others(App *a);

// ---------------------------------------------------------------- the grid
//
// Every tab at once, each in a tile of its own, photographed one at a time.
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
// Driving the page by hand, written down as a Playwright spec. What the page
// sees is what is recorded, so a click sent at coordinates and a link label
// typed from the keyboard come out the same.
void record_start(App *a, const char *path);
void record_stop(App *a);
void record_toggle(App *a);
void record_event(App *a, const char *json);   // one interaction, from the page
void record_goto(App *a, const char *url);
void record_history(App *a, int delta);        // back, or forward
void record_install(App *a, bool fresh);       // the watcher, into a new document

// -------------------------------------------------------------------- mcp
//
// The window on screen, as tools an agent can pick up. Not a window itself: it
// speaks the protocol on stdio and drives the window somebody already has up.
int mcp_serve(void);

// Take over a page the browser opened for itself. The address is moved into a
// tab of our own and the page it came from is closed: a popup is put in the
// window that opened it, and Chrome paints only the tab in front of a window,
// so the page as it stands is one we could hear but never draw.
bool tab_from_popup(App *a, const char *target, const char *url);

// Whether the page in front is one this window has not set up before. The
// per-session half of that setup is done every time the socket moves; the half
// that sticks to the page itself must not be done twice, or a tab switched
// back to a dozen times carries a dozen copies of the watcher script.
bool tab_session_new(App *a);

#endif

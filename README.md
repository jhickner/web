# web

Browse the web in your terminal. Real Chrome, real pixels, inline — and it
works inside tmux.

`web` runs headless Chrome, streams the page over the DevTools protocol, and
draws each frame with the kitty graphics protocol. Keyboard and mouse go back
the other way, so pages are live: links click, forms type, wheels scroll.

## Quick start

You need [Ghostty](https://ghostty.org/) or [kitty](https://sw.kovidgoyal.net/kitty/),
Google Chrome, and a C compiler.

```sh
make
./web news.ycombinator.com
```

No libraries to install: the whole thing is libc and about 2,000 lines of C.

Inside tmux, the graphics escapes need forwarding. Once, in `~/.tmux.conf`:

```sh
set -g allow-passthrough all
```

## Keys

| Key | Action |
|---|---|
| `^L` | address bar — a plain phrase becomes a search |
| `^O` / `^P` | back / forward |
| `^R` | reload |
| `^Y` | copy the page selection, or the address if nothing is selected |
| `^G` | frame size, write time, and throughput |
| `^S` | hide or show the status line |
| `^Q` | quit |
| `alt+0` | reset zoom |
| `alt+f` | fit-to-width on/off |
| `cmd+c` | copy the page selection (needs one line of terminal config, below) |
| `cmd+v` | paste into the page, or into the address bar when it is open |
| mouse | click, drag to select, wheel to scroll |

While reading:

| Key | Action |
|---|---|
| `↓` / `↑` | down / up, a line at a time |
| `←` / `→` | back / forward |
| `j` / `k` | down / up |
| `d` / `u` | half a screen |
| `space` / `b` | a screen |
| `gg` / `G` | top / bottom of the page |
| `[` / `]` | zoom out / in — or resize the window, inline |
| `w` / `W` | step the width the page is told it has |
| `s` | render scale: 1x, 2x, 3x |
| `/` | find in page, then `n` / `N` for next and previous |
| `i` | hand the keyboard to the page |
| `esc` | take it back |

Page up and down and tab always go to the page, as do all four arrows once the
page has the keyboard.

Zoom and the height of the inline window are remembered between runs, in
`~/.config/web/state`. `--zoom` and `--rows` override them for one run without
disturbing them.

`^S` hides the status line, and `--no-status` starts without it. The row is not
left blank: `--full` gives it to the page, and the inline window simply becomes
one row shorter. The address bar and the find prompt are drawn there, so opening
either brings the line back until it is done with.

## Not headless, whatever the string says

Chrome in this mode lays out, paints and composites exactly as it does in a
window — the only thing it lacks is the window. Its user agent disagrees: it
says `HeadlessChrome`, and Google refuses to sign anyone in from that. So `web`
takes the browser's own user agent, removes that one word, and passes it back
with `--user-agent` at launch.

The string is read from Chrome rather than written down here — a hardcoded
version goes stale on the next update, and claiming a version the browser does
not have is a worse tell than the one being removed. That means it can only be
learned from a Chrome that is already running, so it is cached in the state file
and used from the next launch on. A browser that predates the cache — the first
run against a new Chrome build, or one adopted from an earlier run — gets the
same correction through `Emulation.setUserAgentOverride` instead, which fixes
the agent but costs `navigator.userAgentData`. The launch flag keeps both, which
is why it is the one that matters.

Signing in is its own problem: a browser that can be driven over CDP can have
its credentials read that way, so Google will not sign anyone in to one.
`--login` opens an ordinary window on the same profile with no debugging port
at all — but with the same keychain options every other run uses, because those
decide the key Chrome seals its cookies with, and a session sealed with the
wrong one is a session `web` cannot read. Sign in, press Enter, and the browser
is shut down properly so the cookies are committed on the way out.

`w` and `s` are the two halves of how big the page comes out. `w` sets the width
the page is *told* it has — 800, 1024, 1280, 1440, 1600, 1920, then back to
whatever the cells work out to — which is what decides where a layout breaks.
`s` sets how many pixels Chrome renders per pixel the terminal shows: above 1x
it draws larger and comes back down, which is the only way to get detail finer
than the cells, at the square of the cost in bytes.

`cmd+v` needs nothing: the terminal turns it into a bracketed paste, which is
ordinary input. `cmd+c` is the other way around — the terminal keeps that one
for itself, and since the page is an image there is never anything in the
terminal to copy. One line in `~/.config/ghostty/config` hands it over:

```sh
keybind = performable:cmd+c=copy_to_clipboard
```

`performable:` means the key is only consumed when the action can actually run,
so cmd+c still copies a terminal selection everywhere else and only falls
through to the program when there is no selection to take.

## Modes

`j` scrolls when you are reading and types a `j` when you are filling in a
form. The page reports focus changes over a protocol binding, so `web` knows
whether something typable has focus and routes keys accordingly — the status
line shows `INSERT` when the page has them. `i` claims that state by hand and
`esc` releases it, blurring whatever was focused.

Without this the reading keys would make text fields unusable, which is the
trade every keyboard-driven browser has to make somewhere.

## Options

```
--scale N   device pixel ratio (default 1; 2 is sharper but 4x the data)
--zoom F    page magnification (default 1.0)
--rows N    how many cell rows the window gets
--no-status start with the status line hidden (^S toggles it)
--full      take over the whole terminal instead of drawing a window
--show      also open a real Chrome window, for debugging
--mute      start with the page's audio switched off
--login     open a window to sign in with, on the same profile
--keep      leave Chrome running on exit so the next start is instant
```

Most of a start is Chrome coming up: about half a second, against thirty
milliseconds for everything else. `--keep` leaves the browser holding the
profile when `web` exits, and the next run adopts it instead of paying for that
again. The cost is a headless Chrome sitting in the process table until you
kill it.

## The window

By default `web` opens a window where the cursor already is — right under the
command you ran — and draws there, leaving the shell's screen and scrollback
intact. Whether that takes any scrolling depends on how far down the screen you
were, which only the terminal knows, so it is asked rather than assumed.
Quitting leaves the page behind: the placeholder cells are ordinary text, so the
picture scrolls up with everything else and stays in your history.

```sh
./web --rows 20 news.ycombinator.com
```

The block is a window, not a viewport onto a bigger one, so `[` and `]` resize
it and the page is told about it the way it would be told about a dragged
corner: the box sets the cell rect, the cell rect sets the viewport, and the
layout follows. It keeps a 16:10 shape, struck in pixels rather than cells,
which are not square. Growing scrolls the extra rows into view; shrinking keeps
the top edge where it is and simply owns fewer rows, so the history above never
moves. The height you settle on is kept for the next run, and comes back down if
that one is in a shorter terminal. `alt+=` and `alt+-` still zoom either way.

`--full` gives up the window and takes the whole terminal on the alternate
screen instead, restoring it on the way out. There is no box to resize there, so
`[` and `]` go back to zooming.

## Zoom

`[` and `]` zoom out and in by changing the *viewport width* rather than
magnifying pixels, so the page reflows: text gets genuinely larger, and a
responsive site drops to its narrow layout, which is usually the one you want
in a terminal.

Zoom is a request rather than a command. After each change `web` asks the page
whether it still fits, and a page that cannot reflow that narrow gets its
viewport widened back until it does — you cannot zoom into a wide layout and
push half of it off the screen. When that happens the status line says so
(`zoom 77% - page needs 1240px`) and the stored zoom drops to what the page
actually allowed, so `[` responds immediately instead of unwinding steps that
never took effect.

`scrollWidth` never reports less than the viewport, so a reply wider than the
viewport means real horizontal overflow and nothing else — one measurement per
change is enough, and it cannot oscillate.

`alt+f` turns the fitting off if you would rather have the magnification and
scroll sideways; `alt+0` resets to 100%.

Set `WEB_CHROME` to use a different Chrome build, and `WEB_CELL=WxH` if the
page aspect looks stretched — inside tmux the terminal reports no pixel
geometry, so the cell size is a guess (8x17 by default).

## How it works

```
Chrome ──Page.screencastFrame──> base64 PNG ──> kitty graphics ──> terminal
   ^                                                                  |
   └────────── Input.dispatchKeyEvent / dispatchMouseEvent ───────────┘
```

Chrome hands out frames as **base64-encoded PNG**, and the kitty protocol
accepts base64 PNG directly, so a frame is never decoded or re-encoded on the
way through — it is copied from a websocket into an escape sequence. Sending
raw pixels instead would mean about 4 MB a frame rather than 250 KB, so the
"heavier" format is the cheap one here.

Chrome only emits a frame when the page actually changes, and each frame is
acknowledged only after it is on screen. That ack is the backpressure: a slow
terminal cannot be buried in frames it has no chance of drawing, and the event
loop always gets back to the keyboard between them.

That makes the whole cost of scrolling the number of frames it takes, and the
page decides that, not us. Three things decide it:

**Scrollbars are turned off.** The overlay scrollbar appears on a scroll, waits
half a second and fades out over a dozen compositor frames — each one a
full-page PNG, for a widget that cannot be grabbed from a terminal anyway. It
was most of the cost of scrolling.

**Scrolling is one jump.** A step reads the scroller under the point, clamps the
target to its ends, and sets it — `behavior:'instant'`, which a page asking for
smooth scrolling in CSS cannot override. Nothing animates, so a step costs the
one frame it takes to land, and a step at the top or bottom does nothing at all
rather than leaving distance behind to unwind.

**Pointer moves collapse.** A move that arrives while a frame is being written
is held, and the next one replaces it, so a drag costs one dispatch and one
frame instead of one of each per step.

Between them a page-down went from twenty-six frames to one.

Under tmux the picture cannot simply be placed at the cursor, because tmux
neither tracks nor redraws it. The way through is the protocol's Unicode
placeholders: the frame is transmitted with no placement, given a virtual one,
and the cells are filled with U+10EEEE carrying the image id in their
foreground colour and their row and column in combining diacritics. Those cells
are ordinary text, so tmux moves and repaints them like any other characters
and the terminal substitutes pixels underneath. Every graphics escape is
wrapped in a tmux passthrough sequence with its own ESCs doubled.

The browser profile lives in `~/.cache/web/profile`, not `~/.config`: it is
bulk cache with cookies in it, and it grows. Logins persist between runs. A run
that dies without cleaning up leaves Chrome holding that profile, so the next
start adopts the running browser instead of failing — which also makes startup
after the first almost instant.

## Debugging

`WEB_DEBUG=1` writes every byte read from the terminal, every decoded event,
and every CDP message to `/tmp/web_input.log`. It deliberately avoids stderr,
which would paint over the page.

`tools/` holds what was needed to get this working, and is worth keeping:

- `ptycap` — run a terminal program on a pty, capture what it draws, inject
  keystrokes. Lets the whole app be exercised without a visible terminal.
- `gquery` — send graphics commands with responses *enabled* and log what the
  terminal replies. Turns "nothing appears" into `OK` or an explicit error.
- `ttytest` — whether `poll()` reports readable input on `/dev/tty`.

## Notes

Two things are worth knowing if you work on this.

**`poll()` on `/dev/tty` is useless on macOS.** It returns `POLLNVAL`, so a
descriptor from there can be written to all day while never once reporting
that input is waiting. Open the terminal by its real path from
`ttyname(STDIN_FILENO)` instead. The symptom is a program that renders
perfectly and ignores every key.

**The placeholder codepoint is easy to typo.** U+10EEEE is `f4 8e bb ae`;
`f4 8f bb ae` is U+10FEEE, a different private-use character that terminals
draw as a meaningless glyph. Encode it from the codepoint rather than writing
the bytes by hand.

## Limitations

The mouse resolves to a character cell, roughly 8x17 pixels, which is fine for
links and buttons and imprecise for anything smaller. tmux does not carry the
kitty keyboard protocol, so there are no key-release events — held keys and
chords that need them will not work, though ordinary browsing does not care.
Dragging to select text works, but the terminal's own selection would grab
placeholder characters rather than page text, so use `^Y`.

One instance at a time: a second one adopts the same browser, and quitting
either closes it for both.

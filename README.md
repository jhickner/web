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
| `^E` | open this page in the desktop's own browser |
| `^G` | devtools port, frame size, write time, and throughput |
| `^S` | hide or show the status line |
| `^Q` | quit |
| `F8` | cycle recolor mode |
| `alt+0` | reset zoom |
| `alt+f` | fit-to-width on/off |
| `cmd+c` | copy the page selection (needs one line of terminal config, below) |
| `cmd+v` | paste into the page, or into the address bar when it is open |
| `^X` | open or close the command pane |
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
| `t` | frame transport: png, then two jpeg settings that only time it |
| `/` | find in page, then `n` / `N` for next and previous |
| `i` | hand the keyboard to the page |
| `:` | open the command pane |
| `esc` | take it back |

Page up and down and tab always go to the page, as do all four arrows once the
page has the keyboard.

Local files open the same way, on the command line or in the address bar:
`./web TODO.md`, `./web ~/notes/plan.html`. A name is only read as a path if it
resolves to something that is really there, so `example.com` is still a site —
unless there is a file called that next to you, which is then what you meant.

A PDF is a partial exception. Chrome draws it with the viewer extension, in a
frame of its own in a process of its own, and the document `web` can reach is a
stub: an empty body and a stylesheet link. So nothing about a PDF is done with
script — it is all real input, routed by the browser rather than by anything
this end can see.

Scrolling needs nothing special: `j`, `k`, `d`, `u`, `space`, `b` and the wheel
become wheel events, which the browser routes by what is under the pointer.

Keys are the part with a catch. A frame in another process cannot be handed the
keyboard from here — it has to be **clicked in** first. Click anywhere on the
PDF and the viewer's own keys start working: the arrows are handed straight
over, so they move the view in the document and change the page with the
thumbnail rail focused, and `gg` and `G` become its `Home` and `End`. Before
that first click they say so rather than doing nothing.

`/` stays unavailable: `window.find` searches this document, and a PDF's text
is not in it.

Zoom and the height of the inline window are remembered between runs, in
`~/.config/web/state`. `--zoom` and `--rows` override them for one run without
disturbing them.

## When nobody is looking

Every frame costs a PNG out of Chrome and a base64 write across the terminal,
and both are wasted on a pane that is not on screen. So `web` asks the terminal
to report focus, and stops the screencast when it goes away — the picture comes
straight back, redrawn, when the focus does.

Not drawing is only half of it. The page goes on animating into a screencast
nobody is reading, and on anything with a `requestAnimationFrame` loop behind it
that is the whole cost — with no real GPU under `--headless`, the rasterising is
software, and it lands on the CPU. So the renderer is throttled at the same
time, twentyfold by default, which slows what it asks for and takes the raster
behind it with it.

The page is not frozen, though. Timers and sockets keep going, audio is decoded
off the throttled thread, and a video you switched away from to keep listening
to keeps playing. If a heavy player does stutter, `blur_cpu_rate=1` in
`~/.config/web/state` keeps the picture pause and drops the throttle; higher
numbers throttle harder.

It needs the terminal to report focus, and inside tmux that means one line:

```sh
set -g focus-events on
```

Without it nothing is reported, and `web` simply draws all the time, as before.
`pause_on_blur=0` in `~/.config/web/state` turns it off for good; `--no-pause`
turns it off for one run, and leaves the file alone.

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
--clear     erase the window on exit instead of leaving it behind
--full      take over the whole terminal instead of drawing a window
--show      also open a real Chrome window, for debugging
--mute      start with the page's audio switched off
--script F  run a command script; `-` reads stdin
--delay MS  pause between script commands
--step      wait for a key between script commands
--timeout S how long a command waits before giving up (default 5)
--json      script output as one JSON object per value
--recolor M map the page onto your terminal's colours: off, hue, duotone, tint
--recolor-strength F   how far towards it, 0..1 (default 1)
--login     open a window to sign in with, on the same profile
--keep      leave Chrome running on exit so the next start is instant
--port N    fix Chrome's devtools port instead of letting it pick one
--no-pause  keep drawing while the terminal is not focused
--record[=F] write what you do to the page out as commands
--replay F  run a recording back; `-` reads stdin
```

With no address `web` starts blank, so a session can begin from nothing —
which is where a recording usually wants to start.

Most of a start is Chrome coming up: about half a second, against thirty
milliseconds for everything else. `--keep` leaves the browser holding the
profile when `web` exits, and the next run adopts it instead of paying for that
again. The cost is a headless Chrome sitting in the process table until you
kill it.

## Driving it

`web` reads a small command language, so a browser you were watching can also be
one you script. The picture keeps drawing the whole way through - the script is
its own demonstration, which is the thing headed automation does badly over ssh.

```sh
./web --script login.web
printf 'goto example.com\ntext h1\n' | ./web        # stdin, when it is not a tty
```

```
goto duckduckgo.com
wait-for "input[name=q]"
fill "input[name=q]" terminal web browser
press Enter
wait 2000
text "role=heading"
```

Values go to **stdout**, one per line, while the page goes to the terminal - so
the two never collide and `./web --script s.web | jq` works. `--json` wraps each
value in an object instead. A command that cannot be satisfied writes to stderr
and the process exits non-zero, so a script fails like any other program.

| | |
|---|---|
| `goto U` | navigate; a bare phrase becomes a search, a real path becomes a file |
| `back` `forward` `reload` | history |
| `wait N` | pause N ms |
| `wait-for S` / `wait-gone S` | until a selector is there, or is not |
| `click S` | click the first visible match |
| `fill S TEXT` | focus a field and replace its contents |
| `type TEXT` | type into whatever has focus |
| `press K` | `Enter`, `Tab`, `ArrowDown`, `ctrl+a` |
| `scroll N` \| `top` \| `bottom` | scroll |
| `text S` / `html S` / `attr S N` / `count S` | read the page |
| `url` `title` | where we are |
| `eval JS` | the escape hatch |
| `echo TEXT` | a literal line |
| `help` | list every command in the command pane |
| `pick` | toggle click-to-copy selectors |
| `record on` \| `off` | echo what you do as commands |
| `attach N` | drive the Chrome on devtools port N instead |
| `stop` | throw away whatever is queued |

`--delay MS` slows the run down to watch it, and `--step` waits for a key
between commands.

### Selectors

Playwright's shape, small enough to hold in your head, resolved by one script
injected with every call:

```
click "Sign in"                     a bare phrase is matched as text
click "button.primary"              anything that parses as CSS is CSS
click "text=Learn more"             explicit, and case-insensitive
click 'text="Learn more"'           quoted means exactly that
click "role=button[name=Submit]"    by role and accessible name
click ".row >> nth=2 >> a"          chained, and indexed
```

`text=` matches the *deepest* element containing the phrase, so it lands on the
link rather than on the `<body>` that also contains it. `role=` reads an
explicit `role` attribute first and falls back to the implicit one for the tags
that have one.

Every verb that names an element waits for it, the way Playwright does: a
selector that is not there yet, is not visible yet, or has something on top of
it is asked again until `--timeout` runs out. Most of the flakiness of driving a
browser lives in that gap.

Clicks are dispatched as real mouse events at the element's centre rather than
`element.click()`. They are trusted, they leave the hover and focus states a
click leaves - and those are what make the run visible in the picture.

### The command pane

`:` while reading, or `^X` at any time, opens a line editor under the page —
and either one puts it away again. It has history, `^R` search, emacs kill
bindings and a completion dropdown: type `/` to see every verb with its help.
`esc` is the narrower move, handing the keyboard back with the pane still up —
as does clicking the page, so a click into a form field can be typed into.

Lines typed there join the same queue a script uses, so one can be dropped in
behind a run already in progress and it simply happens in turn. `stop` clears
what is waiting.

`help` prints every command in the pane. `pick` turns selector picking on and
off: while it is on, clicking the page writes a readable `role=...` selector
(or a unique CSS selector) to the pane instead of activating the element.
Use Shift+Enter for another input line, Page Up/Page Down or the mouse wheel to
scroll the transcript, and Enter to run the whole multiline command.

### Recording what you do

The command language read backwards: browse by hand and `web` writes down the
commands that would do it again. `--record` starts a session recording, and
`record on` / `record off` turns it on and off from the pane at any point.

```sh
./web --record=flow.web            # starts blank; browse, then ^Q
./web --record=flow.web example.com
./web --replay flow.web            # and watch it happen again
```

The file comes attached to the flag rather than after it, so `web --record
example.com` stays a recording *of* example.com. Without one, recorded lines go
where script values go — to stdout when that is not the terminal the page is on,
and into the pane when it is — so `./web --record > flow.web` works too.

`--replay` runs one back — `--script` under the name that pairs with `--record`;
`--delay MS` and `--step` slow it to a watchable pace.

A script does not exit the moment its last command returns. The last thing a
recording does is usually the thing worth seeing, and leaving on the load event
means leaving before the frame carrying it has been drawn. So the exit waits for
the page to settle: nothing loading, and no frame that differs from the one on
screen for half a second. That is capped at three, because a page with something
animating on it never goes quiet at all. Nothing is timed against the clock, so
a slow page gets what it needs and a quick one is not held up.

To *assert* an outcome rather than wait for one, say what it should be —
`wait-for "Bad login"` fails the replay if that text never turns up, and it is
the line to add by hand when a recording is meant to be a test. Recorded lines also
land in the pane's transcript and history even while it is closed, so `^X` after
a session shows what was written down, and the up arrow brings a line back to
edit and run again.

```
goto file:///tmp/signin.html
wait-for "role=link[name=Sign in]"
click "role=link[name=Sign in]"
wait-for "#q"
fill "#q" hello
press Enter
wait 4400
scroll 183
```

What gets written down:

- **clicks**, as the selector the picker would have given you. A click into a
  text field is not one: `fill` focuses the field itself, so recording both
  would click it twice.
- **edits**, as one `fill` at the end of the edit rather than a `type` per
  keystroke. The pending edit goes out before any `press`, because the Enter
  that submits a form arrives before the form has gone anywhere.
- **keys** that are not typing — `press Enter`, `press Tab`, `press ctrl+a`.
- **addresses you asked for**, from the address bar or `goto`, plus `back`,
  `forward` and `reload`. A page the site moves to on its own is not recorded:
  it is already the consequence of the click above it, and writing it down
  would make the replay jump straight there without ever doing the click.
- **scrolls**, added up per burst — a wheel is a stream of notches, and a line
  per notch is not a recording of anything.
- **pauses**, as `wait MS`. A gap long enough to have been a decision — a
  second or more — is part of what happened, and a replay that skips it is not
  the same run. Rounded to a tenth of a second and capped at ten, so walking
  away does not become a `wait 400000`.
- **page loads**, as `wait-for` on the selector the next action is about to
  use. That is the deterministic version of a wait: the replay holds until the
  element is really there, however long the load takes *this* time, instead of
  waiting out a guess made on the day it was recorded. It stands in for the
  pause, so no blind `wait` goes out beside it. When the next thing is not an
  element — a scroll, a key — the measured pause is kept instead, however short.

A `<select>` comes out as a `#` comment: nothing in the command language sets a
dropdown, so the line says what happened without pretending to replay it.
Anything the script runner is doing is left out — it came from a script
already — so `record` and a running script do not talk over each other, and
recording a replay gives you an empty file rather than a copy.

### Sharing the browser with Playwright

A browser is a devtools port at both ends, so this works in either direction.

`--port` pins the port instead of letting Chrome pick a free one, which gives
Playwright somewhere to connect:

```sh
./web --port 9222 news.ycombinator.com
```

```js
const browser = await chromium.connectOverCDP('http://127.0.0.1:9222');
```

Playwright then drives the page you are watching, in the terminal, live — which
is the thing its own headed mode does badly over ssh. `^G` shows the port on the
status line, so the one Chrome picked for itself is there when you did not name
one.

The other direction is the `attach` command. Start the browser from Playwright:

```js
const browser = await chromium.launch({args: ['--remote-debugging-port=9222']});
```

and take a look at it with `attach 9222`, typed in the command pane or run from
a script. Three things follow from it:

- the browser `web` started is shut down, unless `--keep` said otherwise, and
  the one it attached to is never shut down — quitting leaves it running.
- the picture needs the frames to match the cells they are drawn into, so the
  device metrics override goes on their page too. Playwright sees that viewport.
- `--port N` with a browser already answering there takes it over rather than
  starting a second one, which is also what makes `--keep --port` work. Given no
  address on the command line it leaves that browser on whatever page it is on.

## The window

By default `web` opens a window where the cursor already is — right under the
command you ran — and draws there, leaving the shell's screen and scrollback
intact. Whether that takes any scrolling depends on how far down the screen you
were, which only the terminal knows, so it is asked rather than assumed.
Quitting leaves the page behind: the placeholder cells are ordinary text, so the
picture scrolls up with everything else and stays in your history. `--clear`
takes it away instead — the block is erased and the prompt comes back on the row
the command was run from, as though nothing had been drawn.

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

## Terminal colours

`--recolor` maps the page onto the colours your terminal is already drawing
with, so a page sits in the window like the rest of the session rather than
glaring out of it. The terminal is asked for its foreground and background over
OSC 10 and 11 the first time anything needs them, and colours are matched
perceptually in Oklab.

| Mode | Result |
|---|---|
| `hue` | Text and page furniture ride the terminal's ramp; anything actually coloured keeps its colour |
| `duotone` | The whole page runs from the terminal's background to its foreground |
| `tint` | Everything collapses to tones of the background colour, shading intact |

`--recolor-strength` blends the result back towards the original, and `F8`
cycles the modes while you read. Both are remembered in `~/.config/web/state`.

The ramp runs from the background *up* to the foreground, so a light page comes
out light — in your palette, but light. Any recolor mode therefore also asks
pages for `prefers-color-scheme: dark` when the terminal is dark, which is what
puts a page that has a dark theme of its own the right way up before the
mapping runs.

Chrome does the work: the mapping is an SVG filter injected into the page, so
it costs one more step in a composite the browser was doing anyway. Recolouring
the frames on the way out instead would mean decoding and re-encoding a PNG for
every frame of a live screencast.

`tint` borrows only the background's *hue*, not its saturation: terminal
backgrounds are nearly neutral, and a literal reading of one would be
invisible. A background with no hue at all can only give greyscale.

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

### Where the time actually goes

`t` steps the transport — `png`, `jpeg` at 80, `jpeg` at 90. The jpeg settings
**do not draw**: the kitty protocol takes PNG or raw pixels and nothing else, so
a jpeg frame cannot be put on screen without decoding it here first, which is
exactly the copy this renderer exists to avoid. What they do instead is ask
Chrome for jpeg and time what comes back, so `^G` reads out the size and rate of
frames of that format against the same page. The status line says `jpg80 HELD`
while one is selected, because the picture is standing still.

That measures the one thing worth knowing before writing a decoder: whether
Chrome's PNG encoder is what sets the frame rate. On a text-heavy page it says:

| | KB a frame | gap | our write |
|---|---|---|---|
| `png` | 137 | 37.5 ms | 8.0 ms |
| `jpg80` | 72 | 17.3 ms | — |
| `jpg90` | 95 | 17.5 ms | — |

17 ms is the 60 Hz floor, so under jpeg Chrome is pinned at the compositor rate
and its encode costs nothing. Take that floor and our own write off png's 37.5
and about 13 ms of PNG encode is left, which is the whole of what a format
change could win — and a decoder cannot collect it, since decoding a frame that
size is 17–34 ms before the pixels have gone anywhere, and they then travel raw
at 45x the bytes or deflated to something PNG already beat.

What it leaves is the remote case: over a devtools port jpg80 halves the wire,
and below about 25 Mbps the bytes saved are worth more than the decode. Locally
the wire is free and png is simply the right format.

The other half of that measurement went the other way. Chrome's screencast
takes an `everyNthFrame`, and asking it for fewer frames looks like the one
lever that saves work at both ends at once. It is not, and the ack is why: the
next frame is held until the last is on screen, so Chrome is already producing
frames at the rate this terminal can draw them and no faster. The frames a skip
discards are mostly frames Chrome would never have made. What it does buy is
coarser deltas — on a video, dropping every second frame cost a third of the
rate and took the write share only from 21% to 16%, because the frames that
survive have more changed in them and cost more to write.

The ack was already the frame throttle, and a better one than a number: it
adapts continuously, to the terminal actually in front of it, with nothing to
tune. So `web` asks for every frame and lets the ack pace them.

Under tmux the picture cannot simply be placed at the cursor, because tmux
neither tracks nor redraws it. The way through is the protocol's Unicode
placeholders: the frame is transmitted with no placement, given a virtual one,
and the cells are filled with U+10EEEE carrying the image id in their
foreground colour and their row and column in combining diacritics. Those cells
are ordinary text, so tmux moves and repaints them like any other characters
and the terminal substitutes pixels underneath. Every graphics escape is
wrapped in a tmux passthrough sequence with its own ESCs doubled.

A resize leaves the block on the rows it was already on. A terminal keeps what
is on its screen where it is — a pane that grew has only put empty rows
underneath it — so the window stays under the command it belongs to rather than
dropping to the foot of the screen every time a pane is zoomed. It is pulled
back up only when it no longer fits.

Which is also the one hazard in the scheme: a cell shows the picture because it
*names* it, and a name outlives the layout that drew it. A resize — a tmux pane
zoomed, a window dragged — moves those cells somewhere only the terminal knows,
and placing the image again lights every one of them, so the same page comes up
twice. So an inline resize retires the name: the image is deleted and the next
frame goes up under a new id, leaving the abandoned cells naming an image that
no longer exists, which draws nothing. The id's top byte is that generation,
kept even so it can never collide with the two odd bytes of the pid below it.

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

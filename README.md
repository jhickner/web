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
| `^D` | start or stop a trace into `/tmp/web_input.log` |
| `^S` | hide or show the status line |
| `^Q` | quit |
| `^T` / `^W` | new tab / close tab (the last one closes the window) |
| `^N` / `^B` | next tab / previous tab |
| `alt+1`…`alt+9` | the tab with that number on it |
| `alt+0` | reset zoom and the pinned width, and the window's proportion with it |
| `alt+f` | fit-to-width on/off |
| `cmd+c` | copy the page selection (needs one line of terminal config, below) |
| `cmd+v` | paste into the page, or into the address bar when it is open |
| `^X` | open or close the console |
| mouse | click, drag to select, wheel to scroll |
| tab bar | click to switch, middle-click to close, wheel to cycle |

While reading:

| Key | Action |
|---|---|
| `↓` / `↑` | down / up, a line at a time |
| `←` / `→` | handed to the page — galleries and carousels bind these |
| `backspace` | back (so does `^O`) |
| `j` / `k` | down / up |
| `h` / `l` | left / right, for a page wider than the width it was given |
| `d` / `u` | half a screen |
| `space` / `b` | a screen |
| `gg` / `G` | top / bottom of the page |
| `[` / `]` | zoom out / in — or scale the window, inline: both edges together |
| `shift`+`↑↓←→` | drag one edge of the window, the other stays put (`U`/`D`/`L`/`R` too) |
| `w` / `W` | widen / narrow the width the page is told it has |
| `s` | frame size: auto, then 100%, 75%, 50% held |
| `t` | frame transport: png, then two jpeg settings that only time it |
| `/` | find in page, then `n` / `N` for next and previous |
| `P` | picking: click the page for a CSS selector, into the console |
| `i` | hand the keyboard to the page |
| `:` | open the console |
| `esc` | take it back |

Page up and down and tab always go to the page, as do the sideways arrows, and
as do all four once the page has the keyboard. Sideways is where a gallery, a
slide deck or a carousel puts its controls, and there is no other way to reach
those from here; going back keeps `^O` and gains `backspace`, which is where a
browser puts it anyway. Up and down stay ours and scroll, which is what they
would have done in the page.

An arrow the page does not want is cancelled before the browser can do anything
else with it. Not for the page's sake — every handler it registered has already
run by then, so a viewer that pages through images on an arrow goes on doing it
— but for what happens afterwards. A key nobody claimed is handed back to the
browser process, and on macOS that means the menu bar: it is routed through
`performKeyEquivalent`, which validates the whole menu before concluding that
nothing wanted it. On some pages that validation takes seconds, and the thread
it runs on is the one that dispatches every reply and encodes every frame — so
a few arrow presses in an image viewer are enough to stop the window drawing
at all, while clicking the same arrows on screen costs nothing. Cancelling the
key is what marks it claimed, and it is the whole of the fix. What it costs is
scrolling sideways with the arrows, which `h` and `l` do. `--raw-keys` turns it
off; a text field is left alone either way, or the caret would stop moving.

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

## Tabs

`^T` opens one, with the address bar already waiting; `^W` closes it, and
closing the last one closes the window. The bar is drawn above the page, one
row of box drawing, and only once there is a second tab to name — a window
showing the only tab it has is a window with no tabs, and the row is worth more
to the picture. Click a tab to switch to it, middle-click to close it, or roll
the wheel across the bar to cycle. `^N` and `^B` step, and `alt+1` to `alt+9`
go straight to the number the bar draws beside each name.

A tab is a page of the browser and nothing more. The devtools socket addresses
one page at a time, so switching tabs moves that socket rather than asking
anything to come forward — the page you left goes on running, with its
screencast stopped, and comes back where you left it. Each one is opened in a
window of its own for the same reason each `web` window is: Chrome paints only
the tab in front of a window, so a background tab sharing one screencasts an
empty picture.

The bar gives things up rather than refusing them. A tab keeps its number and
its title while there is room for both; below that the number goes and the
title has the field to itself, then the title goes and the number is all that
is left, and at one column apiece it is a digit for the first nine — the ones
`alt+<n>` reaches — and a letter of its own name beyond them. Narrower still,
the separators go too. Nothing is ever dropped for being too small to name: a
tab you cannot read can still be clicked, and one that has quietly vanished
cannot. Only when there are more tabs than the row has columns does anything
come off it, and then it is the far ends that go, so the tab you are on stays
on screen. Thirty-two is where the list itself stops.

A link that asks for a window of its own gets a tab. Chrome opens the page
either way, and it is a page `web` never asked for: it loads, and plays its
audio, in a window nothing here is drawing. So `web` keeps a second socket open
to the browser itself, which is the only place that news arrives, and when a
page one of ours opened turns up it is brought into the bar and switched to —
the click was aimed there.

What comes across is the address rather than the page. Chrome puts a popup in
the window of the page that asked for it, and paints only the tab in front of a
window, so the page as it stands is one you could hear and never see; a tab of
`web`'s own comes with a window of its own, which is what makes it drawable.
The cost is the opener — a popup that talks back to the page that made it finds
nobody there. Pages that never say where they are going keep to themselves, and
a run with a job to do — a screenshot, a script — is left alone entirely, since
those are aimed at the page they were given.

## When nobody is looking

Every frame costs a PNG out of Chrome and a base64 write across the terminal,
and both are wasted on a pane that is not on screen. So `web` asks the terminal
to report focus, and stops the screencast when it goes away — the picture comes
straight back, redrawn, when the focus does.

Not drawing is most of it. The page goes on animating into a screencast nobody
is reading, and on anything with a `requestAnimationFrame` loop behind it that
is the whole cost.

The page is not frozen. Timers and sockets keep going, and a video you switched
away from to keep listening to keeps playing.

There used to be a CPU throttle alongside the pause — `Emulation.setCPUThrottlingRate`,
twentyfold by default — on the reasoning that slowing the renderer would take
the raster behind it with it. It does the opposite. Chrome emulates a slower
processor rather than asking for less work: a thread of its own interrupts the
renderer's main thread with a signal, and the handler busy-waits on
`mach_absolute_time` to burn away the share of the quantum the rate says it
should not have had. A window left blurred at rate 20 spends a whole core doing
nothing, answers no javascript and paints nothing — indistinguishable, from
outside, from a page that has hung. It is a measurement tool, not a power
setting, so it is gone rather than off: there is no `blur_cpu_rate` and no
`--no-throttle` any more. A `blur_cpu_rate=` line left in
`~/.config/web/state` by an older build is ignored, and dropped the next time
the file is written.

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

`w` and `s` are the two halves of how big the page comes out. `w` and `W` walk
the width the page is *told* it has — 40px a press, between 320 and 2560, from
whatever the cells were giving it — which is what decides where a layout breaks.
A width walked to this way is *pinned*: it stays put while the window is resized
and the magnification moves instead, so a layout can be held at 360px and still
photographed as large as the terminal allows. It also outlives the run, and it
comes back as a width rather than as a ratio, so it means the same thing in the
next terminal. `alt+0` unpins it, and the zoom keys take it off the pin too —
they are the same knob from the other end, and only one end can be held.
`s` sets how big a frame to ask for, against the viewport. It starts on **auto**
— the default, described below — and stepping it moves through 100%, 75% and 50%
*held*, then back to auto. Held sizes are the exception; auto is where it lives.

It used to step *up*, on the idea that drawing larger and coming back down to
the cell rect would buy detail past what the cells can hold. It cannot. The
screencast starts at a scale of one and takes the smaller of that and what it
was asked for, so it can shrink a frame and never enlarge one, and the most it
will ever hand over is the viewport in CSS pixels. Supersampling was being set
up, rendered, and thrown away at the last step.

Downwards is the direction that does something: the page comes back smaller than
the cells it lands in and the terminal stretches it, which trades sharpness for
bytes — worth having on a slow link, and the way to ask for a small screenshot.
The fraction is struck against the viewport, so 50% is half the width and a
quarter of the data whatever the zoom is doing. `--scale` takes any fraction,
not just the three the key steps through, and a number given that way is a size
to *hold* — asked for one, it stays that size moving or still.

**Auto** makes that trade only while it is free, and it is the default. A page sliding past is
measured at 86% Chrome and 14% us, and the pixel count is what both are paid in,
so the resolution drops once three quick frames say the picture is moving and
goes back the moment it stops — 42% of the pixels while scrolling, all of them
the instant you stop to read, and a quarter of them over ssh, where the bytes
are the whole of the cost. The detail it drops is detail nobody could have read
while it was moving. `^G` says `moving` while it is down, and `--scale 1` or one
press of `s` holds the full size if you would rather it did not.

It is spent on the screencast's size cap rather than on the device scale factor,
because that is the only end of it Chrome listens to: the frame that arrives is
the viewport in CSS pixels whatever the scale factor says, and the cap only ever
scales one *down*. Asked for 842 pixels against a 560-pixel viewport, Chrome
sends 560. So nothing happens until the cap goes under the viewport — and going
that way, the page is never re-laid-out on the way in or out, only re-scaled.

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
--scale F   device pixel ratio (default 1; 2 is sharper at 4x the data,
            0.5 is blurrier at a quarter of it)
--zoom F    page magnification (default 1.0)
--rows N    how many cell rows the window gets
--no-status start with the status line hidden (^S toggles it)
--no-clear  leave the window on screen on exit instead of erasing it
--full      take over the whole terminal instead of drawing a window
--show      also open a real Chrome window, for debugging
--mute      start with the page's audio switched off
--eval JS   run javascript in the page and print what it answers
--delay MS  pause between lines of piped javascript
--step      wait for a key between those lines
--timeout S how long a line waits before giving up (default 5)
--json      script output as one JSON object per value
--screenshot F   write the page to F as a png and exit; "-" is stdout
--login     open a window to sign in with, on the same profile
--keep      leave Chrome running on exit so the next start is instant, and
            keep it for every other window too
--endpoint  print every running window as JSON and exit
--browsers  list the Chrome processes web has running, with pids, and say
            which of them a new window could still adopt
--kill      quit this profile's windows and end its browsers, including any
            nothing can reach any more
--exec CMD  run CMD against this window, its output in the console
--port N    fix Chrome's devtools port instead of letting it pick one
--no-pause  keep drawing while the terminal is not focused
```

With no address `web` starts blank, so a session can begin from nothing.

Most of a start is Chrome coming up: about half a second, against thirty
milliseconds for everything else. `--keep` leaves the browser holding the
profile when `web` exits, and the next run adopts it instead of paying for that
again. The cost is a headless Chrome sitting in the process table until you
kill it.

`--browsers` is how you see what is sitting there:

```
$ web --browsers
pid 25495   up 04:11        port 53859
```

A browser is only worth keeping while something can still find it. The profile
records where it is, and if that record goes while the browser does not — a
shutdown that could not finish, a `SingletonLock` cleared away — the browser
goes on holding the profile with nothing able to adopt it, and every later start
launches a second one beside it. That is the difference between a start that
takes 0.09s and one that takes 8.6s, so it is worth being able to see:

```
$ web --browsers
pid 50596   up 26:00        stranded - nothing can reach it
web: 1 stranded browser holding the profile; web --kill ends it
```

`--kill` clears the lot. Windows first, each asked to quit so it shuts its own
browser down and hands its terminal back; then whatever browsers are left, which
are the ones no window ever claimed. A window that will not answer, or a browser
that will not go, is ended rather than waited on forever.

What counts as *this profile's* window is the session file it writes on the way
up and removes on the way down. A `web` process without one — it never got that
far, or it wedged after removing it — is named rather than ended:

```
$ web --kill
web: window 15902 is running but is not this profile's to end; kill 15902 if it is yours
```

Nothing in a process table says which profile a window belongs to, so ending one
on the strength of its name alone would let a `--kill` aimed at one profile take
out a session running under another. The pid is there to be dealt with by hand.

## Screenshots

`--screenshot` writes the page to a PNG and exits:

```sh
./web --screenshot shot.png example.com
./web --screenshot - example.com | pngtopam        # "-" is stdout
```

The shot is what the run waits for, and it waits for the page: the load event,
then anything piped in on stdin, then the fonts the page asked for and the two
frames after them. A webfont swaps in *after* the load event and a first paint
can still be on its way, and either one photographs as a page that is not the
page. Nothing waits forever — a page that never finishes is shot as it stands
after thirty seconds, because a picture of half a page beats a run that does
not come back.

So a shot can be of the page after a script has had its way with it:

```sh
echo 'document.querySelector("#accept").click()' |
    ./web --screenshot shot.png example.com
```

With the picture going to stdout the values that script produces go to stderr
instead: a line of text in front of a PNG is a PNG nothing will open.

Out of a pipeline, with no terminal to draw into, the viewport is a plain
1280x800 and `--scale` is the pixel ratio — `--scale 2` gives a 2560x1600 shot
of the same layout, `--scale 0.5` a 640x400 one. Run where there *is* a terminal it photographs the window
you are looking at instead, at the size it is on screen.

The exit status says whether the file was written, so a shot that failed fails
the script around it.

## Driving it

The console and `--eval` are the same runner, and what they run is JavaScript
against the page:

```sh
./web --eval 'document.title' example.com
./web example.com < check.js                     # a file needs no flag
echo 'document.links.length' | ./web --json example.com
```

Piped stdin is read a line at a time whenever it is not a terminal, so a file of
javascript is just a redirect and there is no script flag to remember.

Each line goes to the page's own `eval`, so it does not have to be an
expression — the completion value is the answer, the way it is in devtools:

```
document.title
let n = document.links.length; n * 2
location.href = "https://example.org"
document.querySelector("h1").textContent
```

Values go to **stdout**, one per line, while the page goes to the terminal — so
the two never collide and `./web example.com < s.js | jq` works. `--json` wraps each
value in an object with the line that produced it. A line that throws prints the
exception to stderr and the process exits non-zero, so a script fails like any
other program. `--delay MS` slows the run down to watch it, `--step` waits for a
key between lines, and `--timeout S` is how long one line may take.

A line that starts a navigation is not over until the page has arrived: the next
line would otherwise run against a document on its way out.

There is deliberately **no command language**. Selectors, waiting and real input
events are a real problem, and things that already solve it are one flag away:
`--exec` hands this window to a program with the endpoint in its environment,
`examples/cdp.mjs` does the job in standard library, and Playwright does it
properly. Those are the two sections below.

### The console

`:` while reading, or `^X` at any time, opens a line editor under the page —
and either one puts it away again. It has history, `^R` search and emacs kill
bindings. `esc` is the narrower move, handing the keyboard back with the console
still up — as does clicking the page, so a click into a form field can be typed
into.

What you type is JavaScript, evaluated in the page. There is nothing else it
could be:

```
> document.querySelectorAll('a').length
42
> let seen = new Set(); for (const a of document.links) seen.add(a.host); [...seen]
news.ycombinator.com,github.com
> location.href = 'https://example.com'
```

Lines join the same queue `--eval` uses, so one can be dropped in behind a run
already in progress and it simply happens in turn.

`P` while reading turns **picking** on and off. While it is on, clicking the
page writes a CSS selector for whatever you hit into the console instead of
activating it — shortest thing that finds it again, ready to paste into a
`document.querySelector`. Use Shift+Enter for another input line, Page
Up/Page Down or the mouse wheel to scroll the transcript, and Enter to run.

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

But the port names the browser, and the browser has a page per window, so on its
own it no longer says which page is the one you are looking at. `--endpoint`
answers that — every running window, one JSON object to a line, no browser
started to find out:

```console
$ web --endpoint
{"pid":4123,"port":9222,"cdp":"http://127.0.0.1:9222","target":"0A32…","url":"https://example.com/","title":"Example Domain"}
```

`target` is the part that matters. Playwright has no accessor for a target id,
so the way to spend it is to ask each page for its own:

```js
const browser = await chromium.connectOverCDP(cdp);
const ctx = browser.contexts()[0];
for (const page of ctx.pages()) {
  const s = await ctx.newCDPSession(page);
  const { targetInfo } = await s.send('Target.getTargetInfo');
  if (targetInfo.targetId === target) { /* the page on screen */ }
}
```

`examples/attach.mjs` is that loop, written out and ready to import.

Playwright is not required to drive the window, though — CDP is a websocket that
takes JSON, and Node has had `WebSocket` and `fetch` built in since 22.
`examples/cdp.mjs` is that connection, standard library only, and
`examples/drive.mjs` is a demo written against it that fits on a screen:

```sh
node examples/drive.mjs                 # against the one window running
web --exec 'node examples/drive.mjs' news.ycombinator.com
```

### Running a script against the window

`--exec` starts a program with the endpoint already in its environment, and puts
its output in the console while the page stays live above it:

```sh
web --exec 'node examples/attach.mjs' example.com
```

The child gets `WEB_CDP_URL`, `WEB_CDP_PORT` and `WEB_TARGET_ID`, so it never
has to go looking. Quitting the window ends it; it ending leaves the window.
Both streams go to the console, because stderr has nowhere else to be — the middle
of the picture is not a good place for a stack trace.

The other direction is the `attach` command. Start the browser from Playwright:

```js
const browser = await chromium.launch({args: ['--remote-debugging-port=9222']});
```

and take a look at it with `attach 9222`, typed in the console or run from
a script. Three things follow from it:

- the browser `web` started is shut down, unless `--keep` said otherwise, and
  the one it attached to is never shut down — quitting leaves it running. The
  shutdown is polite, so it takes a moment; it is waited out by a background
  process rather than by the one holding your prompt.
- `--keep` is asked of the browser, not of the run that asked it: while any
  window has asked for this browser to stay, no window's quit will shut it
  down. The request dies with the browser, so the next one starts unmarked.
- the picture needs the frames to match the cells they are drawn into, so the
  device metrics override goes on their page too. Playwright sees that viewport.
- `--port N` with a browser already answering there takes it over rather than
  starting a second one, which is also what makes `--keep --port` work. Given no
  address on the command line it leaves that browser on whatever page it is on.
- unless that browser turns out to be one of ours. A run writes down the port it
  started Chrome on, so a later `--port` pointed at the same browser knows it is
  an earlier run's rather than a stranger's: it takes a tab of its own, and it
  can shut the browser down like any other run. Chrome writes its own port down
  only when it chose the port itself, so this note is also what lets a plain
  `web` find a `--keep --port` browser at all.

## The window

By default `web` opens a window where the cursor already is — right under the
command you ran — and draws there, leaving the shell's screen and scrollback
intact. Whether that takes any scrolling depends on how far down the screen you
were, which only the terminal knows, so it is asked rather than assumed.
Quitting takes the window away again: the block is erased and the prompt comes
back on the row the command was run from, as though nothing had been drawn.
`--no-clear` leaves the page behind instead — the placeholder cells are ordinary
text, so the picture scrolls up with everything else and stays in your history.

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

Shift and an arrow take the two edges separately, which is the same corner
dragged by hand: `shift+↓` and `shift+→` let the window out, `shift+↑` and
`shift+←` take it back. Each one moves its own edge and leaves the other where
it is, so the height keys never change the width and the width keys never change
the height. The top left stays where it is, so the window only ever grows down
and to the right, and the shell's history above it never moves. A column is a
smaller step than a row — cells are about twice as tall as they are wide — so
the sideways keys move two at a time. `U`, `D`, `L` and `R` do the same four
things, for terminals that keep the shifted arrows for themselves.

`[` and `]` are the other gesture: a smaller and a larger window rather than a
shorter and a taller one, so both edges move together and the window keeps
whatever shape it has been given as it goes. Both numbers are kept for the next
run.

`alt+0` drops a width set by hand and puts the window back on the proportion
above, where it takes its width from its height again.

`--full` gives up the window and takes the whole terminal on the alternate
screen instead, restoring it on the way out. There is no box to resize there, so
`[` and `]` go back to zooming, and the corner keys say so rather than doing
nothing.

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
scroll sideways with `h` and `l`; `alt+0` resets to 100%.

A width pinned with `w` or `W` is exempt: it is a number that was asked for
rather than a ratio that fell out of a window size, so it is honoured whether
the page fits it or not. The measurement still runs — the status line says
`width 360px - page needs 980px` — and what does not fit is reached with `h`
and `l`. Otherwise nothing could ever be looked at below its own minimum
layout width, which is most of what a narrow width is for.

Set `WEB_CHROME` to use a different Chrome build, and `WEB_CELL=WxH` if the
page aspect looks stretched — a terminal that reports no pixel geometry leaves
the cell size a guess (8x17 by default). Modern tmux passes the real numbers
through, so this is rarer than it was; `^G` says which of the two you have.

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

Sending it *early* was the next idea, and it bought nothing. The ack goes out
once the frame is on screen, which reads like Chrome and the terminal taking
turns — 30 ms of Chrome and 5 ms of us, one after the other. Moving it above the
write should have let Chrome encode the next frame under our write of this one,
worth the whole 5 ms. Measured: 35.1 ms a frame against 34.7 before it, which is
noise pointing the wrong way. So the two were never taking turns — either Chrome
already keeps a frame in flight, or the thing setting the pace is not work at
all. The gap sits at almost exactly two 60 Hz intervals, which is what a
pipeline missing a vsync looks like rather than one short of CPU.

That is the same answer resolution gave, twice: cutting the rastered area to 42%
changed nothing, and cutting the delivered frame to 42% bought 6%. Three
different ways of giving Chrome less to do, and none of them made it faster. The
frame rate here is not ours to spend.

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

Two runs at once share that one browser, so each takes a tab of its own rather
than the one already open: a shared tab would mean the address typed in one
window driving the other's screen. The tab gets a browser window of its own as
well, which is what makes it drawable — Chrome only paints the tab in front, so
a second tab in the same window screencasts nothing and arrives as a title with
no picture under it. Quitting closes that tab, and the browser goes with the
last run to leave. A run started in the moment before the first
one's Chrome has written down its port waits for it rather than starting a
second browser on the same profile, which neither of them would survive.

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

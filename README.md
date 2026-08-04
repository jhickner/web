# web

![Hacker News rendered inline in a terminal, with a tab bar above the page](images/ss.png)

Chrome in your terminal. Mostly. Sort of. 
This project uses Chrome's screencast capability combined with kitty graphics
to stream a browser window (with tabs) inline, into your terminal. It works
suprisingly well. It even works in tmux, although it's a bit slower.

`web` runs headless Chrome, streams the page over the DevTools protocol, and
draws each frame with the kitty graphics protocol. Keyboard and mouse go back
the other way, so pages are live: links click, forms type, wheels scroll.

If you like running things in your terminal, check out these other projects:
- [rom](https://github.com/jhickner/rom) - a terminal game emulator (gba, snes, etc.)
- [pix](https://github.com/jhickner/pix) - a terminal image viewer with grid layout
- [vid](https://github.com/jhickner/vid) - a terminal video player with subtitle
support

## Quick start

You need [Ghostty](https://ghostty.org/) or [kitty](https://sw.kovidgoyal.net/kitty/),
Google Chrome, and a C compiler.

```sh
make
./web news.ycombinator.com
```

No libraries to install: the whole thing is libc and about 8,000 lines of C.

Give it more than one address and each one after the first opens in a tab,
with the window starting on the first:

```sh
./web news.ycombinator.com lobste.rs example.com
```

Inside tmux, the graphics escapes need forwarding. Once, in `~/.tmux.conf`:

```sh
set -g allow-passthrough all
```

## Logins

Headless chrome is usually prevented from logging in. Open a normal browser
using web's profile like this:
```
web --login
```
Then log in to whatever sites you want `web` to have access to.


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
| `?` | the key list, over the page — any key puts it away |
| mouse | click, drag to select, wheel to scroll |
| tab bar | click to switch, middle-click to close, wheel to cycle |

While reading:

| Key | Action |
|---|---|
| `↓` / `↑` | down / up, a line at a time |
| `←` / `→` | handed to the page — galleries and carousels bind these |
| `backspace` / `shift+backspace` | back / forward (so do `^O` / `^P`) |
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
| `?` | the key list; `j` / `k` if it is taller than the window |
| `esc` | take it back |




## Options

```
web [options] <url>...

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


`--browsers` lists chrome instances web has started

```
$ web --browsers
pid 25495   up 04:11        port 53859
```

```
$ web --browsers
pid 50596   up 26:00        stranded - nothing can reach it
web: 1 stranded browser holding the profile; web --kill ends it
```

`--kill` clears all browsers started by web

```
$ web --kill
web: window 15902 is running but is not this profile's to end; kill 15902 if it is yours
```

## Screenshots

`--screenshot` writes the page to a PNG and exits:

```sh
./web --screenshot shot.png example.com
./web --screenshot - example.com | pngtopam        # "-" is stdout
```


```sh
echo 'document.querySelector("#accept").click()' |
    ./web --screenshot shot.png example.com
```


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

# web

<img src="images/ss.png" width="530" alt="Hacker News rendered inline in a terminal, with a tab bar above the page">

Chrome in your terminal. Works great with small windows in tmux splits, which
is how I use it.
This project uses Chrome's screencast capability combined with kitty graphics
to stream a browser window (with tabs) inline, into your terminal. It works
surprisingly well. It even works in tmux, although it's a bit slower.

`web` runs headless Chrome, streams the page over the DevTools protocol, and
draws each frame with the kitty graphics protocol. Keyboard and mouse go back
the other way, so pages are live: links click, forms type, wheels scroll.

If you like running things in your terminal, check out these other projects:
- [rom](https://github.com/jhickner/rom) - a terminal game emulator (gba, snes, etc.)
- [pix](https://github.com/jhickner/pix) - a terminal image viewer with grid layout
- [vid](https://github.com/jhickner/vid) - a terminal video player with subtitle
support

## Quick start

Requires [Ghostty](https://ghostty.org/) or [kitty](https://sw.kovidgoyal.net/kitty/),
Google Chrome, and a C compiler. The only library is sqlite3, which macOS ships
and Linux packages as `libsqlite3-dev`.

```sh
make
./web news.ycombinator.com
```

Each address after the first opens in a tab; the window starts on the first:

```sh
./web news.ycombinator.com lobste.rs example.com
```

In tmux, add to `~/.tmux.conf`:

```sh
set -g allow-passthrough all
```

## Logins

Opens a normal browser window on the same profile. Log in to whatever sites you
need and `web` keeps the session:

```sh
web --login
```

## Profiles

Every window shares one profile — `~/.cache/web/profile` — unless `--profile`
names another, which lives in `~/.cache/web/profiles/<name>`:

```sh
web --profile work --login          # its own logins
web --profile work news.ycombinator.com
web --profile - --screenshot shot.png example.com   # throwaway, removed on exit
```

A named profile gets its own browser, history, and tabs. `--endpoint`,
`--browsers`, `--kill` and `--open` answer for one profile, so a run against
one leaves every other window alone. `^G` names the profile when it is not the
shared one, and `--exec` puts it in the environment as `WEB_PROFILE`, which a
window started from there inherits.

## Keys

| Key | Action |
|---|---|
| `^L` | address bar; a plain phrase becomes a search |
| `^O` / `^P` | back / forward |
| `^R` | reload |
| `^Y` | copy the page selection, or the address if nothing is selected |
| `^E` | open this page in the desktop browser |
| `^F` | find in page, then `f3` / `shift+f3` |
| `^G` | devtools port, frame size, write time, throughput |
| `^D` | start or stop a trace into `/tmp/web_input.log` |
| `^S` | hide or show the status line |
| `^Q` | quit |
| `^T` / `^W` | new tab / close tab (the last one closes the window) |
| `^N` / `^B` | next tab / previous tab (`shift+alt+→` / `shift+alt+←` too) |
| `alt+1`…`alt+9` | go to that tab |
| `alt+0` | reset zoom, pinned width, and window proportion |
| `alt+f` | fit-to-width on/off |
| `cmd+c` | copy the page selection |
| `cmd+v` | paste into the page, or into the address bar when it is open |
| `^X` | open or close the console |
| `alt+y` | copy the console transcript |
| `alt+enter` | let a frozen run carry on |
| `alt+g` | every tab at once, in a grid — see [The grid](#the-grid) |
| `?` | key list over the page; any key dismisses it |
| mouse | click, drag to select, wheel to scroll |
| tab bar | click to switch, middle-click to close, wheel to cycle |
| console border | drag up or down to resize the pane |

While reading. Letters this table does not name are typed into the page;
`vim = yes` gives most of them to the window instead — see [Vim keys](#vim-keys).

| Key | Action |
|---|---|
| `↓` / `↑` | down / up a line |
| `←` / `→` | passed to the page |
| `pgdn` / `pgup` | a screen (`space` too) |
| `backspace` / `shift+backspace` | back / forward |
| `[` / `]` | zoom out / in; inline, scales the window from both edges |
| `shift`+`↑↓←→` | drag one window edge (`U`/`D`/`L`/`R` too) |
| `w` / `W` | widen / narrow the width the page is told it has |
| `s` | frame size: auto, 100%, 75%, 50% |
| `f` / `F` | label the links, then type a label: follow it / open it in a tab |
| `P` | picking: click the page for a CSS selector, into the console |
| `i` | hand the keyboard to the page |
| `:` | open the console |
| `?` | key list; `↓` / `↑` to scroll it |
| `esc` | take the keyboard back |

## Vim keys

Set `vim = yes` in `~/.config/web/web.conf`. Without it no letter moves the
page — `j` types a `j` — and the window is driven by the arrows, the chords,
and the few keys above that are this program's own rather than vi's. With it,
the vi vocabulary is the window's:

| Key | Action |
|---|---|
| `j` / `k` | down / up |
| `h` / `l` | left / right, for a page wider than its given width |
| `d` / `u` | half a screen (`^D` / `^U` too) |
| `b` | a screen back; `space` forward, and `^F` / `^B` for both |
| `gg` / `G` | top / bottom |
| `/` | find in page, then `n` / `N` |
| `H` / `L` | back / forward |
| `o` / `O` | address bar: this tab, with the current address / a new tab, blank |
| `:` | address bar, blank |
| `r` / `R` | reload / reload ignoring the cache |
| `T` | find an open tab by name — see [Finding a page](#finding-a-page) |
| `gh` | find a page been to before |
| `gi` | focus the first text field |
| `gf` | label everything clickable, past any `hint-only` rule for the site |
| `yy` | copy the address |
| `yf` | label the links, then type a label to copy its address |
| `ZZ` | quit |

`f` and `F` label the links in either mode, and everything else in the reading
table still applies: `[` `]` `w` `W` `s` for the window, `P`, `i`, `esc`, `?`.

Six keys the plain map uses change meaning while this is on: `L` and `R` stop
sizing the window (`shift`+`←`/`→` still do), `:` stops opening the console
(`^X` still does), `^F` stops finding (`/` does), `^D` stops tracing, and `^B`
stops going to the previous tab (`shift+alt+←` still does).

The layer sits under `web.conf`, so any key the file names keeps what the file
gave it, and the status line says how many it kept. The generated file writes
the defaults as comments for that reason — a live line there would take the key
back from this layer. A file written by an earlier version has them live
instead; comment the key block out to follow the defaults again. `web` says so
on stderr when a line hides a pair, as an old `g = top` hides `gg`.

## Links without a mouse

`f` puts a label on everything clickable in view. Type a label to click it,
`F` to open it in a new tab, `esc` to put the labels away, `backspace` to undo
a character. Labels are `a` `s` `d` `f` `g` `h` `j` `k` `l`, short ones first
and from the top of the page down; a label that can only be one thing fires
without waiting. Every key goes to the labels while they are up, so nothing
half-typed reaches the page.

Links, buttons, fields, and anything carrying `role`, `onclick` or `tabindex`
are labelled, in the page and in same-origin frames and open shadow roots.
Something covered by a banner or a modal is not, since the click would land on
the cover. What gets sent is a real mouse click at the label's own position:
hover, focus, and the popups a click is allowed to open all behave as if the
mouse had done it.

### Site rules

Per-host CSS selectors in `web.conf` decide which elements get labels:

```
hint-only news.ycombinator.com = .titleline > a
hint-skip github.com           = .Header, footer
```

`hint-only` replaces the default set for that host; add `,input,button` to keep
fields and buttons. `hint-skip` keeps the default set and drops what the
selector matches, along with everything inside it. Both may be given for one
host.

The host matches the end of the page's own on a dot, so `ycombinator.com`
covers `news.ycombinator.com`; the first rule of each kind that matches wins.
`hint-all` labels everything whatever the rules say — `gf` under the vim keys,
bindable anywhere else.

## The grid

`alt+g` draws every tab at once, up to nine, each in a tile with its name under
it:

| Key | Action |
|---|---|
| `alt+g` | open the grid, or close it |
| `↓` `↑` `←` `→` | pick the tile next to this one |
| `enter` | open the tile that is in front |
| `esc` | the same |
| `alt+1`…`alt+9` | pick that tile |
| click a tile | pick it; clicking the one in front opens it |

A page opened by something driving the window — one per worker of a test run —
becomes a tab as it appears and goes when it goes. It is the driver that says
so: Chrome's news that a page has appeared says nothing about who asked for it,
and the pages of every other window on the same browser look no different from
here.

Each page is photographed at the size of its own tile, so it reflows to the
width it is drawn at rather than being a shrunken screenshot of a whole window.
One photograph is taken at a time, a few a second, going round the tiles in turn
and stopping twice as often on the one in front — so a grid of nine costs what a
grid of two does. A page that has not changed comes back as the same bytes and
is not drawn again, so a grid of pages sitting still costs nothing at all. The
screencast stops while the grid is up.

## Finding a page

Two lists, opened over the page and narrowed as you type: `search-tabs` for the
tabs already open, `search-history` for the pages this profile has been to.
`T` and `gh` under the vim keys, bindable anywhere else.

| Key | Action |
|---|---|
| any letter | narrow the list |
| `↓` / `↑` | down / up (`^N` / `^P` and `tab` / `shift+tab` too) |
| `pgdn` / `pgup` | a screenful |
| `enter` | go there, or switch to the tab if it is one already open |
| `shift+enter` / `^T` | open it in a new tab |
| `backspace` / `^U` | delete a character / the line |
| `esc` / `^G` | put the list away |

Every word typed has to appear in the title or the address, in any order, so
`hacker news` and `news hacker` find the same page and each word narrows the
list further. Case is ignored until you type a capital, and then it matters.

Ranking is Vimium's: how well the words match the title and address, plus
recency out to a month old. Visit count is not counted. With nothing typed the
history opens on the most recent page first; tabs are ranked on the words alone.

The history is Chrome's own, read from `History` in the profile
(`~/.cache/web/profile`) — everything opened in `web`, including from a
`--login` window.

## Config

`~/.config/web/web.conf`, written with the defaults on first run:

| Setting | Default | Values | Description |
|---|---|---|---|
| `vim` | `no` | yes/no | the vim key layer, under whatever keys this file names |
| `pause-on-blur` | `yes` | yes/no | stop drawing while the terminal is not focused. A window being driven by `--exec` or a script draws either way |
| `status-line` | `yes` | yes/no | show the status line under the page |
| `clear-on-exit` | `yes` | yes/no | erase the window on exit instead of leaving it |
| `full` | `no` | yes/no | take over the whole terminal instead of drawing a window |
| `mute` | `no` | yes/no | start with the page's audio switched off |
| `raw-keys` | `no` | yes/no | let a key the page did not want reach the window system |
| `keep` | `no` | yes/no | leave Chrome running on exit |
| `scale` | `auto` | `auto`, 0.1–3 | frame size as a fraction of the viewport; `auto` is full size when the page is still, smaller while it moves |
| `zoom` | `1.5` | 0.5–3 | page magnification a new window opens at |
| `rows` | `40` | `auto`, a count | cell rows a new window opens at |
| `cols` | `80` | `auto`, a count | cell columns a new window opens at |
| `slowmo` | `0` | 0–60000 | milliseconds between the actions of what `--exec` starts |
| `freeze` | `no` | yes/no | hold the page where a driver failed until `alt+enter` |
| `grid` | `no` | yes/no | show the grid whenever there is more than one page |

Booleans also take `true`, `on` and `1`. Each setting is the command line
option of the same name, which wins for that run. `hint-only` and `hint-skip`
lines go in the same file, one per site — see [Site rules](#site-rules).

`zoom`, `rows` and `cols` are where a new window opens. `[` `]` `alt+0` and
`shift`+arrows move a running window from there, and nothing is written back.
`rows` and `cols` do nothing under `full = yes`; a `cols` wider than the
terminal is drawn at the terminal's width.

Keys go in the same file:

```
shift-alt-right  = tab-next
^y               = copy
y                = copy-url
f5               = reload
gg               = top
g i              = focus-input
```

Keys are `ctrl` `alt` `shift` `cmd` joined by `+` or `-`, then a character or
one of `left` `right` `up` `down` `space` `esc` `enter` `tab` `backspace`
`delete` `home` `end` `pgup` `pgdn` `f1`–`f12`. `^y` is `ctrl+y`. Action names
are the ones in the generated file; `none` unbinds; deleting a line restores
its default. A key without `ctrl`, `alt` or `cmd` acts only while reading.

Two keys pressed one after the other are a binding too, written as a pair of
characters (`gg`, `yf`) or with a space between them where either needs a name
or a modifier (`g i`, `^X f`). The first key of a pair shows on the status line
while it waits. A key bound on its own happens at once, so the pairs starting
with it are never reached: bind `y = none` before binding `yy`.

## Options

```
web [options] <url>...

--scale F   hold the frame at F of the viewport (default auto: full size
            when the page is still, smaller while it moves). Above 1 does
            nothing; the screencast never exceeds the viewport
--zoom F    page magnification (default 1.5)
--rows N    how many cell rows the window gets (default 40)
--cols N    how many cell columns the window gets (default 80, and
            never wider than the terminal)
--no-status start with the status line hidden (^S toggles it)
--no-clear  leave the window on screen on exit instead of erasing it
--full      take over the whole terminal instead of drawing a window
--show      run Chrome with a visible window too
--mute      start with the page's audio switched off
--eval JS   run javascript in the page and print what it answers
--delay MS  pause between lines of piped javascript
--step      wait for a key between those lines
--timeout S how long a line waits before giving up (default 5)
--json      script output as one JSON object per value
--screenshot F   write the page to F as a png and exit; "-" is stdout
--login     open a window to sign in with, on the same profile
--keep      leave Chrome running on exit, for this window and every other
--open URL  open URL in a tab of the window most recently used, and exit.
            Nothing running is an error, so a caller can start one
--endpoint  print every running window as JSON and exit
--browsers  list the Chrome processes web has running, with pids, and say
            which a new window could adopt
--kill      quit this profile's windows and end its browsers, including
            any nothing can reach
--exec CMD  run CMD against this window, its output in the console
--slowmo MS pause MS between the actions of what --exec starts, so a run
            can be watched rather than only finished
--freeze    hold the page where a driver failed instead of tearing it
            down; alt+enter lets the run carry on
--grid      show the grid whenever there is more than one page, so a run
            with several workers opens as one tile each
--profile N run in a profile of its own: its own logins, history and
            browser, and none of the windows already up. "-" is a
            throwaway one, removed on exit
--port N    fix Chrome's devtools port instead of letting it pick one
--no-pause  keep drawing while the terminal is not focused
--raw-keys  let a key the page did not want reach the window system
```

`--browsers` lists the Chrome instances `web` has started:

```
$ web --browsers
pid 25495   up 04:11        port 53859
```

```
$ web --browsers
pid 50596   up 26:00        stranded - nothing can reach it
web: 1 stranded browser holding the profile; web --kill ends it
```

`--kill` ends them:

```
$ web --kill
web: window 15902 is running but is not this profile's to end; kill 15902 if it is yours
```

## Screenshots

`--screenshot` writes the page to a PNG and exits:

```sh
./web --screenshot shot.png example.com
./web --screenshot - example.com | pngtopam        # "-" is stdout
echo 'document.querySelector("#accept").click()' |
    ./web --screenshot shot.png example.com
```

## Scripting

`--eval` and piped stdin both run JavaScript in the page:

```sh
./web --eval 'document.title' example.com
./web example.com < check.js                     # a file needs no flag
echo 'document.links.length' | ./web --json example.com
```

Stdin is read a line at a time whenever it is not a terminal. Each line goes to
the page's `eval`, so it need not be an expression — the completion value is the
answer, as in devtools:

```
document.title
let n = document.links.length; n * 2
location.href = "https://example.org"
document.querySelector("h1").textContent
```

Values go to stdout, one per line, while the page goes to the terminal, so
`./web example.com < s.js | jq` works. `--json` wraps each value in an object
with the line that produced it. A line that throws prints to stderr and exits
non-zero. A line that starts a navigation is not finished until the page has
arrived.

A line that answers a promise is not finished until the promise is, so anything
asynchronous can be a line of its own:

```
fetch("/api/status").then(r => r.json()).then(j => j.state)
```

Waiting is what a page needs and a line cannot say, so the page is given the
verbs for it as `__web`. Each answers a promise, and each gives up after
`--timeout` unless told otherwise:

| Call | What it does |
|---|---|
| `__web.wait(sel[, ms])` | the element, once it is there |
| `__web.gone(sel[, ms])` | waits until nothing matches |
| `__web.until(fn or js[, ms])` | waits until it answers something true |
| `__web.click(sel[, ms])` | waits for it, scrolls to it, clicks it |
| `__web.type(sel, text[, ms])` | waits for it, focuses it, types, fires `input` and `change` |
| `__web.text(sel)` | its text, trimmed, or null |
| `__web.all(sel)` | the text of every match |
| `__web.count(sel)` | how many match |
| `__web.attr(sel, name)` | one attribute, or null |

```sh
./web example.com <<'EOF'
__web.click("a")
__web.wait("h1").then(e => e.textContent.trim())
location.host
EOF
```

A line that starts a navigation is finished when the page has arrived, so the
line after it runs against the new one. A wait that gives up says what it was
waiting for and the run stops, non-zero.

`--delay MS` paces the run, `--step` waits for a key between lines, `--timeout S`
caps one line.

## The console

`:` while reading, or `^X` at any time, opens a line editor under the page;
either closes it. It has history, `^R` search, and emacs kill bindings. `esc`
hands the keyboard back with the console still up, as does clicking the page.

What you type is JavaScript, evaluated in the page:

```
> document.querySelectorAll('a').length
42
> let seen = new Set(); for (const a of document.links) seen.add(a.host); [...seen]
news.ycombinator.com,github.com
> location.href = 'https://example.com'
```

Lines join the same queue `--eval` uses. Shift+Enter adds an input line, Page
Up/Page Down or the wheel scrolls the transcript, Enter runs. Dragging the top
border up or down resizes the pane, down to three rows and up to two rows short
of the whole window; the page takes back whatever it gives up. `alt+y` copies the
whole transcript, from inside the console or out of it — including whatever
`--exec` has printed there.

`__web` is here too, so `__web.click(".titleline > a")` waits for the link and
clicks it, and a line that answers a promise is answered when it resolves.

`P` while reading toggles picking: clicking the page writes the shortest CSS
selector for what you hit into the console instead of activating it.

## Remote control

`--port` pins the devtools port so Playwright can connect:

```sh
./web --port 9222 news.ycombinator.com
```

```js
const browser = await chromium.connectOverCDP('http://127.0.0.1:9222');
```

`^G` shows the port when Chrome picked one itself. The port names the browser,
not the page, so `--endpoint` reports every running window as one JSON object
per line, without starting a browser:

```console
$ web --endpoint
{"pid":4123,"port":9222,"cdp":"http://127.0.0.1:9222","target":"0A32…","url":"https://example.com/","title":"Example Domain","handoff":true,"drive":"…/driving/4123","freeze":""}
```

Playwright has no accessor for a target id, so match it per page:

```js
const browser = await chromium.connectOverCDP(cdp);
const ctx = browser.contexts()[0];
for (const page of ctx.pages()) {
  const s = await ctx.newCDPSession(page);
  const { targetInfo } = await s.send('Target.getTargetInfo');
  if (targetInfo.targetId === target) { /* the page on screen */ }
}
```

The package does that loop for you — see [Playwright](#playwright). Playwright
is not required: CDP is a websocket taking JSON, and Node has had `WebSocket`
and `fetch` built in since 22. `js/cdp.mjs` is that connection in standard
library only, and `examples/drive.mjs` is a demo written against it:

```sh
node examples/drive.mjs                 # against the one window running
web --exec 'node examples/drive.mjs' news.ycombinator.com
```

### Playwright

`@jhickner/web` is this repository as a package. `/test` is `@playwright/test`
with `page` bound to the tab on screen, so a spec file is an ordinary spec file:

```js
import { test, expect } from '@jhickner/web/test';

test('the front page lists stories', async ({ page }) => {
  await page.goto('https://news.ycombinator.com');
  await expect(page.locator('.titleline > a').first()).toBeVisible();
});
```

```sh
npm i @jhickner/web @playwright/test
web --exec 'npx playwright test' about:blank   # watch it run
npx playwright test                            # against a window already up
```

`examples/hn.spec.mjs` is that spec. From a checkout, `npm i @playwright/test`
and:

```sh
web --exec 'npx playwright test examples/hn.spec.mjs' about:blank
```

| Import | What it is |
|---|---|
| `@jhickner/web/test` | `test` and `expect`, with `page`, `context` and `browser` on the window |
| `@jhickner/web/playwright` | `attach()`, `connect()`, `pageFor()` for a script of your own |
| `@jhickner/web/cdp` | `endpoint()` and `page()` over raw CDP, no dependencies |

- The first worker drives the window's own tab; every worker after it opens a
  page of its own, which the window takes into the tab bar as it appears and
  drops again when it goes. `--workers=4` is four tabs, and `alt+g` is four
  tiles — one per worker, all running at once. `--grid` opens the grid by
  itself as soon as the second page appears. See [The grid](#the-grid).
- Playwright's unit of parallelism is the *file*: four tests in one file are one
  worker however many `--workers` allows, and so one tile. `test.describe
  .configure({ mode: 'parallel' })` in the file, or `fullyParallel` in the
  config, is what spreads them. `examples/fleet.spec.mjs` is four pages at once;
  `examples/hn.spec.mjs` is deliberately the other way, its tests handing the
  same page on to each other.
- The tab is the context, so cookies, storage and the page itself carry from one
  test to the next; there is no fresh context per test.
- The viewport is the window's — `--cols`, `--rows`, `[` and `]` set it, not
  `test.use({ viewport })`.
- `--slowmo 200` puts a pause between actions, which is what makes a run
  something to watch.
- A window being driven keeps drawing while the terminal is not focused, so it
  stays live in one pane while the run is watched from another. `--exec` is seen
  directly; a runner started anywhere else says so through the package.
- `--freeze` holds the page where a test failed — see [Freezing](#freezing).
- `WEB_PROFILE=ci`, or `--profile ci`, keeps a run off the browser you are using.
- A failing test attaches a screenshot of the page as it was left.

### Freezing

`--freeze` stops a failing test where it failed and holds the page there:

```sh
web --freeze --exec 'npx playwright test --workers=1' about:blank
web --freeze news.ycombinator.com          # for a runner in another pane
```

The status line says `FROZEN` and the console says which test and why. Nothing
has been torn down: the console runs JavaScript against that page, `P` picks a
CSS selector off the element that was not found, `f` labels what was actually
clickable. `alt+enter` lets the run carry on to the next test.

The test's timeout is lifted while it waits. Only a window started with
`--freeze` freezes anything, so a run with nothing watching it — CI — never
stops.

### --exec

`--exec` starts a program with the endpoint in its environment and puts its
output in the console while the page stays live above it:

```sh
web --exec 'node examples/attach.mjs' example.com
```

The child gets `WEB_CDP_URL`, `WEB_CDP_PORT`, `WEB_TARGET_ID`, and
`WEB_PROFILE` when the window is in a named one. Quitting the
window ends it; it ending leaves the window. Both streams go to the console.

### attach

To go the other way, start the browser from Playwright and take it over:

```js
const browser = await chromium.launch({args: ['--remote-debugging-port=9222']});
```

Then `attach 9222`, typed in the console or run from a script:

- The browser `web` started is shut down unless `--keep` said otherwise. The one
  it attached to is never shut down; quitting leaves it running.
- `--keep` is asked of the browser, not the run: while any window has asked for
  this browser to stay, no window's quit will shut it down. The request dies
  with the browser.
- The device metrics override goes on their page too, so Playwright sees the
  viewport the frames are drawn at.
- `--port N` against a browser already answering there takes it over rather than
  starting a second, which is what makes `--keep --port` work. With no address
  it leaves that browser on whatever page it is on.
- Unless that browser is one of ours: a run records the port it started Chrome
  on, so a later `--port` at the same browser knows it is an earlier run's, takes
  a tab of its own, and can shut it down like any other run.

## Zoom and width

`[` and `]` change the *viewport width* rather than magnifying pixels, so the
page reflows: text gets genuinely larger and responsive sites drop to their
narrow layout.

After each change `web` asks the page whether it still fits, and a page that
cannot reflow that narrow gets its viewport widened back until it does. The
status line says so (`zoom 77% - page needs 1240px`) and the stored zoom drops
to what the page allowed.

`alt+f` turns fitting off if you would rather have the magnification and scroll
sideways with `h` and `l`. `alt+0` resets to 100%.

A width pinned with `w` or `W` is exempt and honoured whether the page fits it
or not. The measurement still runs — `width 360px - page needs 980px` — and what
does not fit is reached with `h` and `l`.

## Environment

| Variable | Effect |
|---|---|
| `WEB_CHROME` | path to a different Chrome build |
| `WEB_PROFILE` | the profile to run in, as `--profile` names it. Set for what `--exec` starts |
| `WEB_WINDOW` | which window a script attaches to, by pid, when several are up |
| `WEB_CELL` | `WxH` cell size, if the page aspect looks stretched. A terminal reporting no pixel geometry leaves it a guess (8x17 default); `^G` says which you have |

## How it works

```
Chrome ──Page.screencastFrame──> base64 PNG ──> kitty graphics ──> terminal
   ^                                                                  |
   └────────── Input.dispatchKeyEvent / dispatchMouseEvent ───────────┘
```

Frames pass through as base64 PNG, never decoded or re-encoded.

Scrollbars are turned off and scrolling is one jump rather than an animation.

## Default browser

`make browser` installs `web` and builds `~/Applications/Web.app`, an
`http`/`https` handler that hands the link on:

```sh
make browser
```

Then System Settings > Desktop & Dock > Default web browser > web, and confirm
the prompt macOS puts up. `duti` and other third-party setters cannot do it.

A link opens as a tab in the `web` window most recently used, which selects its
own tmux pane if it is in one. With no window running, it starts one outwards
from whatever is already on screen:

| Where | What happens |
|---|---|
| a free pane of the tmux window on screen | `web` is run in it |
| no free pane there | a split of that same window |
| no room left to split | a tmux window of its own, in that session |
| no tmux | a tab of the Ghostty window already open |
| no Ghostty | a window of its own |

A pane is free when nothing but a shell is running in it, and the active pane is
taken first. The tmux window on screen is the one the client active last is
showing. The terminal is brought forward either way.

```sh
sh mkbrowser.sh --terminal kitty   # some other terminal
sh mkbrowser.sh --no-activate      # leave the terminal where it is
sh mkbrowser.sh --to /Applications # somewhere other than ~/Applications
```

`--open` is the same handoff from a shell, and is what the bundle runs:

```sh
web --open https://example.com
```

It exits non-zero when there is no window to hand to. A window started by a
`web` older than `--open` is never handed to.

// Finding a `web` window and driving the page it shows, with nothing installed.
// Node has had WebSocket and fetch built in since 22, and CDP is a websocket
// that takes JSON, so the whole of this is the standard library.
//
// It is here so the examples can be about what they do rather than about
// getting connected. Read it once and you have read all of the protocol any of
// them use.

import { execFileSync } from 'node:child_process';

// Under --exec the endpoint is already in the environment. Otherwise ask the
// windows that are running - one of them, or say which.
export function endpoint() {
  if (process.env.WEB_CDP_URL && process.env.WEB_TARGET_ID) {
    return { cdp: process.env.WEB_CDP_URL, target: process.env.WEB_TARGET_ID };
  }
  const out = execFileSync(process.env.WEB_BIN || 'web', ['--endpoint'], {
    encoding: 'utf8',
  }).trim();
  if (!out) throw new Error('no web window is running');
  const windows = out.split('\n').map((l) => JSON.parse(l));
  // Which one, when there are several: WEB_WINDOW names it by pid, and
  // WEB_PROFILE has already narrowed the list to one profile's windows.
  const want = Number(process.env.WEB_WINDOW) || 0;
  if (want) {
    const found = windows.find((w) => w.pid === want);
    if (!found) throw new Error(`no window with pid ${want}`);
    return found;
  }
  if (windows.length > 1) {
    throw new Error(
      'more than one window is running; set WEB_WINDOW to one of these pids:\n' +
        windows.map((w) => `  ${w.pid}  ${w.url}`).join('\n'),
    );
  }
  return windows[0];
}

// One connection, straight to our own page rather than to the browser: the
// target id is what makes it the window on screen rather than whichever tab
// answered first.
async function session({ cdp, target }) {
  const { port } = new URL(cdp);
  const ws = new WebSocket(`ws://127.0.0.1:${port}/devtools/page/${target}`);
  await new Promise((ok, no) => {
    ws.addEventListener('open', ok, { once: true });
    ws.addEventListener('error', () => no(new Error(`cannot reach ${cdp}`)), {
      once: true,
    });
  });

  let nextId = 1;
  const pending = new Map();
  ws.addEventListener('message', (e) => {
    const msg = JSON.parse(e.data);
    const waiting = pending.get(msg.id);
    if (!waiting) return;                       // an event, and nobody asked
    pending.delete(msg.id);
    if (msg.error) waiting.no(new Error(msg.error.message));
    else waiting.ok(msg.result);
  });

  const send = (method, params = {}) =>
    new Promise((ok, no) => {
      const id = nextId++;
      pending.set(id, { ok, no });
      ws.send(JSON.stringify({ id, method, params }));
    });

  return { send, close: () => ws.close() };
}

// The handful of things a demo actually asks a page for. Not an automation
// library: no waiting, no retries, no selector engine - when you want those,
// examples/attach.mjs hands the same window to Playwright, which has them.
export async function page() {
  const cdp = await session(endpoint());

  const evaluate = async (expression) => {
    const { result, exceptionDetails } = await cdp.send('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
    });
    if (exceptionDetails) throw new Error(exceptionDetails.text);
    return result.value;
  };

  const wait = (ms) => new Promise((r) => setTimeout(r, ms));

  // Poll until the page agrees. A fixed sleep is a guess about somebody else's
  // server: too short and the demo reports the page it was on a moment ago,
  // long enough to be safe and it is dead air on screen.
  const until = async (expression, timeout = 15000, every = 250) => {
    const deadline = Date.now() + timeout;
    for (;;) {
      if (await evaluate(expression)) return true;
      if (Date.now() > deadline) return false;
      await wait(every);
    }
  };

  return {
    send: cdp.send,          // the escape hatch: any CDP method, unwrapped
    eval: evaluate,
    wait,
    until,
    title: () => evaluate('document.title'),
    url: () => evaluate('location.href'),
    text: (sel) => evaluate(`document.querySelector(${JSON.stringify(sel)})?.textContent.trim()`),
    count: (sel) => evaluate(`document.querySelectorAll(${JSON.stringify(sel)}).length`),
    scroll: (by = 400) => evaluate(`window.scrollBy({top: ${by}, behavior: "smooth"})`),

    async goto(url, settle = 1500) {
      await cdp.send('Page.navigate', { url });
      await wait(settle);                       // no load event without Page.enable
    },

    // Scrolled into view so it can be watched happening, then clicked through
    // the element rather than by dispatching a mouse event at its coordinates.
    //
    // The coordinate way is what `web` itself does and what Playwright does,
    // and it is the better gesture: it carries hover and focus with it. But it
    // is hit tested against the compositor's idea of where the page is
    // scrolled to, which only catches up when a frame is produced - so after a
    // scroll it needs a frame to have gone out, and nothing guarantees one
    // when the window is not being watched. A click that silently lands on the
    // wrong element is a poor thing to put in an example, so this one takes
    // the reliable route and leaves the real input to the two tools that
    // manage the timing properly.
    async click(sel) {
      const hit = await evaluate(`(() => {
        const el = document.querySelector(${JSON.stringify(sel)});
        if (!el) return false;
        el.scrollIntoView({ block: 'center', behavior: 'instant' });
        el.click();
        return true;
      })()`);
      if (!hit) throw new Error(`no element matching ${sel}`);
    },

    // Just go: the connection is the only thing keeping node alive, and the
    // window carries on with the page exactly where this left it.
    close: cdp.close,
  };
}

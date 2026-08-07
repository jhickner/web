import { execFileSync } from 'node:child_process';
import { unlinkSync, writeFileSync } from 'node:fs';

function mark(window) {
  attached = window;
  if (!window?.drive || !window.pid) return;
  try {
    writeFileSync(window.drive, `${process.pid}\n`);
  } catch {
    return;
  }
  const nudge = () => { try { process.kill(window.pid, 'SIGUSR1'); } catch {} };
  nudge();
  const done = () => {
    try { unlinkSync(window.drive); } catch {}
    nudge();
  };
  process.on('exit', done);
  process.on('SIGINT', () => { done(); process.exit(130); });
  process.on('SIGTERM', () => { done(); process.exit(143); });
}

let attached;
export const attachedWindow = () => attached;

export function endpoint() {
  if (process.env.WEB_CDP_URL && process.env.WEB_TARGET_ID) {
    return (attached = {
      cdp: process.env.WEB_CDP_URL,
      target: process.env.WEB_TARGET_ID,
      freeze: process.env.WEB_FREEZE || '',
    });
  }
  const out = execFileSync(process.env.WEB_BIN || 'web', ['--endpoint'], {
    encoding: 'utf8',
  }).trim();
  if (!out) throw new Error('no web window is running');
  const windows = out.split('\n').map((l) => JSON.parse(l));
  const want = process.env.WEB_WINDOW || '';
  if (want) {
    const found = windows.find((w) => String(w.pid) === want || w.name === want);
    if (!found) throw new Error(`no window called ${want}`);
    mark(found);
    return found;
  }
  if (windows.length > 1) {
    throw new Error(
      'more than one window is running; set WEB_WINDOW to one of these:\n' +
        windows.map((w) => `  ${w.name || w.pid}  ${w.url}`).join('\n'),
    );
  }
  mark(windows[0]);
  return windows[0];
}

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
    if (!waiting) return;
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

  const until = async (expression, timeout = 15000, every = 250) => {
    const deadline = Date.now() + timeout;
    for (;;) {
      if (await evaluate(expression)) return true;
      if (Date.now() > deadline) return false;
      await wait(every);
    }
  };

  return {
    send: cdp.send,
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

    close: cdp.close,
  };
}

// Run this from another terminal while a window has stopped drawing.
//
// It answers the question the window itself cannot: whether Chrome is busy or
// stopped. Our own CDP connection goes quiet either way, because a session
// dispatches in order and one slow command holds up everything behind it - so
// the useful measurements are the ones taken from outside that queue.
//
//   node tools/wedge.mjs
//
// Needs nothing installed; node 22+ has WebSocket and fetch.

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function endpoint() {
  const out = execFileSync(process.env.WEB_BIN || 'web', ['--endpoint'], {
    encoding: 'utf8',
  }).trim();
  if (!out) throw new Error('no web window is running');
  const wins = out.split('\n').map((l) => JSON.parse(l));
  if (wins.length > 1 && !process.argv[2]) {
    throw new Error('more than one window; pass a pid:\n' +
      wins.map((w) => `  ${w.pid}  ${w.url}`).join('\n'));
  }
  return process.argv[2]
    ? wins.find((w) => String(w.pid) === process.argv[2])
    : wins[0];
}

// One command on a connection of our own, with a deadline. A fresh connection
// is the point: it shares the renderer with the wedged window but not its
// backlog, so a reply here means the page is alive and the queue is merely long.
async function ask(url, method, params = {}, ms = 4000) {
  const t0 = Date.now();
  let ws;
  try {
    ws = new WebSocket(url);
    await new Promise((ok, no) => {
      const t = setTimeout(() => no(new Error('connect timed out')), ms);
      ws.addEventListener('open', () => { clearTimeout(t); ok(); }, { once: true });
      ws.addEventListener('error', () => { clearTimeout(t); no(new Error('connect failed')); }, { once: true });
    });
    const reply = await new Promise((ok, no) => {
      const t = setTimeout(() => no(new Error(`no reply in ${ms}ms`)), ms);
      ws.addEventListener('message', (e) => {
        const m = JSON.parse(e.data);
        if (m.id === 1) { clearTimeout(t); ok(m); }
      });
      ws.send(JSON.stringify({ id: 1, method, params }));
    });
    return { ms: Date.now() - t0, reply };
  } finally {
    try { ws && ws.close(); } catch {}
  }
}

// Every Chrome process on this machine and what it is burning, so a renderer at
// a hundred per cent can be told from one that is doing nothing at all. The
// renderer is the one carrying --type=renderer; the browser has no --type.
function cpu() {
  const out = execFileSync('/bin/ps', ['-axww', '-o', 'pid=,%cpu=,rss=,command='],
    { encoding: 'utf8', maxBuffer: 1 << 24 });
  return out.split('\n')
    .filter((l) => /Google Chrome|Chromium/.test(l) && !/wedge\.mjs/.test(l))
    .map((l) => {
      const m = /^\s*(\d+)\s+([\d.]+)\s+(\d+)\s+(.*)$/.exec(l);
      if (!m) return null;
      const type = /--type=(\w+)/.exec(m[4]);
      return { pid: +m[1], cpu: +m[2], mb: Math.round(+m[3] / 1024),
               kind: type ? type[1] : 'browser' };
    })
    .filter(Boolean)
    .filter((p) => p.kind === 'browser' || p.kind === 'renderer' || p.cpu > 5)
    .sort((a, b) => b.cpu - a.cpu);
}

const win = endpoint();
const { port } = new URL(win.cdp);
console.log(`window pid ${win.pid} on port ${port}`);
console.log(`url ${win.url}\n`);

// Which process is actually serving the debugging port. Asked of the port
// rather than of the process table, because a machine with an ordinary Chrome
// open as well has more than one browser process on it and only one of them is
// ours. This is the process that dispatches every reply we wait on and encodes
// every frame we draw, so it is the one worth watching.
let browserPid = 0;
try {
  const out = execFileSync('/usr/sbin/lsof',
    ['-nP', `-iTCP:${port}`, '-sTCP:LISTEN', '-t'], { encoding: 'utf8' });
  browserPid = parseInt(out.trim().split('\n')[0], 10) || 0;
} catch { /* lsof says nothing when nothing is listening */ }
console.log(browserPid
  ? `the debugging port is served by pid ${browserPid}\n`
  : `nothing is listening on ${port} - the debugging server is gone\n`);

// Two samples a second apart: a process that is busy stays busy, and %cpu from
// ps alone is an average over the process's whole life.
console.log('chrome processes (two samples, one second apart):');
const a = cpu();
await sleep(1000);
const b = cpu();
for (const p of b.slice(0, 8)) {
  const was = a.find((x) => x.pid === p.pid);
  console.log(`  ${String(p.pid).padStart(7)}  ${p.kind.padEnd(9)} ` +
              `cpu ${String(p.cpu).padStart(6)}%  (was ${was ? was.cpu : '?'}%)  ${p.mb} MB`);
}

console.log('\nreachability, each on a connection of its own:');

// Latency rather than a yes or no. A browser process whose main thread is
// merely backed up answers everything eventually, and the size of "eventually"
// is the whole finding: a handshake that normally takes 3ms taking seconds is a
// thread that is saturated, which looks like death only because our own probes
// give up before it gets round to them.
let webSocketDebuggerUrl = null;
{
  const t0 = Date.now();
  try {
    const v = await fetch(`http://127.0.0.1:${port}/json/version`,
                          { signal: AbortSignal.timeout(20000) });
    ({ webSocketDebuggerUrl } = await v.json());
    console.log(`  browser http    ok in ${Date.now() - t0}ms`);
  } catch (e) {
    console.log(`  browser http    FAILED after ${Date.now() - t0}ms: ${e.message}`);
  }
}

if (webSocketDebuggerUrl) {
  try {
    const { ms } = await ask(webSocketDebuggerUrl, 'Target.getTargets', {}, 20000);
    console.log(`  browser cdp     ok in ${ms}ms`);
  } catch (e) {
    console.log(`  browser cdp     FAILED: ${e.message}`);
  }
}

// The wedged page, on a second connection. This is the one that matters: a
// reply means the renderer is running and our window is stuck behind its own
// backlog; no reply means the renderer itself has stopped.
const pageWs = `ws://127.0.0.1:${port}/devtools/page/${win.target}`;
try {
  const { ms, reply } = await ask(pageWs, 'Runtime.evaluate',
    { expression: 'Date.now()', returnByValue: true });
  console.log(`  page eval       ok in ${ms}ms -> ${JSON.stringify(reply.result?.result?.value)}`);
} catch (e) {
  console.log(`  page eval       FAILED: ${e.message}   <- the renderer is not running js`);
}

// Whether it is painting at all, which is what the screencast is waiting for.
let painting = true;
try {
  const { ms, reply } = await ask(pageWs, 'Runtime.evaluate', {
    expression: `new Promise(ok => { const t0 = performance.now(); let n = 0;
      (function f(){ n++; performance.now() - t0 < 1000 ? requestAnimationFrame(f)
                                                       : ok(n); })(); })`,
    awaitPromise: true, returnByValue: true,
  }, 6000);
  console.log(`  page rAF        ${reply.result?.result?.value} ticks in 1s (took ${ms}ms)`);
} catch (e) {
  painting = false;
  console.log(`  page rAF        FAILED: ${e.message}   <- the page is not painting`);
}

// A renderer burning a whole core while refusing to run javascript is inside
// one synchronous task and not coming out of it. Nothing that goes through the
// page can say what that task is - it is the reason nothing goes through the
// page - so the stack is read from outside, the way any hung process is.
// Sampling needs nothing from the process it is sampling, which is the point:
// tracing has to be started over the very connection that is jammed, and asking
// a jammed thread to describe itself is how the last attempt ended.
function grab(pid, what, file) {
  if (!pid) return;
  console.log(`\nsampling ${what} (pid ${pid}) for 5s...`);
  try {
    execFileSync('/usr/bin/sample', [String(pid), '5', '-file', file],
                 { stdio: 'ignore' });
    console.log(`wrote ${file}`);
    // Chrome emulates a slow cpu by interrupting the thread and spinning in the
    // handler, so a throttled renderer burns a whole core and answers nothing -
    // which is indistinguishable from the fault this tool is looking for. Said
    // out loud, because measuring it by accident is the easiest mistake here.
    // readFileSync, not cat: a sample is megabytes and execFileSync's default
    // buffer is one, which fails the check rather than the sample.
    const text = readFileSync(file, 'utf8');
    if (/CPUThrottlingThread/.test(text)) {
      console.log('\n  !! this process is under Emulation.setCPUThrottlingRate.');
      console.log('     web applies that on blur, so switching terminals to run');
      console.log('     this puts it there. Start web with --no-pause, or the');
      console.log('     100% cpu and dead javascript below are just the throttle.');
    }
  } catch (e) {
    console.log(`sample failed: ${e.message}`);
    console.log(`run it by hand:  sample ${pid} 5 -file ${file}`);
  }
}

// The browser process first. It dispatches every reply the window waits on and
// encodes every frame it draws, so when it is behind, everything is.
grab(browserPid, 'the browser process', '/tmp/web_browser.sample');

const busiest = b.find((p) => p.kind === 'renderer' && p.cpu > 50);
if (busiest && !painting) grab(busiest.pid, 'the busiest renderer', '/tmp/web_renderer.sample');

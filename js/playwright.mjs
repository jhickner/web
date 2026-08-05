// The window on screen, as a Playwright page.
//
// connectOverCDP joins a browser that is already running rather than starting
// one, so nothing here downloads a browser: playwright-core is enough, and the
// full playwright package resolves to it anyway.

import { mkdirSync, unlinkSync, writeFileSync } from 'node:fs';
import { chromium } from 'playwright-core';
import { endpoint, attachedWindow } from './cdp.mjs';

// What makes a run watchable: automation moves faster than a screencast can be
// read, so a test that passes looks like a flicker. `--slowmo 200`, or `slowmo`
// in web.conf, is where it is asked for; the window passes it down from there.
const slowMoDefault = () => Number(process.env.WEB_SLOWMO) || 0;

export async function connect({ slowMo } = {}) {
  const { cdp, target } = endpoint();
  const browser = await chromium.connectOverCDP(cdp, {
    slowMo: slowMo ?? slowMoDefault(),
  });
  // The tab on screen lives in the browser's own context. newContext() makes a
  // second, incognito one, and its pages are not shown by anything: they are
  // not the window's, and the window never hears about them.
  const [context] = browser.contexts();
  if (!context) throw new Error(`no browser context at ${cdp}`);
  return { browser, context, target };
}

// Playwright has no accessor for a target id, so each page is asked for its own
// over CDP. One round trip per tab, on a list as long as the tabs you have open
// - and the id is what makes this the page on screen rather than whichever tab
// answered first.
export async function pageFor(context, target) {
  // Under --exec the id is in the environment. Otherwise it comes back from
  // the same place the address did, which is the window itself.
  target ||= process.env.WEB_TARGET_ID || endpoint().target;
  if (!target) throw new Error('no target id: no window said which page is its');
  // A connection that has just been made may not have heard about every tab
  // yet, so a miss is worth asking again about before it is an error.
  for (let tries = 0; tries < 20; tries++) {
    for (const page of context.pages())
      if (await targetOf(context, page) === target) return page;
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`no page with target ${target}`);
}

// The id Chrome knows a page by, which Playwright does not expose.
export async function targetOf(context, page) {
  const session = await context.newCDPSession(page);
  const { targetInfo } = await session.send('Target.getTargetInfo');
  await session.detach();
  return targetInfo.targetId;
}

// Tell the window that this page is one of ours, so it takes it into its tab
// bar and its grid - and untell it when the page goes.
//
// The window cannot work this out for itself: Chrome's news that a page has
// appeared says nothing about who asked for it, and the pages of every other
// window on the browser look exactly the same from there. So the one who knows
// says so.
export function claim(target) {
  const dir = process.env.WEB_PAGES || attachedWindow()?.pages;
  if (!dir || !target) return () => {};
  const path = `${dir}/${target}`;
  try {
    // Made rather than assumed: a window writes this directory when it writes
    // its session file, and a worker can be quicker than that.
    mkdirSync(dir, { recursive: true });
    writeFileSync(path, `${process.pid}\n`);
  } catch (e) {
    // Said out loud. Silently doing nothing here is a page the window never
    // shows, which looks like the grid being broken rather than like this.
    console.warn(`web: cannot claim this page (${e.message})`);
    return () => {};
  }
  return () => { try { unlinkSync(path); } catch {} };
}

// The whole of it in one call, for a script that wants the page and nothing
// else. The browser is left connected: it belongs to the window.
export async function attach(opts) {
  const { context, target } = await connect(opts);
  return pageFor(context, target);
}

// Whether this window freezes, and where to say so - a window says nothing
// there unless it was started with `--freeze`. Under `--exec` it is in the
// environment; a runner started from another pane reads it off the window.
//
// A run nothing is watching must never stop and wait for a key that is not
// coming, which is every run in CI, so the window asking for it is the whole of
// the permission.
export const freezing = () =>
  process.env.WEB_FREEZE || attachedWindow()?.freeze || '';

// Stop here and leave the page exactly as it is, until the window says carry on.
//
// The value is in what does not happen: no teardown, no next test navigating
// away, no context closed. The failure is still on screen and still live, so
// the console can query it, `P` can pick a selector off the thing that was not
// found, and the labels can show what was actually clickable.
//
// Two files rather than stdout: a runner captures what a worker prints and
// holds a failing test's output back until the test is over, which is after the
// freeze it was announcing.
export async function freeze(why = '') {
  const base = freezing();
  if (!base) return false;
  writeFileSync(`${base}.pause`, `${String(why).replace(/\s+/g, ' ')}\n`);
  for (;;) {
    try {
      unlinkSync(`${base}.resume`);           // it appeared, and is ours to clear
      return true;
    } catch {
      await new Promise((r) => setTimeout(r, 100));
    }
  }
}

// The window on screen, as a Playwright page.
//
// connectOverCDP joins a browser that is already running rather than starting
// one, so nothing here downloads a browser: playwright-core is enough, and the
// full playwright package resolves to it anyway.

import { chromium } from 'playwright-core';
import { endpoint } from './cdp.mjs';

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
    for (const page of context.pages()) {
      const session = await context.newCDPSession(page);
      const { targetInfo } = await session.send('Target.getTargetInfo');
      await session.detach();
      if (targetInfo.targetId === target) return page;
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`no page with target ${target}`);
}

// The whole of it in one call, for a script that wants the page and nothing
// else. The browser is left connected: it belongs to the window.
export async function attach(opts) {
  const { context, target } = await connect(opts);
  return pageFor(context, target);
}

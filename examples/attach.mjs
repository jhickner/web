// The same window, handed to Playwright — for when you want its selectors, its
// auto-waiting and its assertions rather than raw CDP.
//
//   npm --prefix /tmp/pw i playwright-core
//   NODE_PATH=/tmp/pw/node_modules node examples/attach.mjs
//   NODE_PATH=/tmp/pw/node_modules web --exec 'node examples/attach.mjs' example.com
//
// playwright-core rather than playwright: the browser is already running and
// already ours, so there is nothing here worth downloading three more for. It
// resolves either way, since the full package depends on this one.

import { chromium } from 'playwright-core';
import { endpoint } from './cdp.mjs';

// Playwright has no accessor for a target id, so each page is asked for its own
// over CDP. One round trip per tab, on a list only as long as the number of
// windows you have open - and the id is what makes this ours rather than
// whichever tab happens to be first.
export async function attach() {
  const { cdp, target } = endpoint();
  const browser = await chromium.connectOverCDP(cdp);
  const context = browser.contexts()[0];
  for (const page of context.pages()) {
    const session = await context.newCDPSession(page);
    const { targetInfo } = await session.send('Target.getTargetInfo');
    await session.detach();
    if (targetInfo.targetId === target) return page;
  }
  throw new Error(`no page with target ${target}`);
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const page = await attach();
  console.log('attached to:', await page.title());

  await page.goto('https://news.ycombinator.com');
  const stories = page.locator('.titleline > a');
  console.log('stories:', await stories.count());

  for (let i = 1; i <= 3; i++) {
    await page.mouse.wheel(0, 400);
    await page.waitForTimeout(700);
  }

  console.log('opening:', await stories.first().textContent());
  await stories.first().click();
  await page.waitForLoadState('domcontentloaded');
  console.log('now at:', page.url());

  // Just go, rather than browser.close(): the browser belongs to the window,
  // not to this script, and node would otherwise sit on the live connection.
  process.exit(0);
}

// `@playwright/test`, with `page` bound to the window on screen.
//
//   import { test, expect } from '@jhickner/web/test';
//
//   test('the front page lists stories', async ({ page }) => {
//     await page.goto('https://news.ycombinator.com');
//     await expect(page.locator('.titleline > a').first()).toBeVisible();
//   });
//
// Nothing else in the file changes: the spec is an ordinary Playwright spec,
// run by the ordinary runner. What changes is which browser it lands in.

import { test as base, expect } from '@playwright/test';
import { claim, connect, freeze, freezing, pageFor, targetOf } from './playwright.mjs';

// The window has one line to say why it stopped. Playwright's message carries
// the call log and a rendered diff after it, which is a screenful.
const firstLine = (s) =>
  (s ?? 'failed').replace(/\x1b\[[0-9;]*m/g, '').split('\n')[0].slice(0, 120);

export const test = base.extend({
  // The connection and the page, once per worker.
  //
  // The first worker drives the window's own tab - the one on screen, the one
  // --endpoint named. Every worker after it opens a page of its own instead, so
  // no two are ever working the same page: `--workers=4` is four pages, and the
  // window shows them as four tabs. `alt+g` is then four tiles, one per worker,
  // which is the whole reason for wanting them separate.
  //
  // parallelIndex rather than workerIndex: a worker replaced after a failure
  // gets a fresh workerIndex, so that one counts restarts as well as workers,
  // and the replacement would open a second page instead of taking over.
  web: [
    async ({}, use, workerInfo) => {
      const conn = await connect();
      const own = workerInfo.parallelIndex > 0;
      const page = own ? await conn.context.newPage()
                       : await pageFor(conn.context, conn.target);
      // Claimed so the window shows it: it has no way of telling this page from
      // one belonging to some other window on the same browser.
      const unclaim = own ? claim(await targetOf(conn.context, page)) : () => {};
      await use({ ...conn, page, own });
      // A page this worker opened is this worker's to close; the window's own
      // tab is not. The browser stays either way - it belongs to the window,
      // and disconnecting is all that is ours to do.
      unclaim();
      if (own) await page.close().catch(() => {});
    },
    { scope: 'worker' },
  ],

  browser: [async ({ web }, use) => use(web.browser), { scope: 'worker' }],

  // The context the tab is in, rather than a fresh one per test: a new context
  // is a new browser window nothing on screen shows. Cookies, storage and the
  // page itself therefore carry from one test to the next, the way they would
  // for somebody sitting in front of it.
  context: async ({ web }, use) => use(web.context),

  page: async ({ web }, use, testInfo) => {
    const page = web.page;
    await use(page);
    // Playwright takes its own screenshots from inside the context it made,
    // which is the one thing overriding the context above costs. Taken here
    // instead, so a failure still leaves something to look at. In the fixture
    // rather than an afterEach: a hook belongs to a test file, and this is a
    // module test files import.
    if (testInfo.status === testInfo.expectedStatus) return;
    try {
      await testInfo.attach('screenshot', {
        body: await page.screenshot(),
        contentType: 'image/png',
      });
    } catch {
      // The page went with the window, which the failure above already says.
    }
    // Before the next test navigates away, and before anything is torn down.
    // Does nothing unless the window asked to freeze - and when it did, the
    // clock has to stop too: teardown is spent out of what is left of the
    // test's own timeout, and a test that failed by timing out has none left.
    if (freezing()) {
      testInfo.setTimeout(0);
      await freeze(`${testInfo.title}: ${firstLine(testInfo.error?.message)}`);
    }
  },
});

export { expect };

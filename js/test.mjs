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
import { connect, freeze, freezing, pageFor } from './playwright.mjs';

// The window has one line to say why it stopped. Playwright's message carries
// the call log and a rendered diff after it, which is a screenful.
const firstLine = (s) =>
  (s ?? 'failed').replace(/\x1b\[[0-9;]*m/g, '').split('\n')[0].slice(0, 120);

export const test = base.extend({
  // The connection, once per worker: the browser, the context the tab is in,
  // and which page is the tab. The last of those is the whole point and is
  // known only here - it came back from the window along with the address, and
  // nothing downstream could work it out again.
  web: [
    async ({}, use, workerInfo) => {
      // One window, one tab, one test at a time. Several workers would drive
      // the same page at once and read each other's navigations as flakiness,
      // which is a hard thing to see for what it is.
      //
      // parallelIndex rather than workerIndex: a worker replaced after a
      // failure gets a fresh workerIndex, so that one counts restarts as well
      // as workers and would call the second failure a parallelism problem.
      if (workerInfo.parallelIndex > 0) {
        throw new Error(
          'a web window is a single page: run with --workers=1',
        );
      }
      await use(await connect());
      // Nothing closed. The browser belongs to the window, and the window is
      // still showing it; disconnecting is all that is ours to do, and node
      // exiting does that.
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
    const page = await pageFor(web.context, web.target);
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

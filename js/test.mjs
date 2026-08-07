import { test as base, expect } from '@playwright/test';
import { claim, connect, freeze, freezing, pageFor, targetOf } from './playwright.mjs';

// strip ansi color escapes
const firstLine = (s) =>
  (s ?? 'failed').replace(/\x1b\[[0-9;]*m/g, '').split('\n')[0].slice(0, 120);

export const test = base.extend({
  web: [
    async ({}, use, workerInfo) => {
      const conn = await connect();
      const own = workerInfo.parallelIndex > 0;
      const page = own ? await conn.context.newPage()
                       : await pageFor(conn.context, conn.target);
      const unclaim = own ? claim(await targetOf(conn.context, page)) : () => {};
      await use({ ...conn, page, own });
      unclaim();
      if (own) await page.close().catch(() => {});
    },
    { scope: 'worker' },
  ],

  browser: [async ({ web }, use) => use(web.browser), { scope: 'worker' }],

  context: async ({ web }, use) => use(web.context),

  page: async ({ web }, use, testInfo) => {
    const page = web.page;
    await use(page);
    if (testInfo.status === testInfo.expectedStatus) return;
    try {
      await testInfo.attach('screenshot', {
        body: await page.screenshot(),
        contentType: 'image/png',
      });
    } catch {
    }
    if (freezing()) {
      testInfo.setTimeout(0);
      await freeze(`${testInfo.title}: ${firstLine(testInfo.error?.message)}`);
    }
  },
});

export { expect };

// Several pages at once, one per worker, to be watched in the grid.
// `../js/test.mjs` is `@jhickner/web/test` once this is a package.
//
//   npm i @playwright/test         # in this checkout
//   web --exec 'npx playwright test examples/fleet.spec.mjs --workers=4' about:blank
//   WEB_WINDOW=hn npx playwright test examples/fleet.spec.mjs --workers=4
//
// Then `alt+g`: four tiles, four workers, all running at the same time. The
// first worker drives the window's own tab and the rest open pages of their
// own, which the window takes into the bar as they appear.
//
// `mode: 'parallel'` is what spreads the tests of ONE file across workers.
// Playwright's unit of parallelism is the file, so four tests in one file with
// `--workers=4` and nothing else said would still be one worker doing them in
// turn - and one tile.

import { test, expect } from '../js/test.mjs';

test.describe.configure({ mode: 'parallel' });

const SITES = [
  ['news', 'https://news.ycombinator.com'],
  ['lobsters', 'https://lobste.rs'],
  ['example', 'https://example.com'],
  ['wikipedia', 'https://en.wikipedia.org/wiki/Terminal_emulator'],
];

for (const [name, url] of SITES) {
  test(name, async ({ page }) => {
    await page.goto(url);
    await expect(page).toHaveTitle(/./);

    // Scrolled slowly and on purpose: what is being demonstrated is four pages
    // moving at once, and a test that is over in 200ms demonstrates nothing.
    for (let i = 0; i < 6; i++) {
      await page.mouse.wheel(0, 300);
      await page.waitForTimeout(400);
    }
    expect(await page.evaluate(() => document.links.length)).toBeGreaterThan(0);
  });
}

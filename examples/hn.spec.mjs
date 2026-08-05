// An ordinary Playwright spec, run by the ordinary runner, against the window
// on screen. `test` and `expect` are the only import that differs from the one
// the runner's own template writes.
//
//   npm i @playwright/test         # in this checkout
//
//   web --exec 'npx playwright test examples/hn.spec.mjs' about:blank
//   npx playwright test examples/hn.spec.mjs    # against a window already up
//
// web --slowmo 200 puts a pause between actions, which is what makes a run
// something to watch rather than a flicker.
//
// `../js/test.mjs` because this file lives in the repository. Installed, the
// same import is `@jhickner/web/test`.

import { test, expect } from '../js/test.mjs';

test('the front page lists stories', async ({ page }) => {
  await page.goto('https://news.ycombinator.com');
  await expect(page).toHaveTitle(/Hacker News/);

  const stories = page.locator('.titleline > a');
  await expect(stories.first()).toBeVisible();
  expect(await stories.count()).toBeGreaterThan(10);
});

test('a story opens', async ({ page }) => {
  // The page is where the test above left it: one window, one tab, and no
  // fresh context between tests.
  const first = page.locator('.titleline > a').first();
  const title = await first.textContent();
  await first.click();
  await page.waitForLoadState('domcontentloaded');
  console.log(`opened: ${title} - ${page.url()}`);
});

test('search finds this project', async ({ page }) => {
  await page.goto('https://hn.algolia.com/?q=terminal+browser');
  await expect(page.locator('.SearchResults')).toBeVisible();
});

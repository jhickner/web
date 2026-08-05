// The same window, handed to Playwright — for when you want its selectors, its
// auto-waiting and its assertions rather than raw CDP.
//
//   npm i playwright-core          # in this checkout: `import` reads
//   node examples/attach.mjs       # node_modules beside the file, not NODE_PATH
//   web --exec 'node examples/attach.mjs' example.com
//
// playwright-core rather than playwright: the browser is already running and
// already ours, so there is nothing here worth downloading three more for. It
// resolves either way, since the full package depends on this one.
//
// `../js/playwright.mjs` because this file lives in the repository. Installed,
// the same import is `@jhickner/web/playwright`.

import { attach } from '../js/playwright.mjs';

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

// Just go, rather than browser.close(): the browser belongs to the window, not
// to this script, and node would otherwise sit on the live connection.
process.exit(0);

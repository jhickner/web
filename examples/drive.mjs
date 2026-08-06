// Drive the page a `web` window is showing, over raw CDP with nothing
// installed. `../js/cdp.mjs` is `@jhickner/web/cdp` once this is a package.
//
//   web --exec 'node examples/drive.mjs' news.ycombinator.com
//   node examples/drive.mjs                    # the one window running
//   WEB_WINDOW=hn node examples/drive.mjs      # by --name, or by pid

import { page } from '../js/cdp.mjs';

const p = await page();
console.log('attached to:', await p.title());

await p.goto(process.argv[2] || 'https://news.ycombinator.com');
console.log(`loaded: ${await p.title()} - ${await p.count('a')} links`);

for (let i = 1; i <= 3; i++) {
  await p.scroll(400);
  await p.wait(700);
  console.log(`scrolled ${i}`);
}

const here = await p.url();
console.log('opening:', await p.text('.titleline > a'));
await p.click('.titleline > a');
await p.until(`location.href !== ${JSON.stringify(here)}`);
console.log('now at:', await p.url());

p.close();

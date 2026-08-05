// Drive the page a `web` window is showing. Nothing installed, nothing to set
// up: js/cdp.mjs is the connection, and it is all standard library.
//
//   web --exec 'node examples/drive.mjs' news.ycombinator.com
//   node examples/drive.mjs                 # against the one window running

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

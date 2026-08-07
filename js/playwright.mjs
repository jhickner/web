import { mkdirSync, unlinkSync, writeFileSync } from 'node:fs';
import { chromium } from 'playwright-core';
import { endpoint, attachedWindow } from './cdp.mjs';

const slowMoDefault = () => Number(process.env.WEB_SLOWMO) || 0;

export async function connect({ slowMo } = {}) {
  const { cdp, target } = endpoint();
  const browser = await chromium.connectOverCDP(cdp, {
    slowMo: slowMo ?? slowMoDefault(),
  });
  const [context] = browser.contexts();
  if (!context) throw new Error(`no browser context at ${cdp}`);
  return { browser, context, target };
}

export async function pageFor(context, target) {
  target ||= process.env.WEB_TARGET_ID || endpoint().target;
  if (!target) throw new Error('no target id: no window said which page is its');
  for (let tries = 0; tries < 20; tries++) {
    for (const page of context.pages())
      if (await targetOf(context, page) === target) return page;
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`no page with target ${target}`);
}

export async function targetOf(context, page) {
  const session = await context.newCDPSession(page);
  const { targetInfo } = await session.send('Target.getTargetInfo');
  await session.detach();
  return targetInfo.targetId;
}

export function claim(target) {
  const dir = process.env.WEB_PAGES || attachedWindow()?.pages;
  if (!dir || !target) return () => {};
  const path = `${dir}/${target}`;
  try {
    mkdirSync(dir, { recursive: true });
    writeFileSync(path, `${process.pid}\n`);
  } catch (e) {
    console.warn(`web: cannot claim this page (${e.message})`);
    return () => {};
  }
  return () => { try { unlinkSync(path); } catch {} };
}

export async function attach(opts) {
  const { context, target } = await connect(opts);
  return pageFor(context, target);
}

export const freezing = () =>
  process.env.WEB_FREEZE || attachedWindow()?.freeze || '';

export async function freeze(why = '') {
  const base = freezing();
  if (!base) return false;
  writeFileSync(`${base}.pause`, `${String(why).replace(/\s+/g, ' ')}\n`);
  for (;;) {
    try {
      unlinkSync(`${base}.resume`);
      return true;
    } catch {
      await new Promise((r) => setTimeout(r, 100));
    }
  }
}

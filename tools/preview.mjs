// Renders the launcher (mock data) in headless Chromium and saves screenshots.
// usage: node tools/preview.mjs [outDir]
import { chromium } from '/opt/node22/lib/node_modules/playwright/index.mjs';
import path from 'node:path';
import url from 'node:url';

const root = path.dirname(path.dirname(url.fileURLToPath(import.meta.url)));
const out = process.argv[2] || path.join(root, 'preview');
const page_ = url.pathToFileURL(path.join(root, 'app/index.html')).href;

const browser = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium' });
const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
page.on('console', m => m.type() === 'error' && errors.push(m.text()));

const shots = [
  ['home', '', []],
  ['stremio-focused', '?state=grid', []],
  ['stremio-shelf', '?state=shelf', []],
  ['scrolled', '?state=scrolled', []],
  ['sheet', '?state=grid', ['hold']],
  ['login', '?state=login', []]
];
for (const [name, q, actions] of shots) {
  await page.goto(page_ + q);
  await page.waitForTimeout(1300);
  for (const a of actions) {
    if (a === 'hold') { await page.keyboard.down('Enter'); await page.waitForTimeout(900); await page.keyboard.up('Enter'); await page.waitForTimeout(500); }
  }
  await page.screenshot({ path: path.join(out, name + '.png') });
  console.log('saved', name);
}
await browser.close();
if (errors.length) { console.error('page errors:\n' + errors.join('\n')); process.exit(1); }

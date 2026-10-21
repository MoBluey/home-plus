/* The service's local HTTP API, end to end: the real service.js on the preview harness's
 * stand-in webOS (tools/native-preview/harness.js), with its mock Stremio library and TMDB.
 * It runs on its own port with its config in a temporary folder, so no root is needed.
 * HOMEPLUS_SERVICE_NODE runs it with another Node, such as the TV's Node 16 (CI does). */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const http = require('http');
const { spawn } = require('child_process');
const { until } = require('./helpers');

const ROOT = path.join(__dirname, '..');
const NODE = process.env.HOMEPLUS_SERVICE_NODE || process.execPath;
let child, port, token, dir, log = '';

function freePort() {
  return new Promise((resolve) => {
    const s = net.createServer().listen(0, '127.0.0.1', () => {
      const p = s.address().port;
      s.close(() => resolve(p));
    });
  });
}

/* a request as the launcher makes it (token, Host 127.0.0.1), unless told otherwise */
function call(method, p, { body, headers = {}, auth = true } = {}) {
  return new Promise((resolve, reject) => {
    const data = body === undefined ? '' : JSON.stringify(body);
    const h = Object.assign(auth ? { 'X-Homeplus-Token': token } : {}, data ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } : {}, headers);
    const req = http.request({ host: '127.0.0.1', port, path: p, method, headers: h }, (res) => {
      let buf = '';
      res.on('data', (c) => { buf += c; });
      res.on('end', () => {
        let json = null;
        try { json = JSON.parse(buf); } catch (e) { /* not JSON */ }
        resolve({ status: res.statusCode, json });
      });
    });
    req.on('error', reject);
    req.end(data || undefined);
  });
}

test.before(async () => {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-api-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix,youtube|YouTube,stremio|Stremio');
  for (let i = 0; i < 5; i++) fs.writeFileSync(path.join(dir, `show${i}.jpg`), `a picture of show ${i}`); /* the mock Stremio's */
  fs.mkdirSync(path.join(dir, 'lginputhook')); /* LG Input Hook, with the Home key bound to Home+ */
  fs.writeFileSync(path.join(dir, 'lginputhook', 'keybinds.json'), JSON.stringify({ 773: { action: 'launch', id: 'com.github.pmiraka.homeplus' } }));
  port = await freePort();
  child = spawn(NODE, [path.join(ROOT, 'tools/native-preview/harness.js'), dir, path.join(ROOT, 'service')], {
    env: Object.assign({}, process.env, {
      NODE_PATH: path.join(ROOT, 'tools/native-preview/stub'),
      HOMEPLUS_CONFIG_DIR: path.join(dir, 'config'),
      HOMEPLUS_TOKEN_FILE: path.join(dir, 'api-token'),
      HOMEPLUS_PORT: String(port),
      HOMEPLUS_PREVIEW: '1',
      HOMEPLUS_PREVIEW_TMDB: '1',
      HOMEPLUS_PREVIEW_FOREGROUND: 'com.github.pmiraka.homeplus', /* Home+ is in front */
      HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'lginputhook', 'keybinds.json')
    }),
    stdio: ['ignore', 'pipe', 'pipe']
  });
  child.stdout.on('data', (d) => { log += d; });
  child.stderr.on('data', (d) => { log += d; });
  await until(async () => {
    if (child.exitCode !== null) throw new Error(`the service stopped (exit code ${child.exitCode})`);
    try { return (await call('GET', '/api/status', { auth: false })).status === 200; } catch (e) { return false; }
  }, 15000, 'the service to start').catch((e) => { throw new Error(`${e.message}. Its output:\n${log}`); });
  token = fs.readFileSync(path.join(dir, 'api-token'), 'utf8').trim();
});

test.after(async () => {
  if (child && child.exitCode === null) { /* Windows won't delete files the service still has open */
    const gone = new Promise((resolve) => child.once('exit', resolve));
    child.kill();
    await gone;
  }
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
});

test('api: /api/status answers without the token (the launcher asks before it has one)', async () => {
  const r = await call('GET', '/api/status', { auth: false });
  assert.equal(r.status, 200);
  assert.equal(r.json.version, JSON.parse(fs.readFileSync(path.join(ROOT, 'service/package.json'), 'utf8')).version);
  assert.equal(r.json.token, true, 'the token file was written');
});

test('api: everything else needs the token', async () => {
  assert.equal((await call('GET', '/api/apps', { auth: false })).status, 403);
  assert.equal((await call('GET', '/api/apps', { headers: { 'X-Homeplus-Token': 'wrong' } })).status, 403);
  const r = await call('GET', '/api/apps');
  assert.equal(r.status, 200);
  assert.deepEqual(r.json.apps.map((a) => a.id).sort(), ['netflix', 'stremio', 'youtube']);
});

test('api: web pages and other hosts are turned away, even with the token', async () => {
  assert.equal((await call('GET', '/api/apps', { headers: { Origin: 'http://example.com' } })).status, 403);
  assert.equal((await call('GET', '/api/status', { auth: false, headers: { Origin: 'http://example.com' } })).status, 403);
  assert.equal((await call('GET', '/api/apps', { headers: { Host: 'evil.example.com' } })).status, 403, 'DNS rebinding');
});

test('api: Continue Watching from Stremio, with every account\'s state for Settings', async () => {
  const r = await call('GET', '/api/continue');
  assert.equal(r.status, 200);
  assert.equal(r.json.source, 'stremio');
  assert.ok(r.json.items.length > 0);
  assert.ok(r.json.items.every((it) => it.source === 'stremio' && it.name));
  assert.ok(r.json.items.every((it) => !it.background || it.background.startsWith('/img/')), 'pictures go through the service');
  assert.equal(r.json.tmdb.configured, true);
  assert.equal(typeof r.json.jellyfin.connected, 'boolean');
  assert.equal(typeof r.json.plex.connected, 'boolean');
});

test('api: what\'s popular on a streaming app, and what\'s trending for an app with no picture', async () => {
  const netflix = await call('GET', '/api/featured?app=netflix');
  assert.equal(netflix.json.kind, 'popular');
  assert.equal(netflix.json.provider, 'Netflix');
  assert.ok(netflix.json.items.length >= 3);
  assert.ok(netflix.json.items.every((it) => it.background.startsWith('/img/')));
  assert.equal((await call('GET', '/api/featured?app=youtube')).json.kind, 'trending');
  assert.equal((await call('GET', '/api/featured?app=nothing.installed')).json.items.length, 0);
});

test('api: a title\'s picture comes through the service, and is kept for next time', async () => {
  const pic = (await call('GET', '/api/continue')).json.items.find((it) => it.background).background;
  const get = () => new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port, path: pic, headers: { 'X-Homeplus-Token': token } }, (res) => {
      let body = '';
      res.on('data', (c) => { body += c; });
      res.on('end', () => resolve({ status: res.statusCode, body }));
    }).on('error', reject);
  });
  const first = await get();
  assert.equal(first.status, 200);
  assert.match(first.body, /^a picture of show \d$/);
  const kept = path.join(dir, 'config', 'images', pic.slice('/img/'.length));
  await until(() => fs.existsSync(kept), 5000, 'the picture to be kept');
  assert.equal(fs.readFileSync(kept, 'utf8'), first.body);
  assert.deepEqual(await get(), first);
});

test('api: Recents lists the apps you opened that still run, and a closed one stays out', async () => {
  /* the stand-in TV runs Netflix, YouTube, Spotify and Disney+; YouTube, then Netflix, then
   * Spotify were opened (Spotify isn't on this home screen); Disney+ never was */
  const ids = async () => (await call('GET', '/api/running')).json.apps.map((a) => a.id);
  assert.deepEqual(await ids(), ['youtube', 'netflix']);
  assert.equal((await call('POST', '/api/close', { body: { id: 'youtube' } })).status, 200);
  assert.deepEqual(await ids(), ['netflix'], 'webOS still runs YouTube (it may start it again), but you closed it');
});

test('api: the Control Center reads the TV and changes it', async () => {
  const st = await call('GET', '/api/control');
  assert.equal(st.status, 200);
  assert.deepEqual(st.json.network, { kind: 'wifi', name: 'Home Wi-Fi' });
  assert.ok(st.json.inputs.some((i) => i.device === 'PS5'));
  const before = st.json.sleepTimer;
  const after = await call('POST', '/api/control', { body: { action: 'sleepTimer', value: 'next' } });
  assert.equal(after.status, 200);
  assert.notEqual(after.json.sleepTimer, before);
  const bad = await call('POST', '/api/control', { body: { action: 'sleepTimer', value: '45' } });
  assert.ok(bad.status >= 400, 'a value LG doesn\'t have is refused');
  assert.match(bad.json.errorText, /sleep timer/);
  assert.equal((await call('GET', '/api/control')).json.sleepTimer, after.json.sleepTimer, 'and nothing changed');
});

test('api: preferences are kept in the config file', async () => {
  await call('POST', '/api/prefs', { body: { clock24: true } });
  assert.equal((await call('GET', '/api/prefs')).json.clock24, true);
  const cfg = JSON.parse(fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8'));
  assert.equal(cfg.prefs.clock24, true);
});

test('api: sign-ins change things, so they take POST only', async () => {
  assert.equal((await call('GET', '/api/stremio/link')).status, 405);
  assert.equal((await call('GET', '/api/stremio/status')).status, 200);
});

test('api: the Settings key binds through LG Input Hook, and only Home+\'s bind comes and goes', async () => {
  const binds = () => JSON.parse(fs.readFileSync(path.join(dir, 'lginputhook', 'keybinds.json'), 'utf8'));
  assert.deepEqual((await call('GET', '/api/home')).json.settingsKey, { available: true, on: false });
  const on = await call('POST', '/api/home', { body: { settingsKey: true } });
  assert.deepEqual(on.json.settingsKey, { available: true, on: true });
  assert.equal(binds()[139].action, 'exec');
  assert.match(binds()[139].command, /settings-key\.sh$/);
  assert.equal(binds()[773].id, 'com.github.pmiraka.homeplus', 'the Home key\'s bind stays');
  await call('POST', '/api/home', { body: { settingsKey: false } });
  assert.equal(binds()[139], undefined);
  assert.equal(binds()[773].id, 'com.github.pmiraka.homeplus');
});

test('api: the Settings key, with Home+ in front, reaches the launcher as an event', async () => {
  const waiting = call('GET', '/api/events'); /* the launcher's request, answered when something happens */
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.deepEqual((await call('POST', '/api/key', { body: { key: 'settings' } })).json, { opened: 'controlCenter' });
  assert.deepEqual((await waiting).json, { events: ['controlCenter'] });
  assert.ok((await call('POST', '/api/key', { body: { key: 'volumeUp' } })).status >= 400, 'only keys Home+ handles');
});

test('api: an app can be deleted from the TV, but not Home+ itself or what roots the TV', async () => {
  const apps = (await call('GET', '/api/apps')).json.apps;
  assert.equal(apps.find((a) => a.id === 'youtube').removable, true);
  assert.deepEqual((await call('POST', '/api/remove', { body: { id: 'youtube' } })).json, { returnValue: true, deleted: 'youtube' });
  const self = await call('POST', '/api/remove', { body: { id: 'com.github.pmiraka.homeplus' } });
  assert.ok(self.status >= 400);
  assert.match(self.json.errorText, /can't be deleted/);
  assert.ok((await call('POST', '/api/remove', { body: { id: 'not.installed' } })).status >= 400);
});

'use strict';
const test = require('node:test'), assert = require('node:assert/strict'), fs = require('fs'), path = require('path'), os = require('os');
const { startService, until } = require('./helpers'), remove = require('../service/remove');
test('authenticated production updates HTTP route checks, keeps across restart and rejects forged installs', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-updates-api-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const env = { HOMEPLUS_PREVIEW_UPDATES: 'conflict', HOMEPLUS_PREVIEW_KEEP_CONFIG: '1', HOMEPLUS_INITD_DIR: path.join(dir, 'init.d'), HOMEPLUS_DEFAULT_HOME_DIR: path.join(dir, 'home-state'), HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'keys.json') };
  let svc;
  try {
    svc = await startService(dir, env);
    assert.strictEqual((await svc.call('GET', '/api/updates', { auth: false })).status, 403);
    assert.strictEqual((await svc.call('POST', '/api/updates', { auth: false, body: { action: 'install', version: '99.0.0' } })).status, 403);
    let r = await svc.call('POST', '/api/updates', { body: { action: 'check' } }); assert.strictEqual(r.json.phase, 'checking');
    await until(async () => (await svc.call('GET', '/api/updates')).json.phase !== 'checking');
    r = await svc.call('GET', '/api/updates'); assert.strictEqual(r.json.phase, 'conflict'); assert.strictEqual(r.json.canInstallUpstream, true);
    const forged = await svc.call('POST', '/api/updates', { body: { action: 'install', version: r.json.upstreamVersion, confirmation: 'forged', target: 'upstream', url: 'https://evil.example/payload' } });
    assert.strictEqual(forged.json.returnValue, false);
    await svc.call('POST', '/api/updates', { body: { action: 'keep', version: r.json.upstreamVersion } });
    await svc.stop(); svc = await startService(dir, env);
    r = await svc.call('GET', '/api/updates'); assert.strictEqual(r.json.prompt, false); assert.strictEqual(r.json.ignoredVersion, '99.0.0');
    await svc.call('POST', '/api/updates', { body: { action: 'automatic', enabled: false } });
    r = await svc.call('GET', '/api/updates'); assert.strictEqual(r.json.automatic, false);
    const stateDir = path.join(dir, 'config', 'update-state'); fs.mkdirSync(stateDir);
    fs.writeFileSync(path.join(stateDir, 'transaction.json'), JSON.stringify({ schemaVersion: 1, active: true, phase: 'installing' }));
    const before = fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8');
    assert.strictEqual((await svc.call('POST', '/api/home', { body: { startupEnabled: true } })).json.returnValue, false);
    assert.strictEqual((await svc.call('POST', '/api/uninstall', { body: {} })).json.returnValue, false);
    assert.strictEqual(fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8'), before);

  } finally { if (svc) await svc.stop(); remove.rmSync(dir, { recursive: true, force: true }); }
});

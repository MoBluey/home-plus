'use strict';
const { describe, test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { restore } = require('../tools/default-home-recovery');
const { until } = require('./helpers');
const APP = 'com.github.pbalazaar.homeplus';
const STOCK = 'com.webos.app.home';

function model(defaults, options = {}) {
  const state = { defaults: Object.assign({}, defaults), writes: [] };
  const luna = async (uri, p) => {
    if (uri.endsWith('/getSystemSettings')) return options.unreadable ? { returnValue: false } :
      { returnValue: true, settings: { defaultApps: state.defaults } };
    state.writes.push({ uri, params: p });
    if (uri.endsWith('/setDefaultApp')) state.defaults.home = p.appId;
    else if (uri.endsWith('/setSystemSettings')) {
      if (options.refuseClear) return { returnValue: false, errorText: 'Clear refused' };
      state.defaults = p.settings.defaultApps;
    } else throw new Error('Unexpected write ' + uri);
    return { returnValue: true };
  };
  return { state, luna };
}

describe('default Home recovery ownership', () => {
  test('restores an absent Home without resetting the other live categories', async () => {
    const m = model({ home: APP, browser: 'changed.browser', kidsApp: 'kids' });
    await restore(m.luna, { browser: 'old.browser', kidsApp: 'kids' });
    assert.deepEqual(m.state.defaults, { browser: 'changed.browser', kidsApp: 'kids' });
    assert.equal(m.state.writes[0].params.appId, STOCK);
  });
  test('resumes after LG Home was restored but clearing the entry failed', async () => {
    const m = model({ home: STOCK, browser: 'browser' });
    await restore(m.luna, { browser: 'browser' });
    assert.deepEqual(m.state.defaults, { browser: 'browser' });
    assert.equal(m.state.writes.length, 1);
  });
  test('preserves another launcher instead of replacing it', async () => {
    const m = model({ home: 'another.launcher' });
    assert.deepEqual(await restore(m.luna, {}), { preserved: true });
    assert.equal(m.state.writes.length, 0);
  });
  test('does not write while ownership cannot be read', async () => {
    const m = model({ home: APP }, { unreadable: true });
    await assert.rejects(restore(m.luna, {}), /Cannot read/);
    assert.equal(m.state.writes.length, 0);
  });
  test('a refused clear is reported, with LG Home still selected', async () => {
    const m = model({ home: APP }, { refuseClear: true });
    await assert.rejects(restore(m.luna, {}), /Clear refused/);
    assert.equal(m.state.defaults.home, STOCK);
  });
  test('already restored is idempotent', async () => {
    const m = model({ browser: 'browser' });
    assert.deepEqual(await restore(m.luna, { browser: 'browser' }), { restored: true });
    assert.equal(m.state.writes.length, 0);
  });
});

describe('independent recovery process', { skip: process.platform !== 'linux' }, () => {
  async function setup() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-recovery-'));
    const app = path.join(dir, 'app');
    fs.mkdirSync(app);
    fs.writeFileSync(path.join(app, 'appinfo.json'), '{}');
    fs.writeFileSync(path.join(app, 'homeplus'), 'fixture');
    const recovery = path.join(dir, 'recovery.js');
    fs.copyFileSync(path.join(__dirname, '../tools/default-home-recovery.js'), recovery);
    const binary = path.join(dir, "luna send's fixture");
    fs.copyFileSync(path.join(__dirname, 'fixtures/default-home-luna.js'), binary);
    fs.chmodSync(binary, 0o755);
    const modelFile = path.join(dir, 'model.json'), stateFile = path.join(dir, 'state.json');
    const before = { kidsApp: 'kids', MembershipApp: 'membership' };
    fs.writeFileSync(modelFile, JSON.stringify({ defaults: Object.assign({}, before), calls: [], failReads: 0 }));
    fs.writeFileSync(stateFile, JSON.stringify({ version: 1, active: true, deadline: Date.now() + 120000, applicationDir: app, before }));
    const children = [], detached = [];
    function guard(mode, clockJump = false) {
      const args = clockJump ? ['--require', path.join(__dirname, 'fixtures/recovery-clock.js')] : [];
      const child = spawn(process.execPath, args.concat([recovery, mode, stateFile]), {
        env: Object.assign({}, process.env, { HOMEPLUS_RECOVERY_LUNA: binary, HOMEPLUS_MOCK_LUNA_STATE: modelFile }), stdio: ['ignore', 'pipe', 'pipe']
      });
      children.push(child);
      let output = '';
      child.stdout.on('data', (d) => { output += d; });
      child.stderr.on('data', (d) => { output += d; });
      const exited = new Promise((resolve, reject) => {
        child.once('error', reject);
        child.once('exit', (code) => code === 0 ? resolve() : reject(new Error(output)));
      });
      return { child, exited };
    }
    const read = (file) => JSON.parse(fs.readFileSync(file, 'utf8'));
    const update = (file, change) => { const j = read(file); change(j); fs.writeFileSync(file + '.new', JSON.stringify(j)); fs.renameSync(file + '.new', file); };
    return { dir, app, recovery, modelFile, stateFile, before, guard, read, update, children, detached,
      async close() {
        for (const c of children) if (c.exitCode === null && c.signalCode === null) {
          const gone = new Promise((resolve) => c.once('exit', resolve)); c.kill('SIGKILL'); await gone;
        }
        for (const pid of detached) { try { process.kill(pid, 'SIGTERM'); } catch (e) { if (e.code !== 'ESRCH') throw e; } }
        fs.rmSync(dir, { recursive: true, force: true });
      }
    };
  }
  test('survives an abruptly killed controller and restores the absent default', async () => {
    const t = await setup();
    try {
      const g = t.guard('--start');
      await g.exited; // the launcher has exited; recovery must be in a separate session
      await until(() => fs.existsSync(t.stateFile + '.ready'));
      t.detached.push(t.read(t.stateFile + '.ready').pid);
      const controller = spawn(process.execPath, ['-e', 'const fs=require("fs");const p=process.argv[1];const j=JSON.parse(fs.readFileSync(p));j.defaults.home=process.argv[2];fs.writeFileSync(p,JSON.stringify(j));setInterval(()=>{},1000)', t.modelFile, APP]);
      t.children.push(controller);
      await until(() => t.read(t.modelFile).defaults.home === APP);
      t.update(t.stateFile, (s) => { s.deadline = Date.now() + 500; });
      const gone = new Promise((resolve) => controller.once('exit', resolve));
      controller.kill('SIGKILL'); await gone;
      await until(() => t.read(t.stateFile).active === false);
      assert.deepEqual(t.read(t.modelFile).defaults, t.before);
      assert.equal(t.read(t.stateFile).active, false);
    } finally { await t.close(); }
  });
  test('boot recovery survives the old expiry window, a removed app and unavailable settings', async () => {
    const t = await setup();
    try {
      t.update(t.modelFile, (m) => { m.defaults.home = APP; m.failReads = 2; });
      fs.rmSync(t.app, { recursive: true, force: true });
      await t.guard('--boot', true).exited;
      assert.deepEqual(t.read(t.modelFile).defaults, t.before);
      assert.equal(t.read(t.stateFile).active, false);
    } finally { await t.close(); }
  });
  test('a removed launcher triggers recovery before the trial deadline', async () => {
    const t = await setup();
    try {
      t.update(t.modelFile, (m) => { m.defaults.home = APP; });
      const g = t.guard('--watch');
      await until(() => fs.existsSync(t.stateFile + '.ready'));
      fs.unlinkSync(path.join(t.app, 'homeplus'));
      await g.exited;
      assert.deepEqual(t.read(t.modelFile).defaults, t.before);
      assert.ok(t.read(t.stateFile).recoveredAt < t.read(t.stateFile).deadline);
    } finally { await t.close(); }
  });
});

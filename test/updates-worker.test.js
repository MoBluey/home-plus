'use strict';
const test = require('node:test'), assert = require('node:assert/strict'), fs = require('fs'), path = require('path'), os = require('os');
const archive = require('../service/update-archive'), worker = require('../service/update-worker'), recovery = require('../service/default-home-guard');
const { mkdirSync } = require('../service/mkdir'), remove = require('../service/remove');
const APP = 'com.github.pbalazaar.homeplus', SERVICE = APP + '.service';
function files(version) {
  const f = {};
  function add(i, name, value) { f[archive.roots[i] + '/' + name] = { mode: name === 'homeplus' ? 0o755 : 0o644, data: Buffer.from(typeof value === 'object' ? JSON.stringify(value) : value) }; }
  add(0, 'appinfo.json', { id: APP, version, type: 'native', main: 'homeplus' }); add(0, 'homeplus', 'native-' + version);
  add(1, 'package.json', { name: SERVICE, version }); add(1, 'services.json', { id: SERVICE }); add(1, 'service.js', 'service-' + version);
  add(2, 'packageinfo.json', { id: APP, app: APP, version, services: [SERVICE] }); return f;
}
function put(entries, dirs) {
  Object.keys(entries).forEach(name => { const i = archive.roots.findIndex(r => name.indexOf(r + '/') === 0), file = path.join(dirs[i], name.slice(archive.roots[i].length + 1)); mkdirSync(path.dirname(file)); fs.writeFileSync(file, entries[name].data, { mode: entries[name].mode }); });
}
async function fixture(failure) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-worker-')), dirs = ['app', 'service', 'package'].map(n => path.join(dir, n));
  const before = files('0.25.0'), after = files('0.26.0'); put(before, dirs);
  const old = archive.pack(before, '0.25.0'), target = archive.pack(after, '0.26.0');
  const config = { accounts: { fixture: 'private-sentinel' }, layout: ['app1'], inputKey: true, startupEnabled: false, prefs: { theme: 'dark' } };
  const file = path.join(dir, 'transaction.json');
  const s = { schemaVersion: 1, id: 'a'.repeat(24), active: true, phase: 'prepared', previousVersion: '0.25.0', artifact: { version: '0.26.0', size: target.length, sha256: archive.hash(target) }, directories: dirs,
    backup: path.join(dir, 'previous.ipk'), targetFile: path.join(dir, 'target.ipk'), configPath: path.join(dir, 'config.json'), configBackup: path.join(dir, 'previous-config.json'),
    hook: path.join(dir, '01-homeplus-update'), boot: path.join(dir, 'boot.sh'), defaultHome: false, backupHash: archive.hash(old), targetInventory: (await archive.inspect(target, '0.26.0')).inventory };
  fs.writeFileSync(s.backup, old); fs.writeFileSync(s.targetFile, target); recovery.atomic(s.configPath, config); recovery.atomic(s.configBackup, config); recovery.atomic(file, s); fs.writeFileSync(s.boot, 'fixture'); fs.symlinkSync(s.boot, s.hook);
  let targetInstalls = 0, backupInstalls = 0, fallback = false;
  const ops = { foreground: async () => failure === 'left' ? 'other.app' : APP,
    async installTarget() { targetInstalls++; if (failure === 'install') throw new Error('install failed'); put(after, dirs); },
    async installBackup() { backupInstalls++; put(before, dirs); },
    async elevate() { if (failure === 'elevate' && !backupInstalls) throw new Error('elevation failed'); },
    async health(v) { assert.strictEqual(JSON.parse(fs.readFileSync(path.join(dirs[0], 'appinfo.json'))).version, v); if (failure === 'health' && !backupInstalls) throw new Error('health failed'); },
    async enter() { return true; }, async restoreHome() {}, async fallback() { fallback = true; } };
  return { dir, dirs, file, s, before, after, ops, config, counts: () => ({ targetInstalls, backupInstalls, fallback }), close() { remove.rmSync(dir, { recursive: true, force: true }); } };
}
test('archive rejects wrong identity, paths and damaged headers; backup is reinstallable', async () => {
  const f = files('0.25.0'), ipk = archive.pack(f, '0.25.0');
  assert.strictEqual(Object.keys((await archive.inspect(ipk, '0.25.0')).inventory).length, 6);
  await assert.rejects(archive.inspect(ipk, '0.26.0'), /identity/);
  f['etc/shadow'] = { mode: 0o600, data: Buffer.from('bad') };
  await assert.rejects(archive.inspect(archive.pack(f, '0.25.0'), '0.25.0'), /outside/);
  delete f['etc/shadow']; f['../escape'] = { mode: 0o644, data: Buffer.from('bad') }; assert.throws(() => archive.pack(f, '0.25.0'), /Unsafe/);
  const corrupt = Buffer.from(ipk); corrupt[0] = 0; await assert.rejects(archive.inspect(corrupt, '0.25.0'), /header/);
});
test('progress acknowledgement is not terminal success', () => {
  assert.strictEqual(worker.finalReply({ returnValue: true, statusText: 'Installing' }, false), false);
  assert.strictEqual(worker.finalReply({ returnValue: true, finished: true }, false), true);
  assert.strictEqual(worker.finalReply({ statusValue: 30, details: { packageId: APP } }, true), true);
  assert.strictEqual(!!worker.finalReply({ statusValue: 30, details: { packageId: 'other.app' } }, true), false);
  assert.throws(() => worker.finalReply({ returnValue: false, finished: true }, false));
});
test('verified replacement preserves accounts, layout, startup and input', async () => {
  const f = await fixture();
  try { await worker.run(f.file, f.ops); const s = JSON.parse(fs.readFileSync(f.file)); assert.strictEqual(s.phase, 'completed'); assert.strictEqual(s.active, false); assert.strictEqual(fs.existsSync(f.s.hook), false); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config); assert.strictEqual(f.counts().backupInstalls, 0); }
  finally { f.close(); }
});
for (const failure of ['install', 'elevate', 'health']) test(failure + ' failure reinstalls previous package and preserves private state', async () => {
  const f = await fixture(failure);
  try { await worker.run(f.file, f.ops); const s = JSON.parse(fs.readFileSync(f.file)); assert.strictEqual(s.phase, 'failed'); assert.strictEqual(s.active, false); assert.strictEqual(f.counts().backupInstalls, 1); assert.strictEqual(f.counts().fallback, true); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config); assert.strictEqual(JSON.parse(fs.readFileSync(path.join(f.dirs[0], 'appinfo.json'))).version, '0.25.0'); }
  finally { f.close(); }
});
test('leaving Home cancels before replacement', async () => {
  const f = await fixture('left'); try { await worker.run(f.file, f.ops); assert.deepStrictEqual(f.counts(), { targetInstalls: 0, backupInstalls: 0, fallback: false }); assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, false); } finally { f.close(); }
});
test('interrupted replacement recovers without repeating target install', async () => {
  const f = await fixture(); f.s.phase = 'installing'; recovery.atomic(f.file, f.s); put(f.after, f.dirs);
  try { await worker.run(f.file, f.ops); assert.strictEqual(f.counts().targetInstalls, 0); assert.strictEqual(f.counts().backupInstalls, 1); assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'failed'); } finally { f.close(); }
});
test('copied helper completes after replacement kills its launching process', async () => {
  const f = await fixture(), childProcess = require('child_process');
  try {
    for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js'])
      fs.copyFileSync(path.join(__dirname, '..', 'service', name), path.join(f.dir, name));
    const runner = path.join(f.dir, 'fixture-runner.js');
    fs.writeFileSync(runner, "'use strict';\n" +
      "const fs=require('fs'),path=require('path'),a=require('./update-archive'),w=require('./update-worker'),m=require('./mkdir');\n" +
      "const file=process.argv[2],s=w.load(file);\n" +
      "const replace=async p=>{const r=await a.inspect(fs.readFileSync(p),p===s.backup?s.previousVersion:s.artifact.version);Object.keys(r.files).forEach(n=>{if(r.files[n].directory)return;const i=a.roots.findIndex(x=>n.indexOf(x+'/')===0),f=path.join(s.directories[i],n.slice(a.roots[i].length+1));m.mkdirSync(path.dirname(f));fs.writeFileSync(f,r.files[n].data);});};\n" +
      "w.run(file,{foreground:async()=>'" + APP + "',installTarget:async()=>{await replace(s.targetFile);process.kill(Number(process.argv[3]),'SIGKILL');},installBackup:()=>replace(s.backup),elevate:async()=>{},health:async()=>{},enter:async()=>true,restoreHome:async()=>{},fallback:async()=>{}}).catch(e=>{console.error(e.message);process.exitCode=1;});\n");
    const parent = childProcess.spawn(process.execPath, ['-e',
      "const c=require('child_process').spawn(process.execPath,[process.argv[1],process.argv[2],String(process.pid)],{detached:true,stdio:'ignore'});c.unref();setInterval(()=>{},1000);",
      runner, f.file], { stdio: 'ignore' });
    const exited = new Promise(resolve => parent.on('exit', (code, signal) => resolve(signal)));
    assert.strictEqual(await exited, 'SIGKILL');
    const until = require('./helpers').until;
    await until(() => JSON.parse(fs.readFileSync(f.file)).active === false, 5000);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'completed');
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config);
  } finally { f.close(); }
});
test('installer subscription uses a real PTY and waits for finished, not early acknowledgement', { skip: process.platform !== 'linux' }, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-installer-')), previous = process.env.PATH;
  try {
    const tool = path.join(dir, 'luna-send');
    fs.writeFileSync(tool, '#!/bin/sh\nprintf \'{"returnValue":true,"statusText":"Installing"}\\n\'\nsleep 0.1\nprintf \'{"returnValue":true,"finished":true}\\n\'\nsleep 5\n', { mode: 0o700 });
    process.env.PATH = dir + ':' + previous;
    const start = Date.now();
    await worker.installSubscription('luna://fixture/install', {}, false, { timeout: 3000 });
    assert(Date.now() - start >= 80);
    fs.writeFileSync(tool, '#!/bin/sh\nprintf \'{"returnValue":true,"statusText":"Installing"}\\n\'\nsleep 5\n', { mode: 0o700 });
    await assert.rejects(worker.installSubscription('luna://fixture/install', {}, false, { timeout: 100 }), /final completion/);
  } finally { process.env.PATH = previous; remove.rmSync(dir, { recursive: true, force: true }); }
});
test('unknown installer completion never races HBC with a rollback', async () => {
  const f = await fixture();
  f.ops.installTarget = async () => { const e = new Error('completion unknown'); e.installerUncertain = true; throw e; };
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'uncertain');
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, true);
    assert.strictEqual(f.counts().backupInstalls, 0);
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    const s = JSON.parse(fs.readFileSync(f.file)); s.installBoot = 'previous-boot'; recovery.atomic(f.file, s);
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 1);
  } finally { f.close(); }
});
test('insufficient storage and unsupported prerequisites refuse before package replacement', async () => {
  assert.throws(() => worker.checkSpace('Filesystem 1024-blocks Used Available Capacity Mounted on\n/dev/mock 100 90 10 90% /var', 1000, 1000), /free space/);
  assert.throws(() => worker.checkSpace('unreadable', 1000, 1000), /free space/);
  worker.checkSpace('Filesystem 1024-blocks Used Available Capacity Mounted on\n/dev/mock 100000 0 100000 0% /var', 1000, 1000);
  const f = await fixture();
  try {
    await assert.rejects(worker.prepare({ isRoot: () => false, stateDir: f.dir }), /elevated webOS/);
    assert.strictEqual(JSON.parse(fs.readFileSync(path.join(f.dirs[0], 'appinfo.json'))).version, '0.25.0');
  } finally { f.close(); }
});
test('tampered staged target aborts without replacing either package', async () => {
  const f = await fixture(); fs.appendFileSync(f.s.targetFile, 'tamper');
  try { await worker.run(f.file, f.ops); assert.deepStrictEqual(f.counts(), { targetInstalls: 0, backupInstalls: 0, fallback: false }); assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'error'); }
  finally { f.close(); }
});
test('a second independent helper refuses the live worker lock', { skip: process.platform !== 'linux' }, async () => {
  const f = await fixture(), cp = require('child_process'), until = require('./helpers').until;
  let child;
  try {
    for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js'])
      fs.copyFileSync(path.join(__dirname, '..', 'service', name), path.join(f.dir, name));
    const luna = path.join(f.dir, 'fixture-luna');
    fs.writeFileSync(luna, '#!/bin/sh\nsleep 2\n', { mode: 0o700 });
    const opts = { env: Object.assign({}, process.env, { HOMEPLUS_RECOVERY_LUNA: luna }), stdio: ['ignore', 'ignore', 'pipe'] };
    const args = [path.join(f.dir, 'update-worker.js'), '--run', f.file];
    child = cp.spawn(process.execPath, args, opts);
    await until(() => fs.existsSync(f.file + '.worker'), 3000);
    const second = cp.spawn(process.execPath, args, opts); let output = '';
    second.stderr.on('data', data => { output += data; });
    assert.strictEqual(await new Promise(resolve => second.on('exit', resolve)), 1);
    assert.match(output, /already running/);
    assert.strictEqual(JSON.parse(fs.readFileSync(path.join(f.dirs[0], 'appinfo.json'))).version, '0.25.0');
  } finally {
    if (child && child.exitCode === null) { const exited = new Promise(resolve => child.on('exit', resolve)); child.kill(); await exited; }
    f.close();
  }
});

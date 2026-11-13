'use strict';
const test = require('node:test'), assert = require('node:assert/strict'), fs = require('fs'), path = require('path'), os = require('os');
const archive = require('../service/update-archive'), worker = require('../service/update-worker'), recovery = require('../service/default-home-guard');
const { mkdirSync } = require('../service/mkdir'), remove = require('../service/remove');
const APP = 'com.github.pbalazaar.homeplus', SERVICE = APP + '.service';
async function assertKilled(child, file) {
  const result = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => resolve({ code, signal }));
  });
  assert.deepStrictEqual(result, process.platform === 'win32' ? { code: 1, signal: null } : { code: null, signal: 'SIGKILL' });
  assert.strictEqual(fs.existsSync(file + '.graceful-exit'), false);
}
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
  const f = await fixture(); f.s.phase = 'installing'; f.s.installBoot = 'previous-boot'; recovery.atomic(f.file, f.s); put(f.after, f.dirs);
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
      "w.run(file,{foreground:async()=>'" + APP + "',installTarget:async()=>{await replace(s.targetFile);process.kill(Number(process.argv[3]),'SIGKILL');},installBackup:()=>replace(s.backup),elevate:async()=>{},health:async()=>{},enter:async()=>true,restoreHome:async()=>{},fallback:async()=>{}}).then(()=>console.log('helper-completed'),e=>{console.error(e.message);process.exitCode=1;});\n");
    const parent = childProcess.spawn(process.execPath, ['-e',
      "process.on('exit',()=>require('fs').writeFileSync(process.argv[2]+'.graceful-exit','unexpected'));" +
      "const c=require('child_process').spawn(process.execPath,[process.argv[1],process.argv[2],String(process.pid)],{detached:true,stdio:['ignore',1,2]});c.unref();setInterval(()=>{},1000);",
      runner, f.file], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '', errors = '';
    parent.stdout.on('data', data => { output += data; });
    parent.stderr.on('data', data => { errors += data; });
    // The detached helper inherits these pipes. 'close' waits for it to finish too,
    // including worker lock cleanup after transaction.active becomes false.
    await assertKilled(parent, f.file);
    assert.strictEqual(errors, '');
    assert.strictEqual(output.trim(), 'helper-completed');
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, false);
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

// Review regressions: later user state must outlive an update transaction's snapshot.
function laterConfig(f) {
  return Object.assign({}, f.config, { accounts: { fixture: 'refreshed-token' }, layout: ['new-app'], inputKey: false, startupEnabled: true,
    prefs: { theme: 'light', sounds: false }, updates: { automatic: false, ignoredVersion: '0.26.0' }, addedAfterSnapshot: true });
}
const fixtureBoot = () => fs.existsSync('/proc/sys/kernel/random/boot_id') ? fs.readFileSync('/proc/sys/kernel/random/boot_id', 'utf8').trim() : '';
for (const failure of ['left', 'tampered']) test('pre-replacement ' + failure + ' preserves post-snapshot user settings', async () => {
  const f = await fixture(failure), current = laterConfig(f);
  recovery.atomic(f.s.configPath, current);
  if (failure === 'tampered') fs.appendFileSync(f.s.targetFile, 'tampered');
  try { await worker.run(f.file, f.ops); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current); assert.strictEqual(f.counts().backupInstalls, 0); }
  finally { f.close(); }
});
test('legitimate config writes during installation do not cause rollback', async () => {
  const f = await fixture(), install = f.ops.installTarget, current = laterConfig(f);
  f.ops.installTarget = async () => { await install(); recovery.atomic(f.s.configPath, current); };
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'completed');
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current);
  } finally { f.close(); }
});
test('delayed recovery preserves newer account, layout, preference and update fields', async () => {
  const f = await fixture(), current = laterConfig(f);
  f.s.phase = 'uncertain'; f.s.installBoot = 'previous-boot'; recovery.atomic(f.file, f.s); recovery.atomic(f.s.configPath, current); put(f.after, f.dirs);
  const install = f.ops.installBackup;
  f.ops.installBackup = async () => { await install(); current.duringRecovery = 'retained'; recovery.atomic(f.s.configPath, current); };
  try { await worker.run(f.file, f.ops); assert.strictEqual(f.counts().backupInstalls, 1); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current); }
  finally { f.close(); }
});
test('uncertain recovery install records the current boot before invoking the installer', async () => {
  const f = await fixture(); f.s.phase = 'uncertain'; f.s.installBoot = 'previous-boot'; recovery.atomic(f.file, f.s);
  let backupCalls = 0, observed;
  f.ops.installBackup = async () => {
    backupCalls++; observed = JSON.parse(fs.readFileSync(f.file));
    const e = new Error('backup completion unknown'); e.installerUncertain = true; throw e;
  };
  try {
    await assert.rejects(worker.run(f.file, f.ops), /could not complete/);
    assert.strictEqual(observed.installBoot, fixtureBoot());
    assert.strictEqual(observed.installerPending, true);
    await worker.run(f.file, f.ops);
    assert.strictEqual(backupCalls, 1);
  } finally { f.close(); }
});
for (const kind of ['target', 'backup']) test('helper SIGKILL during ' + kind + ' install cannot start a same-boot rollback', async () => {
  const f = await fixture(), cp = require('child_process'), runner = path.join(f.dir, 'interrupted-helper.js');
  for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js'])
    fs.copyFileSync(path.join(__dirname, '..', 'service', name), path.join(f.dir, name));
  if (kind === 'backup') { f.s.phase = 'uncertain'; f.s.installBoot = 'previous-boot'; recovery.atomic(f.file, f.s); }
  fs.writeFileSync(runner, "process.on('exit',()=>require('fs').writeFileSync(process.argv[2]+'.graceful-exit','unexpected'));" +
    "const w=require('./update-worker');w.run(process.argv[2],{foreground:async()=>'" + APP +
    "',fallback:async()=>{}," + (kind === 'target' ? 'installTarget' : 'installBackup') + ":async()=>process.kill(process.pid,'SIGKILL')}).catch(()=>process.exit(2));");
  try {
    const child = cp.spawn(process.execPath, [runner, f.file], { stdio: 'ignore' });
    await assertKilled(child, f.file);
    const interrupted = JSON.parse(fs.readFileSync(f.file));
    assert.strictEqual(interrupted.phase, kind === 'target' ? 'installing' : 'recovering');
    assert.strictEqual(interrupted.installerPending, true); assert.strictEqual(interrupted.installBoot, fixtureBoot());
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'uncertain');
    const s = JSON.parse(fs.readFileSync(f.file)); s.installBoot = 'previous-boot'; recovery.atomic(f.file, s);
    await worker.run(f.file, f.ops); assert.strictEqual(f.counts().backupInstalls, 1);
  } finally { f.close(); }
});
test('cancellation restores only updater-owned Home fields in usable current config', async () => {
  const f = await fixture('left');
  Object.assign(f.config, { homeMode: 'redirect', homeOverride: true, defaultHomeEnabled: false });
  recovery.atomic(f.s.configBackup, f.config);
  const current = Object.assign(laterConfig(f), { homeOverride: false, defaultHomeEnabled: true });
  recovery.atomic(f.s.configPath, current);
  try {
    await worker.run(f.file, f.ops);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), Object.assign({}, current, { homeOverride: true }));
  } finally { f.close(); }
});
test('complete config snapshot is used only when current config is absent or unusable', async () => {
  for (const invalid of [undefined, '{', 'null', '[]']) {
    const f = await fixture('left');
    if (invalid === undefined) fs.unlinkSync(f.s.configPath); else fs.writeFileSync(f.s.configPath, invalid);
    try { await worker.run(f.file, f.ops); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config); }
    finally { f.close(); }
  }
});

test('leaving Home during archive validation cancels at the replacement boundary', async () => {
  const f = await fixture(), inspect = archive.inspect;
  let front = APP;
  f.ops.foreground = async () => front;
  archive.inspect = async function () { const result = await inspect.apply(this, arguments); front = 'user.selected.app'; return result; };
  try {
    await worker.run(f.file, f.ops);
    assert.deepStrictEqual(f.counts(), { targetInstalls: 0, backupInstalls: 0, fallback: false });
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, false);
    assert.match(JSON.parse(fs.readFileSync(f.file)).message, /cancelled/);
  } finally { archive.inspect = inspect; f.close(); }
});
test('an accepted native launch that immediately exits is unhealthy, not deferred', async () => {
  const luna = recovery.luna; let launches = 0;
  recovery.luna = async uri => {
    if (uri.endsWith('/launch')) { launches++; return { returnValue: true }; }
    return { returnValue: true, appId: launches ? 'com.webos.app.home' : APP };
  };
  try { await assert.rejects(worker.production({}).enter(), /did not stay open/); assert.strictEqual(launches, 1); }
  finally { recovery.luna = luna; }
});
test('native entry defers to an already selected foreground app without launching', async () => {
  const luna = recovery.luna; let launches = 0;
  recovery.luna = async uri => {
    if (uri.endsWith('/launch')) launches++;
    return { returnValue: true, appId: 'user.selected.app' };
  };
  try { assert.strictEqual(await worker.production({}).enter(), false); assert.strictEqual(launches, 0); }
  finally { recovery.luna = luna; }
});
for (const stale of [false, true]) for (const point of ['writing', 'published']) test('SIGKILL while ' + point + (stale ? ' a stale lock successor' : ' the worker lock') + ' permits a safe restart', async () => {
  const f = await fixture(), cp = require('child_process'), runner = path.join(f.dir, 'lock-interruption.js');
  if (stale) recovery.atomic(f.file + '.worker', { pid: 2147483647, boot: fixtureBoot(), token: 'dead-owner' });
  fs.writeFileSync(runner, "const fs=require('fs'),file=process.argv[2],point=process.argv[3],write=fs.writeFileSync,link=fs.linkSync;" +
    "process.on('exit',()=>write(file+'.graceful-exit','unexpected'));" +
    "fs.writeFileSync=function(p){if(typeof p==='number'||p===file+'.worker'){" +
    "if(point==='writing'){const fd=typeof p==='number'?p:fs.openSync(p,'wx',384);fs.writeSync(fd,'{');process.kill(process.pid,'SIGKILL');}" +
    "const result=write.apply(this,arguments);if(p===file+'.worker')process.kill(process.pid,'SIGKILL');return result;}return write.apply(this,arguments);};" +
    "fs.linkSync=function(a,b){const result=link.apply(this,arguments);if(b===file+'.worker'||b.indexOf(file+'.worker.after-')===0)process.kill(process.pid,'SIGKILL');return result;};" +
    "require(" + JSON.stringify(require.resolve('../service/update-worker')) + ").run(file,{}).catch(()=>process.exit(2));");
  try {
    const child = cp.spawn(process.execPath, [runner, f.file, point], { stdio: 'ignore' });
    await assertKilled(child, f.file);
    if (fs.existsSync(f.file + '.worker')) assert.strictEqual(typeof JSON.parse(fs.readFileSync(f.file + '.worker')).pid, 'number');
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().targetInstalls, 1);
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'completed');
  } finally { f.close(); }
});

test('healthy service with a crashed native entry reinstalls the previous package', async () => {
  const f = await fixture(), luna = recovery.luna, healthy = [], health = f.ops.health;
  let launched = false;
  recovery.luna = async uri => {
    if (uri.endsWith('/launch')) { launched = true; return { returnValue: true }; }
    return { returnValue: true, appId: launched ? 'com.webos.app.home' : APP };
  };
  const enter = worker.production(f.s).enter;
  f.ops.health = async version => { await health(version); healthy.push(version); };
  f.ops.enter = async () => f.counts().backupInstalls ? true : enter();
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().targetInstalls, 1);
    assert.strictEqual(f.counts().backupInstalls, 1);
    assert(healthy.includes('0.26.0')); assert(healthy.includes('0.25.0'));
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'failed');
  } finally { recovery.luna = luna; f.close(); }
});
test('concurrent stale-lock takeover grants ownership to only one live helper', async () => {
  const f = await fixture(), cp = require('child_process'), until = require('./helpers').until, children = [];
  f.s.phase = 'uncertain'; f.s.installBoot = fixtureBoot(); recovery.atomic(f.file, f.s);
  recovery.atomic(f.file + '.worker', { pid: 2147483647, boot: fixtureBoot(), token: 'dead-owner' });
  const runner = path.join(f.dir, 'lock-contender.js');
  fs.writeFileSync(runner, "const fs=require('fs'),file=process.argv[2],id=process.argv[3];fs.writeFileSync(file+'.ready-'+id,'ready');" +
    "const timer=setInterval(()=>{if(!fs.existsSync(file+'.go'))return;clearInterval(timer);require(" +
    JSON.stringify(require.resolve('../service/update-worker')) + ").run(file,{fallback:async()=>{" +
    "fs.writeFileSync(file+'.inside',String(process.pid),{flag:'wx'});await new Promise(r=>setTimeout(r,500));fs.unlinkSync(file+'.inside');" +
    "}}).then(()=>process.exit(0),e=>process.exit(/already running/.test(e.message)?3:4));},10);");
  try {
    const exits = [];
    for (let i = 0; i < 4; i++) {
      const child = cp.spawn(process.execPath, [runner, f.file, String(i)], { stdio: 'ignore' }); children.push(child);
      exits.push(new Promise(resolve => child.on('exit', resolve)));
    }
    await until(() => children.every((child, i) => fs.existsSync(f.file + '.ready-' + i)), 5000);
    fs.writeFileSync(f.file + '.go', 'go');
    const codes = await Promise.all(exits);
    assert.strictEqual(codes.filter(code => code === 0).length, 1);
    assert.strictEqual(codes.filter(code => code === 3).length, 3);
    assert.strictEqual(fs.existsSync(f.file + '.worker'), false);
  } finally {
    for (const child of children) if (child.exitCode === null) child.kill();
    f.close();
  }
});
test('an unverifiable pre-existing worker lock is never removed blindly', async () => {
  const f = await fixture();
  fs.writeFileSync(f.file + '.worker', '{');
  try {
    await assert.rejects(worker.run(f.file, f.ops), SyntaxError);
    assert.strictEqual(fs.readFileSync(f.file + '.worker', 'utf8'), '{');
    assert.strictEqual(f.counts().targetInstalls, 0);
    assert.strictEqual(f.counts().backupInstalls, 0);
  } finally { f.close(); }
});

test('application PulseAudio cookie is private runtime data, including legacy backup inventories', async () => {
  const f = await fixture(), relative = '.config/pulse/cookie', key = archive.roots[0] + '/' + relative;
  const cookie = path.join(f.dirs[0], relative), fake = Buffer.alloc(256, 7);
  mkdirSync(path.dirname(cookie)); fs.writeFileSync(cookie, fake); fs.chmodSync(cookie, 0o777);
  const legacy = await archive.inspect(archive.pack(Object.assign({}, f.before, { [key]: { mode: 0o777, data: fake } }), '0.25.0'), '0.25.0');
  const read = fs.readFileSync;
  try {
    fs.readFileSync = function (file) { if (file === cookie) throw new Error('Runtime cookie contents must not be read'); return read.apply(this, arguments); };
    const snapshot = archive.snapshot(f.dirs);
    assert.strictEqual(snapshot[key], undefined);
    assert.deepStrictEqual(Object.keys(snapshot).sort(), Object.keys(f.before).sort());
    const clean = await archive.inspect(archive.pack(snapshot, '0.25.0'), '0.25.0');
    assert.strictEqual(clean.files[key], undefined);
    archive.verifyInstalled(clean.inventory, f.dirs);
    fs.writeFileSync(cookie, Buffer.alloc(256, 9));
    archive.verifyInstalled(legacy.inventory, f.dirs);
    fs.unlinkSync(cookie);
    archive.verifyInstalled(legacy.inventory, f.dirs);
    fs.writeFileSync(path.join(f.dirs[0], 'homeplus'), 'changed payload');
    assert.throws(() => archive.verifyInstalled(legacy.inventory, f.dirs), /verification/);
  } finally { fs.readFileSync = read; f.close(); }
});
test('cookie exception never hides neighbouring code, service paths or non-regular impostors', async () => {
  const f = await fixture(), expected = (await archive.inspect(archive.pack(f.before, '0.25.0'), '0.25.0')).inventory;
  const cookie = path.join(f.dirs[0], '.config/pulse/cookie');
  try {
    for (const file of [path.join(f.dirs[0], '.config/pulse/extra.js'), path.join(f.dirs[0], '.config/extra.js'), path.join(f.dirs[1], '.config/pulse/cookie')]) {
      mkdirSync(path.dirname(file)); fs.writeFileSync(file, 'unexpected code');
      assert.throws(() => archive.verifyInstalled(expected, f.dirs), /Unexpected/);
      fs.unlinkSync(file);
    }
    fs.symlinkSync(path.join(f.dirs[0], 'homeplus'), cookie);
    assert.throws(() => archive.snapshot(f.dirs), /Unsupported/);
    assert.throws(() => archive.verifyInstalled(expected, f.dirs), /Unsupported/);
    fs.unlinkSync(cookie); fs.mkdirSync(cookie);
    assert.throws(() => archive.snapshot(f.dirs), /Unsupported/);
    fs.rmdirSync(cookie);
    if (process.platform === 'linux') {
      require('child_process').execFileSync('mkfifo', [cookie]);
      assert.throws(() => archive.snapshot(f.dirs), /Unsupported/);
      fs.unlinkSync(cookie);
    }
  } finally { f.close(); }
});

/* Copied outside the package before install. Never depends on the launching service surviving. */
'use strict';
const fs = require('fs');
const path = require('path');
const { execFile, spawn } = require('child_process');
const archive = require('./update-archive');
const release = require('./update-release');
const recovery = require('./default-home-guard');
const { requestJSON } = require('./net');
const { mkdirSync } = require('./mkdir');
const APP = release.APP, SERVICE = APP + '.service';
const HBC = 'luna://org.webosbrew.hbchannel.service/';
const MANAGER = 'luna://com.webos.applicationManager/';
const ELEVATE = '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service';
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const quote = s => "'" + s.replace(/'/g, "'\\''") + "'";
const read = file => JSON.parse(fs.readFileSync(file, 'utf8'));
function privateDir(dir) {
  mkdirSync(dir, 0o700);
  const st = fs.lstatSync(dir);
  if (!st.isDirectory() || st.isSymbolicLink() || (process.getuid && st.uid !== process.getuid())) throw new Error('Unsafe recovery directory');
  fs.chmodSync(dir, 0o700);
}
function runFile(command, args) {
  return new Promise((resolve, reject) => execFile(command, args, { timeout: 15000, maxBuffer: 1024 * 1024 }, (err, stdout) => err ? reject(new Error('Required update tool failed: ' + path.basename(command))) : resolve(String(stdout))));
}
function finalReply(r, local) {
  if (!r || typeof r !== 'object' || Array.isArray(r)) return false;
  if (r.returnValue === false || (r.details && r.details.errorCode !== undefined)) throw new Error('Package installer refused the update');
  return local ? r.statusValue === 30 && r.details && r.details.packageId === APP : r.finished === true;
}
function installSubscription(uri, params, local, options) {
  return new Promise((resolve, reject) => {
    const command = 'exec luna-send -i ' + quote(uri) + ' ' + quote(JSON.stringify(params));
    const child = spawn('script', ['-q', '-c', command, '/dev/null'], { detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let partial = '', bytes = 0, done = false;
    const uncertain = message => { const error = new Error(message); error.installerUncertain = true; return error; };
    const finish = error => {
      if (done) return; done = true; clearTimeout(timer);
      try { process.kill(-child.pid, 'SIGTERM'); } catch (e) { child.kill(); }
      if (error) reject(error); else resolve();
    };
    const timer = setTimeout(() => finish(uncertain('Installer did not report final completion')), (options && options.timeout) || 180000);
    child.stdout.on('data', data => {
      bytes += data.length; if (bytes > 1024 * 1024) return finish(uncertain('Installer output exceeds limit'));
      partial += data.toString();
      let pos;
      while ((pos = partial.indexOf('\n')) >= 0) {
        const line = partial.slice(0, pos).trim(); partial = partial.slice(pos + 1);
        let r; try { r = JSON.parse(line); } catch (e) { continue; }
        try { if (finalReply(r, local)) finish(); } catch (e) { finish(e); }
      }
    });
    child.stderr.on('data', () => {});
    child.on('error', () => finish(new Error('Installer terminal could not start')));
    child.on('exit', () => { if (!done) finish(uncertain('Installer exited without final completion')); });
  });
}
function production(state) {
  const luna = recovery.luna;
  const accepted = (uri, params) => recovery.accepted(luna, uri, params);
  const front = async () => { const r = await accepted(MANAGER + 'getForegroundAppInfo', {}); return r.appId || (r.foregroundAppInfo && r.foregroundAppInfo[0] && r.foregroundAppInfo[0].appId) || ''; };
  return {
    foreground: front,
    installTarget: () => installSubscription(HBC + 'install', { id: APP, ipkUrl: state.artifact.url, ipkHash: state.artifact.sha256, subscribe: true }, false),
    installBackup: () => installSubscription('luna://com.webos.appInstallService/dev/install', { id: APP, ipkUrl: state.backup, subscribe: true }, true),
    async elevate() { await runFile(ELEVATE, [SERVICE]); },
    async health(version) {
      const end = Date.now() + 20000;
      while (Date.now() < end) {
        try {
          await accepted('luna://' + SERVICE + '/ping', {});
          const r = await requestJSON('GET', 'http://127.0.0.1:47823/api/status', { timeout: 2000 });
          if (r.status === 200 && r.json && r.json.id === SERVICE && r.json.version === version) {
            if (r.json.elevated === true) return;
            await requestJSON('POST', 'http://127.0.0.1:47823/api/restart', { timeout: 2000 });
          }
        } catch (e) { /* the package manager may still be releasing the old service */ }
        await wait(500);
      }
      throw new Error('Updated service health check failed');
    },
    async enter() {
      const id = await front();
      // A foreground app selected during installation owns the screen, even LG Home.
      if (id && id !== APP) return false;
      await accepted(MANAGER + 'launch', { id: APP });
      const end = Date.now() + 15000; let stable = 0;
      while (Date.now() < end) { if (await front() !== APP) throw new Error('Home+ did not stay open'); if (++stable >= 10) return true; await wait(500); }
      throw new Error('Home+ did not stay open');
    },
    async restoreHome() {
      const saved = read(state.configBackup);
      restoreHomeConfig(state);
      const defaults = state.defaultHome ? await recovery.readDefaults(luna) : {};
      if (state.defaultHome && defaults.home !== APP) {
        if (!state.homeStateFile || !fs.existsSync(state.homeStateFile)) throw new Error('Original Home recovery state is unavailable');
        const home = recovery.load(state.homeStateFile);
        if (defaults.home && defaults.home !== home.stockHome && defaults.home !== home.before.home) return;
        const cfg = currentConfig(state); // defaults lookup awaited the bus; keep any intervening user writes
        if (cfg.defaultHomeEnabled === true) { cfg.defaultHomeEnabled = false; cfg.homeOverride = false; recovery.atomic(state.configPath, cfg); }
        if (await front() !== APP) throw new Error('Home restoration waits for Home+ to be in front');
      }
      const token = fs.readFileSync(path.join(state.directories[0], 'api-token'), 'utf8').trim();
      const r = await requestJSON('POST', 'http://127.0.0.1:47823/api/home', { timeout: 45000, headers: { 'X-Homeplus-Token': token }, body: Object.assign({ updateTransaction: state.id }, state.defaultHome ? { defaultHome: true } : { mode: saved.homeOverride === false ? 'off' : saved.homeMode || 'watch' }) });
      if (r.status !== 200 || !r.json || r.json.returnValue === false || (state.defaultHome && (await recovery.readDefaults(luna)).home !== APP)) throw new Error('Default Home compatibility restoration failed');
    },
    async fallback() {
      currentConfig(state); // the Home guard also needs usable config, without replacing newer valid fields
      // Keep the existing independent Home guard effective; explicitly use its saved restoration if present.
      if (!state.homeStateFile || !fs.existsSync(state.homeStateFile)) return;
      const s = recovery.load(state.homeStateFile);
      await recovery.restore(luna, s.before, s.stockHome);
      recovery.restoreConfig(s); s.active = false; recovery.atomic(state.homeStateFile, s);
    }
  };
}
// Other service routes may keep saving accounts and preferences throughout recovery.
function currentConfig(state) {
  let cfg;
  try { cfg = read(state.configPath); }
  catch (e) { if (e.code !== 'ENOENT' && !(e instanceof SyntaxError)) throw e; }
  if (cfg && typeof cfg === 'object' && !Array.isArray(cfg)) return cfg;
  cfg = read(state.configBackup);
  if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) throw new Error('Recovery config snapshot is unusable');
  recovery.atomic(state.configPath, cfg);
  return cfg;
}
function restoreHomeConfig(state, suspendDefault) {
  const cfg = currentConfig(state), saved = read(state.configBackup);
  for (const key of ['homeMode', 'homeOverride']) {
    if (Object.prototype.hasOwnProperty.call(saved, key)) cfg[key] = saved[key]; else delete cfg[key];
  }
  if (suspendDefault && state.defaultHome) { cfg.defaultHomeEnabled = false; cfg.homeOverride = false; }
  recovery.atomic(state.configPath, cfg);
  return cfg;
}
function ownHook(s) {
  try { return fs.readlinkSync(s.hook) === s.boot; } catch (e) { if (e.code === 'ENOENT') return false; throw e; }
}
function finishHook(s) {
  if (ownHook(s)) { fs.unlinkSync(s.hook); recovery.syncDir(path.dirname(s.hook)); }
}
function load(file) {
  const s = read(file);
  if (s.schemaVersion !== 1 || typeof s.active !== 'boolean' || !/^[0-9a-f]{24}$/.test(s.id) || !Array.isArray(s.directories) || s.directories.length !== 3 ||
      ![s.backup, s.targetFile, s.configPath, s.configBackup, s.hook, s.boot].concat(s.directories).every(p => typeof p === 'string' && path.isAbsolute(p)) || !s.artifact) throw new Error('Invalid update recovery record');
  release.version(s.previousVersion); release.version(s.artifact.version);
  return s;
}
const generationId = value => typeof value === 'string' && /^runtime-v2:[0-9a-f]{64}$/.test(value);
function bootId(runtime, mountInfo) {
  runtime = runtime || '/dev'; // fixed volatile parent; never fall back to another mount
  if (mountInfo === undefined) mountInfo = fs.readFileSync('/proc/self/mountinfo', 'utf8');
  const root = fs.lstatSync(runtime);
  const mounted = mountInfo.split('\n').some(line => {
    const parts = line.split(' - '), fields = parts[0].split(' ');
    return parts.length === 2 && fields[3] === '/' && fields[4] === runtime && ['tmpfs', 'devtmpfs'].includes(parts[1].split(' ')[0]);
  });
  if (!mounted || !root.isDirectory() || root.isSymbolicLink() || root.uid !== 0 || (root.mode & 0o022) || fs.realpathSync(runtime) !== runtime)
    throw new Error('Cannot verify volatile runtime mount for update recovery');
  const dir = path.join(runtime, 'homeplus-update-boot'), file = path.join(dir, 'generation.json');
  let created = false;
  try { fs.mkdirSync(dir, 0o700); created = true; } catch (e) { if (e.code !== 'EEXIST') throw e; }
  const st = fs.lstatSync(dir);
  if (!st.isDirectory() || st.isSymbolicLink() || st.uid !== 0 || (st.mode & 0o777) !== 0o700)
    throw new Error('Unsafe update boot generation directory');
  // Only the exclusive mkdir winner may initialise. A killed creator leaves an
  // incomplete directory that must fail closed until the volatile mount resets.
  // No updater path removes this directory or replaces an existing generation.
  if (created) recovery.atomic(file, { schemaVersion: 1, generation: require('crypto').randomBytes(32).toString('hex') });
  const marker = fs.lstatSync(file);
  if (!marker.isFile() || marker.uid !== 0 || (marker.mode & 0o777) !== 0o600 || marker.size > 256)
    throw new Error('Unsafe update boot generation record');
  const record = read(file), id = 'runtime-v2:' + record.generation;
  if (record.schemaVersion !== 1 || !generationId(id)) throw new Error('Invalid update boot generation record');
  return id;
}
function processStart(pid) {
  try { const stat = fs.readFileSync('/proc/' + pid + '/stat', 'utf8'); return stat.slice(stat.lastIndexOf(')') + 2).split(' ')[19]; }
  catch (e) { return ''; }
}
function workerAlive(owner, boot) {
  // A legacy raw UUID or runtime-v1 token cannot prove a different boot during migration.
  if (generationId(owner.boot) && owner.boot !== boot) return false;
  try { process.kill(owner.pid, 0); }
  catch (e) { return e.code !== 'ESRCH'; }
  const start = processStart(owner.pid);
  return !owner.start || !start || start === owner.start;
}
function lockRecord(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const owner = JSON.parse(fs.readFileSync(fd, 'utf8'));
    if (!owner || !Number.isInteger(owner.pid) || owner.pid <= 0 || typeof owner.boot !== 'string') throw new Error('Invalid worker owner');
    return { owner, stat: fs.fstatSync(fd) };
  } finally { fs.closeSync(fd); }
}
function sameLock(file, stat) {
  try { const now = fs.statSync(file); return now.dev === stat.dev && now.ino === stat.ino; }
  catch (e) { if (e.code === 'ENOENT') return false; throw e; }
}
const successor = (lock, owner) => lock + '.after-' + archive.hash(Buffer.from(JSON.stringify(owner)));
function acquireWorker(file, boot) {
  const lock = file + '.worker', candidate = lock + '.owner-' + require('crypto').randomBytes(12).toString('hex');
  // Write and flush privately, then publish exclusively with link(2). A torn private file is never a lock.
  recovery.atomic(candidate, { pid: process.pid, boot: boot, start: processStart(process.pid), token: path.basename(candidate) });
  let at = lock, anchor = null, chain = [];
  try {
    for (;;) {
      let linked = false;
      try { fs.linkSync(candidate, at); linked = true; }
      catch (e) { if (e.code !== 'EEXIST') throw e; }
      if (linked) {
        recovery.syncDir(path.dirname(lock));
        if (!anchor) anchor = fs.statSync(at);
        if (!sameLock(lock, anchor)) { fs.unlinkSync(at); at = lock; anchor = null; chain = []; continue; }
        chain.push(at);
        return () => {
          if (!sameLock(lock, anchor)) return;
          fs.unlinkSync(lock);
          for (const entry of chain) if (entry !== lock) fs.unlinkSync(entry);
          recovery.syncDir(path.dirname(lock));
        };
      }
      let previous;
      try { previous = lockRecord(at); }
      catch (e) { if (e.code !== 'ENOENT') throw e; at = lock; anchor = null; chain = []; continue; }
      if (!anchor) anchor = previous.stat;
      if (!sameLock(lock, anchor)) { at = lock; anchor = null; chain = []; continue; }
      if (workerAlive(previous.owner, boot)) throw new Error('Update recovery is already running');
      // Never unlink a stale lock: concurrent reapers could otherwise unlink the new live owner.
      // Exactly one successor wins each immutable owner's exclusive link; interrupted takeover is recoverable.
      chain.push(at); at = successor(lock, previous.owner);
    }
  } finally { fs.unlinkSync(candidate); }
}
async function run(file, injected) {
  let s = load(file);
  if (!s.active) { finishHook(s); return; }
  const boot = injected && injected.bootId ? injected.bootId() : bootId();
  if (!generationId(boot)) throw new Error('Invalid current boot generation');
  const releaseWorker = acquireWorker(file, boot);
  try { s = load(file); } catch (e) { releaseWorker(); throw e; }
  if (!s.active) { releaseWorker(); finishHook(s); return; }
  const ops = injected || production(s);
  let replacing = s.phase !== 'prepared';
  const save = (phase, message) => { s.phase = phase; s.message = message; recovery.atomic(file, s); };
  async function install(kind) {
    s.installBoot = boot; s.installerPending = true; s.installer = kind;
    save(kind === 'target' ? 'installing' : 'recovering', kind === 'target' ?
      'Installing Home+. Recovery continues independently.' : 'Reinstalling the verified previous package.');
    try { await (kind === 'target' ? ops.installTarget() : ops.installBackup()); }
    catch (e) {
      if (!e.installerUncertain) {
        s.installerPending = false;
        save('recovering', 'Installer reported failure. Preparing recovery.');
      }
      throw e;
    }
    s.installerPending = false;
    save('verifying', 'Checking the installed package and service');
  }
  async function complete() {
    const previous = s.recovered ? await archive.inspect(fs.readFileSync(s.backup), s.previousVersion) : null;
    archive.verifyInstalled(previous ? previous.inventory : s.targetInventory, s.directories);
    await ops.health(s.recovered ? s.previousVersion : s.artifact.version);
    save('awaiting-entry', 'Open Home+ to finish checking the installation.');
    while (!await ops.enter()) { await wait(2000); }
    save('restoring-home', 'Checking the requested Home settings');
    await ops.restoreHome();
    s.active = false;
    save(s.recovered ? 'failed' : 'completed', s.recovered ? 'Update failed. The previous Home+ build was restored.' : 'Home+ updated successfully');
    finishHook(s);
  }
  try {
    const pending = s.installerPending === true || ['installing', 'uncertain'].includes(s.phase);
    if (pending && (!generationId(s.installBoot) || s.installBoot === boot)) {
      // A killed helper cannot record uncertainty. Both installers may still be running.
      // Legacy/missing identity is baselined without authorising a rollback.
      // Only a later verified runtime generation can clear this quarantine.
      if (!generationId(s.installBoot)) s.installBoot = boot;
      save('uncertain', 'Installer completion is unknown. Recovery needs a full webOS restart, not standby. Your backup is retained.');
      try { await ops.fallback(); } catch (e) { /* retain the independent Home guard and recovery hook */ }
      return;
    }
    recovery.atomic(file + '.ready', { id: s.id, pid: process.pid });
    if (s.phase === 'prepared') {
      if (await ops.foreground() !== APP) { restoreHomeConfig(s); save('restoring-home', 'Cancelling update before replacement'); await ops.restoreHome(); s.active = false; save('error', 'Update cancelled because Home+ is no longer in front'); finishHook(s); return; }
      const target = fs.readFileSync(s.targetFile);
      if (archive.hash(target) !== s.artifact.sha256 || target.length !== s.artifact.size) throw new Error('Staged package hash changed');
      await archive.inspect(target, s.artifact.version);
      const backup = fs.readFileSync(s.backup);
      if (archive.hash(backup) !== s.backupHash) throw new Error('Recovery backup hash changed');
      const prior = await archive.inspect(backup, s.previousVersion);
      archive.verifyInstalled(prior.inventory, s.directories);
      if (await ops.foreground() !== APP) throw new Error('Update cancelled because Home+ is no longer in front');
      replacing = true;
      await install('target');
      archive.verifyInstalled(s.targetInventory, s.directories);
      await ops.elevate();
      await ops.health(s.artifact.version);
      currentConfig(s); // only an absent or unusable config needs the complete snapshot
      save('awaiting-entry', 'Installed. Open Home+ to finish checking the update.');
    } else if (!['awaiting-entry', 'restoring-home'].includes(s.phase)) {
      throw new Error('Interrupted installation requires recovery');
    }
    await complete();

  } catch (error) {
    if (error.installerUncertain) {
      try { await ops.fallback(); } catch (e) { /* the existing independent Home guard also remains effective */ }
      save('uncertain', 'Installer completion is unknown. Recovery needs a full webOS restart, not standby. Your backup is retained.');
      return;
    }
    if (!replacing) {
      restoreHomeConfig(s);
      save('restoring-home', 'Restoring Home settings before cancelling the update');
      try { await ops.restoreHome(); } catch (e) { error = new Error(error.message + '; Home settings need attention'); }
      s.active = false; save('error', 'Update stopped before replacement: ' + release.plain(error.message, 200)); finishHook(s); return;
    }
    try {
      save('recovering', 'Update did not complete. Restoring the verified previous package.');
      await ops.fallback();
      const backup = fs.readFileSync(s.backup);
      if (archive.hash(backup) !== s.backupHash) throw new Error('Recovery backup hash mismatch');
      const old = await archive.inspect(backup, s.previousVersion);
      await install('backup'); archive.verifyInstalled(old.inventory, s.directories);
      restoreHomeConfig(s, true);
      await ops.elevate(); await ops.health(s.previousVersion);
      s.recovered = true;
      save('awaiting-entry', 'Previous build restored. Open Home+ to finish checking recovery.');
      await complete();
    } catch (rollback) {
      try { await ops.fallback(); } catch (ignored) { /* independent default Home guard also remains installed */ }
      if (rollback.installerUncertain) s.phase = 'uncertain';
      save(rollback.installerUncertain ? 'uncertain' : 'recovering', 'Recovery needs attention. The verified backup and recovery hook have been retained.');
      throw new Error('Update recovery could not complete');
    }
  } finally { releaseWorker(); }
}
function checkSpace(df, bytes, artifactSize) {
  const line = df.trim().split('\n').pop().trim().split(/\s+/), available = Number(line[line.length - 3]) * 1024;
  if (!Number.isFinite(available) || available < bytes * 3 + artifactSize * 3 + 32 * 1024 * 1024) throw new Error('Not enough free space for installation and recovery');
}
async function prepare(options) {
  if (process.platform !== 'linux' || !options.isRoot()) throw new Error('Updating Home+ requires elevated webOS');
  const dir = options.stateDir;
  privateDir(dir);
  const file = path.join(dir, 'transaction.json');
  const previous = fs.existsSync(file) ? load(file) : null;
  if (previous && previous.active) throw new Error('An update transaction is already running');
  const boot = bootId(); // refuse unsupported recovery identity before changing Home or replacing packages
  let lock = file + '.worker';
  while (fs.existsSync(lock)) {
    let owner;
    try { owner = lockRecord(lock).owner; } catch (e) { if (e.code === 'ENOENT') break; throw e; }
    if (workerAlive(owner, boot)) throw new Error('The previous update helper is still finishing');
    lock = successor(file + '.worker', owner);
  }
  if (options.externalBusy && options.externalBusy()) throw new Error('Another Home+ setup operation is running');
  fs.accessSync(ELEVATE, fs.constants.X_OK);
  fs.accessSync(process.execPath, fs.constants.X_OK);
  if (options.appDir && process.execPath.indexOf(options.appDir + path.sep) === 0) throw new Error('Recovery runtime must be outside the application');
  await runFile('script', ['-q', '-c', 'printf homeplus-update-terminal', '/dev/null']).then(out => { if (out.indexOf('homeplus-update-terminal') < 0) throw new Error('Installer terminal is unavailable'); });
  fs.accessSync('/usr/bin/luna-send', fs.constants.X_OK);
  const hbc = await recovery.accepted(recovery.luna, HBC + 'getConfiguration', {});
  if (hbc.root !== true) throw new Error('Homebrew Channel is not elevated');
  await recovery.accepted(recovery.luna, HBC + 'exec', { command: quote(process.execPath) + ' --version' });
  const directories = [options.appDir, options.serviceDir, options.packageDir];
  if (directories.some(d => !d || !path.isAbsolute(d))) throw new Error('Installed package locations are unknown');
  if (directories.some(d => options.configPath === d || options.configPath.indexOf(d + path.sep) === 0)) throw new Error('Home+ settings must be outside the replaced package');
  if (options.readConfig().defaultHomeEnabled === true) {
    const guard = recovery.load(options.homeStateFile);
    if (!guard.active || guard.deadline !== null) throw new Error('Default Home recovery is not ready');
  }
  const files = archive.snapshot(directories);
  const bytes = Object.keys(files).reduce((sum, name) => sum + files[name].data.length, 0);
  const df = await runFile('df', ['-Pk', dir]);
  checkSpace(df, bytes, options.artifact.size);
  const backup = archive.pack(files, options.version), verified = await archive.inspect(backup, options.version);
  archive.verifyInstalled(verified.inventory, directories);
  const data = await release.download(options.artifact.url, options.artifact.size);
  if (data.length !== options.artifact.size || archive.hash(data) !== options.artifact.sha256) throw new Error('Downloaded package hash or size mismatch');
  const target = await archive.inspect(data, options.artifact.version);
  const installedBytes = Object.keys(target.files).reduce((sum, name) => sum + target.files[name].data.length, 0);
  checkSpace(await runFile('df', ['-Pk', options.appDir]), bytes, installedBytes);
  if (options.readConfig().defaultHomeEnabled === true) {
    const app = JSON.parse(target.files[archive.roots[0] + '/appinfo.json'].data.toString());
    if (!Array.isArray(app.appCategory) || !app.appCategory.includes('home')) throw new Error('This package cannot preserve the requested default Home app');
  }
  archive.verifyInstalled(verified.inventory, directories);
  if (!options.foreground()) throw new Error('Update cancelled because Home+ is no longer in front');
  const id = require('crypto').randomBytes(12).toString('hex');
  const s = { schemaVersion: 1, id, active: true, phase: 'prepared', message: 'Preparing independent installation', previousVersion: options.version,
    artifact: options.artifact, backup: path.join(dir, id + '-previous.ipk'), targetFile: path.join(dir, id + '-target.ipk'), backupHash: archive.hash(backup), targetInventory: target.inventory,
    configPath: options.configPath, configBackup: path.join(dir, id + '-config.json'), directories, defaultHome: options.readConfig().defaultHomeEnabled === true,
    homeStateFile: options.homeStateFile, homeBackup: path.join(dir, id + '-home.json'), hook: path.join(options.initDir, '01-homeplus-update'), boot: path.join(dir, 'boot.sh') };
  if (fs.existsSync(s.hook) && !ownHook(s)) throw new Error('Update recovery hook path is already in use');
  if (options.homeStateFile && fs.existsSync(options.homeStateFile)) recovery.durable(s.homeBackup, fs.readFileSync(options.homeStateFile));
  const startup = path.join(options.initDir, '50-homeplus');
  if (fs.existsSync(startup) && !fs.lstatSync(startup).isSymbolicLink()) throw new Error('Home+ startup hook must use the supported package link before updating');
  s.startup = fs.existsSync(startup) ? { target: fs.readlinkSync(startup) } : null;
  recovery.durable(s.backup, backup); recovery.durable(s.targetFile, data); recovery.durable(s.configBackup, fs.readFileSync(s.configPath));
  for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js']) recovery.durable(path.join(dir, name), fs.readFileSync(path.join(__dirname, name)));
  recovery.durable(s.boot, '#!/bin/sh\n' + quote(process.execPath) + ' ' + quote(path.join(dir, 'update-worker.js')) + ' --start ' + quote(file) + '\n', 0o700);
  if (!ownHook(s)) fs.symlinkSync(s.boot, s.hook); recovery.syncDir(options.initDir);
  recovery.atomic(file, s);
  // The new private backup has been verified and flushed before retiring the previous transaction's files.
  if (previous) {
    for (const suffix of ['-previous.ipk', '-target.ipk', '-config.json', '-home.json']) {
      try { fs.unlinkSync(path.join(dir, previous.id + suffix)); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    }
    recovery.syncDir(dir);
  }
  try { if (options.suspendHome) await options.suspendHome(); }
  catch (e) {
    restoreHomeConfig(s);
    s.active = false; s.phase = 'error'; s.message = 'Update stopped before replacement'; recovery.atomic(file, s); finishHook(s);
    if (options.resumeHome) await options.resumeHome();
    throw e;
  }
  await recovery.accepted(recovery.luna, HBC + 'exec', { command: quote(process.execPath) + ' ' + quote(path.join(dir, 'update-worker.js')) + ' --start ' + quote(file) });
  const end = Date.now() + 5000;
  while (Date.now() < end) {
    try { const ready = read(file + '.ready'); if (ready.id === id) { process.kill(ready.pid, 0); return; } } catch (e) { /* worker starting */ }
    await wait(50);
  }
  throw new Error('Independent installer did not start; recovery hook remains armed');
}
function start(file) {
  const fd = fs.openSync(file + '.log', 'a', 0o600);
  const child = spawn(process.execPath, [path.join(path.dirname(file), 'update-worker.js'), '--run', file], { detached: true, stdio: ['ignore', fd, fd], cwd: path.dirname(file), env: { PATH: '/usr/bin:/bin:/usr/sbin:/sbin', HOME: '/home/root' } });
  child.unref(); fs.closeSync(fd);
}
module.exports = { bootId, prepare, run, start, load, production, finalReply, installSubscription, checkSpace };
if (require.main === module) {
  const mode = process.argv[2], file = process.argv[3];
  if (mode === '--start') start(file);
  else if (mode === '--run') run(file).catch(e => { console.error(e.message); process.exitCode = 1; });
  else { console.error('Invalid update worker invocation'); process.exitCode = 1; }
}

/* Standalone recovery for the opt-in developer trial. Copy outside the app/service.
 * --watch survives a disconnected controller; --boot restores before a trial continues
 * after reboot. No package files, accounts, inputs or firmware paths are written here. */
'use strict';
const fs = require('fs');
const { execFile, spawn } = require('child_process');
const APP = 'com.github.pbalazaar.homeplus';
const STOCK = 'com.webos.app.home';
const MANAGER = 'luna://com.webos.applicationManager/';
const SETTINGS = 'luna://com.webos.settingsservice/';
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const validId = (s) => typeof s === 'string' && /^[a-zA-Z0-9._-]+$/.exec(s);

async function readDefaults(luna) {
  const r = await luna(SETTINGS + 'getSystemSettings', { category: 'general', keys: ['defaultApps'] });
  const apps = r && r.settings && r.settings.defaultApps;
  if (!r || r.returnValue !== true || !apps || typeof apps !== 'object' || Array.isArray(apps) ||
      (own(apps, 'home') && !validId(apps.home))) throw new Error('Cannot read the default Home setting');
  return Object.assign({}, apps);
}
async function accepted(luna, uri, params) {
  const r = await luna(uri, params);
  if (!r || r.returnValue !== true) throw new Error((r && r.errorText) || 'Recovery request refused');
}
async function restore(luna, before) {
  if (!before || typeof before !== 'object' || Array.isArray(before) ||
      (own(before, 'home') && !validId(before.home))) throw new Error('Invalid recovery snapshot');
  let current = await readDefaults(luna);
  if (own(current, 'home') === own(before, 'home') && current.home === before.home) return { restored: true };
  if (current.home !== APP && !(current.home === STOCK && !own(before, 'home'))) return { preserved: true };
  if (own(before, 'home')) {
    await accepted(luna, MANAGER + 'setDefaultApp', { category: 'home', appId: before.home });
    if ((await readDefaults(luna)).home !== before.home) throw new Error('Previous Home was not restored');
  } else {
    // First leave SAM pointing at LG Home, then restore the original absent setting.
    // Merge from the live map, so unrelated default categories are not rolled back.
    if (current.home === APP) await accepted(luna, MANAGER + 'setDefaultApp', { category: 'home', appId: STOCK });
    current = await readDefaults(luna);
    if (!own(current, 'home')) return { restored: true };
    if (current.home !== STOCK) throw new Error('LG Home was not restored before clearing the trial entry');
    delete current.home;
    await accepted(luna, SETTINGS + 'setSystemSettings', { category: 'general', settings: { defaultApps: current } });
    if (own(await readDefaults(luna), 'home')) throw new Error('Trial Home entry was not removed');
  }
  return { restored: true };
}

// script(1) gives luna-send a PTY: some webOS builds otherwise lose the JSON reply.
function luna(uri, params) {
  const quote = (s) => "'" + s.replace(/'/g, "'\\''") + "'";
  const binary = process.env.HOMEPLUS_RECOVERY_LUNA || 'luna-send';
  const command = 'exec ' + quote(binary) + ' -n 1 -w 4000 ' + quote(uri) + ' ' + quote(JSON.stringify(params));
  return new Promise((resolve, reject) => {
    const child = execFile('script', ['-q', '-c', command, '/dev/null'], { timeout: 8000 }, (err, stdout) => {
      if (err) return reject(new Error('Recovery Luna call failed: ' + err.message));
      let result;
      for (const line of String(stdout).split('\n')) {
        try { const r = JSON.parse(line.trim()); if (r && typeof r === 'object') result = r; } catch (e) { /* PTY status */ }
      }
      if (!result) return reject(new Error('Recovery Luna call returned no JSON'));
      resolve(result);
    });
    child.stdin.end();
  });
}
function load(file) {
  const state = JSON.parse(fs.readFileSync(file, 'utf8'));
  if (state.version !== 1 || typeof state.active !== 'boolean' || !Number.isFinite(state.deadline) ||
      typeof state.applicationDir !== 'string' || state.applicationDir[0] !== '/' || !state.before ||
      typeof state.before !== 'object' || Array.isArray(state.before) ||
      (own(state.before, 'home') && !validId(state.before.home))) {
    throw new Error('Invalid recovery state');
  }
  return state;
}
function save(file, state) {
  const tmp = file + '.' + process.pid + '.tmp';
  try { fs.unlinkSync(tmp); } catch (e) { if (e.code !== 'ENOENT') throw e; }
  fs.writeFileSync(tmp, JSON.stringify(state, null, 2), { mode: 0o600, flag: 'wx' });
  fs.renameSync(tmp, file);
}
async function main(mode, file) {
  if (['--start', '--start-boot'].includes(mode) && file) {
    load(file); // refuse invalid recovery state before claiming a guard was started
    const out = fs.openSync(file + '.log', 'a', 0o600);
    const child = spawn(process.execPath, [__filename, mode === '--start' ? '--watch' : '--boot', file],
      { detached: true, stdio: ['ignore', out, out] });
    child.unref();
    fs.closeSync(out);
    console.log(JSON.stringify({ pid: child.pid }));
    return;
  }
  if (!['--watch', '--boot', '--restore'].includes(mode) || !file) throw new Error('usage: default-home-recovery.js --watch|--boot|--restore <state.json>');
  let state = load(file);
  if (!state.active) return;
  if (mode === '--watch') {
    fs.writeFileSync(file + '.ready', JSON.stringify({ pid: process.pid, deadline: state.deadline }), { mode: 0o600 });
    console.log('recovery guard ready');
  }
  let retryDelay = 500;
  for (;;) {
    state = load(file);
    if (!state.active) return;
    const available = fs.existsSync(state.applicationDir + '/appinfo.json') && fs.existsSync(state.applicationDir + '/homeplus');
    if (mode === '--watch' && available && Date.now() < state.deadline) {
      await new Promise((resolve) => setTimeout(resolve, 250));
      continue;
    }
    try {
      const result = await restore(luna, state.before);
      state = load(file);
      state.active = false;
      state.result = result;
      state.recoveredAt = Date.now();
      save(file, state);
      console.log(JSON.stringify(result));
      return;
    } catch (e) {
      // Stay independent of the controller while settings are unavailable.
      // Bounded backoff keeps recovery alive without a busy loop or blind writes.
      await new Promise((resolve) => setTimeout(resolve, retryDelay));
      retryDelay = Math.min(5000, retryDelay * 2);
    }
  }
}
module.exports = { readDefaults, restore, luna };
if (require.main === module) main(process.argv[2], process.argv[3]).catch((e) => { console.error(e.message); process.exitCode = 1; });

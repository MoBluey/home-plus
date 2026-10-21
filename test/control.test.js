'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const createControl = require('../service/control');

/* A stand-in TV: answers the luna calls the Control Center makes, and records them. */
function fakeTv({ arc = true, livetv = true, wifi = 'Home Wi-Fi', wired = false, output = 'external_arc', earc = 'off', sleep = 0, readConfig, writeConfig } = {}) {
  const tv = { soundOutput: output, eArcSupport: earc, sleepMinutes: sleep, pictureMode: 'expert1', eyeComfortMode: 'off' }, calls = [];
  const luna = async (uri, params) => {
    calls.push({ uri, params });
    if (uri.endsWith('/getAllInputStatus')) {
      return { returnValue: true, arcDevice: arc, devices: [
        { id: 'HDMI_1', appId: 'com.webos.app.hdmi1', label: 'HDMI 1', hdmiPlugIn: false },
        { id: 'HDMI_2', appId: 'com.webos.app.hdmi2', label: 'HW-Q990F', spdProductDescription: 'PS5', hdmiPlugIn: true },
        { id: 'BROKEN' }] };
    }
    if (uri.endsWith('/listApps')) return { returnValue: true, apps: livetv ? [{ id: 'com.webos.app.livetv' }] : [] };
    if (uri.endsWith('connectionmanager/getStatus')) {
      return { returnValue: true, wifi: wifi ? { state: 'connected', ssid: wifi } : { state: 'disconnected' }, wired: { state: wired ? 'connected' : 'disconnected' } };
    }
    if (uri.endsWith('/getSystemSettingDesc')) { /* the picture modes, as webOS 23 lists them for SDR */
      return { returnValue: true, results: [{ key: 'pictureMode', values: { arrayExt: ['vivid', 'normal', 'cinema', 'filmMaker', 'expert1']
        .map((value) => ({ value, visible: true, active: true })).concat([{ value: 'hdrVivid', visible: false, active: true }]) } }] };
    }
    if (uri.endsWith('/getSystemSettings')) return { returnValue: true, settings: Object.fromEntries(params.keys.map((k) => [k, tv[k]])) };
    /* LG's sleep timer: minutes and seconds left (no minutes when it's off) */
    if (uri.endsWith('/timer/sleep/getRemainTime')) return Object.assign({ returnValue: true, sleepTimeSecond: 0 }, tv.sleepMinutes ? { sleepTimeMinute: tv.sleepMinutes - 1, sleepTimeSecond: 59 } : {});
    if (uri.endsWith('/timer/sleep/add')) { tv.sleepMinutes = params.sleepTimeMinute; return { returnValue: true }; }
    if (uri.endsWith('/setSystemSettings')) { Object.assign(tv, params.settings); return { returnValue: true }; }
    return { returnValue: true };
  };
  return { control: createControl({ luna, log: () => {}, readConfig, writeConfig }), tv, calls };
}

test('control: what the panel shows: network, sound output, sleep timer and inputs', async () => {
  const { control } = fakeTv();
  const st = await control.state();
  assert.deepEqual(st.network, { kind: 'wifi', name: 'Home Wi-Fi' });
  assert.deepEqual(st.sound, { output: 'external_arc', label: 'HDMI ARC', outputs: ['tv_speaker', 'external_arc'] });
  assert.equal(st.sleepTimer, 'off');
  assert.deepEqual(st.inputs, [
    { appId: 'com.webos.app.livetv', label: 'Live TV', device: '', plugged: true },
    { appId: 'com.webos.app.hdmi1', label: 'HDMI 1', device: '', plugged: false },
    { appId: 'com.webos.app.hdmi2', label: 'HW-Q990F', device: 'PS5', plugged: true }]);
});

test('control: no Wi-Fi: Ethernet (the tile says so; its line says "Connected"), or not connected', async () => {
  assert.deepEqual((await fakeTv({ wifi: '', wired: true }).control.state()).network, { kind: 'wired', name: 'Connected' });
  assert.deepEqual((await fakeTv({ wifi: '' }).control.state()).network, { kind: 'none', name: 'Not Connected' });
});

test('control: Sound offers HDMI ARC even while webOS reports no ARC device (a soundbar asleep)', async () => {
  const { control, tv } = fakeTv({ arc: false, output: 'tv_speaker' });
  assert.deepEqual((await control.state()).sound.outputs, ['tv_speaker', 'external_arc']);
  await control.act('sound', 'next');
  assert.equal(tv.soundOutput, 'external_arc');
  await control.act('sound', 'next');
  assert.equal(tv.soundOutput, 'tv_speaker', 'and back');
});

test('control: Sound also offers the other outputs the TV has used, and remembers them', async () => {
  const cfg = {};
  const opts = { readConfig: () => JSON.parse(JSON.stringify(cfg)), writeConfig: (c) => Object.assign(cfg, c) };
  assert.deepEqual((await fakeTv({ arc: false, output: 'bt_soundbar', ...opts }).control.state()).sound.outputs, ['tv_speaker', 'external_arc', 'bt_soundbar']);
  assert.deepEqual(cfg.soundOutputs, ['bt_soundbar']);
  assert.deepEqual((await fakeTv({ arc: false, output: 'tv_speaker', ...opts }).control.state()).sound.outputs, ['tv_speaker', 'external_arc', 'bt_soundbar'], 'after a restart too');
});

test('control: Sound steps through the outputs', async () => {
  const { control, tv } = fakeTv({ output: 'tv_speaker' });
  assert.deepEqual(await control.act('sound', 'next'), { soundOutput: 'external_arc' });
  assert.equal(tv.soundOutput, 'external_arc');
  await control.act('sound', 'next');
  assert.equal(tv.soundOutput, 'tv_speaker');
  await assert.rejects(control.act('sound', 'loudspeaker'), /not a sound output/);
});

test('control: Sleep Timer steps through LG\'s own values, on the timer that turns the TV off', async () => {
  const { control, tv, calls } = fakeTv();
  const seen = [];
  for (let i = 0; i < 9; i++) seen.push((await control.act('sleepTimer', 'next')).sleepTimer);
  assert.deepEqual(seen, ['10', '20', '30', '60', '90', '120', '180', '240', 'off']);
  assert.equal(tv.sleepMinutes, 0, 'off');
  assert.ok(calls.some((c) => c.uri === 'luna://com.webos.service.tvReservationAgent/timer/sleep/add' && c.params.sleepTimeMinute === 240));
  await assert.rejects(control.act('sleepTimer', '45'), /sleep timer/);
});

test('control: the Sleep Timer shows the minutes left, and Next goes on from the step they\'re in', async () => {
  const { control, tv } = fakeTv({ sleep: 47 });
  assert.equal((await control.state()).sleepTimer, '47');
  assert.equal((await control.act('sleepTimer', 'next')).sleepTimer, '90', '47 minutes left is the 60-minute timer');
  assert.equal(tv.sleepMinutes, 90);
});

test('control: HDMI ARC is "HDMI eARC" when eARC is on', async () => {
  assert.equal((await fakeTv({ earc: 'on' }).control.state()).sound.label, 'HDMI eARC');
  assert.equal((await fakeTv().control.state()).sound.label, 'HDMI ARC');
});

test('control: an input opens only if the TV has it', async () => {
  const { control, calls } = fakeTv();
  await control.act('input', 'com.webos.app.hdmi2');
  assert.deepEqual(calls.at(-1), { uri: 'luna://com.webos.applicationManager/launch', params: { id: 'com.webos.app.hdmi2' } });
  await assert.rejects(control.act('input', 'com.evil.app'), /not an input/);
});

test('control: Power Off and Screen Off ask the TV\'s power service', async () => {
  const { control, calls } = fakeTv();
  await control.act('powerOff');
  assert.deepEqual(calls.at(-1), { uri: 'luna://com.webos.service.tvpower/power/powerOff', params: { reason: 'remoteKey' } });
  await control.act('screenOff');
  assert.deepEqual(calls.at(-1), { uri: 'luna://com.webos.service.tvpower/power/turnOffScreen', params: { standbyMode: 'active' } });
  await assert.rejects(control.act('reboot'), /unknown action/);
});

test('control: the picture mode, with the modes LG offers for what\'s on now, by LG\'s names', async () => {
  const { control, tv } = fakeTv();
  const st = await control.state();
  assert.deepEqual(st.picture, { mode: 'expert1', label: 'Expert (Bright)', modes: [
    { id: 'vivid', label: 'Vivid' }, { id: 'normal', label: 'Standard' }, { id: 'cinema', label: 'Cinema' },
    { id: 'filmMaker', label: 'Filmmaker Mode' }, { id: 'expert1', label: 'Expert (Bright)' }] });
  await control.act('pictureMode', 'cinema');
  assert.equal(tv.pictureMode, 'cinema');
  await assert.rejects(control.act('pictureMode', 'hdrVivid'), /picture mode/, 'an HDR mode, while SDR is on');
  assert.equal(tv.pictureMode, 'cinema');
});

test('control: Eye Comfort turns on and off', async () => {
  const { control, tv } = fakeTv();
  assert.equal((await control.state()).eyeComfort, 'off');
  assert.equal((await control.act('eyeComfort', 'next')).eyeComfort, 'on');
  assert.equal(tv.eyeComfortMode, 'on');
  assert.equal((await control.act('eyeComfort', 'next')).eyeComfort, 'off');
  await assert.rejects(control.act('eyeComfort', 'bright'), /on or off/);
});

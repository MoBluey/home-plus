/* Control Center: the TV's quick controls, like tvOS's: power, screen off, sound
 * output, sleep timer and inputs, through webOS's own services (this service runs
 * as root). Every action is also the luna method `control`, so LG Input Hook can
 * put one on a remote button.
 */
'use strict';

const POWER = 'luna://com.webos.service.tvpower/power';
const SETTINGS = 'luna://com.webos.settingsservice';
// The sleep timer that turns the TV off (what LG's Quick Settings use; the time/sleepTimer
// setting doesn't start it), in LG's steps
const TIMER = 'luna://com.webos.service.tvReservationAgent/timer/sleep';
const SLEEP = ['off', '10', '20', '30', '60', '90', '120', '180', '240'];
// Sound switches between the TV's speakers and HDMI ARC (always: with the output on the speakers,
// webOS stops reporting an ARC device that's asleep, and switching to ARC wakes it over CEC)
const TOGGLE = ['tv_speaker', 'external_arc'];
const OUTPUT_NAMES = {
  tv_speaker: 'TV Speakers', tv_speakerbar: 'TV Speakers', external_arc: 'HDMI ARC', external_optical: 'Optical',
  bt_soundbar: 'Bluetooth', wisa_speaker: 'WiSA Speakers', mobile_phone: 'Phone', lineout: 'Line Out',
  builtin_soundbar: 'Soundbar', headphone: 'Headphones'
};

// LG's names for its picture modes (its Quick Settings' strings); SDR, HDR and Dolby Vision
// content each have their own ids
const PICTURE_NAMES = {
  normal: 'Standard', hdrStandard: 'Standard', dolbyHdrStandard: 'Standard',
  vivid: 'Vivid', hdrVivid: 'Vivid', dolbyHdrVivid: 'Vivid',
  eco: 'Eco', hdrEco: 'Eco',
  cinema: 'Cinema', hdrCinema: 'Cinema', hdrCinemaBright: 'Cinema Home', dolbyHdrCinemaBright: 'Cinema Home',
  sports: 'Sports', photo: 'Photo',
  game: 'Game Optimizer', hdrGame: 'Game Optimizer', dolbyHdrGame: 'Game Optimizer',
  filmMaker: 'Filmmaker Mode', hdrFilmMaker: 'Filmmaker Mode', dolbyHdrCinema: 'Filmmaker Mode',
  expert1: 'Expert (Bright)', expert2: 'Expert (Dark)',
  personalized: 'Personalized', hdrPersonalized: 'Personalized', dolbyHdrPersonalized: 'Personalized'
};

module.exports = function createControl({ luna, log, readConfig = () => ({}), writeConfig = () => {} }) {
  // the other outputs this TV has used (Optical, Bluetooth...), so Sound can switch back to them
  const used = new Set(readConfig().soundOutputs || []);
  function remember(out) {
    if (!out || TOGGLE.includes(out) || !OUTPUT_NAMES[out] || used.has(out)) return;
    used.add(out);
    const cfg = readConfig();
    cfg.soundOutputs = [...used];
    writeConfig(cfg);
  }

  async function settings(category, keys) {
    const r = await luna(`${SETTINGS}/getSystemSettings`, { category, keys });
    return (r && r.settings) || {}; /* a key this TV doesn't have fails the call but not the others */
  }

  // minutes until the sleep timer turns the TV off (0: it's off), and its step (LG shows the
  // step it's within: 47 minutes left is the 60-minute timer)
  async function sleepLeft() {
    const r = await luna(`${TIMER}/getRemainTime`, {});
    return r && r.returnValue !== false ? (Number(r.sleepTimeMinute) || 0) + (Number(r.sleepTimeSecond) || 0) / 60 : 0;
  }
  // the picture modes LG offers for what's on now (as its Quick Settings list them)
  async function pictureModes() {
    const r = await luna(`${SETTINGS}/getSystemSettingDesc`, { category: 'picture', keys: ['pictureMode'] });
    const values = (r && r.results && r.results[0] && r.results[0].values && r.results[0].values.arrayExt) || [];
    return values.filter((m) => m.visible && m.active).map((m) => ({ id: m.value, label: PICTURE_NAMES[m.value] || m.value }));
  }
  const sleepStep = (left) => (left > 0 ? SLEEP.find((m) => m !== 'off' && left <= Number(m)) || '240' : 'off');

  // the inputs: Live TV, then LG's names for the HDMI ports (a CEC device's, like
  // "HW-Q990F") and what's behind them ("PS5")
  async function inputs() {
    const [r, apps] = await Promise.all([luna('luna://com.webos.service.eim/getAllInputStatus', {}),
      luna('luna://com.webos.applicationManager/listApps', {})]);
    const live = ((apps && apps.apps) || []).some((a) => a.id === 'com.webos.app.livetv');
    return {
      list: (live ? [{ appId: 'com.webos.app.livetv', label: 'Live TV', device: '', plugged: true }] : []).concat(
        ((r && r.devices) || []).filter((d) => d.appId).map((d) => ({
          appId: d.appId, label: d.label || d.id, device: d.spdProductDescription || '', plugged: !!d.hdmiPlugIn
        })))
    };
  }

  async function state() {
    const [net, sound, left, inp, pic, modes] = await Promise.all([
      luna('luna://com.webos.service.connectionmanager/getStatus', {}),
      settings('sound', ['soundOutput', 'eArcSupport']), sleepLeft(), inputs(),
      settings('picture', ['pictureMode', 'eyeComfortMode']), pictureModes()
    ]);
    const output = sound.soundOutput;
    const wifi = net && net.wifi && net.wifi.state === 'connected' ? net.wifi.ssid || net.wifi.displayName || 'Wi-Fi' : '';
    const wired = !!(net && net.wired && net.wired.state === 'connected');
    remember(output);
    const outputs = [...new Set([...TOGGLE, ...used])];
    return {
      network: wifi ? { kind: 'wifi', name: wifi } : { kind: wired ? 'wired' : 'none', name: wired ? 'Connected' : 'Not Connected' },
      sound: { output: output || '', label: output === 'external_arc' && sound.eArcSupport === 'on' ? 'HDMI eARC' : OUTPUT_NAMES[output] || output || '', outputs },
      sleepTimer: left > 0 ? String(Math.ceil(left)) : 'off', /* minutes left */
      picture: { mode: pic.pictureMode || '', label: PICTURE_NAMES[pic.pictureMode] || pic.pictureMode || '', modes },
      eyeComfort: pic.eyeComfortMode || '',
      inputs: inp.list
    };
  }

  const next = (list, cur) => list[(list.indexOf(cur) + 1) % list.length];

  // webOS answers a refusal with returnValue false: that's an error, so the launcher says so
  function ok(r) {
    if (!r || r.returnValue === false) throw new Error((r && r.errorText) || "the TV didn't answer");
    return r;
  }

  // action: powerOff, screenOff, sound (an output, or "next"), sleepTimer (minutes, "off" or
  // "next"), pictureMode (one of the modes on offer now), eyeComfort ("on", "off" or "next"),
  // input (an input's app id)
  async function act(action, value) {
    switch (action) {
      case 'powerOff':
        return ok(await luna(`${POWER}/powerOff`, { reason: 'remoteKey' }));
      case 'screenOff': /* "active": any key brings the picture back ("passive" can't) */
        return ok(await luna(`${POWER}/turnOffScreen`, { standbyMode: 'active' }));
      case 'sound': {
        const st = await state();
        const out = value === 'next' ? next(st.sound.outputs, st.sound.output) : String(value || '');
        if (!OUTPUT_NAMES[out]) throw new Error(`not a sound output: ${out}`);
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category: 'sound', settings: { soundOutput: out } }));
        return { soundOutput: out };
      }
      case 'sleepTimer': {
        const v = value === 'next' ? next(SLEEP, sleepStep(await sleepLeft())) : String(value || '');
        if (!SLEEP.includes(v)) throw new Error(`the sleep timer is off or ${SLEEP.slice(1).join(', ')} minutes`);
        ok(await luna(`${TIMER}/add`, { sleepTimeMinute: v === 'off' ? 0 : Number(v) }));
        return { sleepTimer: v };
      }
      case 'pictureMode': { /* for what's on now: LG keeps one per input and SDR/HDR/Dolby Vision */
        if (!(await pictureModes()).some((m) => m.id === value)) throw new Error(`not a picture mode the TV offers now: ${value}`);
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category: 'picture', settings: { pictureMode: value } }));
        return { pictureMode: value };
      }
      case 'eyeComfort': {
        const cur = (await settings('picture', ['eyeComfortMode'])).eyeComfortMode;
        const v = value === 'next' ? (cur === 'on' ? 'off' : 'on') : String(value || '');
        if (v !== 'on' && v !== 'off') throw new Error('Eye Comfort is on or off');
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category: 'picture', settings: { eyeComfortMode: v } }));
        return { eyeComfort: v };
      }
      case 'input': {
        const inp = await inputs();
        if (!inp.list.some((d) => d.appId === value)) throw new Error(`not an input: ${value}`);
        return ok(await luna('luna://com.webos.applicationManager/launch', { id: value }));
      }
      default:
        throw new Error(`unknown action: ${action}`);
    }
  }

  return { state, act };
};

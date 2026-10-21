/* Control Center: the TV's quick controls, like tvOS's: power, screen off, sound
 * output, sleep timer and inputs, through webOS's own services (this service runs
 * as root). Every action is also the luna method `control`, so LG Input Hook can
 * put one on a remote button.
 */
'use strict';

const POWER = 'luna://com.webos.service.tvpower/power';
const SETTINGS = 'luna://com.webos.settingsservice';
// LG's own sleep timer steps (getSystemSettingDesc time/sleepTimer)
const SLEEP = ['off', '10', '20', '30', '60', '90', '120', '180', '240'];
const OUTPUT_NAMES = {
  tv_speaker: 'TV Speakers', tv_speakerbar: 'TV Speakers', external_arc: 'HDMI ARC', external_optical: 'Optical',
  bt_soundbar: 'Bluetooth', wisa_speaker: 'WiSA Speakers', mobile_phone: 'Phone', lineout: 'Line Out',
  builtin_soundbar: 'Soundbar', headphone: 'Headphones'
};

module.exports = function createControl({ luna, log }) {
  async function setting(category, key) {
    const r = await luna(`${SETTINGS}/getSystemSettings`, { category, keys: [key] });
    return r && r.settings ? r.settings[key] : undefined;
  }

  // the inputs: Live TV, then LG's names for the HDMI ports (a CEC device's, like
  // "HW-Q990F") and what's behind them ("PS5")
  async function inputs() {
    const [r, apps] = await Promise.all([luna('luna://com.webos.service.eim/getAllInputStatus', {}),
      luna('luna://com.webos.applicationManager/listApps', {})]);
    const live = ((apps && apps.apps) || []).some((a) => a.id === 'com.webos.app.livetv');
    return {
      arc: !!(r && r.arcDevice),
      list: (live ? [{ appId: 'com.webos.app.livetv', label: 'Live TV', device: '', plugged: true }] : []).concat(
        ((r && r.devices) || []).filter((d) => d.appId).map((d) => ({
          appId: d.appId, label: d.label || d.id, device: d.spdProductDescription || '', plugged: !!d.hdmiPlugIn
        })))
    };
  }

  async function state() {
    const [net, output, sleep, inp] = await Promise.all([
      luna('luna://com.webos.service.connectionmanager/getStatus', {}),
      setting('sound', 'soundOutput'), setting('time', 'sleepTimer'), inputs()
    ]);
    const wifi = net && net.wifi && net.wifi.state === 'connected' ? net.wifi.ssid || net.wifi.displayName || 'Wi-Fi' : '';
    const wired = !!(net && net.wired && net.wired.state === 'connected');
    const outputs = ['tv_speaker'].concat(inp.arc ? ['external_arc'] : []);
    if (output && !outputs.includes(output)) outputs.push(output);
    return {
      network: wifi ? { kind: 'wifi', name: wifi } : { kind: wired ? 'wired' : 'none', name: wired ? 'Connected' : 'Not Connected' },
      sound: { output: output || '', label: OUTPUT_NAMES[output] || output || '', outputs },
      sleepTimer: sleep || 'off',
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
  // "next"), input (an input's app id)
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
        const cur = (await setting('time', 'sleepTimer')) || 'off';
        const v = value === 'next' ? next(SLEEP, cur) : String(value || '');
        if (!SLEEP.includes(v)) throw new Error(`the sleep timer is off or ${SLEEP.slice(1).join(', ')} minutes`);
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category: 'time', settings: { sleepTimer: v } }));
        return { sleepTimer: v };
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

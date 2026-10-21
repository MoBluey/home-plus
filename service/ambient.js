/* Ambient light, for people who run HyperHDR and PicCap on the TV: two Control Center
 * switches, HyperHDR's LEDs and PicCap's screen capture. They're offered only when those
 * are installed. Nothing else in Home+ knows about them: deleting this file and its line
 * in service.js removes them.
 */
'use strict';

const fs = require('fs');
const { requestJSON } = require('./net');

const SERVICES = '/media/developer/apps/usr/palm/services';
// HyperHDR answers JSON-RPC on 127.0.0.1:8090 (no token from the TV itself); its loader service
// starts it. Tests point both elsewhere.
const HYPERHDR = process.env.HOMEPLUS_HYPERHDR || 'http://127.0.0.1:8090/json-rpc';
const HYPERHDR_DIR = process.env.HOMEPLUS_HYPERHDR_DIR || `${SERVICES}/org.webosbrew.hyperhdr.loader.service`;
const PICCAP_DIR = process.env.HOMEPLUS_PICCAP_DIR || `${SERVICES}/org.webosbrew.piccap.service`;
const LOADER = 'luna://org.webosbrew.hyperhdr.loader.service';
const PICCAP = 'luna://org.webosbrew.piccap.service';

module.exports = function createAmbient({ luna, log }) {
  const installed = (dir) => async () => fs.existsSync(dir);
  const rpc = async (cmd) => {
    const r = await requestJSON('POST', HYPERHDR, { body: Object.assign({ tan: 1 }, cmd), timeout: 2000 });
    if (!r.json || r.json.success === false) throw new Error(`HyperHDR: ${(r.json && r.json.error) || r.status}`);
    return r.json;
  };
  // HyperHDR's LEDs: its LEDDEVICE component; not running counts as off
  async function leds() {
    try {
      const c = ((await rpc({ command: 'serverinfo' })).info.components || []).find((x) => x.name === 'LEDDEVICE');
      return { running: true, on: !!(c && c.enabled) };
    } catch (e) {
      return { running: false, on: false };
    }
  }
  const capture = async () => {
    const r = await luna(`${PICCAP}/status`, {});
    if (!r || r.returnValue === false) throw new Error(`PicCap: ${(r && r.errorText) || "it didn't answer"}`);
    return !!r.isRunning;
  };
  const onOff = (on) => ({ on, value: on ? 'On' : 'Off' });

  return [
    {
      id: 'ambientLight', title: 'Ambient Light', kind: 'switch', icon: 'ambient', group: 'Ambient Light', sizes: ['wide', 'small'],
      available: installed(HYPERHDR_DIR),
      read: async () => onOff((await leds()).on),
      run: async () => {
        const l = await leds();
        if (!l.running) { /* off with HyperHDR: start it, and its LEDs come on */
          const r = await luna(`${LOADER}/start`, {});
          if (!r || r.returnValue === false) throw new Error(`HyperHDR didn't start: ${(r && r.errorText) || 'no answer'}`);
          log('ambient: HyperHDR started');
          return;
        }
        await rpc({ command: 'componentstate', componentstate: { component: 'LEDDEVICE', state: !l.on } });
        log('ambient: LEDs', l.on ? 'off' : 'on');
      }
    },
    {
      id: 'screenCapture', title: 'Screen Capture', kind: 'switch', icon: 'capture', group: 'Ambient Light', sizes: ['wide', 'small'],
      available: installed(PICCAP_DIR),
      read: async () => onOff(await capture()),
      run: async () => {
        const on = await capture();
        const r = await luna(`${PICCAP}/${on ? 'stop' : 'start'}`, {});
        if (!r || r.returnValue === false) throw new Error(`PicCap: ${(r && r.errorText) || "it didn't answer"}`);
        log('ambient: capture', on ? 'stopped' : 'started');
      }
    }
  ];
};

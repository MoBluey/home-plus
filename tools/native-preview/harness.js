// Runs the real Home+ service on this machine with a stubbed webOS runtime,
// fake apps and fake Stremio data, so the native front end can be previewed.
// usage: NODE_PATH=<dir with webos-service stub> node harness.js <assetsDir> <serviceDir>
'use strict';
const fs = require('fs');
const path = require('path');
const https = require('https');
const { EventEmitter } = require('events');

const [assets, serviceDir] = process.argv.slice(2);
const APPS = fs.readFileSync(path.join(assets, 'apps.txt'), 'utf8').trim().split(',').map((s) => s.split('|'));
const COLORS = { netflix: '#E50914', prime: '#00A8E1', disney: '#113CCF', hdmi1: '#3A3A40' };

// Stremio: a signed-in config and five shows mid-watch.
fs.mkdirSync('/var/lib/homeplus', { recursive: true });
fs.writeFileSync('/var/lib/homeplus/config.json', JSON.stringify({ stremioAuthKey: 'preview' }));
const SHOWS = ['Severed Signals', 'The Long Orbit', 'Northbound', 'Glass City', 'Midnight Kitchen'];
const library = SHOWS.map((name, i) => ({
  _id: `tt000000${i}`, type: i % 2 ? 'movie' : 'series', name, removed: false,
  state: { timeOffset: 600000 * (i + 2), duration: 3000000 + i * 900000, video_id: i % 2 ? '' : `tt000000${i}:${i + 1}:${i + 3}`,
    lastWatched: `2026-10-${20 - i}T10:00:00Z` }
}));

https.request = (url, opts, cb) => {
  const req = new EventEmitter();
  req.end = () => {
    const res = new EventEmitter();
    res.setEncoding = () => {};
    cb(res);
    res.emit('data', JSON.stringify({ result: String(url).endsWith('datastoreGet') ? library : {} }));
    res.emit('end');
  };
  req.destroy = () => {};
  return req;
};
https.get = (url, opts, cb) => {
  const m = /tt000000(\d)/.exec(String(url));
  const res = fs.createReadStream(path.join(assets, `show${m ? m[1] : 0}.jpg`));
  res.statusCode = /\/logo\//.test(url) ? 404 : 200;
  res.headers = { 'content-type': 'image/jpeg' };
  setImmediate(() => cb(res));
  return new EventEmitter();
};

global.__stubApps = APPS.map(([id, title]) => ({
  id, title, version: '1.0', folderPath: assets, icon: `${id}.png`, iconColor: COLORS[id] || ''
}));
require(path.join(serviceDir, 'service.js'));

/* JSON over HTTP or HTTPS, for the media-server modules (Jellyfin, Plex).
 * Resolves { status, json } and never throws for an HTTP error status;
 * network errors and timeouts reject. */
'use strict';

const http = require('http');
const https = require('https');

function requestJSON(method, url, { headers = {}, body, timeout = 10000 } = {}) {
  return new Promise((resolve, reject) => {
    let u;
    try { u = new URL(url); } catch (e) { reject(new Error(`bad URL: ${url}`)); return; }
    const mod = u.protocol === 'https:' ? https : u.protocol === 'http:' ? http : null;
    if (!mod) { reject(new Error(`unsupported URL: ${url}`)); return; }
    const data = body === undefined ? '' : typeof body === 'string' ? body : JSON.stringify(body);
    const h = Object.assign({ Accept: 'application/json', 'User-Agent': 'HomePlus (webOS)' }, headers);
    if (data) {
      if (!h['Content-Type']) h['Content-Type'] = 'application/json';
      h['Content-Length'] = Buffer.byteLength(data);
    }
    const req = mod.request(u, { method, headers: h, timeout }, (res) => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { if (buf.length < 4 * 1024 * 1024) buf += c; });
      res.on('end', () => {
        let json = null;
        try { json = buf ? JSON.parse(buf) : null; } catch (e) { /* not JSON */ }
        resolve({ status: res.statusCode, json });
      });
    });
    req.on('timeout', () => req.destroy(new Error(`timed out: ${u.host}`)));
    req.on('error', reject);
    req.end(data || undefined);
  });
}

module.exports = { requestJSON };

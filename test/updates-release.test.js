'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const https = require('https'), http = require('http'), { URL } = require('url');
const release = require('../service/update-release'), { mockServer } = require('./helpers');
test('production release reader validates a controlled HTTP feed and official routing never requests the fork', async () => {
  const APP = release.APP, version = '0.26.0', tag = 'v' + version;
  const manifestName = APP + '.manifest.json', ipkName = APP + '_' + version + '_all.ipk';
  const prefix = '/pbalazaar/home-plus/releases/download/' + tag + '/';
  const asset = (name, size) => ({ name, size, browser_download_url: 'https://github.com' + prefix + name });
  const r = { draft: false, prerelease: false, tag_name: tag, body: 'A release', assets: [asset(manifestName, 512), asset(ipkName, 100)] };
  const manifest = { id: APP, version, ipkUrl: ipkName, ipkSize: 100, ipkHash: { sha256: 'b'.repeat(64) } };
  let mode = 'normal';
  const server = await mockServer((req, u) => {
    if (mode === 'rate') return { status: 403, json: {} };
    if (mode === 'redirect') return { status: 302, headers: { Location: 'https://evil.example/payload' } };
    if (u.pathname === '/repos/pbalazaar/home-plus/releases/latest') return { json: r };
    if (u.pathname === prefix + manifestName) return { json: manifest };
    if (u.pathname === '/repos/pbalazaar/home-plus/git/ref/tags/' + tag) return { json: { object: { type: 'commit', sha: 'a'.repeat(40) } } };
  });
  const previous = https.request, base = new URL(server.base);
  https.request = (options, callback) => http.request(Object.assign({}, options, { protocol: 'http:', hostname: '127.0.0.1', port: base.port }), callback);
  try {
    const result = await release.fetchRelease(null); assert.strictEqual(result.upstream.version, version); assert.strictEqual(result.result, null);
    assert.strictEqual(server.hits.length, 3); assert(server.hits.every(h => h.path.indexOf('MoBluey') === -1));
    assert(server.hits.every(h => !h.path.endsWith('.ipk')));
    mode = 'rate'; await assert.rejects(release.fetchRelease(null), /HTTP 403/);
    mode = 'redirect'; await assert.rejects(release.download('https://github.com/pbalazaar/home-plus', 1024), /Untrusted/);
  } finally { https.request = previous; await server.close(); }
});

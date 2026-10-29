'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('http');
const https = require('https');
const { requestOptions } = require('../service/net');
const createTrakt = require('../service/trakt');
const { mockServer, configStore, quiet, until } = require('./helpers');

let server, originalRequest;
test.before(async () => {
  server = await mockServer((req, url) => {
    if (url.pathname === '/oauth/device/code') {
      return { json: { device_code: 'device-code', user_code: 'ABCD1234',
        verification_url: 'https://trakt.tv/activate', expires_in: 600, interval: 0.01 } };
    }
    if (url.pathname === '/oauth/device/token') {
      return { json: { access_token: 'access', refresh_token: 'refresh', expires_in: 86400 } };
    }
    if (url.pathname === '/oauth/token') {
      return { json: { access_token: 'access-next', refresh_token: 'refresh-next', expires_in: 86400 } };
    }
    if (url.pathname === '/sync/playback') return { json: [] };
    return null;
  });
  originalRequest = https.request;
  https.request = (opts, cb) => http.request(Object.assign({}, opts, requestOptions(server.base + opts.path)), cb);
});
test.after(async () => {
  https.request = originalRequest;
  await server.close();
});

async function signedIn(clientId, clientSecret) {
  const store = configStore();
  const trakt = createTrakt(Object.assign({}, store, { log: quiet }));
  const start = server.hits.length;
  trakt.setup(clientId, clientSecret);
  const pending = await trakt.startDevice();
  assert.equal(pending.pending.code, 'ABCD1234');
  await until(() => trakt.status().connected, 2000, 'Trakt device sign-in');
  return { trakt, store, start };
}

function bodySince(start, path) {
  const hit = server.hits.slice(start).find((h) => h.path === path);
  assert.ok(hit, `request to ${path}`);
  return JSON.parse(hit.body);
}

test('trakt: a Client ID alone signs in and refreshes without a Client Secret', async () => {
  const { trakt, store, start } = await signedIn('public-id');
  assert.deepEqual(bodySince(start, '/oauth/device/token'), { code: 'device-code', client_id: 'public-id' });
  store.writeConfig(Object.assign({}, store.get(), { traktExpires: 0 }));
  assert.deepEqual(await trakt.playback(), []);
  assert.deepEqual(bodySince(start, '/oauth/token'), {
    refresh_token: 'refresh', client_id: 'public-id',
    redirect_uri: 'http://localhost', grant_type: 'refresh_token'
  });
  assert.equal(store.get().traktRefresh, 'refresh-next');
});

test('trakt: an older app keeps its Client Secret and OOB reply URI', async () => {
  const { trakt, store, start } = await signedIn('old-id', 'old-secret');
  assert.deepEqual(bodySince(start, '/oauth/device/token'), {
    code: 'device-code', client_id: 'old-id', client_secret: 'old-secret'
  });
  store.writeConfig(Object.assign({}, store.get(), { traktExpires: 0 }));
  assert.deepEqual(await trakt.playback(), []);
  assert.deepEqual(bodySince(start, '/oauth/token'), {
    refresh_token: 'refresh', client_id: 'old-id', client_secret: 'old-secret',
    redirect_uri: 'urn:ietf:wg:oauth:2.0:oob', grant_type: 'refresh_token'
  });
});

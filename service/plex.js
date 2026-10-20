/* Plex: Continue Watching from your Plex Media Server.
 *
 * Sign-in is Plex's PIN: the TV shows a 4-character code you enter at
 * plex.tv/link while signed in to Plex, and Home+ gets a token for your
 * account. It then picks your server (one you own first), trying its local
 * address before remote ones; relay is the last resort. The row is the
 * server's Continue Watching hub (On Deck on older servers).
 */
'use strict';

const crypto = require('crypto');
const { requestJSON } = require('./net');

// The preview harness points these at a mock; on the TV they're Plex's.
const PLEX_TV = process.env.HOMEPLUS_PLEX_TV || 'https://plex.tv';
const PLEX_CLIENTS = process.env.HOMEPLUS_PLEX_CLIENTS || 'https://clients.plex.tv';

module.exports = function createPlex({ readConfig, writeConfig, log, version }) {
  let pending = null; // { id, code, expires_at }
  let error = '';
  let pollTimer = null;
  let cache = { at: 0, items: null };

  function account() {
    return readConfig().plex || null; // { token, url, server, serverToken, user }
  }

  function headers(token) {
    const cfg = readConfig();
    if (!cfg.plexClient) {
      cfg.plexClient = crypto.randomBytes(12).toString('hex');
      writeConfig(cfg);
    }
    const h = {
      'X-Plex-Product': 'Home+',
      'X-Plex-Version': version,
      'X-Plex-Client-Identifier': cfg.plexClient,
      'X-Plex-Platform': 'webOS',
      'X-Plex-Device': 'LG TV',
      'X-Plex-Device-Name': 'Home+'
    };
    if (token) h['X-Plex-Token'] = token;
    return h;
  }

  function status() {
    const a = account();
    if (pending && Date.now() > pending.expires_at) pending = null;
    return {
      connected: !!(a && a.serverToken),
      server: a ? a.server : '',
      user: a ? a.user : '',
      pending: pending ? { code: pending.code, url: 'plex.tv/link', qr: pending.qr } : null,
      error
    };
  }

  // The first server that answers: owned ones first, local addresses before remote, relay last.
  async function pickServer(token) {
    const r = await requestJSON('GET', `${PLEX_CLIENTS}/api/v2/resources?includeHttps=1&includeRelay=1`, { headers: headers(token) });
    if (r.status !== 200 || !Array.isArray(r.json)) throw new Error(`Plex didn't list your servers (${r.status})`);
    const servers = r.json.filter((d) => String(d.provides || '').split(',').includes('server'));
    servers.sort((a, b) => (b.owned ? 1 : 0) - (a.owned ? 1 : 0));
    for (const s of servers) {
      const conns = (s.connections || []).slice().sort((a, b) =>
        (a.relay ? 2 : a.local ? 0 : 1) - (b.relay ? 2 : b.local ? 0 : 1));
      for (const c of conns) {
        try {
          const id = await requestJSON('GET', `${c.uri}/identity`, { headers: headers(s.accessToken || token), timeout: 4000 });
          if (id.status === 200) return { url: c.uri.replace(/\/+$/, ''), server: s.name, serverToken: s.accessToken || token };
        } catch (e) { /* try the next address */ }
      }
    }
    return null;
  }

  async function finish(token) {
    const server = await pickServer(token);
    if (!server) {
      error = 'Signed in, but none of your Plex servers answered from this TV.';
      return;
    }
    let user = '';
    try {
      const u = await requestJSON('GET', `${PLEX_TV}/api/v2/user`, { headers: headers(token) });
      user = (u.json && (u.json.username || u.json.title)) || '';
    } catch (e) { /* the name is only for show */ }
    const cfg = readConfig();
    cfg.plex = Object.assign({ token, user }, server);
    writeConfig(cfg);
    cache = { at: 0, items: null };
    log('plex: connected to', server.server, 'at', server.url);
  }

  async function startLink() {
    error = '';
    const r = await requestJSON('POST', `${PLEX_TV}/api/v2/pins`, { headers: headers() });
    if ((r.status !== 200 && r.status !== 201) || !r.json || !r.json.code) {
      error = `Plex didn't give a code (${r.status}).`;
      return status();
    }
    const pin = r.json;
    pending = { id: pin.id, code: pin.code, qr: pin.qr || '', expires_at: Date.now() + (pin.expiresIn || 900) * 1000 };
    log('plex: link code issued');
    clearTimeout(pollTimer);
    const poll = async () => {
      const p = pending;
      if (!p || Date.now() > p.expires_at) { pending = null; return; }
      try {
        const c = await requestJSON('GET', `${PLEX_TV}/api/v2/pins/${p.id}`, { headers: headers() });
        if (c.status === 200 && c.json && c.json.authToken) {
          pending = null;
          await finish(c.json.authToken);
          return;
        }
        if (c.status === 404) {
          error = 'The code expired. Try again.';
          pending = null;
          return;
        }
      } catch (e) {
        log('plex: poll error', e.message);
      }
      pollTimer = setTimeout(poll, 3000);
    };
    pollTimer = setTimeout(poll, 3000);
    return status();
  }

  function disconnect() {
    const cfg = readConfig();
    delete cfg.plex;
    writeConfig(cfg);
    pending = null;
    error = '';
    cache = { at: 0, items: null };
    return status();
  }

  // A Plex metadata item -> the shape of Stremio's Continue Watching items.
  function toItem(a, m) {
    const ep = m.type === 'episode';
    const photo = (p, w, h) => p ? `${a.url}/photo/:/transcode?width=${w}&height=${h}&minSize=1&upscale=1&url=${encodeURIComponent(p)}&X-Plex-Token=${a.serverToken}` : '';
    const art = m.art || m.grandparentArt || '';
    const duration = m.duration || 0, offset = m.viewOffset || 0;
    return {
      id: String(m.ratingKey),
      source: 'plex',
      type: ep ? 'series' : 'movie',
      name: ep ? m.grandparentTitle || m.title : m.title,
      background: photo(art, 1920, 1080),
      // the 16:9 card: an episode's own still; a movie's thumb is a poster, so its art
      thumbnail: photo(ep ? m.thumb || art : art || m.thumb, 480, 270),
      logo: '',
      poster: '',
      season: ep ? m.parentIndex || 0 : 0,
      episode: ep ? m.index || 0 : 0,
      progress: duration ? Math.max(0, Math.min(1, offset / duration)) : 0,
      remainingMin: duration ? Math.max(1, Math.round((duration - offset) / 60000)) : 0,
      lastWatched: m.lastViewedAt ? new Date(m.lastViewedAt * 1000).toISOString() : '',
      deepLink: ''
    };
  }

  async function items() {
    if (cache.items && Date.now() - cache.at < 60 * 1000) return cache.items;
    const a = account();
    if (!a || !a.serverToken) return [];
    let r = await requestJSON('GET', `${a.url}/hubs/continueWatching?count=16`, { headers: headers(a.serverToken) });
    let list = [];
    if (r.status === 200 && r.json && r.json.MediaContainer) {
      for (const hub of r.json.MediaContainer.Hub || []) list.push(...(hub.Metadata || []));
    } else if (r.status === 404) { /* older servers: On Deck */
      r = await requestJSON('GET', `${a.url}/library/onDeck`, { headers: headers(a.serverToken) });
      if (r.status === 200 && r.json && r.json.MediaContainer) list = r.json.MediaContainer.Metadata || [];
    }
    if (r.status === 401) {
      log('plex: token rejected; sign in again');
      disconnect();
      return [];
    }
    if (r.status !== 200) throw new Error(`Plex Continue Watching failed (${r.status})`);
    cache = { at: Date.now(), items: list.slice(0, 16).map((m) => toItem(a, m)) };
    return cache.items;
  }

  return { status, startLink, disconnect, items };
};

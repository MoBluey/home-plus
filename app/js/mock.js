/* Fake app list for previewing the launcher in a desktop browser. */
(function (global) {
  'use strict';

  var APPS = [
    ['Netflix', '229,9,20', 'N'], ['YouTube', '255,0,0', '▶'], ['Stremio', '123,91,245', 'S'], ['Prime Video', '0,168,225', 'p'],
    ['Disney+', '17,60,207', 'D+'], ['Apple TV', '20,20,22', ''], ['Plex', '229,160,13', '›'],
    ['Spotify', '30,215,96', '♪'], ['Twitch', '145,70,255', 't'], ['Max', '0,40,231', 'max'],
    ['HDMI 1', '58,58,64', '⎚'], ['HDMI 2', '58,58,64', '⎚'], ['Live TV', '200,30,80', 'TV'],
    ['Web Browser', '0,122,255', '◎'], ['Photos', '255,149,0', '✿'], ['Music', '252,60,68', '♫'],
    ['Settings', '142,142,147', '⚙'], ['Jellyfin', '170,92,195', 'J'], ['Kodi', '23,178,231', 'K'],
    ['Crunchyroll', '244,117,33', 'C'], ['Paramount+', '0,100,255', 'P+'], ['Homebrew Channel', '40,40,48', '🍺'],
    ['Gallery', '88,86,214', '▦'], ['Apple Music', '250,45,72', '♬'], ['Sports', '52,199,89', '⚽']
  ];

  function icon(rgb, glyph) {
    var svg =
      '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">' +
      '<rect width="256" height="256" rx="56" fill="rgb(' + rgb + ')"/>' +
      '<text x="128" y="128" dy=".35em" text-anchor="middle" font-family="Helvetica, Arial, sans-serif" ' +
      'font-weight="700" font-size="' + (glyph.length > 2 ? 72 : 112) + '" fill="#fff">' + glyph + '</text></svg>';
    return 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
  }

  global.MockApps = APPS.map(function (a, i) {
    return {
      id: 'mock.' + a[0].toLowerCase().replace(/[^a-z0-9]+/g, ''),
      title: a[0],
      icon: icon(a[1], a[2]),
      largeIcon: icon(a[1], a[2]),
      iconColor: 'rgb(' + a[1] + ')',
      visible: true
    };
  });

  // ---- Stremio mock: a fake Continue Watching list with generated artwork ----

  function art(a, b) {
    var svg =
      '<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="720" viewBox="0 0 1280 720">' +
      '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="' + a + '"/>' +
      '<stop offset="1" stop-color="' + b + '"/></linearGradient>' +
      '<radialGradient id="r" cx=".72" cy=".35" r=".6"><stop offset="0" stop-color="#fff" stop-opacity=".35"/>' +
      '<stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient></defs>' +
      '<rect width="1280" height="720" fill="url(#g)"/><rect width="1280" height="720" fill="url(#r)"/>' +
      '<circle cx="980" cy="260" r="170" fill="#000" fill-opacity=".18"/>' + '</svg>';
    return 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
  }

  var SHOWS = [
    ['Severed Signals', 'series', '#0f2a44', '#3b7bbf', 2, 5, 0.42, 31],
    ['The Long Orbit', 'movie', '#2a0f33', '#c0508f', 0, 0, 0.67, 48],
    ['Northbound', 'series', '#13301f', '#4fae6d', 1, 8, 0.15, 44],
    ['Glass City', 'series', '#33200f', '#d08a3a', 3, 2, 0.81, 9],
    ['Midnight Kitchen', 'series', '#2b0f0f', '#bf3b3b', 1, 1, 0.3, 22],
    ['Paper Moons', 'movie', '#1d1d33', '#7a7ad0', 0, 0, 0.55, 51],
    ['Tidewater', 'series', '#0f2f33', '#3bb0bf', 4, 10, 0.9, 5]
  ];

  var connected = true;

  global.MockStremio = function (method) {
    if (method === 'stremioStatus') return Promise.resolve({ returnValue: true, connected: connected });
    if (method === 'stremioLogin') { connected = true; return Promise.resolve({ returnValue: true, connected: true }); }
    if (method === 'stremioLogout') { connected = false; return Promise.resolve({ returnValue: true }); }
    if (!connected) return Promise.resolve({ returnValue: true, connected: false, items: [] });
    return Promise.resolve({
      returnValue: true,
      connected: true,
      items: SHOWS.map(function (s, i) {
        var a = art(s[2], s[3]);
        return {
          id: 'tt00000' + i, type: s[1], name: s[0],
          poster: a, background: a, thumbnail: a, logo: '',
          season: s[4], episode: s[5], progress: s[6], remainingMin: s[7],
          deepLink: 'stremio:///detail/' + s[1] + '/tt00000' + i
        };
      })
    });
  };
})(window);

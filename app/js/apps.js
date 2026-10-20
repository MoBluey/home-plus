/* App data: listing, launching, and the user's saved layout. */
(function (global) {
  'use strict';

  var SELF = 'com.atvlauncher.app';
  var SERVICE = 'luna://com.atvlauncher.app.service';
  var APPMGR = 'luna://com.webos.applicationManager';
  var LAYOUT_KEY = 'homeplus.layout.v1';

  // System apps that make no sense on a home screen.
  var ALWAYS_HIDDEN = [SELF, 'com.webos.app.home', 'com.webos.app.homeconnect', 'com.webos.app.voice'];

  var mode = 'mock'; // 'service' | 'direct' | 'mock'

  function resolvePath(p, folder) {
    if (!p) return '';
    if (/^(\/|[a-z]+:)/i.test(p)) return p;
    return (folder ? folder.replace(/\/$/, '') + '/' : '') + p;
  }

  function parseColor(c) {
    if (!c || typeof c !== 'string') return null;
    var m = c.match(/^#?([0-9a-f]{6})$/i);
    if (m) {
      var n = parseInt(m[1], 16);
      return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
    }
    m = c.match(/rgba?\((\d+)[ ,]+(\d+)[ ,]+(\d+)/i);
    return m ? [+m[1], +m[2], +m[3]] : null;
  }

  function normalize(a) {
    var folder = a.folderPath || '';
    return {
      id: a.id,
      title: a.title || a.id,
      // the service serves icons over localhost; raw paths only work for mock/direct mode
      icon: a.iconUrl || resolvePath(a.largeIcon || a.icon, folder),
      art: a.artUrl || resolvePath(a.bgImage || a.splashBackground || '', folder),
      remote: !!a.iconUrl,
      color: parseColor(a.iconColor) || parseColor(a.bgColor)
    };
  }

  function usable(a) {
    return a && a.id && a.visible !== false && ALWAYS_HIDDEN.indexOf(a.id) < 0;
  }

  function list() {
    if (!Luna.available) {
      mode = 'mock';
      return Promise.resolve(MockApps.map(normalize));
    }
    return Luna.call(SERVICE + '/listApps', {})
      .then(function (r) { mode = 'service'; return r; })
      .catch(function () {
        return Luna.call(APPMGR + '/listApps', {}).then(function (r) { mode = 'direct'; return r; });
      })
      .then(function (r) {
        return (r.apps || []).filter(usable).map(normalize);
      });
  }

  function launch(id, appParams) {
    if (mode === 'mock') {
      console.log('[mock] launch', id, appParams || '');
      return new Promise(function (res) { setTimeout(res, 900); });
    }
    var params = { id: id };
    if (appParams) params.params = appParams;
    if (mode === 'service') {
      return Luna.call(SERVICE + '/launch', params).catch(function () {
        return Luna.call(APPMGR + '/launch', params);
      });
    }
    return Luna.call(APPMGR + '/launch', params);
  }

  // Last resort for an icon that wouldn't load: ask the service for a data: URL.
  function iconData(id) {
    if (mode !== 'service') return Promise.reject({ errorText: 'no service' });
    return Luna.call(SERVICE + '/icon', { id: id }).then(function (r) { return r.dataUrl; });
  }

  // Sends a diagnostic line to the service log (/tmp/homeplus-service.log on the TV).
  var logged = 0;
  function report(message) {
    if (mode !== 'service' || logged++ > 20) return;
    Luna.call(SERVICE + '/log', { message: message }).catch(function () {});
  }

  // ---- Stremio (network calls go through the service: no CORS, keeps the authKey off the page) ----

  function isStremio(app) {
    return !!app && (/stremio/i.test(app.id) || /stremio/i.test(app.title));
  }

  function stremio(method, params) {
    if (mode === 'mock') return MockStremio(method, params);
    if (mode !== 'service') return Promise.reject({ errorText: 'Home+ service not available' });
    return Luna.call(SERVICE + '/' + method, params || {});
  }

  // ---- layout persistence (order + hidden) ----

  function loadLayout() {
    try {
      var l = JSON.parse(localStorage.getItem(LAYOUT_KEY) || 'null');
      if (l && Array.isArray(l.order) && Array.isArray(l.hidden)) return l;
    } catch (e) { /* storage unavailable or corrupt */ }
    return { order: [], hidden: [] };
  }

  function saveLayout(layout) {
    try { localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout)); } catch (e) { /* ignore */ }
  }

  // Apply saved order; apps not in the saved order (newly installed) go to the end.
  function arrange(apps, layout) {
    var byId = {};
    apps.forEach(function (a) { byId[a.id] = a; });
    var out = [];
    layout.order.forEach(function (id) {
      if (byId[id]) { out.push(byId[id]); delete byId[id]; }
    });
    apps.forEach(function (a) { if (byId[a.id]) out.push(a); });
    return out;
  }

  global.Apps = {
    list: list,
    launch: launch,
    iconData: iconData,
    report: report,
    loadLayout: loadLayout,
    saveLayout: saveLayout,
    arrange: arrange,
    isStremio: isStremio,
    stremioStatus: function () { return stremio('stremioStatus'); },
    stremioContinue: function () { return stremio('stremioContinue'); },
    stremioLogin: function (email, password) { return stremio('stremioLogin', { email: email, password: password }); },
    stremioLogout: function () { return stremio('stremioLogout'); },
    mode: function () { return mode; }
  };
})(window);

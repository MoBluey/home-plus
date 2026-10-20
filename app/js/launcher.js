/* Home+ — tvOS-style home screen: Top Shelf, parallax glass tiles, long-press to
 * move/hide, and a Stremio "Continue Watching" shelf you can press Up into. */
(function () {
  'use strict';

  var COLS = 5;
  var LONG_PRESS_MS = 650;
  var RAIL_STEP = 334;    // card width + gap
  var RAIL_VISIBLE = 4;   // cards before the rail starts scrolling
  var DEFAULT_RGB = [70, 70, 82];

  var KEY = {
    LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, OK: 13,
    BACK: 461, BACKSPACE: 8, ESC: 27, PLAY_PAUSE: 19, PLAY: 415
  };

  var $ = function (id) { return document.getElementById(id); };
  var body = document.body;
  var stage = $('stage');
  var rowsEl = $('rows');
  var contentEl = $('content');
  var shelfLayers = [$('shelfA'), $('shelfB')];
  var railArts = [$('railArtA'), $('railArtB')];
  var railEl = document.querySelector('.rail');
  var sheetEl = $('sheet');
  var messageEl = $('message');

  var state = {
    all: [],          // every app, in saved order
    items: [],        // what is on screen: visible apps (+ the "Hidden Apps" tile)
    layout: Apps.loadLayout(),
    focus: 0,
    zone: 'grid',     // 'grid' | 'shelf' (the Stremio rail)
    tiles: [],
    moving: null,     // id of the app being moved
    sheet: null,      // { items, index, els }
    shelfLayer: 0,
    shelfFor: null,
    shelfTimer: 0,
    rail: { connected: false, items: [], index: 0, art: 0, artFor: null, sig: '' },
    prevFocus: -1,
    appsSig: '',
    ok: { down: 0, timer: 0, long: false }
  };

  // Icon colours are sampled once and remembered across launches.
  var COLOR_KEY = 'homeplus.colors.v1';
  var colorCache = {};
  try { colorCache = JSON.parse(localStorage.getItem(COLOR_KEY) || '{}') || {}; } catch (e) { colorCache = {}; }
  var colorSaveTimer = 0;
  function saveColors() {
    clearTimeout(colorSaveTimer);
    colorSaveTimer = setTimeout(function () {
      try { localStorage.setItem(COLOR_KEY, JSON.stringify(colorCache)); } catch (e) { /* ignore */ }
    }, 2000);
  }

  // ---------------------------------------------------------------- stage

  function fitStage() {
    var s = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
    stage.style.transform = s === 1 ? '' : 'scale(' + s + ')';
    stage.style.left = s === 1 ? '' : (window.innerWidth - 1920 * s) / 2 + 'px';
    stage.style.top = s === 1 ? '' : (window.innerHeight - 1080 * s) / 2 + 'px';
  }

  function tickClock() {
    var t = new Date().toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
    var el = $('clock');
    if (el.textContent !== t) el.textContent = t;
  }

  var messageTimer = 0;
  function showMessage(text, ms) {
    messageEl.textContent = text;
    messageEl.hidden = false;
    clearTimeout(messageTimer);
    if (ms) messageTimer = setTimeout(function () { messageEl.hidden = true; }, ms);
  }

  // ---------------------------------------------------------------- colour

  // Look at the icon's edges: if they're one solid colour, the tile can extend
  // it so the icon reads as full-bleed. Otherwise use a dimmed average.
  // Fails quietly if the canvas is tainted.
  function sampleIcon(img) {
    try {
      var n = 48, c = document.createElement('canvas');
      c.width = c.height = n;
      var ctx = c.getContext('2d');
      ctx.drawImage(img, 0, 0, n, n);
      var d = ctx.getImageData(0, 0, n, n).data;

      var edge = [], all = [0, 0, 0, 0];
      var inset = 10; // clear of the rounded corners most icons have
      for (var y = 0; y < n; y++) {
        for (var x = 0; x < n; x++) {
          var i = (y * n + x) * 4;
          if (d[i + 3] < 220) continue;
          all[0] += d[i]; all[1] += d[i + 1]; all[2] += d[i + 2]; all[3]++;
          var onRing = (x === inset || x === n - 1 - inset || y === inset || y === n - 1 - inset) &&
            x >= inset && x <= n - 1 - inset && y >= inset && y <= n - 1 - inset;
          if (onRing) edge.push([d[i], d[i + 1], d[i + 2]]);
        }
      }
      var ringSize = 4 * (n - 2 * inset) - 4;
      if (edge.length > ringSize * 0.85) {
        var m = [0, 0, 0];
        edge.forEach(function (p) { m[0] += p[0]; m[1] += p[1]; m[2] += p[2]; });
        m = m.map(function (v) { return v / edge.length; });
        var dev = 0;
        edge.forEach(function (p) {
          dev += Math.abs(p[0] - m[0]) + Math.abs(p[1] - m[1]) + Math.abs(p[2] - m[2]);
        });
        if (dev / edge.length < 30) return { rgb: m.map(Math.round), fullbleed: true };
      }
      if (all[3]) {
        return {
          rgb: [all[0], all[1], all[2]].map(function (v) { return Math.round((v / all[3]) * 0.6); }),
          fullbleed: false
        };
      }
    } catch (e) { /* tainted canvas (file:// icons on some builds) */ }
    return null;
  }

  function rgbStr(rgb) { return rgb.join(','); }

  function appColor(app) {
    var s = colorCache[app.icon];
    if (s && s.fullbleed) return s.rgb;
    return app.color || (s && s.rgb) || DEFAULT_RGB;
  }

  // ---------------------------------------------------------------- render

  function rebuildItems() {
    var hidden = state.layout.hidden;
    state.items = state.all.filter(function (a) { return hidden.indexOf(a.id) < 0; });
    if (hidden.some(function (id) { return state.all.some(function (a) { return a.id === id; }); })) {
      state.items.push({ id: '__hidden__', title: 'Hidden Apps', special: true });
    }
    if (state.focus >= state.items.length) state.focus = Math.max(0, state.items.length - 1);
  }

  function makeTile(app, index) {
    var tile = document.createElement('div');
    tile.className = 'tile' + (app.special ? ' special' : '');
    tile.innerHTML =
      '<div class="tile-card"></div>' +
      '<div class="tile-label"></div>';
    var card = tile.querySelector('.tile-card');
    tile.querySelector('.tile-label').textContent = app.title;

    function asText() {
      var t = document.createElement('div');
      t.className = 'tile-text';
      t.textContent = app.special ? '• • •   Hidden' : app.title;
      card.insertBefore(t, card.firstChild);
    }

    if (app.special || !app.icon) {
      asText();
    } else {
      var img = new Image();
      img.className = 'tile-icon';
      img.decoding = 'async';
      if (app.remote) img.crossOrigin = 'anonymous'; // service sends CORS headers, so colours can be sampled
      var cached = colorCache[app.icon];
      if (cached && cached.fullbleed) tile.classList.add('fullbleed');
      img.onload = function () {
        if (colorCache[app.icon] !== undefined) return; // colour already known: nothing to repaint
        colorCache[app.icon] = sampleIcon(img);
        saveColors();
        var s = colorCache[app.icon];
        if (s && s.fullbleed) tile.classList.add('fullbleed');
        card.style.setProperty('--c', rgbStr(appColor(app)));
        if (index === state.focus) { state.shelfFor = null; scheduleShelf(); }
      };
      var triedData = false;
      img.onerror = function () {
        if (!triedData) {
          triedData = true;
          Apps.report('icon failed: ' + app.id + ' ' + String(app.icon).slice(0, 120));
          img.removeAttribute('crossorigin');
          Apps.iconData(app.id).then(function (url) { img.src = url; }).catch(function (e) {
            Apps.report('icon data failed: ' + app.id + ' ' + ((e && e.errorText) || ''));
            img.remove();
            asText();
          });
          return;
        }
        img.remove();
        asText();
      };
      img.src = app.icon;
      card.insertBefore(img, card.firstChild);
      card.style.setProperty('--c', rgbStr(appColor(app)));
    }

    tile.addEventListener('mouseenter', function () {
      if (state.sheet || state.moving) return;
      state.zone = 'grid';
      focusAt(index, true);
    });
    tile.addEventListener('click', function () { if (!state.sheet) activate(); });
    return tile;
  }

  function render() {
    rebuildItems();
    rowsEl.innerHTML = '';
    state.tiles = [];
    var row;
    state.items.forEach(function (app, i) {
      if (i % COLS === 0) {
        row = document.createElement('div');
        row.className = 'row';
        rowsEl.appendChild(row);
      }
      var t = makeTile(app, i);
      state.tiles.push(t);
      row.appendChild(t);
    });
    state.prevFocus = -1;
    applyFocus(false);
  }

  // ---------------------------------------------------------------- focus

  function focusAt(i, fromPointer) {
    if (i < 0 || i >= state.items.length) return;
    if (i === state.focus) { applyFocus(fromPointer); return; }
    state.focus = i;
    applyFocus(fromPointer);
  }

  function setBodyClass(name, on) {
    if (body.classList.contains(name) !== on) body.classList.toggle(name, on);
  }

  function applyFocus(fromPointer) {
    // Only the previously focused tile and the new one change.
    var prev = state.tiles[state.prevFocus];
    if (prev && state.prevFocus !== state.focus) {
      prev.classList.remove('focused', 'moving');
    }
    var cur = state.tiles[state.focus];
    if (cur) {
      cur.classList.add('focused');
      cur.classList.toggle('moving', !!state.moving);
    }
    state.prevFocus = state.focus;

    var row = Math.floor(state.focus / COLS);
    setBodyClass('scrolled', row > 0);
    setBodyClass('zone-shelf', state.zone === 'shelf');
    // Pointer users get a still grid; only scroll if the focused row is off-screen.
    if (!fromPointer || !rowVisible(row)) scrollToRow(row);
    scheduleShelf();
    paintRail();
  }

  var scrollRow = -1;
  function scrollToRow(row) {
    if (row === scrollRow) return;
    scrollRow = row;
    var y = row === 0 ? 0 : 380 - (740 + row * 290);
    contentEl.style.transform = 'translateY(' + y + 'px)';
  }
  function rowVisible(row) {
    return row === scrollRow || (scrollRow > 0 && row > 0 && Math.abs(row - scrollRow) <= 1);
  }

  function focusedApp() { return state.items[state.focus]; }

  function railActive() {
    return state.focus < COLS && Apps.isStremio(focusedApp()) && state.rail.items.length > 0;
  }

  function move(dx, dy) {
    if (state.zone === 'shelf') return moveRail(dx, dy);

    var n = state.items.length;
    var i = state.focus;
    var next = i;
    if (dx) {
      next = i + dx;
      // don't wrap across rows with left/right
      if (next < 0 || next >= n || Math.floor(next / COLS) !== Math.floor(i / COLS)) return;
    }
    if (dy > 0) {
      next = i + COLS;
      if (next >= n) {
        if (Math.floor((n - 1) / COLS) > Math.floor(i / COLS)) next = n - 1; // partial last row
        else return;
      }
    }
    if (dy < 0) {
      if (i < COLS) {
        // Up from the top row goes into the Stremio shelf, like tvOS' Top Shelf.
        if (!state.moving && railActive()) { state.zone = 'shelf'; applyFocus(false); }
        return;
      }
      next = i - COLS;
    }
    if (state.moving) { moveApp(next); return; }
    state.focus = next;
    applyFocus(false);
  }

  // ---------------------------------------------------------------- top shelf

  function scheduleShelf() {
    clearTimeout(state.shelfTimer);
    var app = focusedApp();
    setBodyClass('rail-on', railActive());
    if (state.focus >= COLS || !app || app.special || railActive()) return;
    state.shelfTimer = setTimeout(function () { showShelf(app); }, 180);
  }

  function showShelf(app) {
    if (state.shelfFor === app.id) return;
    state.shelfFor = app.id;

    var next = shelfLayers[1 - state.shelfLayer];
    var cur = shelfLayers[state.shelfLayer];
    var rgb = rgbStr(appColor(app));
    var sub = Apps.isStremio(app) && !state.rail.connected
      ? 'Hold OK to connect your Stremio account'
      : 'Press OK to open · Hold for options';

    function commit(html) {
      if (state.shelfFor !== app.id) return; // focus moved on while art loaded
      next.innerHTML = html;
      next.querySelector('.shelf-title').textContent = app.title;
      next.querySelector('.shelf-sub').textContent = sub;
      next.classList.add('on');
      cur.classList.remove('on');
      state.shelfLayer = 1 - state.shelfLayer;
    }

    var text = '<div class="shelf-scrim"></div><div class="shelf-text"><div class="shelf-title"></div><div class="shelf-sub"></div></div>';
    var icon = escapeAttr(app.icon);
    var generated =
      '<div class="shelf-gradient" style="--c:' + rgb + '">' +
      (app.icon ? '<img class="shelf-icon" src="' + icon + '"><div class="shelf-lens"></div>' : '') +
      '</div>' + text;

    if (!app.art) { commit(generated); return; }
    var probe = new Image();
    probe.onload = function () {
      commit('<div class="shelf-art" style="background-image:url(\'' + escapeAttr(app.art) + '\')"></div>' + text);
    };
    probe.onerror = function () { commit(generated); };
    probe.src = app.art;
  }

  function escapeAttr(s) {
    return String(s || '').replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/'/g, '%27').replace(/</g, '&lt;');
  }

  // ---------------------------------------------------------------- Stremio rail

  function loadRail() {
    var stremioApp = state.all.filter(Apps.isStremio)[0];
    if (!stremioApp) return Promise.resolve();
    return Apps.stremioContinue().then(function (r) {
      state.rail.connected = !!r.connected;
      var items = r.items || [];
      var sig = items.map(function (it) { return it.id + ':' + it.progress; }).join('|');
      state.rail.items = items;
      if (state.rail.index >= items.length) state.rail.index = 0;
      if (sig === state.rail.sig) return;
      state.rail.sig = sig;
      state.rail.artFor = null;
      renderRail();
      if (!railActive() && state.zone === 'shelf') state.zone = 'grid';
      state.shelfFor = null;
      applyFocus(true);
    }).catch(function () {
      state.rail.connected = false;
      state.rail.items = [];
    });
  }

  function renderRail() {
    railEl.innerHTML = '';
    state.rail.items.forEach(function (it, i) {
      var card = document.createElement('div');
      card.className = 'rail-card';
      if (it.thumbnail || it.background || it.poster) {
        card.style.backgroundImage = 'url("' + (it.thumbnail || it.background || it.poster).replace(/"/g, '%22') + '")';
      }
      card.innerHTML = '<div class="rail-name"></div><div class="rail-progress"><b></b></div>';
      card.querySelector('.rail-name').textContent = it.name;
      card.querySelector('.rail-progress b').style.width = Math.round((it.progress || 0) * 100) + '%';
      card.addEventListener('mouseenter', function () {
        if (state.sheet) return;
        state.zone = 'shelf';
        state.rail.index = i;
        applyFocus(true);
      });
      card.addEventListener('click', function () { openRailItem(); });
      railEl.appendChild(card);
    });
  }

  function railMeta(it) {
    var left = it.remainingMin ? it.remainingMin + ' min left' : '';
    var parts = it.type === 'series' && it.season
      ? ['Season ' + it.season, 'Episode ' + it.episode]
      : [it.type === 'movie' ? 'Movie' : ''];
    if (left) parts.push(left);
    return parts.filter(Boolean).join('  ·  ');
  }

  function paintRail() {
    var r = state.rail;
    if (!railActive()) return;
    var cards = railEl.children;
    for (var i = 0; i < cards.length; i++) {
      cards[i].classList.toggle('focused', state.zone === 'shelf' && i === r.index);
    }
    var shift = Math.max(0, r.index - (RAIL_VISIBLE - 1)) * RAIL_STEP;
    railEl.style.transform = 'translateX(' + -shift + 'px)';

    var it = r.items[r.index];
    var title = document.querySelector('.rail-title');
    var logo = document.querySelector('.rail-logo');
    title.textContent = it.name;
    document.querySelector('.rail-meta').textContent = railMeta(it);
    title.style.display = '';
    logo.style.display = 'none';
    if (it.logo) {
      logo.onload = function () { logo.style.display = ''; title.style.display = 'none'; };
      logo.onerror = function () { logo.style.display = 'none'; title.style.display = ''; };
      logo.src = it.logo;
    }

    var bg = it.background || it.thumbnail || it.poster;
    if (bg && r.artFor !== bg) {
      r.artFor = bg;
      var next = railArts[1 - r.art];
      next.style.backgroundImage = 'url("' + bg.replace(/"/g, '%22') + '")';
      next.classList.add('on');
      railArts[r.art].classList.remove('on');
      r.art = 1 - r.art;
    }
  }

  function moveRail(dx, dy) {
    var r = state.rail;
    if (dy > 0) { state.zone = 'grid'; applyFocus(false); return; }
    if (dx) {
      var n = r.index + dx;
      if (n < 0 || n >= r.items.length) return;
      r.index = n;
      paintRail();
    }
  }

  function openRailItem() {
    var it = state.rail.items[state.rail.index];
    var app = state.all.filter(Apps.isStremio)[0];
    if (!it || !app) return;
    launchApp(app, it.deepLink ? { contentTarget: it.deepLink } : null, railEl.children[state.rail.index]);
  }

  function openStremioLogin() {
    openSheet('Connect Stremio', [
      { input: 'email', type: 'email', placeholder: 'Email' },
      { input: 'password', type: 'password', placeholder: 'Password' },
      {
        label: 'Sign In',
        run: function (v) {
          if (!v.email || !v.password) { showMessage('Enter your Stremio email and password.', 3000); return; }
          showMessage('Signing in to Stremio…');
          Apps.stremioLogin(v.email, v.password).then(function () {
            showMessage('Stremio connected', 2500);
            loadRail();
          }).catch(function (e) {
            showMessage('Stremio sign-in failed' + (e && e.errorText ? ': ' + e.errorText : ''), 4500);
          });
        }
      },
      { label: 'Cancel', run: function () {} }
    ], 'Your password goes straight to api.strem.io. Home+ keeps only the session key, on this TV.');
  }

  // ---------------------------------------------------------------- actions

  function activate() {
    if (state.zone === 'shelf') { openRailItem(); return; }
    var app = focusedApp();
    if (!app) return;
    if (state.moving) { endMove(); return; }
    if (app.special) { openHiddenSheet(); return; }
    launchApp(app, null, state.tiles[state.focus]);
  }

  function launchApp(app, params, el) {
    if (el) {
      el.classList.add('pressed');
      setTimeout(function () { el.classList.remove('pressed'); }, 160);
    }
    body.classList.add('launching');
    var fallback = setTimeout(endLaunching, 4000);

    Apps.launch(app.id, params).then(function () {
      if (Apps.mode() === 'mock') { clearTimeout(fallback); endLaunching(); }
    }).catch(function (err) {
      clearTimeout(fallback);
      endLaunching();
      showMessage('Couldn’t open ' + app.title + (err && err.errorText ? ': ' + err.errorText : ''), 4000);
    });
  }

  function endLaunching() { body.classList.remove('launching'); }

  function openAppSheet() {
    var app = focusedApp();
    if (!app || app.special || state.zone !== 'grid') return;
    var items = [
      { label: 'Move', run: startMove },
      { label: 'Move to Front', run: function () { moveApp(0); endMove(); } }
    ];
    if (Apps.isStremio(app)) {
      items.push(state.rail.connected
        ? { label: 'Disconnect Stremio Account', run: function () { Apps.stremioLogout().then(loadRail); } }
        : { label: 'Connect Stremio Account', run: openStremioLogin });
    }
    items.push({ label: 'Hide App', run: function () { hideApp(app.id); }, destructive: true });
    items.push({ label: 'Cancel', run: function () {} });
    openSheet(app.title, items);
  }

  function openHiddenSheet() {
    var hidden = state.all.filter(function (a) { return state.layout.hidden.indexOf(a.id) >= 0; });
    var items = hidden.map(function (a) {
      return { label: 'Show “' + a.title + '”', run: function () { unhide([a.id]); } };
    });
    if (hidden.length > 1) {
      items.push({ label: 'Show All', run: function () { unhide(hidden.map(function (a) { return a.id; })); } });
    }
    items.push({ label: 'Cancel', run: function () {} });
    openSheet('Hidden Apps', items);
  }

  function hideApp(id) {
    state.layout.hidden.push(id);
    persist();
    render();
  }

  function unhide(ids) {
    state.layout.hidden = state.layout.hidden.filter(function (id) { return ids.indexOf(id) < 0; });
    persist();
    render();
  }

  function startMove() {
    state.moving = focusedApp().id;
    applyFocus(false);
  }

  function moveApp(to) {
    var visible = state.items.filter(function (a) { return !a.special; });
    var from = state.focus;
    if (to >= visible.length) to = visible.length - 1;
    if (to === from || to < 0) return;
    var app = visible.splice(from, 1)[0];
    visible.splice(to, 0, app);
    var hidden = state.all.filter(function (a) { return state.layout.hidden.indexOf(a.id) >= 0; });
    state.all = visible.concat(hidden);
    state.focus = to;
    render();
  }

  function endMove() {
    state.moving = null;
    persist();
    applyFocus(false);
  }

  function persist() {
    state.layout.order = state.all.map(function (a) { return a.id; });
    Apps.saveLayout(state.layout);
  }

  // ---------------------------------------------------------------- sheet

  function openSheet(title, items, note) {
    var list = sheetEl.querySelector('.sheet-items');
    var noteEl = sheetEl.querySelector('.sheet-note');
    sheetEl.querySelector('.sheet-title').textContent = title;
    noteEl.textContent = note || '';
    noteEl.hidden = !note;
    list.innerHTML = '';
    var els = items.map(function (it, i) {
      var el = document.createElement('div');
      el.className = 'sheet-item' + (it.destructive ? ' destructive' : '');
      if (it.input) {
        var input = document.createElement('input');
        input.type = it.type || 'text';
        input.name = it.input;
        input.placeholder = it.placeholder || '';
        input.autocomplete = 'off';
        el.appendChild(input);
      } else {
        el.textContent = it.label;
      }
      el.addEventListener('mouseenter', function () { if (state.sheet) { state.sheet.index = i; paintSheet(); } });
      el.addEventListener('click', function () { if (state.sheet) { state.sheet.index = i; chooseSheet(); } });
      list.appendChild(el);
      return el;
    });
    state.sheet = { items: items, index: 0, els: els };
    sheetEl.hidden = false;
    paintSheet();
  }

  function paintSheet() {
    var s = state.sheet;
    s.els.forEach(function (el, i) { el.classList.toggle('focused', i === s.index); });
    var f = s.els[s.index];
    if (f) sheetEl.querySelector('.sheet-items').scrollTop = Math.max(0, f.offsetTop - 300);
  }

  function sheetValues() {
    var v = {};
    state.sheet.els.forEach(function (el) {
      var input = el.querySelector('input');
      if (input) v[input.name] = input.value.trim();
    });
    return v;
  }

  function chooseSheet() {
    var s = state.sheet;
    var it = s.items[s.index];
    if (!it) return;
    if (it.input) { s.els[s.index].querySelector('input').focus(); return; } // opens the webOS keyboard
    var values = sheetValues();
    closeSheet();
    it.run(values);
  }

  function closeSheet() {
    if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
    state.sheet = null;
    sheetEl.hidden = true;
  }

  function sheetStep(d) {
    var s = state.sheet;
    var n = s.index + d;
    if (n < 0 || n >= s.items.length) return;
    s.index = n;
    paintSheet();
  }

  // ---------------------------------------------------------------- input

  function onKeyDown(e) {
    setBodyClass('pointer', false);
    var k = e.keyCode;

    // Typing in a sheet field: let the keyboard work, OK/arrows leave the field.
    if (document.activeElement && document.activeElement.tagName === 'INPUT') {
      if (k === KEY.OK || k === KEY.DOWN || k === KEY.UP) {
        e.preventDefault();
        document.activeElement.blur();
        sheetStep(k === KEY.UP ? -1 : 1);
      } else if (k === KEY.BACK || k === KEY.ESC) {
        document.activeElement.blur();
      }
      return;
    }

    if (k === KEY.OK) {
      e.preventDefault();
      if (e.repeat || state.ok.down) return;
      state.ok.down = Date.now();
      state.ok.long = false;
      if (!state.sheet && !state.moving && state.zone === 'grid') {
        var t = state.tiles[state.focus];
        if (t) t.classList.add('pressed');
        state.ok.timer = setTimeout(function () {
          state.ok.long = true;
          if (t) t.classList.remove('pressed');
          openAppSheet();
        }, LONG_PRESS_MS);
      }
      return;
    }

    if (state.sheet) {
      if (k === KEY.UP) sheetStep(-1);
      else if (k === KEY.DOWN) sheetStep(1);
      else if (isBack(k)) closeSheet();
      e.preventDefault();
      return;
    }

    switch (k) {
      case KEY.LEFT: move(-1, 0); break;
      case KEY.RIGHT: move(1, 0); break;
      case KEY.UP: move(0, -1); break;
      case KEY.DOWN: move(0, 1); break;
      case KEY.PLAY:
      case KEY.PLAY_PAUSE:
        if (!state.moving) openAppSheet();
        break;
      default:
        if (!isBack(k)) return;
        // Back never exits the home screen: leave the shelf / move mode, else jump to the top.
        if (state.zone === 'shelf') { state.zone = 'grid'; applyFocus(false); }
        else if (state.moving) endMove();
        else if (state.focus !== 0) { state.focus = 0; applyFocus(false); }
    }
    e.preventDefault();
  }

  function onKeyUp(e) {
    if (e.keyCode !== KEY.OK || !state.ok.down) return;
    e.preventDefault();
    clearTimeout(state.ok.timer);
    var wasLong = state.ok.long;
    state.ok.down = 0;
    state.tiles.forEach(function (t) { t.classList.remove('pressed'); });
    if (wasLong) return;
    if (state.sheet) chooseSheet();
    else activate();
  }

  function isBack(k) { return k === KEY.BACK || k === KEY.BACKSPACE || k === KEY.ESC; }

  var wheelLock = 0;
  function onWheel(e) {
    if (state.sheet || Date.now() < wheelLock) return;
    wheelLock = Date.now() + 250;
    move(0, e.deltaY > 0 ? 1 : -1);
  }

  // ---------------------------------------------------------------- lifecycle

  function load() {
    var keep = focusedApp();
    return Apps.list().then(function (apps) {
      var sig = apps.map(function (a) { return a.id + '|' + a.icon; }).join(',');
      if (sig === state.appsSig) {
        applyFocus(false); // same apps as last time: keep the DOM and decoded images
        return loadRail();
      }
      state.appsSig = sig;
      state.all = Apps.arrange(apps, state.layout);
      rebuildItems();
      var i = keep ? state.items.findIndex(function (a) { return a.id === keep.id; }) : -1;
      state.focus = i >= 0 ? i : Math.min(state.focus, Math.max(0, state.items.length - 1));
      state.shelfFor = null;
      render();
      if (!apps.length) showMessage('No apps found.');
      return loadRail();
    }).catch(function (err) {
      showMessage('Couldn’t read the app list (' + ((err && err.errorText) || 'unknown error') +
        '). Run the Home+ service elevation step from the README, then reopen.');
    });
  }

  // Pressing Home (once it's routed here) or relaunching returns to the top, like tvOS.
  function onRelaunch() {
    closeSheet();
    if (state.moving) endMove();
    endLaunching();
    state.zone = 'grid';
    state.focus = 0;
    load();
  }

  function init() {
    fitStage();
    tickClock();
    setInterval(tickClock, 10000);

    window.addEventListener('resize', fitStage);
    document.addEventListener('keydown', onKeyDown);
    document.addEventListener('keyup', onKeyUp);
    document.addEventListener('wheel', onWheel, { passive: true });
    document.addEventListener('mousemove', function () { setBodyClass('pointer', true); });
    document.addEventListener('cursorStateChange', function (e) {
      setBodyClass('pointer', !!(e.detail && e.detail.visibility));
    });
    document.addEventListener('webOSRelaunch', onRelaunch);
    document.addEventListener('visibilitychange', function () {
      if (!document.hidden) { endLaunching(); load(); }
    });

    // Debug/preview hook: ?state=shelf|sheet|scrolled
    var q = (location.search.match(/state=(\w+)/) || [])[1];
    load().then(function () {
      var s = state.all.findIndex(Apps.isStremio);
      if (q === 'shelf' && s >= 0) { state.focus = s; state.zone = 'shelf'; state.rail.index = 1; applyFocus(false); }
      if (q === 'grid' && s >= 0) { state.focus = s; applyFocus(false); }
      if (q === 'scrolled') { state.focus = 7; applyFocus(false); }
      if (q === 'sheet') openAppSheet();
      if (q === 'login') openStremioLogin();
    });
  }

  init();
})();

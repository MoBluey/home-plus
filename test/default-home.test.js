'use strict';

const { describe, test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const trial = require('../tools/default-home-trial');
const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, '../native/appinfo.json'), 'utf8'));
const APP = manifest.id;
const STOCK = 'com.webos.app.home';

/* SAM's registry and default setting, with the documented category check. The trial uses
 * the package's real manifest; accepting a setting does not make a non-home app launchable. */
function sam(options = {}) {
  const state = { home: options.noDefault ? undefined : options.alreadyDefault ? APP : STOCK, browser: 'browser', foreground: options.alreadyInFront ? APP : 'netflix', launched: false, calls: [] };
  const luna = async (uri, params) => {
    state.calls.push({ uri, params });
    const method = uri.slice(uri.lastIndexOf('/') + 1);
    if (method === 'getSystemSettings') {
      assert.equal(params.category, 'general');
      assert.deepEqual(params.keys, ['defaultApps']);
      if (options.unreadable || (options.unreadableAfterSet && state.home === APP) ||
          (options.unreadableAfterLaunch && state.launched)) return { returnValue: false };
      return { returnValue: true, settings: { defaultApps: Object.assign({ browser: state.browser }, state.home === undefined ? {} : { home: state.home }) } };
    }
    if (method === 'setSystemSettings') {
      state.home = params.settings.defaultApps.home;
      state.browser = params.settings.defaultApps.browser;
      return { returnValue: true };
    }
    if (method === 'getAppInfo') {
      if (params.id === STOCK) return { returnValue: !options.noStock, appInfo: { id: STOCK, appCategory: ['home'] } };
      return { returnValue: true, appInfo: Object.assign({}, manifest, options.noCategory ? { appCategory: [] } : {}) };
    }
    if (method === 'setDefaultApp') {
      assert.equal(params.category, 'home');
      if (options.unsupported) return { returnValue: false, errorText: 'Unknown method' };
      if (options.restoreFails && params.appId === STOCK) return { returnValue: false, errorText: 'Restore refused' };
      if (!(options.ignoreSet && params.appId === APP)) state.home = params.appId;
      return { returnValue: true };
    }
    if (method === 'launchDefaultApp') {
      assert.deepEqual(params, { category: 'home' });
      state.launched = true;
      if (options.launchFails) return { returnValue: false, errorText: 'Launch failed' };
      if (options.otherHome) state.home = 'another.launcher';
      if (options.changeBrowser) state.browser = 'another.browser';
      if (!options.wrongTarget) state.foreground = APP;
      return { returnValue: true };
    }
    if (method === 'getForegroundAppInfo') return { returnValue: !options.foregroundUnreadable, appId: state.foreground };
    throw new Error('Unexpected Luna method: ' + uri);
  };
  return { state, luna, changed: () => state.calls.filter((x) => x.uri.endsWith('/setDefaultApp')) };
}

describe('local default Home trial (not a firmware compatibility check)', () => {
  test('uses the packaged home category and restores the previous default', async () => {
    assert.ok(manifest.appCategory.includes('home'));
    const s = sam();
    assert.deepEqual(await trial(s.luna), { launched: APP, previous: STOCK });
    assert.equal(s.state.home, STOCK);
    assert.deepEqual(s.changed().map((x) => x.params.appId), [APP, STOCK]);
  });
  for (const [name, options, message] of [
    ['unreadable previous setting', { unreadable: true }, /Cannot read/],
    ['unregistered home category', { noCategory: true }, /not registered/],
    ['absent LG recovery app', { noStock: true }, /recovery app/],
    ['Home+ already in front', { alreadyInFront: true, wrongTarget: true }, /another app/],
    ['unreadable starting foreground', { foregroundUnreadable: true }, /another app/]
  ]) {
    test(name + ': changes nothing', async () => {
      const s = sam(options);
      await assert.rejects(trial(s.luna), message);
      assert.equal(s.changed().length, 0);
      assert.equal(s.state.home, STOCK);
    });
  }
  for (const [name, options, message] of [
    ['unsupported API', { unsupported: true }, /Unknown method/],
    ['ignored default setting', { ignoreSet: true }, /did not keep/],
    ['failed launcher', { launchFails: true }, /Launch failed/],
    ['wrong foreground target', { wrongTarget: true }, /did not reach/]
  ]) {
    test(name + ': does not leave Home+ as default', async () => {
      const s = sam(options);
      await assert.rejects(trial(s.luna, { timeout: 0 }), message);
      assert.equal(s.state.home, STOCK);
    });
  }
  test('does not overwrite another Home selected during the trial', async () => {
    const s = sam({ otherHome: true });
    await trial(s.luna);
    assert.equal(s.state.home, 'another.launcher');
    assert.deepEqual(s.changed().map((x) => x.params.appId), [APP]);
  });
  for (const options of [
    { unreadableAfterSet: true },
    { unreadableAfterLaunch: true },
    { otherHome: true, unreadableAfterLaunch: true }
  ]) {
    test('reports unresolved recovery instead of guessing ownership: ' + JSON.stringify(options), async () => {
      const s = sam(options);
      await assert.rejects(trial(s.luna), (err) => {
        assert.match(err.message, /Recovery unconfirmed/);
        assert.equal(err.restoreTarget, STOCK);
        return true;
      });
      assert.equal(s.state.home, options.otherHome ? 'another.launcher' : APP);
      assert.deepEqual(s.changed().map((x) => x.params.appId), [APP]);
    });
  }
  test('reports refused restoration instead of claiming a successful trial', async () => {
    const s = sam({ restoreFails: true });
    await assert.rejects(trial(s.luna), /Restore refused/);
    assert.equal(s.state.home, APP);
  });
  test('preserves Home+ if it was already the default before the trial', async () => {
    const s = sam({ alreadyDefault: true });
    await trial(s.luna);
    assert.equal(s.state.home, APP);
    assert.equal(s.changed().length, 0);
  });
  test('restores an originally absent Home entry and keeps other live defaults', async () => {
    const s = sam({ noDefault: true, changeBrowser: true });
    await trial(s.luna);
    assert.equal(s.state.home, undefined);
    assert.equal(s.state.browser, 'another.browser');
    assert.ok(s.state.calls.some((c) => c.uri.endsWith('/setSystemSettings')));
  });
});

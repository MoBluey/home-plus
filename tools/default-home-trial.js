/* Local proof of the default-Home lifecycle. This is not installed or enabled by the package.
 * The caller supplies Luna and the deadline; a TV trial also needs an independent rollback
 * before invoking this helper. A successful setDefaultApp reply alone is not acceptance. */
'use strict';

const APP = 'com.github.pbalazaar.homeplus';
const STOCK = 'com.webos.app.home';
const MANAGER = 'luna://com.webos.applicationManager/';
const SETTINGS = 'luna://com.webos.settingsservice/getSystemSettings';

module.exports = async function defaultHomeTrial(luna, options = {}) {
  const sleep = options.sleep || ((ms) => new Promise((resolve) => setTimeout(resolve, ms)));
  const timeout = options.timeout === undefined ? 5000 : options.timeout;
  const now = options.now || Date.now;
  async function defaults() {
    const r = await luna(SETTINGS, { category: 'general', keys: ['defaultApps'] });
    const apps = r && r.settings && r.settings.defaultApps;
    if (!r || r.returnValue !== true || !apps || typeof apps.home !== 'string' || !apps.home) {
      throw new Error('Cannot read the default Home setting');
    }
    return apps.home;
  }
  async function set(appId) {
    const r = await luna(MANAGER + 'setDefaultApp', { category: 'home', appId });
    if (!r || r.returnValue !== true) throw new Error((r && r.errorText) || 'setDefaultApp failed');
  }
  const before = await defaults();
  const info = await luna(MANAGER + 'getAppInfo', { id: APP });
  const categories = info && info.appInfo && info.appInfo.appCategory;
  if (!info || info.returnValue !== true || !Array.isArray(categories) || categories.indexOf('home') === -1) {
    throw new Error('Home+ is not registered in the home category; nothing was switched');
  }
  const stock = await luna(MANAGER + 'getAppInfo', { id: STOCK });
  if (!stock || stock.returnValue !== true) throw new Error('LG Home recovery app is unavailable; nothing was switched');
  const initial = await luna(MANAGER + 'getForegroundAppInfo', {});
  if (!initial || initial.returnValue !== true || !initial.appId || initial.appId === APP) {
    throw new Error('Start the trial from another app; nothing was switched');
  }
  let attempted = false;
  try {
    attempted = before !== APP;
    if (attempted) await set(APP);
    if (await defaults() !== APP) throw new Error('webOS did not keep Home+ as the default');
    const opened = await luna(MANAGER + 'launchDefaultApp', { category: 'home' });
    if (!opened || opened.returnValue !== true) {
      throw new Error((opened && opened.errorText) || 'Default Home could not launch');
    }
    const deadline = now() + timeout;
    for (;;) {
      const foreground = await luna(MANAGER + 'getForegroundAppInfo', {});
      if (foreground && foreground.returnValue === true && foreground.appId === APP) {
        return { launched: APP, previous: before };
      }
      if (now() >= deadline) throw new Error('Default Home did not reach the foreground');
      await sleep(50);
    }
  } finally {
    if (attempted) {
      // Do not overwrite a different Home selected while the trial was running.
      let current;
      try { current = await defaults(); } catch (e) {
        const failed = new Error('Recovery unconfirmed: cannot read the current default Home');
        failed.restoreTarget = before; // for the independent recovery path; not permission to overwrite another Home
        throw failed;
      }
      if (current === APP) {
        await set(before);
        if (await defaults() !== before) throw new Error('Previous Home was not restored');
      }
    }
  }
};

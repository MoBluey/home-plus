// Minimal stand-in for webOS's webos-service module (preview harness only).
const { EventEmitter } = require('events');
module.exports = class Service {
  constructor() { this.activityManager = { create() {} }; }
  register() {}
  subscribe(uri) { // the app in front: HOMEPLUS_PREVIEW_FOREGROUND, once the service asks
    const sub = new EventEmitter();
    const app = process.env.HOMEPLUS_PREVIEW_FOREGROUND;
    if (app && uri.endsWith('/getForegroundAppInfo')) setImmediate(() => sub.emit('response', { payload: { appId: app } }));
    return sub;
  }
  call(uri, params, cb) {
    if (uri.endsWith('/launch')) return cb({ payload: { returnValue: true } });
    const fake = global.__stubLuna && global.__stubLuna(uri, params); // the harness's stand-in TV
    if (fake) return cb({ payload: fake });
    cb({ payload: { returnValue: true, apps: global.__stubApps || [] } });
  }
};

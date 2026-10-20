// Minimal stand-in for webOS's webos-service module (preview harness only).
const { EventEmitter } = require('events');
module.exports = class Service {
  constructor() { this.activityManager = { create() {} }; }
  register() {}
  subscribe() { return new EventEmitter(); }
  call(uri, params, cb) {
    if (uri.endsWith('/launch')) return cb({ payload: { returnValue: true } });
    const fake = global.__stubLuna && global.__stubLuna(uri, params); // the harness's stand-in TV
    if (fake) return cb({ payload: fake });
    cb({ payload: { returnValue: true, apps: global.__stubApps || [] } });
  }
};

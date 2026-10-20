/* Minimal Luna bus client built on PalmServiceBridge (what webOSTV.js wraps). */
(function (global) {
  'use strict';

  var live = {}; // keep bridges referenced until they answer, or they get GC'd
  var seq = 0;

  function call(uri, params) {
    return new Promise(function (resolve, reject) {
      if (typeof global.PalmServiceBridge === 'undefined') {
        reject({ returnValue: false, errorText: 'PalmServiceBridge unavailable' });
        return;
      }
      var id = ++seq;
      var bridge = new global.PalmServiceBridge();
      live[id] = bridge;
      bridge.onservicecallback = function (msg) {
        delete live[id];
        var res;
        try { res = JSON.parse(msg); } catch (e) { res = { returnValue: false, errorText: 'Bad response' }; }
        if (res.returnValue === false) reject(res); else resolve(res);
      };
      bridge.call(uri, JSON.stringify(params || {}));
    });
  }

  global.Luna = {
    call: call,
    available: typeof global.PalmServiceBridge !== 'undefined'
  };
})(window);

'use strict';
const { test } = require('node:test');
const assert = require('assert');
const Service = require('../tools/native-preview/stub/webos-service');
const { until } = require('./helpers');

test('paced subscriptions preserve scenario order across registration delays', async () => {
  const saved = { script: process.env.HOMEPLUS_PREVIEW_SUBSCRIPTIONS, paced: process.env.HOMEPLUS_PREVIEW_PACED_SUBSCRIPTIONS };
  process.env.HOMEPLUS_PREVIEW_PACED_SUBSCRIPTIONS = '1';
  process.env.HOMEPLUS_PREVIEW_SUBSCRIPTIONS = JSON.stringify({
    getAppLifeEvents: [[230, { event: 'close' }], [235, { event: 'stop' }]],
    getPowerState: [[100, { state: 'Active' }], [270, { state: 'Active Standby' }]],
    getForegroundAppInfo: [[120, { appId: 'home' }], [250, { appId: '' }]]
  });
  const events = [], svc = new Service();
  const sub = name => svc.subscribe('luna://fixture/' + name).on('response', m => events.push({ name, payload: m.payload, at: Date.now() }));
  try {
    sub('getAppLifeEvents'); sub('getPowerState');
    // Reproduce the observed subscription registration skew without changing the scenario.
    const end = Date.now() + 60; while (Date.now() < end) {}
    sub('getForegroundAppInfo');
    await until(() => events.length === 6, 2500, 'the complete subscription scenario');
    assert.deepStrictEqual(events.map(e => e.payload), [
      { state: 'Active' }, { appId: 'home' }, { event: 'close' }, { event: 'stop' }, { appId: '' }, { state: 'Active Standby' }
    ]);
    assert.ok(events[5].at - events[4].at >= 19, 'the scripted 20 ms gap is preserved');
  } finally {
    for (const [key, value] of [['HOMEPLUS_PREVIEW_SUBSCRIPTIONS', saved.script], ['HOMEPLUS_PREVIEW_PACED_SUBSCRIPTIONS', saved.paced]]) {
      if (value === undefined) delete process.env[key]; else process.env[key] = value;
    }
  }
});

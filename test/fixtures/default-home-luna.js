#!/usr/bin/env node
/* File-backed Luna stand-in for the standalone recovery process, outside any app. */
'use strict';
const fs = require('fs');
const file = process.env.HOMEPLUS_MOCK_LUNA_STATE;
const model = JSON.parse(fs.readFileSync(file, 'utf8'));
const uri = process.argv[process.argv.length - 2];
const params = JSON.parse(process.argv[process.argv.length - 1]);
model.calls.push({ uri, params });
let reply;
if (uri.endsWith('/getSystemSettings')) {
  if (model.failReads > 0) { model.failReads--; reply = { returnValue: false }; }
  else reply = { returnValue: true, settings: { defaultApps: model.defaults } };
} else if (uri.endsWith('/setDefaultApp')) {
  model.defaults.home = params.appId;
  reply = { returnValue: true };
} else if (uri.endsWith('/setSystemSettings')) {
  model.defaults = params.settings.defaultApps;
  reply = { returnValue: true };
} else reply = { returnValue: false, errorText: 'Unsupported mock method' };
fs.writeFileSync(file + '.tmp', JSON.stringify(model));
fs.renameSync(file + '.tmp', file);
console.log(JSON.stringify(reply));

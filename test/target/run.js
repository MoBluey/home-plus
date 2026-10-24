/* Runs one of the in-process test files (test/*.test.js that load service modules themselves) on the
 * TV's Node: `node test/target/run.js test/tmdb.test.js`. webOS 6's Node 8 has no node:test, no
 * assert/strict and no node: prefix, so this stands in for the little of them the tests use: test()
 * with skip and timeout, test.before/beforeEach/after, t.after, t.mock.method, and strict assert with
 * match and rejects. Only the test framework: the service and the tests get no polyfills, so a newer
 * API either of them calls fails here as it would on the TV (the service's own require('node:fs') too).
 * A rejection nobody awaited fails the test it came in, as node:test does (Node 8 and 12 only warn).
 * Its assert differs from node:assert/strict in corners no test relies on: deepEqual takes -0 for 0 and
 * leaves out symbol keys, and finds [NaN] unequal to [NaN]. runtime.test.js runs each file with it. */
'use strict';

const Module = require('module');
const path = require('path');
const util = require('util');
const base = require('assert');

function matches(e, want) {
  if (want === undefined) return;
  if (want instanceof RegExp) return base.ok(want.test(String(e)), `${String(e)} doesn't match ${want}`); /* (as Node: "TypeError: ...") */
  if (typeof want === 'function') {
    if (want.prototype !== undefined && e instanceof want) return;
    return base.ok(want(e) === true, `${e && e.message} isn't what was wanted`);
  }
  for (const k of Object.keys(want)) {
    if (want[k] instanceof RegExp) base.ok(want[k].test(e[k]), `${k}: ${e[k]} doesn't match ${want[k]}`);
    else base.deepStrictEqual(e[k], want[k]);
  }
}

const is = (a, b, message) => { if (!Object.is(a, b)) base.fail(a, b, message, 'Object.is', is); }; /* (strict's equal: NaN is NaN, -0 isn't 0) */
const isNot = (a, b, message) => { if (Object.is(a, b)) base.fail(a, b, message, '!Object.is', isNot); };
const string = (s) => base.ok(typeof s === 'string', `${util.inspect(s)} isn't a string`);
const assert = (v, message) => base.ok(v, message);
Object.assign(assert, {
  ok: base.ok, fail: base.fail, equal: is, notEqual: isNot, strictEqual: is,
  deepEqual: base.deepStrictEqual, deepStrictEqual: base.deepStrictEqual, notDeepEqual: base.notDeepStrictEqual,
  doesNotThrow: base.doesNotThrow,
  throws(f, want, message) {
    if (typeof want === 'string') { message = want; want = undefined; } /* (a message, not what to match) */
    let error, threw = false;
    try { f(); } catch (e) { error = e; threw = true; }
    base.ok(threw, message || 'it throws');
    matches(error, want);
  },
  async rejects(p, want, message) {
    if (typeof want === 'string') { message = want; want = undefined; }
    let error, threw = false;
    try { await (typeof p === 'function' ? p() : p); } catch (e) { error = e; threw = true; }
    base.ok(threw, message || 'it rejects');
    matches(error, want);
  },
  match: (s, re, message) => { string(s); base.ok(re.test(s), message || `${util.inspect(s)} doesn't match ${re}`); },
  doesNotMatch: (s, re, message) => { string(s); base.ok(!re.test(s), message || `${util.inspect(s)} matches ${re}`); }
});
assert.strict = assert;

const tests = [], hooks = { before: [], beforeEach: [], after: [] };
function test(name, options, fn) {
  if (typeof options === 'function') { fn = options; options = {}; }
  tests.push({ name, options: options || {}, fn });
}
for (const k of Object.keys(hooks)) test[k] = (fn) => hooks[k].push(fn);
test.test = test;

const SERVICE = path.join(__dirname, '..', '..', 'service') + path.sep;
const load = Module._load;
Module._load = function (request, parent, ...rest) {
  if (request === 'node:test') return test;
  if (request === 'node:assert/strict' || request === 'assert/strict') return assert;
  const fromService = parent && parent.filename && parent.filename.startsWith(SERVICE); /* (as on the TV) */
  return load.call(this, fromService ? request : request.replace(/^node:/, ''), parent, ...rest);
};

let unhandled = [];
process.on('unhandledRejection', (e) => unhandled.push(e));
/* A rejection nobody awaited fails the test it came in (one turn of the loop for it to arrive). */
const settle = () => new Promise((resolve) => setImmediate(resolve)).then(() => {
  const e = unhandled[0];
  unhandled = [];
  if (e) throw new Error(`a rejection nobody awaited: ${(e && e.stack) || e}`);
});

async function main(file) {
  require(path.resolve(file));
  let failed = 0;
  for (const h of hooks.before) await h();
  for (const t of tests) {
    if (t.options.skip) { console.log(`skip ${t.name}`); continue; }
    const afters = [], restores = [];
    const ctx = {
      name: t.name,
      after: (f) => afters.push(f),
      mock: { method(object, key, impl) { const was = object[key]; object[key] = impl; restores.push(() => { object[key] = was; }); } }
    };
    let timer;
    try {
      for (const h of hooks.beforeEach) await h(ctx);
      const ms = t.options.timeout || 60000;
      await Promise.race([t.fn(ctx), new Promise((resolve, reject) => { timer = setTimeout(() => reject(new Error(`timed out after ${ms} ms`)), ms); })]);
      await settle();
      console.log(`ok ${t.name}`);
    } catch (e) {
      if (!t.options.todo) failed++; /* (a todo test's failure doesn't count, as in node:test) */
      unhandled = [];
      console.log(`not ok ${t.name}\n  ${String((e && e.stack) || e).replace(/\n/g, '\n  ')}`);
    }
    clearTimeout(timer);
    for (const f of afters.reverse()) await f(); /* as node:test: its after hooks, then the mocks go */
    for (const r of restores.reverse()) r();
  }
  for (const h of hooks.after) await h();
  await settle();
  console.log(`# ${tests.length - failed} of ${tests.length} passed`);
  return failed;
}

main(process.argv[2]).then((failed) => process.exit(failed ? 1 : 0), (e) => {
  console.error(e);
  process.exit(1);
});

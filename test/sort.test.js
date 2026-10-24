'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { sortStable } = require('../service/sort');

test('sort: ties keep their order past 10 items (webOS 6\'s Node 8 sorts those unstably), in place', () => {
  const list = Array.from({ length: 30 }, (v, i) => ({ i, odd: i % 2 }));
  assert.equal(sortStable(list, (a, b) => b.odd - a.odd), list);
  assert.deepEqual(list.map((x) => x.i), [...Array.from({ length: 15 }, (v, i) => 2 * i + 1), ...Array.from({ length: 15 }, (v, i) => 2 * i)]);
});

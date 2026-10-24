/* Runs on Node 8 and 12 too: exercise the real filesystem, including links out of a tree. */
'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const remove = require('../../service/remove');
const { mkdirSync } = require('../../service/mkdir');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-remove-'));
const file = (name) => path.join(dir, name);
const mode = process.argv[2];
const rm = mode === 'sync'
  ? async (p, options) => remove.rmSync(p, options)
  : (p, options) => new Promise((resolve, reject) => {
    let returned = false;
    remove.rm(p, options, (e) => {
      assert.ok(returned, 'the callback is asynchronous');
      if (e) reject(e); else resolve();
    });
    returned = true;
  });
// Node 8's assert has no rejects, and its throws takes no object to match
const code = (want) => (e) => e && e.code === want;
const rejects = (f, match = () => true) => f().then(() => assert.fail('it rejects'), (e) => assert.ok(match(e), e && e.message));

(async () => {
  try {
    fs.writeFileSync(file('regular'), 'remove this');
    await rm(file('regular'), { force: true });
    assert.ok(!fs.existsSync(file('regular')));
    await rm(file('missing'), { force: true });
    await rm(file('missing-tree'), { recursive: true, force: true });
    await rejects(() => rm(file('missing'), { force: false }), code('ENOENT'));

    mkdirSync(file('tree/nested'));
    mkdirSync(file('tree/nested')); /* already there */
    fs.writeFileSync(file('plain'), '');
    assert.throws(() => mkdirSync(file('plain')), code('EEXIST'));
    /* (Windows says ENOENT under a file, so it's the file itself that's in the way there) */
    assert.throws(() => mkdirSync(file('plain/below')), code(process.platform === 'win32' ? 'EEXIST' : 'ENOTDIR'));
    fs.writeFileSync(file('tree/nested/data'), 'remove this too');
    fs.mkdirSync(file('outside'));
    fs.writeFileSync(file('outside/keep'), 'keep this');
    if (process.platform !== 'win32') {
      fs.symlinkSync(file('outside'), file('tree/link'), 'dir');
      fs.symlinkSync(file('absent'), file('dangling'));
      await rm(file('dangling'), { force: true });
      assert.throws(() => fs.lstatSync(file('dangling')), code('ENOENT'));
      fs.symlinkSync(file('outside'), file('linked-tree'), 'dir');
      await rm(file('linked-tree'), { recursive: true, force: true });
      assert.throws(() => fs.lstatSync(file('linked-tree')), code('ENOENT'));
    }
    await rejects(() => rm(file('tree'), { force: true }));
    assert.equal(fs.readFileSync(file('tree/nested/data'), 'utf8'), 'remove this too');
    await rm(file('tree'), { recursive: true, force: true });
    assert.ok(!fs.existsSync(file('tree')));
    assert.equal(fs.readFileSync(file('outside/keep'), 'utf8'), 'keep this');
  } finally {
    /* The runner always cleans up, natively where it can (whatever the helper did). */
    if (fs.rmSync) fs.rmSync(dir, { recursive: true, force: true });
    else remove.rmSync(dir, { recursive: true, force: true });
  }
})().catch((e) => { console.error(e); process.exitCode = 1; });

/* Forced file/tree cleanup on webOS 6's Node 8 and webOS 22/23's Node 12 as well as newer TVs. Unlink
 * first on Node 12: a symlink is removed itself, never traversed. Only a directory needs rmdir. */
'use strict';

const fs = require('fs');
const path = require('path');
const missing = (e, options) => options.force && e.code === 'ENOENT';
const directory = (e, options) => options.recursive && (e.code === 'EISDIR' || e.code === 'EPERM');

function rmSync(file, options) {
  if (fs.rmSync) return fs.rmSync(file, options);
  try {
    fs.unlinkSync(file);
  } catch (e) {
    if (missing(e, options)) return;
    if (!directory(e, options)) throw e;
    // Node 12's recursive rmdirSync can spin forever on a permission error. Walk explicitly
    // so that error propagates; each child's unlink also leaves symlink targets alone.
    try {
      for (const name of fs.readdirSync(file)) rmSync(path.join(file, name), options);
      fs.rmdirSync(file);
    } catch (e) { if (!missing(e, options)) throw e; }
  }
}

function rm(file, options, done) {
  if (fs.rm) return fs.rm(file, options, done);
  fs.unlink(file, (e) => {
    if (!e || missing(e, options)) return done(null);
    if (!directory(e, options)) return done(e);
    // As rmSync: Node 8's rmdir has no recursive, and Node 12's can spin on a permission error.
    fs.readdir(file, (e, names) => {
      if (e) return done(missing(e, options) ? null : e);
      const next = (i) => {
        if (i === names.length) return fs.rmdir(file, (e) => done(!e || missing(e, options) ? null : e));
        rm(path.join(file, names[i]), options, (e) => (e ? done(e) : next(i + 1)));
      };
      next(0);
    });
  });
}

module.exports = { rm, rmSync };

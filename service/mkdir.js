/* mkdir -p for webOS 6's Node 8 as well as newer TVs: Node 8 has no { recursive: true } (it reads the
 * options as a mode and fails with EEXIST or ENOENT). Each folder it makes gets the mode. */
'use strict';

const fs = require('fs');
const path = require('path');

function mkdirSync(dir, mode) {
  try {
    fs.mkdirSync(dir, mode);
  } catch (e) {
    if (e.code === 'EEXIST' && fs.statSync(dir).isDirectory()) return;
    if (e.code !== 'ENOENT' || path.dirname(dir) === dir) throw e;
    mkdirSync(path.dirname(dir), mode);
    mkdirSync(dir, mode);
  }
}

module.exports = { mkdirSync };

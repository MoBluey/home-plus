#!/usr/bin/env node
// Runs only from the trusted workflow checkout. Candidate code is never executed here.
'use strict';
const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const { finish, repository } = require('./prepare-fork-update');
const ID = 'com.github.pbalazaar.homeplus';
function checkedResult(value, expected) {
  if (!value || value.schemaVersion !== 1 || !value.upstream || !value.source || value.upstream.repository !== 'pbalazaar/home-plus' ||
      !/^\d+\.\d+\.\d+$/.test(value.upstream.version) || value.upstream.tag !== 'v' + value.upstream.version || value.upstream.version !== expected.version ||
      value.source.repository !== expected.repository || !repository(value.source.repository) || value.source.commit !== expected.source ||
      !['ready', 'failed', 'conflict'].includes(value.status)) throw new Error('Unexpected fork result identity');
  for (const commit of [value.source.commit, value.source.base, value.upstream.commit]) if (!/^[0-9a-f]{40}$/.test(commit)) throw new Error('Invalid result commit');
  for (const list of [value.conflicts, value.customisations]) if (!Array.isArray(list) || list.some(item => typeof item !== 'string' || item.length > 512 || item.includes('..') || /^[\\/]|[\x00-\x1f]/.test(item))) throw new Error('Invalid result paths');
  return { schemaVersion: 1, upstream: value.upstream, source: value.source, status: value.status,
    message: String(value.message).slice(0, 1000), conflicts: value.conflicts, customisations: value.customisations };
}
function gh(args) { return cp.execFileSync('gh', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }); }
function finalise(repo, file, expected, passed) {
  let result = checkedResult(JSON.parse(fs.readFileSync(file)), expected);
  try {
    let official;
    if (passed && result.status !== 'conflict' && !result.customisations.length) {
      const dir = path.join(path.dirname(file), 'official'); fs.mkdirSync(dir);
      const name = ID + '_' + result.upstream.version + '_all.ipk';
      gh(['release', 'download', result.upstream.tag, '--repo', 'pbalazaar/home-plus', '--pattern', ID + '.manifest.json', '--pattern', name, '--dir', dir]);
      const manifest = JSON.parse(fs.readFileSync(path.join(dir, ID + '.manifest.json'))), data = fs.readFileSync(path.join(dir, name));
      const hash = crypto.createHash('sha256').update(data).digest('hex');
      if (manifest.id !== ID || manifest.version !== result.upstream.version || manifest.ipkUrl !== name || manifest.ipkSize !== data.length || !manifest.ipkHash || manifest.ipkHash.sha256 !== hash) throw new Error('Official package failed identity/hash verification');
      official = { url: 'https://github.com/pbalazaar/home-plus/releases/download/' + result.upstream.tag + '/' + name, sha256: hash, size: data.length, version: result.upstream.version };
    }
    if (result.status !== 'conflict') result = finish(result, repo, passed, official);
  } catch (error) { result.status = 'failed'; result.message = 'Candidate package verification failed. See the workflow checks.'; delete result.artifact; }
  if (!passed && result.status !== 'conflict' && process.env.CHECK_RESULTS) result.message = 'Candidate checks failed: ' + process.env.CHECK_RESULTS.slice(0, 300);
  if (result.status === 'ready' && result.customisations.length) {
    const src = path.join(repo, 'dist', ID + '_' + result.upstream.version + '_all.ipk');
    fs.copyFileSync(src, path.join(path.dirname(file), path.basename(result.artifact.url)));
  }
  fs.writeFileSync(file, JSON.stringify(result, null, 2) + '\n');
  return result;
}
function publish(repo, file, expected) {
  const raw = JSON.parse(fs.readFileSync(file));
  const result = checkedResult(raw, expected), tag = 'upstream-v' + result.upstream.version;
  if (process.env.GH_REPO !== 'MoBluey/home-plus') throw new Error('Publication is restricted to the managed fork');
  let assets;
  try { assets = JSON.parse(gh(['release', 'view', tag, '--json', 'assets'])).assets; }
  catch (_) {
    // Creation never force-moves an existing tag. A failed network/API route fails closed.
    gh(['release', 'create', tag, '--target', expected.source, '--title', 'Home+ upstream ' + result.upstream.version, '--notes', 'Source-preserving update results for upstream ' + result.upstream.version + '.', '--latest=false']);
    assets = [];
  }
  if (result.status === 'ready' && result.customisations.length) {
    const artifact = raw.artifact;
    if (!artifact || !/^[a-f0-9]{64}$/.test(artifact.sha256) || artifact.version !== expected.version || !Number.isSafeInteger(artifact.size) || artifact.size < 1) throw new Error('Invalid published artifact');
    const name = ID + '_' + expected.version + '_' + expected.source.slice(0, 12) + '_' + artifact.sha256.slice(0, 12) + '_all.ipk';
    if (artifact.url !== 'https://github.com/' + expected.repository + '/releases/download/' + tag + '/' + name) throw new Error('Unexpected artifact URL');
    const local = path.join(path.dirname(file), name), data = fs.readFileSync(local);
    if (data.length !== artifact.size || crypto.createHash('sha256').update(data).digest('hex') !== artifact.sha256) throw new Error('Artifact changed after checks');
    const existing = assets.find(asset => asset.name === name);
    if (existing) {
      const downloaded = cp.execFileSync('gh', ['release', 'download', tag, '--pattern', name, '--output', '-'], { maxBuffer: 128 * 1024 * 1024 });
      if (!downloaded.equals(data)) throw new Error('Refusing to overwrite immutable package');
    } else gh(['release', 'upload', tag, local]);
  }
  // The only mutable asset is the small result pointer. Previously published packages remain immutable.
  gh(['release', 'upload', tag, file, '--clobber']);
}
module.exports = { checkedResult, finalise };
if (require.main === module) {
  try {
    const [command, repo, file] = process.argv.slice(2);
    const expected = { version: process.env.EXPECT_VERSION, source: process.env.EXPECT_SOURCE, repository: process.env.GITHUB_REPOSITORY };
    if (command === 'finalise') finalise(repo, file, expected, process.env.CHECKS_PASSED === 'true');
    else if (command === 'publish') publish(repo, file, expected);
    else throw new Error('Expected finalise or publish');
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}

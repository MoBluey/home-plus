// Builds the .ipk packages into dist/ without ares-cli.
// An .ipk is an `ar` archive of: debian-binary, control.tar.gz, data.tar.gz.
//   node tools/build.mjs          -> web app + service (com.atvlauncher.app)
//   node tools/build.mjs native   -> also the native front end (com.atvlauncher.native);
//                                    run `make -C native webos` first
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';
import { execFileSync } from 'node:child_process';

const root = path.dirname(path.dirname(url.fileURLToPath(import.meta.url)));
const dist = path.join(root, 'dist');
const readJSON = (p) => JSON.parse(fs.readFileSync(path.join(root, p), 'utf8'));

function duKB(dir) {
  let total = 0;
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    total += e.isDirectory() ? duKB(p) * 1024 : fs.statSync(p).size;
  }
  return Math.ceil(total / 1024);
}

// Plain (BSD-style) ar: every member name here fits in 16 chars, no long-name table needed.
function ar(dir, members) {
  const chunks = [Buffer.from('!<arch>\n')];
  const now = Math.floor(Date.now() / 1000);
  for (const name of members) {
    const data = fs.readFileSync(path.join(dir, name));
    const h = name.padEnd(16) + String(now).padEnd(12) + '0'.padEnd(6) + '0'.padEnd(6) +
      '100644'.padEnd(8) + String(data.length).padEnd(10) + '`\n';
    chunks.push(Buffer.from(h, 'ascii'), data);
    if (data.length % 2) chunks.push(Buffer.from('\n'));
  }
  return Buffer.concat(chunks);
}

// stage(appDir) fills the data tree; services lists service ids for packageinfo.
function buildPackage({ appinfo, stage: fill, services = [] }) {
  const { id, version } = appinfo;
  const build = path.join(root, 'build', id);
  const data = path.join(build, 'data');
  const ctrl = path.join(build, 'control');
  fs.rmSync(build, { recursive: true, force: true });

  const appDir = path.join(data, 'usr/palm/applications', id);
  fs.mkdirSync(appDir, { recursive: true });
  fill(appDir, data);
  const pkgDir = path.join(data, 'usr/palm/packages', id);
  fs.mkdirSync(pkgDir, { recursive: true });
  fs.writeFileSync(path.join(pkgDir, 'packageinfo.json'), JSON.stringify({
    app: id, id, loc_name: appinfo.title, package_format_version: 2,
    vendor: appinfo.vendor || 'unknown', version, ...(services.length ? { services } : {})
  }, null, 2));

  fs.mkdirSync(ctrl, { recursive: true });
  fs.writeFileSync(path.join(ctrl, 'control'), [
    `Package: ${id}`, `Version: ${version}`, 'Section: misc', 'Priority: optional', 'Architecture: all',
    `Installed-Size: ${duKB(data)}`, 'Maintainer: N/A <nobody@example.com>', `Description: ${appinfo.title}`,
    'webOS-Package-Format-Version: 2', 'webOS-Packager-Version: x.y.x', ''
  ].join('\n'));

  const tarArgs = ['--format=gnu', '--owner=0', '--group=0', '--numeric-owner', '--sort=name', '-czf'];
  execFileSync('tar', [...tarArgs, path.join(build, 'control.tar.gz'), '-C', ctrl, '.']);
  execFileSync('tar', [...tarArgs, path.join(build, 'data.tar.gz'), '-C', data, '.']);
  fs.writeFileSync(path.join(build, 'debian-binary'), '2.0\n');

  fs.mkdirSync(dist, { recursive: true });
  const out = path.join(dist, `${id}_${version}_all.ipk`);
  fs.writeFileSync(out, ar(build, ['debian-binary', 'control.tar.gz', 'data.tar.gz']));
  console.log(`built ${path.relative(root, out)} (${fs.statSync(out).size} bytes)`);
}

// ---- web app + service ----
const svc = readJSON('service/services.json');
buildPackage({
  appinfo: readJSON('app/appinfo.json'),
  services: [svc.id],
  stage(appDir, data) {
    fs.cpSync(path.join(root, 'app'), appDir, { recursive: true });
    // boot script travels with the app so it can be installed on the TV with cp
    fs.cpSync(path.join(root, 'init.d'), path.join(appDir, 'init.d'), { recursive: true });
    fs.cpSync(path.join(root, 'service'), path.join(data, 'usr/palm/services', svc.id), {
      recursive: true,
      filter: (src) => !src.includes(`${path.sep}data`) && !src.endsWith('.test.js')
    });
  }
});

// ---- native front end ----
if (process.argv.includes('native')) {
  const bin = path.join(root, 'native/build/webos/homeplus');
  if (!fs.existsSync(bin)) throw new Error('native/build/webos/homeplus missing: run `make -C native webos` first');
  buildPackage({
    appinfo: readJSON('native/appinfo.json'),
    stage(appDir) {
      fs.copyFileSync(path.join(root, 'native/appinfo.json'), path.join(appDir, 'appinfo.json'));
      fs.copyFileSync(bin, path.join(appDir, 'homeplus'));
      fs.chmodSync(path.join(appDir, 'homeplus'), 0o755);
      fs.cpSync(path.join(root, 'native/assets'), path.join(appDir, 'assets'), { recursive: true });
      fs.copyFileSync(path.join(root, 'app/img/icon.png'), path.join(appDir, 'icon.png'));
      fs.copyFileSync(path.join(root, 'app/img/largeIcon.png'), path.join(appDir, 'largeIcon.png'));
    }
  });
}

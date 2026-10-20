# Home+ — notes for Claude

tvOS-style launcher for a rooted LG C3 (webOS 23.20.59, kernel aarch64, **32-bit ARM userland**,
SDL2 2.0.14, GLES2/EGL/Wayland). Owner's TV: `192.168.1.50`, root SSH via Homebrew Channel
(`ssh lgtv` if the key/alias from the README is set up; otherwise `root@192.168.1.50`).
PicCap and HyperHDR run on the TV, so keep GPU/CPU use low.

## Pieces
- `service/` — Node 16 webOS service `com.atvlauncher.app.service`, elevated with Homebrew
  Channel's `elevate-service`. Luna methods + a local HTTP API on `127.0.0.1:47823` (see README).
  Also: Home button takeover (`watch` mode, and `redirect` = no-flash bind mount over LG home's
  start page, approved by the owner), Stremio + Trakt Continue Watching, settings apps, recents.
  Logs to `/tmp/homeplus-service.log` (the native app logs there too, via `POST /api/log`).
- `native/` — the launcher people actually use (`com.atvlauncher.native`): C, SDL2 + GLES2,
  liquid-glass panels via render targets (`src/glass.c`). Build only against the vendored
  SDL **2.0.14** headers in `third_party/SDL2` — newer SDL functions don't exist on the TV.
- `app/` — the hidden "Home+ Service" package (`com.atvlauncher.app`, `visible: false`). It carries the
  service and the boot script, so it must stay installed. Its page only hands over to the native app;
  the old web launcher was removed (owner only wants the native one).

## Building
- Native for the TV needs the Linux toolchain `openlgtv/buildroot-nc4`
  (`arm-webos-linux-gnueabi_sdk-buildroot`, from its GitHub releases): `make -C native webos WEBOS_SDK=...`.
  It doesn't run on macOS. On an Apple Silicon Mac, use the `-aarch64` SDK tarball in a
  `linux/arm64` Debian container with `make` and `nodejs` (run `relocate-sdk.sh` once after unpacking).
- Packages: `node tools/build.mjs native` → `dist/*.ipk`. It needs GNU tar, so on a Mac run it in
  that container too. Bump versions in `app/appinfo.json`, `service/package.json`, `native/appinfo.json`.
- Desktop preview (Linux): `tools/native-preview/run.sh` → `preview/native-*.png`.

## Installing on the TV
Copy the `.ipk`s to `/tmp` on the TV, then for each:
`luna-send -w 60000 -i luna://com.webos.appInstallService/dev/install '{"id":"com.ares.defaultName","ipkUrl":"/tmp/<file>","subscribe":true}'`
(stop it after ~20 s), then re-run `elevate-service com.atvlauncher.app.service`.
Before installing, close the apps and stop the old service. **Never `pkill -f` a pattern that
appears in your own SSH command line** — it kills the session. Use:
`for p in $(pgrep -f atvlauncher.app.service); do [ "$p" != "$$" ] && kill "$p"; done`
- The TV has no `timeout` command, which `tools/install.sh` relies on.
- `luna-send` prints nothing unless it has a terminal: use `ssh -tt`. The install subscription
  never exits by itself. Run it in the background, wait until the installed file (e.g.
  `/media/developer/apps/usr/palm/applications/com.atvlauncher.native/homeplus`) matches the new
  build, then kill it. The install also un-elevates the service, so always re-run `elevate-service`.
- Screenshot: `luna-send -n 1 luna://com.webos.service.capture/executeOneShot '{"path":"/tmp/s.png","method":"DISPLAY","format":"PNG","width":1920,"height":1080}'`.

## Known TV quirks
- LG's SDL puts the *scancode* in `keysym.sym` (Right arrow arrives as 79); see `remote_key()`.
  LG's own codes come the same way, with `scancode=1`: Back 482 (seen), pointer show/hide 484/485
  (sent when the pointer wakes or sleeps, not real presses), Red/Green/Yellow/Blue 486–489 (from
  LG's `SDL_webOS.h`; not seen yet). Settings and Input never reach apps. Keys Home+ doesn't act on
  are logged as `unknown key sym=...`.
- No-flash mode can't work on the C3: LG's home is a native app (`/usr/bin/com.webos.app.home`),
  so the service falls back to `watch` and reports `noFlashAvailable: false`.
- No flash on the C3 comes from lginputhook (`org.webosbrew.inputhook` 1.5.1; 1.4.0 couldn't
  inject on webOS 23). `/home/root/.config/lginputhook/keybinds.json` binds Home to
  `{"773":{"action":"launch","id":"com.atvlauncher.native"}}`, so LG's home never starts.
  Binding keys are the hook log's `uinput_code`, not `keyid`
  (`/tmp/lginput-hook-lginput2.log`). The hook injects once per boot, guarded by the `/tmp/inputhook`
  marker; after updating it, delete the marker and restart its service, or reboot.
- Developer-installed web apps can't read other apps' files; icons are served by the service.
- Trakt titles open in Stremio (by IMDb id); whether webOS Stremio honours
  `contentTarget` deep links is unverified.

## Open items
- Confirm the colour keys on a remote that has them.
- Confirm Stremio deep links resume the right title.

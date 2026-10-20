# Home+ — notes for Claude

tvOS-style launcher for a rooted LG C3 (webOS 23.20.59, kernel aarch64, **32-bit ARM userland**,
SDL2 2.0.14, GLES2/EGL/Wayland). Owner's TV: `192.168.1.50`, root SSH via Homebrew Channel
(`ssh lgtv` if the key/alias from the README is set up; otherwise `root@192.168.1.50`).
PicCap and HyperHDR run on the TV, so keep GPU/CPU use low.

## Pieces
- `service/` — Node 16 webOS service `com.atvlauncher.app.service`, elevated with Homebrew
  Channel's `elevate-service`. Luna methods + a local HTTP API on `127.0.0.1:47823` (see README).
  Also: Home button takeover (`watch` mode, and `redirect` = no-flash bind mount over LG home's
  start page, approved by the owner), Stremio + Trakt Continue Watching, inputs/settings, recents.
  Logs to `/tmp/homeplus-service.log` (the native app logs there too, via `POST /api/log`).
- `native/` — the launcher people actually use (`com.atvlauncher.native`): C, SDL2 + GLES2,
  liquid-glass panels via render targets (`src/glass.c`). Build only against the vendored
  SDL **2.0.14** headers in `third_party/SDL2` — newer SDL functions don't exist on the TV.
- `app/` — the older web launcher; ships the service and the boot script, so it must stay installed.

## Building
- Native for the TV needs the Linux toolchain `openlgtv/buildroot-nc4`
  (`arm-webos-linux-gnueabi_sdk-buildroot`, from its GitHub releases): `make -C native webos WEBOS_SDK=...`.
  It doesn't run on macOS; use Docker (linux/amd64) or a Linux machine.
- Packages: `node tools/build.mjs native` → `dist/*.ipk`. Bump versions in `app/appinfo.json`,
  `service/package.json`, `native/appinfo.json`.
- Desktop preview (Linux): `tools/native-preview/run.sh` → `preview/native-*.png`.

## Installing on the TV
Copy the `.ipk`s to `/tmp` on the TV, then for each:
`luna-send -w 60000 -i luna://com.webos.appInstallService/dev/install '{"id":"com.ares.defaultName","ipkUrl":"/tmp/<file>","subscribe":true}'`
(stop it after ~20 s), then re-run `elevate-service com.atvlauncher.app.service`.
Before installing, close the apps and stop the old service. **Never `pkill -f` a pattern that
appears in your own SSH command line** — it kills the session. Use:
`for p in $(pgrep -f atvlauncher.app.service); do [ "$p" != "$$" ] && kill "$p"; done`

## Known TV quirks
- LG's SDL puts the *scancode* in `keysym.sym` (Right arrow arrives as 79); see `remote_key()`.
  Back, colour keys and Settings aren't mapped yet — unknown keys are logged as `unknown key sym=...`.
- Developer-installed web apps can't read other apps' files; icons are served by the service.
- Trakt titles open in Stremio (by IMDb id); whether webOS Stremio honours
  `contentTarget` deep links is unverified.

## Open items
- Verify no-flash mode on the TV (`grep no-flash /tmp/homeplus-service.log`).
- Map Back / Settings / colour keys from the log.
- Confirm Stremio deep links resume the right title.

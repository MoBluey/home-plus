# Home+

A tvOS-style home screen for rooted LG webOS TVs, in a Liquid Glass look. Built and laid out for an LG C3 on webOS 23.20.59.

- **Top Shelf:** focus any app in the top row and the screen fills with its artwork (or a glow generated from its icon).
- **Liquid Glass:** the dock, clock, buttons and menus are real Liquid Glass: they bend the light at their edges and stay clear, blurring only behind text or icons that would otherwise be hard to read. Focused tiles lift, sway for a moment and catch a moving light; the Magic Remote pointer steers it.
- **Motion and sound:** everything moves on springs, and menus grow out of the button or tile that opened them. A soft tick plays when focus moves and a click on select (**Settings → Sounds** turns them off).
- **OLED care:** the picture drifts by a pixel every minute, and an idle Home+ dims, then fades to black (see [OLED protection](#oled-protection)).
- **Stremio Continue Watching:** focus Stremio in the top row and the Top Shelf turns into your Continue Watching rail. Press **Up** to browse it and **OK** to resume.
- **Rearrange and hide:** hold **OK** (or press **Yellow**) on an app to Move, Move to Front or Hide it. Hidden apps collect in a "Hidden" tile at the end.
- **Remote keys:** **Back** never leaves the home screen. It closes a menu (from an app in Recents, back to the list), steps out of Continue Watching, the top buttons or move mode, then jumps to the first app. The colour keys are shortcuts: **Red** Recents, **Green** Settings, **Yellow** the focused app's options, **Blue** move the focused app (press again when done). The remote's **Settings** and **Input** keys open LG's own menus; apps never receive them.

## How it works

Home+ comes as two packages, and both need to be installed:
- **Home+ Native** (`com.atvlauncher.native`, built from `native/`): the launcher itself. It's written in C with SDL2 and OpenGL ES 2, the libraries the TV already has.
  - **Drawing:** our own OpenGL ES 2 renderer (`native/src/render.c`) with shaders for Liquid Glass, rounded shapes and soft shadows; a frame is a few dozen quads. The TV's GPU is shared with PicCap and HyperHDR, so glass refracts a half-resolution copy of the screen and blurs it in two cheap passes (about 3 MB in total).
  - **Animation:** focus, scrolling and crossfades are springs that run at the display's refresh rate.
  - **Idle:** nothing is redrawn while nothing moves.
- **Home+ Service** (`com.atvlauncher.app`, hidden from the app list): it carries the service in `service/` and the boot script. The launcher gets its data from the service's local API on `127.0.0.1:47823`:

| Endpoint | What it does |
|---|---|
| `GET /api/apps` | App list |
| `GET /api/stremio` | Stremio Continue Watching |
| `POST /api/launch` | Open an app |
| `GET` / `POST /api/layout` | Read or save the tile order and hidden apps |
| `GET /img/<key>` | Stremio artwork, fetched through the service |
| `GET /api/continue` | Continue Watching from Stremio and Trakt |
| `GET /api/system` | Settings apps |
| `GET /api/running`, `POST /api/close` | Recent apps, and closing them |
| `GET` / `POST /api/home` | Home button mode |
| `GET` / `POST /api/prefs` | Launcher preferences: `sounds`, `dimMinutes`, `offMinutes` |

To build:

```sh
make -C native webos WEBOS_SDK=/path/to/arm-webos-linux-gnueabi_sdk-buildroot   # toolchain: openlgtv/buildroot-nc4
node tools/build.mjs native
```

`tools/native-preview/run.sh` renders screenshots on a Linux desktop. It uses the real service with a stubbed webOS runtime, and the results land in `preview/native-*.png`.

## Control Center

From the top row, press **Up** to reach the two round buttons next to the clock: **Recents** and **Settings**. On the Stremio tile, Up goes into Continue Watching instead; press Up again from there. There's no Inputs button: the remote's Input key already opens LG's input menu.
- **Recents:** lists the apps that are running. Pick one to open or close it, or choose **Close All**.
- **Settings:** opens LG's settings apps, switches focus sounds, and has the sign-in options for Trakt and Stremio, plus the Home button mode on TVs where no-flash mode can work.

## Trakt

Continue Watching combines your Stremio library with Trakt's paused playback, newest first. Stremio also reports to Trakt, so a title can appear in both; Home+ keeps the most recently watched copy. Trakt doesn't record which app played something, so its titles open in Stremio, using their IMDb ID.

One-time setup:
1. Create a Trakt API app at <https://trakt.tv/oauth/applications/new>. Use `urn:ietf:wg:oauth:2.0:oob` as the redirect URI.
2. Give the app's credentials to the service:
   `luna-send -n 1 luna://com.atvlauncher.app.service/traktSetup '{"clientId":"…","clientSecret":"…"}'`
3. On the TV, go to **Settings → Connect Trakt**. A code appears; enter it at <https://trakt.tv/activate>.

## Home button

The service takes over the Home button in one of two modes:
- **No-flash (default):** while the service runs, LG's home app (`com.webos.app.home`) has its start page replaced by a black page that opens Home+ immediately, so LG's home screen never appears. The replacement is a bind mount, in memory only; nothing on disk changes, and rebooting without the service brings LG's home back. If LG's home isn't a web app, or the mount fails, Home+ falls back to standard mode.
- **Standard:** the service watches the foreground app. When LG's home comes up, it opens Home+. LG's home flashes for a moment first.

On the LG C3 (webOS 23), LG's home is a native app (`/usr/bin/com.webos.app.home`), so no-flash mode can't work there and Settings doesn't offer the switch. Instead, [lginputhook](https://github.com/Simon34545/lginputhook) 1.5.1 or later (from Homebrew Channel) catches the Home button itself and opens Home+ directly, so LG's home never appears. In the LG Input Hook app, add a keybind for button **773**, choose **Launch App** and pick **Home+ Native**, or put `{"773":{"action":"launch","id":"com.atvlauncher.native"}}` in `/home/root/.config/lginputhook/keybinds.json`. Standard mode still catches LG's home when it appears some other way, such as at boot.

Switch modes in **Settings → Home Button**, or with:

```sh
luna-send -n 1 luna://com.atvlauncher.app.service/setHomeOverride '{"mode":"watch"}'     # standard
luna-send -n 1 luna://com.atvlauncher.app.service/setHomeOverride '{"mode":"redirect"}'  # no-flash
luna-send -n 1 luna://com.atvlauncher.app.service/setHomeOverride '{"enabled":false}'    # off
```

Each foreground change and mode change is written to `/tmp/homeplus-service.log`.

## OLED protection

A home screen keeps bright static elements (the clock, glass rims, labels) in place for hours, so Home+ looks after an OLED panel on top of LG's own Pixel Refresher and Screen Shift:
- **Pixel orbit:** every minute the whole picture moves by one pixel along a slow path, never more than 4 px from centre.
- **Idle dimming:** after 2 minutes without input everything but the focused tile dims, as on tvOS.
- **Black screen:** after 10 minutes it fades to black, with the pixels off, and Home+ stops drawing. The first press only wakes it.
- **Static text** stays at about 85% white.

Change the timings (in minutes) with:

```sh
wget -qO- --post-data '{"dimMinutes":5,"offMinutes":20}' --header 'Content-Type: application/json' http://127.0.0.1:47823/api/prefs
```

## Icons

Developer-installed apps can't read other apps' files, so the service, running as root, serves icons and artwork on `http://127.0.0.1:47823/icon/<appId>` and `http://127.0.0.1:47823/art/<appId>`. The server only accepts connections from the TV itself, and it only serves paths that the application manager reported for that app. Icon colours are sampled once, then cached.

## Layout

```
native/     the launcher (C, SDL2 + OpenGL ES 2)
service/    Node JS service: app list/launch, Stremio + Trakt, Home button (elevated via Homebrew Channel)
app/        the hidden "Home+ Service" package that carries the service and the boot script
init.d/     boot script for /var/lib/webosbrew/init.d (elevate + start Home+ at boot)
tools/      build.mjs (makes the .ipks), install.sh (build + install over SSH), native-preview/, make-icons.py
preview/    screenshots from tools/native-preview
```

## Install on the TV

You need root SSH to the TV, which Homebrew Channel provides, and Node 18+ on your computer.

```sh
tools/install.sh 192.168.1.50 --autostart
```

That command does five things:
1. builds the packages in `dist/`;
2. installs it with `appInstallService/dev/install`;
3. elevates `com.atvlauncher.app.service` with Homebrew Channel's `elevate-service`, so it may call `listApps`;
4. with `--autostart`, copies `init.d/50-homeplus`, so the TV boots into Home+ and the service is re-elevated on every boot;
5. launches Home+.

You can also install the `.ipk` by hand (`ares-install`, or the Homebrew Channel app) and then run this on the TV:
`/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service com.atvlauncher.app.service`

**Quick access without root tricks:** long-press a number key on the Magic Remote and assign Home+ Native.

## Stremio

Home+ has no sign-in form. Sign in once from a terminal over SSH:

```sh
luna-send -n 1 luna://com.atvlauncher.app.service/stremioLogin '{"email":"…","password":"…"}'
```

The password goes only to `api.strem.io`. The service stores just the session key, in `/var/lib/homeplus/config.json` (mode 600).

Artwork: the full-screen background uses Stremio's large image size, and the cards use the medium size. Fetching a larger source works better than upscaling on the TV.

Continue Watching shows your library items that have a playback position, newest first. The list is cached for 60 seconds.

Stremio's LG app can't open a specific title: it ignores every launch parameter we tried (`stremio://` links, its own `#/detail/…` routes, and pointing it at its interface page). Pressing OK on a Continue Watching card opens Stremio, and a system notification names the title to pick from Stremio's own Continue Watching row.

## Troubleshooting

- **No apps on the home screen:** the service isn't elevated. Re-run `elevate-service`, as shown above.
- **Boot log:** `/tmp/homeplus-boot.log` on the TV.
- **Service log:** `/tmp/homeplus-service.log` on the TV. It records the start-up, a sample of what `listApps` returns, missing icon files, and what the launcher reports (lines starting `native:`).
- **Undo autostart:** `rm /var/lib/webosbrew/init.d/50-homeplus`.
- **Boot problems:** Homebrew Channel's failsafe skips `init.d` scripts after a crash during boot.
- **Layout and hidden apps:** stored in `/var/lib/homeplus/config.json`, along with the Stremio and Trakt sessions.

## License

Home+ is released under the [MIT License](LICENSE). The third-party code it bundles keeps its own license:

- **SDL 2.0.14 headers** (`native/third_party/SDL2`): zlib license, at the top of each header. Only the headers are bundled; the app links the TV's own libSDL2.
- **cJSON** (`native/third_party/cJSON.*`): MIT, see [LICENSE-cJSON](native/third_party/LICENSE-cJSON). It's compiled into the launcher, so the notice ships in the package.
- **stb_image, stb_image_write, stb_truetype** (`native/third_party`): public domain or MIT, your choice (at the end of each file).
- **Inter** font (`native/assets/fonts`): SIL Open Font License 1.1, see [LICENSE-Inter.txt](native/assets/fonts/LICENSE-Inter.txt).

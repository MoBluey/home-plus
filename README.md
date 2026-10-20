# Home+

A tvOS-style home screen for rooted LG webOS TVs, in a Liquid Glass look. Built and laid out for an LG C3 on webOS 23.20.59.

- **Top Shelf:** focus any app in the top row and the screen fills with its artwork (or a glow generated from its icon).
- **Glass tiles:** focused tiles float, tilt and catch the light. On the Magic Remote pointer they tilt toward the cursor.
- **Stremio Continue Watching:** focus Stremio in the top row and the Top Shelf turns into your Continue Watching rail. Press **Up** to browse it and **OK** to resume.
- **Rearrange and hide:** hold **OK** (or press Play/Pause) on an app to Move, Move to Front or Hide it. Hidden apps collect in a "Hidden" tile at the end.
- **Remote keys:** **Back** never leaves the home screen. It closes a menu (from an app in Recents, back to the list), steps out of Continue Watching, the top buttons or move mode, then jumps to the first app. The colour keys are shortcuts: **Red** Recents, **Green** Settings, **Yellow** the focused app's options, **Blue** move the focused app (press again when done). The remote's **Settings** and **Input** keys open LG's own menus; apps never receive them.

## Native front end (recommended)

`native/` is the same launcher written in C with SDL2 and OpenGL ES 2, the libraries the TV already has. The web engine is skipped entirely:
- **Drawing:** every frame is a few dozen tinted textured rectangles drawn by the GPU.
- **Animation:** focus, scrolling and crossfades are springs that run at the display's refresh rate.
- **Idle:** the loop sleeps when nothing is moving.

It gets its data from the service's local API on `127.0.0.1:47823`, so it needs the `com.atvlauncher.app` package installed too:

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

To build:

```sh
make -C native webos WEBOS_SDK=/path/to/arm-webos-linux-gnueabi_sdk-buildroot   # toolchain: openlgtv/buildroot-nc4
node tools/build.mjs native
```

`tools/native-preview/run.sh` renders screenshots on a Linux desktop. It uses the real service with a stubbed webOS runtime, and the results land in `preview/native-*.png`.

## Control Center

From the top row, press **Up** to reach the two round buttons next to the clock: **Recents** and **Settings**. On the Stremio tile, Up goes into Continue Watching instead; press Up again from there. There's no Inputs button: the remote's Input key already opens LG's input menu.
- **Recents:** lists the apps that are running. Pick one to open or close it, or choose **Close All**.
- **Settings:** opens LG's settings apps and has the sign-in options for Trakt and Stremio, plus the Home button mode on TVs where no-flash mode can work.

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
- **Standard:** the service watches the foreground app. When LG's home comes up, it opens Home+, the native version if it's installed, otherwise the web version. LG's home flashes for a moment first.

On the LG C3 (webOS 23), LG's home is a native app (`/usr/bin/com.webos.app.home`), so no-flash mode can't work there and Settings doesn't offer the switch. Instead, [lginputhook](https://github.com/Simon34545/lginputhook) 1.5.1 or later (from Homebrew Channel) catches the Home button itself and opens Home+ directly, so LG's home never appears. In the LG Input Hook app, add a keybind for button **773**, choose **Launch App** and pick **Home+ Native**, or put `{"773":{"action":"launch","id":"com.atvlauncher.native"}}` in `/home/root/.config/lginputhook/keybinds.json`. Standard mode still catches LG's home when it appears some other way, such as at boot.

Switch modes in **Settings → Home Button**, or with:

```sh
luna-send -n 1 luna://com.atvlauncher.app.service/setHomeOverride '{"mode":"watch"}'     # standard
luna-send -n 1 luna://com.atvlauncher.app.service/setHomeOverride '{"mode":"redirect"}'  # no-flash
luna-send -n 1 luna://com.atvlauncher.app.service/setHomeOverride '{"enabled":false}'    # off
```

Each foreground change and mode change is written to `/tmp/homeplus-service.log`.

## Performance

The TV's GPU is shared with PicCap and HyperHDR, so Home+ draws only what it must:
- **Glass look:** translucent fills and thin borders only. There's no shadow, blur, filter, mask or text shadow.
- **No full-screen effects:** the Top Shelf and dock switch instantly, with no fades.
- **Animation:** only the focused tile's short scale and the grid scroll move. Nothing animates while idle.
- **Returning home:** the grid isn't rebuilt unless the app list changed.
- **Icon colours:** sampled once, then cached.

## Icons

webOS won't let a developer-installed web app read other apps' files. Instead, the service, running as root, serves icons and artwork on `http://127.0.0.1:47823/icon/<appId>` and `http://127.0.0.1:47823/art/<appId>`. The server only accepts connections from the TV itself, and it only serves paths that the application manager reported for that app. If that port can't be reached, the page asks the service for each icon as a `data:` URL instead.

## Layout

```
app/        the webOS web app (HTML/CSS/JS, no build step, no dependencies)
service/    Node JS service: app list/launch + Stremio API (elevated via Homebrew Channel)
init.d/     boot script for /var/lib/webosbrew/init.d (elevate + start Home+ at boot)
tools/      build.mjs (makes the .ipk), install.sh (build + install over SSH), preview.mjs, make-icons.py
preview/    screenshots rendered from the mock data
```

## Preview on a computer

Open `app/index.html` in Chrome. With no webOS bridge it uses mock apps and mock Stremio data. Arrow keys move focus. **Enter** opens, and holding **Enter** opens the options sheet. **Esc** is Back.
To jump straight to a state, add `?state=shelf`, `grid`, `scrolled`, `sheet` or `login` to the URL.
`node tools/preview.mjs` re-renders the screenshots in `preview/`.

## Install on the TV

You need root SSH to the TV, which Homebrew Channel provides, and Node 18+ on your computer.

```sh
tools/install.sh 192.168.1.50 --autostart
```

That command does five things:
1. builds `dist/com.atvlauncher.app_<version>_all.ipk`;
2. installs it with `appInstallService/dev/install`;
3. elevates `com.atvlauncher.app.service` with Homebrew Channel's `elevate-service`, so it may call `listApps`;
4. with `--autostart`, copies `init.d/50-homeplus`, so the TV boots into Home+ and the service is re-elevated on every boot;
5. launches Home+.

You can also install the `.ipk` by hand (`ares-install`, or the Homebrew Channel app) and then run this on the TV:
`/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service com.atvlauncher.app.service`

**Quick access without root tricks:** long-press a number key on the Magic Remote and assign Home+.

## Stremio

1. Hold **OK** on Stremio and choose **Connect Stremio Account**.
2. Sign in. The webOS on-screen keyboard appears when a field is selected.

The password goes only to `api.strem.io`. The service stores just the session key, in `/var/lib/homeplus/config.json` (mode 600).

Artwork: the full-screen background uses Stremio's large image size, and the cards use the medium size. Fetching a larger source works better than upscaling on the TV.

Continue Watching shows your library items that have a playback position, newest first. The list is cached for 60 seconds.

Resuming launches Stremio with `{"contentTarget": "stremio:///detail/<type>/<id>/<videoId>"}`, LG's usual deep-link convention.
**Not yet verified:** whether the webOS Stremio app honours that parameter. If it ignores it, Stremio still opens normally. Check that on the TV and adjust `openRailItem()` in `app/js/launcher.js`.

## Troubleshooting

- **"Couldn't read the app list"**: the service isn't elevated. Re-run `elevate-service`, as shown above.
- **Boot log:** `/tmp/homeplus-boot.log` on the TV.
- **Service log:** `/tmp/homeplus-service.log` on the TV. It records the start-up, a sample of what `listApps` returns, missing icon files and icon errors the page reports.
- **Undo autostart:** `rm /var/lib/webosbrew/init.d/50-homeplus`.
- **Boot problems:** Homebrew Channel's failsafe skips `init.d` scripts after a crash during boot.
- **Layout and hidden apps:** stored in the app's `localStorage`. Uninstalling Home+ resets them.

## Roadmap

- **Option 3 (Home button takeover):** a root daemon will watch `getForegroundAppInfo`. When LG's `com.webos.app.home` comes to the front, it relaunches Home+. Home+ already handles relaunch: it returns to the top like tvOS.

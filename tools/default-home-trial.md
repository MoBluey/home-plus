# Default Home registration: guarded developer trial

This spike checks the default-Home API lifecycle and independent recovery. It is not an installed Home Button mode.

- native/appinfo.json declares the home app category. It does not change the default by itself.
- tools/default-home-trial.js uses a supplied Luna transport to read the current default, check registration and LG Home availability, switch, resolve the default, and check that Home+ reaches the foreground from another app.
- It restores the previous default after a successful or failed attempt when ownership can be confirmed. It preserves a different launcher selected during the attempt.
- An unreadable cleanup setting or refused restoration is an error, not a successful trial.
- tools/default-home-recovery.js is the standalone guard. Its persisted snapshot and copied code live outside Home+'s app/service. Controller failure is covered by a deadline, an unavailable launcher triggers early restoration, and a separate boot hook invokes recovery after reboot. Failed settings reads retry with backoff capped at five seconds.
- An originally absent Home entry is restored as absent, while keeping other current default categories.
- The helper and its checks are development files; neither is included in the IPK.

## Run the local checks

```sh
node --test test/default-home.test.js
node --test test/home-return.test.js
node tools/build.mjs
node tools/check-package.mjs
```

The local SAM model checks the real package manifest. A desktop integration probe also started the real Home+ native launcher with the existing preview service, captured its rendered window, and restored the modelled default. That is not LG's SAM, compositor, bootd or CEC running locally.

## Required before a TV trial

Back up the actual default map, package and configuration. Stage the standalone recovery copy and its state outside the app/service, and install a temporary symlink in webosbrew init.d to a script invoking it with --start-boot. Arm --start and verify its ready marker and process before switching any default. Keep LG Home installed and preserve account settings and input mappings.

Recovery state is JSON with version 1, active, deadline (epoch milliseconds), applicationDir and before (the original defaultApps object). With that state saved privately:

```sh
node default-home-recovery.js --start /absolute/path/state.json
node default-home-recovery.js --start-boot /absolute/path/state.json
node default-home-recovery.js --restore /absolute/path/state.json
```

Start detaches a watcher that runs until the deadline or unavailable launcher, then restores only Home. Start-boot detaches immediate recovery, retrying while settings are unavailable. Restore is the manual path. An observed different launcher is preserved. This is trial tooling, not a permanent Home Button mode or packaged uninstall supervisor.

Existing HomeBack hooks, foreground watching and No Flash must not mask the default route during that check.

Do not enable this through an install or create an upstream PR from local checks alone.

## OLED65C4PSA / webOS 26 findings, 1 November 2026

- The TV accepts the home category and setDefaultApp. launchDefaultApp opens Home+. Paired Home presses from Live TV and Netflix reached the Home+ foreground in 521 ms and 321 ms respectively, with Watch and No Flash off. These are measured upper bounds including the foreground query, not first-paint measurements or physical-button checks.
- Default registration alone does not cover app exit: closing Netflix still opened LG Home. Existing Watch mode returned to Home+ instead. Keep the existing routing and boot hooks.
- An independent watcher restored the exact original default map 2.887 seconds after the controller was killed. With the native launcher deliberately unavailable, the watcher restored the map and launchDefaultApp opened LG Home while that launcher was still unavailable. SAM acknowledging a launch is not proof that a native app started.
- With a trial mapping active, a TV reboot ran the independent boot recovery and restored the exact original map. Reset uptime and a fresh boot-hook log confirmed the restart; the TV's kernel boot ID did not change. This checks recovery across reboot, not persistent replacement of every boot route.
- Adding the category exposed a wake race: app-exit cleanup erased recent Home+ history before standby used it. Preserve that history only during power-off processing. Local checks cover manual close, stale history and HDMI/explicit-launch cancellation. The TV event capture confirms power-off processing precedes Home+'s close.
- With the correction, Home+ returned after standby when previously in front; Live TV and an HDMI 1 input with no device connected retained their foregrounds. Power was controlled through the paired remote and Wake-on-Wi-Fi. Connected-HDMI picture/audio, ARC/eARC and CEC remain unverified.
- A full mains-off cold boot, physical-button routing without the existing HomeBack hook, and a permanent default mode/uninstall supervisor remain separate acceptance work. Do not claim a complete launcher replacement from these results.

## API reference

The reference implementation reads settingsservice/getSystemSettings with category general and keys defaultApps. It writes applicationManager/setDefaultApp with category home and appId, then launches applicationManager/launchDefaultApp with category home.

- [Galaxy Launcher implementation](https://github.com/guntiss/webos-galaxy-launcher/blob/main/service/service.js)
- [Galaxy Launcher replacement notes](https://github.com/guntiss/webos-galaxy-launcher#replacing-the-stock-home)
- [OpenLauncher firmware caveats](https://github.com/MinerBlox/TVLauncher#replace)

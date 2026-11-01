# Default Home registration: local prototype

This spike checks the default-Home API lifecycle before a TV trial. It is not an installed Home Button mode.

- native/appinfo.json declares the home app category. It does not change the default by itself.
- tools/default-home-trial.js uses a supplied Luna transport to read the current default, check registration and LG Home availability, switch, resolve the default, and check that Home+ reaches the foreground from another app.
- It restores the previous default after a successful or failed attempt when ownership can be confirmed. It preserves a different launcher selected during the attempt.
- An unreadable cleanup setting or refused restoration is an error, not a successful trial. The helper cannot independently recover from its caller being killed.
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

Prepare a backup and an independent recovery process plus boot recovery outside the Home+ app/service. The recovery path must survive a killed controller or removed Home+ and must not guess ownership when settings cannot be read. Confirm it locally before switching a TV default. Keep LG Home installed and preserve account settings and existing key mappings.

The C4/webOS 26 must still prove that it honours setDefaultApp and resolves the home category. Home from apps/Live TV/HDMI, app exit, cold boot, standby/wake, physical CEC, LG Home fallback, unavailable-app recovery and rollback remain TV acceptance cases. Existing HomeBack hooks, foreground watching and No Flash must not mask the default route during that check.

Do not enable this through an install or create an upstream PR from local checks alone.

## API reference

The reference implementation reads settingsservice/getSystemSettings with category general and keys defaultApps. It writes applicationManager/setDefaultApp with category home and appId, then launches applicationManager/launchDefaultApp with category home.

- [Galaxy Launcher implementation](https://github.com/guntiss/webos-galaxy-launcher/blob/main/service/service.js)
- [Galaxy Launcher replacement notes](https://github.com/guntiss/webos-galaxy-launcher#replacing-the-stock-home)
- [OpenLauncher firmware caveats](https://github.com/MinerBlox/TVLauncher#replace)

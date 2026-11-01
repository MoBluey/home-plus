# Default Home and startup compatibility

Research and C4 trial results, 2 November 2026. This concerns the developer default-Home trial, not a released Home+ mode.

## Advice

Keep LG Home installed and launchable. Treat default registration, the Home button, app exit, startup and standby as separate capabilities. A successful settings write is not proof of routing or a visible launcher.

Home+ currently declares webOS 5 or later as its runtime target. That does not establish this new default-Home route on every supported TV. [Home+ requirements](https://github.com/pbalazaar/home-plus#requirements).

| Retail webOS | Evidence | Advice for this approach |
| --- | --- | --- |
| 26 / internal 11.x | C4 OLED65C4PSA, 11.2.0-3703, firmware 43.21.73: our default registration plus existing startup hooks survived a real mains disconnect. Home+ opened automatically; physical short/long Home preserved both launchers. | Confirmed on this model/firmware and this combined build. Other TVs still need a guarded check. |
| 25 / internal 10.x | Galaxy Launcher reports an LG G4 on 10.3.1, using default registration and a boot hook. This is another launcher, not a Home+ acceptance report. | A reasonable candidate for an opt-in capability probe; do not label Home+ verified. [Galaxy Launcher](https://github.com/guntiss/webos-galaxy-launcher#replacing-the-stock-home). |
| 24, 23, 22 / internal 9.x, 8.x, 7.x | Home+'s author develops on a C3 with 9.2.1. The current compatibility report does not validate this new default route. | Retain existing Watch/startup/key handling. Do not assume default registration replaces those paths. [Home+ reports](https://github.com/pbalazaar/home-plus/blob/main/COMPATIBILITY.md). |
| 6.x | The C1 6.5.3 report confirms Home+ 0.21 setup, elevation and its boot script. Home-button handling was not covered. | Startup through the existing hook has evidence; this default route remains unverified. [C1 report](https://github.com/pbalazaar/home-plus/issues/37). |
| 5.x | Within Home+'s declared runtime target, but no Home+ hardware report in the compatibility table. | Experimental. Check both runtime and actual routing before enabling anything. |
| 4.x and earlier | Outside Home+'s declared runtime target; its service requires newer Node than these TVs ship. | Do not offer this as compatible with the current package. |

Version numbers alone are insufficient. OpenLauncher's implementation distinguishes older Home-key routing from newer default-app routing, but explicitly says detection cannot prove that firmware honours setDefaultApp. This supports capability checks, not a universal 25/26 guarantee. [OpenLauncher implementation notes](https://github.com/MinerBlox/TVLauncher#replace).

## What a guarded check must establish

- The package is registered for the home category; the original default can be read and restored exactly.
- Setting Home+ as default actually resolves to it. Check the real Home route with other interception disabled, not only launchDefaultApp acknowledging a request.
- Startup and app exit work through their existing hooks where the default route does not cover them. A reboot, remote wake and mains-off start are distinct cases.
- LG Home remains reachable, and failed launch/uninstall recovery can run outside Home+'s package. An uninstall removes the app, not necessarily its default setting.

Unsupported methods or failed observations should restore the snapshot and leave the existing Home+ mode available. Do not silently substitute a system-file replacement.

## C4 cold-start result and limits

With Home Auto Launch off, bootd put Live TV in front at 17.258 seconds of uptime and Home+ at 26.122 seconds. The observer first saw Home+ at 26.89 seconds. The owner saw no LG Home and confirmed both physical Home and long Home. All 67 package files matched after boot; the only parsed configuration change was enabling homeOverride for Watch.

The default survived the power cut. Independent recovery restarted with the original bounded deadline. The startup hook launches Home+ by ID, so this does not show default registration alone causes startup or improves its speed. The 26-second figure is TV uptime, not measured time from the physical power-button press or first paint.

Connected HDMI, ARC/eARC and CEC remain unverified. No older TV was available for direct checks. A permanent default mode and uninstall supervisor are separate implementation work.

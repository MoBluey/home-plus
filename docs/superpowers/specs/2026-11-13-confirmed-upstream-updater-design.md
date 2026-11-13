# Confirmed upstream updates for Home+

Status: approved by Mo on 13 November 2026 in msg_01M79AVVVXE3ANZ2EQ35JFPXCX. Implementation authorised; TV deployment remains separate.

## Intent

Check for official Home+ releases, preserve tracked custom source changes through fork builds, show a prompt that matches the current theme, and update only after confirmation. Keep accounts, layout, remote settings and access to LG Home. This updates Home+ only, never LG firmware or other apps.

Mo chose pbalazaar/home-plus releases as the upstream base, with a companion fork workflow preserving custom changes. An official-package fallback explicitly warns that it drops custom code. Never label a patched fork package as the official asset.

## Source-preserving builds

Mo approved fork builds that preserve source changes. When changes conflict, offer Keep current build or install upstream while dropping custom changes. Keeping the current build suppresses automatic notices for that upstream version; only a strictly newer upstream version repeats the process.

The TV has a compiled ARM launcher and JavaScript service, rather than the source checkout and ARM toolchain. A separate fork workflow records the upstream base and custom source revision, identifies exact changes already upstream, integrates the remainder in an isolated checkout, runs their checks, and builds a complete custom IPK. The TV receives that verified build with an explicit upstream-base/custom-change label and still requires confirmation. Reuse the existing ARM/package/check workflow; do not add Git or a compiler to the TV.

Stop source integration on conflicts, ambiguous equivalence or failed checks. Publish an honest per-release result with concise conflict/check information, rather than a broken package. A clean patch application does not prove runtime compatibility. Do not infer or merge arbitrary binary edits, or drop a patch merely because its PR was merged. Unknown installed modifications receive a warning and backup, not an unproven rebase. Track source commits rather than stashing somebody's uncommitted working tree.

Publish a result manifest alongside the fork's existing release artifacts. It identifies the exact upstream version/release/base commit and source revision, plus either a verified package or a conflict/check-failure result. An absent, pending or unreachable result is not a conflict. Fork builds embed their own fixed repository/feed provenance; official builds follow the official feed. The TV needs no free-form feed URL or GitHub credential entry.

Check upstream on a modest schedule and allow a manual workflow run. Produce one candidate per new upstream release/source revision without rewriting or merging existing user branches. Publish only from the trusted fork context after required checks; untrusted PR jobs never receive publishing credentials. The precise trigger and job reuse are builder decisions, not extra app preferences.

If all runtime custom changes are exactly absorbed upstream, use the verified official package. Semantically rewritten equivalents require review. A fork package whose customisations include the updater must preserve it too.

## What the user sees

Under Settings > About:

```text
Check for Updates        Home+ 0.x.y
Automatic Update Checks On
```

When a newer stable version exists, show a normal Home+ confirmation sheet only while Home+ is foreground and no other sheet, first-run flow or editor is open:

```text
Home+ update available
Installed 0.x.y  >  Available 0.x.z
Brief release notes

[ Keep current build ]  [ Update now ]
```

For a ready fork build, show that custom changes are preserved and identify the upstream base. For a conflict or failed check, explain the cause and show:

```text
Custom build could not be prepared
Upstream 0.x.z conflicts with our custom changes
Brief affected-change/file information

[ Keep current build ]  [ Install upstream ]
```

The official action explicitly states that it drops all custom code in that package. Keep current build is always focused by default; Back has the same keep/suppress effect. Unknown or manually modified builds receive a replacement warning.

Use existing sheet styling and readable progress while Home+ is running. Before closing it, explain that installation/recovery continues independently and Home+ will reopen when ready. Its own sheet cannot draw while its process is closed. Do not restart the TV, put overlays over other apps, force a foreground return over a new user-selected app, or change volume, input or power settings.

An automatic check runs at most daily after service startup settles; cached results never delay opening Home+. A later foreground entry may display a ready result. Choosing it persistently ignores that upstream version. Do not re-prompt after reboot, cache expiry, reissued same-version assets or a same-version fork build changing from failed to ready. Only a strictly newer upstream version repeats the process. A manual check may show the ignored release's status and offer an explicit action without clearing suppression. Automatic Checks Off leaves manual checks available. No automatic installation, prereleases, downgrades or reinstall button in this feature.

The daily cadence is a builder proposal. Persistent version suppression and the two conflict outcomes are explicit user requirements.

## Approaches

1. Reuse Homebrew Channel's documented install service, with a small independent Home+ helper for backup, completion and recovery. Homebrew Channel already downloads, verifies SHA-256 and installs packages; the helper survives Home+'s replacement. This is the recommended implementation choice.
2. Move the whole `tools/install.sh` procedure onto the TV and call the platform installer directly. The C4 has exercised this route, but maintaining another complete installer would duplicate more platform handling.

The user delegated routine implementation choices. Use approach 1 where its preflight checks pass. Fail clearly if a required capability is absent; do not silently switch installers or change platform permissions.

## Release checks and custom-build warnings

- Read the latest non-draft, non-prerelease upstream release and manifest over HTTPS. Fork builds also read their fixed fork-result feed for that exact upstream version. The TV needs no GitHub account/token or new dependency.
- Validate exact app ID, upstream/base version, artifact version, release tag, repository, asset name, size and SHA-256. Accept only packaged designated repositories and bounded HTTPS redirects to their GitHub asset hosts. User requests cannot provide another URL, shell command, package ID or file path.
- Compare versions numerically, not lexically. Cache successful checks and back off failures. A failed automatic check is quiet; About shows the failure and allows retry.
- Build provenance records upstream base, source revision and managed customisations. A shipped-file hash inventory detects unknown installed edits. Version equality alone never proves an official build. Installations without provenance receive a warning, not a stock-build assumption.
- Manual/automatic checks do not download an IPK. Installation requires a fresh, authenticated confirmation for the exact version and checksum shown, revalidated if release metadata changed.

## Installation and recovery

Home+ and its service are killed during package replacement on supported webOS versions. Neither the installer subscription nor rollback may depend on those processes staying alive.

1. Preflight elevated access, Homebrew Channel installation support, enough storage, required backup/installer tools and a single-transaction lock. Refuse a second update or other Home+ removal/default-Home setup changes during the transaction. If the user leaves Home+ before the update is committed, cancel rather than interrupt their new app.
2. Download and verify the selected IPK before changing the installation. Inspect its app/package identity and version without extracting arbitrary paths. Reject malformed archives, unexpected IDs or unsupported package contents. The implementation may let Homebrew Channel download the same immutable, hash-verified asset again to reuse its documented installer; avoid a new local download server.
3. Create a private, durable recovery directory outside the application. Back up the exact installed app/service/package as a reinstallable IPK, and separately back up the current Home+ configuration and relevant startup/default-Home state. Verify the backup before proceeding; stop if it cannot be made. Never back up credentials into repository files or public packages.
4. Copy the installation helper outside the package, with a durable transaction record and a temporary Homebrew startup recovery hook. Launch it independently before replacing Home+. Use helper code shipped with the installed package, never a downloaded shell script. Resolve its runtime/tool availability before beginning.
5. The helper holds the Homebrew Channel install subscription through its final result and restores the Home+ service's elevation using the existing elevation mechanism. Then start the new service and check the expected version and root health before reopening the app.
6. Verify the installed payload against the approved package, the launcher's normal foreground entry and preservation of account/layout/preferences. Keep the original LG Home recovery guard effective. If it restored LG Home during a long package replacement, re-establish the user's Home+ default through the existing compatibility-checked operation only after the replacement is healthy. Preserve Startup and Input button choices.
7. On failure, reinstall the verified previous package, restore its configuration and relevant state, re-elevate and check it. If recovery cannot complete, retain the backup/log and leave LG Home available with a clear failure message. Never report success from an early installer acknowledgement or a timeout.
8. Persist state transitions before each irreversible step. After power loss or helper failure, the temporary startup hook resumes recovery without waiting for the broken app to run. Remove the hook only after verified success or recovery. Retain one last known-good backup; replace it only after a later transaction can do so safely.

An installer whose completion is unknown remains quarantined until a verified new runtime generation. webOS may reuse its kernel boot UUID across a full restart, so use a random generation created once under a verified [SECRET_xiuzfr]-owned, private directory on the volatile /run tmpfs. Only the exclusive directory creator may initialise it; existing incomplete, corrupt or unsafe state refuses recovery rather than regenerating. No normal updater path removes or replaces that state. Standby and service restarts retain the generation. Legacy UUID-only pending transactions are baselined to the current generation without installing, then require a later generation; legacy live worker locks still use PID/start identity. Verify the runtime directory's standby/restart lifecycle on the target before delivery.

Cancellation is available before package replacement. Once installation starts, explain that recovery must finish before Home+ can reopen; do not promise that Back cancels the platform installer. No forced TV reboot is part of normal updating.

The helper owns only Home+'s package, private state and its own temporary recovery hook. It must not remove unrelated hooks, change user-selected apps/inputs, touch firmware partitions, weaken TLS or expose an unauthenticated installation endpoint. Use fixed paths, private permissions, safe process arguments, bounded downloads/timeouts and the current local API's caller protection. Keep Node 8.12 compatibility and SDL 2.0.14 compatibility.

## Scope and limits

- Add one update controller, one independent installation/recovery helper and the minimum native/settings integration. Reuse current networking, sheets, root setup and packaging patterns. No new framework, package manager or background daemon.
- Include provenance in packaged builds and update/check status in the authenticated local API. Match the native About page and the QML settings panel, so checks behave consistently.
- Current extra features are provider-page fallback and pending PRs #66/#67; the updater is also custom until upstream accepts it. Preserve changes through complete fork packages, never hidden patches layered over a verified official release.
- The TV currently runs customised 0.24.0. Preparing this feature does not authorise installing an official package or disrupting playback. Arrange a separate TV trial after local verification.

## Acceptance checks

- The real native launcher and real service can check a release, display the correct themed prompt, defer it and require explicit confirmation. Exercise production routes, not only direct store mutation.
- Check numeric versions, current/older/prerelease releases, custom/unknown provenance, edited installed files, stale confirmations, network/rate-limit failures, unexpected hosts, malformed metadata/packages and hash failures. No invalid result reaches installation.
- Exercise clean fork integration, exact changes already upstream, conflicts, failed checks and pending/unavailable build results. Keep current build must suppress the same upstream version across restart, changed build status and reissued assets; a newer version prompts again. Official fallback must use the upstream hash, warn about custom-code loss and preserve settings.
- Exercise independent-helper completion after the launching service dies, duplicate requests, cancellation, insufficient space, missing tools, install/elevation/health failures and recovery from persisted interrupted transactions. Account values never appear in public logs or fixtures.
- Check new native/QML screens, focus/Back behaviour, service execution on Node 8.12, the ARM package and existing Home/default-Home paths affected by transaction locking.
- Independently review the root installation/recovery boundaries before delivery. A TV trial needs a verified backup and consent, and must include normal remote launch, saved settings/accounts, Home/hold-Home/Input behaviour, an actual update and a controlled rollback. Until that trial passes, report it as locally verified and not TV-ready.

## Evidence and implementation calls

- Upstream v0.25.0 publishes an IPK and a manifest with its app ID, version, byte size and SHA-256: https://github.com/pbalazaar/home-plus/releases/tag/v0.25.0.
- The installed TV has Homebrew Channel 0.7.3. Its version-matched README documents `/install` (`ipkUrl`, `ipkHash`, subscription; final `finished: true`), `/exec`, `/spawn` and `/elevateService`: https://github.com/webosbrew/webos-homebrew-channel/blob/v0.7.3/README.md.
- Its 0.7.3 `services/service.ts` confirms that arbitrary app installation does not re-elevate that app's service; its special self-update path is only for Homebrew Channel. Home+ still needs independent completion and recovery.
- Home+'s `tools/install.sh` and `tools/build.mjs` provide the existing verified-install and package-layout patterns. `service/default-home-guard.js` restores LG Home after a package is absent for five seconds; this must be reconciled, never casually disabled.
- Exact helper internals, concise error wording, storage limits and archive-validation mechanics are left to the builder, within these invariants. Any required relaxation of backup, confirmation or recovery comes back to Mo rather than being omitted.

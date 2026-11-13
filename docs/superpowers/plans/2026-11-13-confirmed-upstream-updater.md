# Confirmed Home+ Updater Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan with separate owners and required reviews. Steps use checkbox syntax for tracking.

**Goal:** Deliver confirmed Home+ updates that preserve tracked custom code through fork builds and offer an explicit official fallback, without repeated same-version notices.

**Architecture:** A Node 8.12 controller validates release/build metadata and owns persistent consent/suppression state. A copied independent helper backs up, installs through Homebrew Channel and recovers; a separate fork workflow prepares checked packages. The native launcher and QML settings consume the same authenticated API.

**Tech Stack:** Existing C/SDL 2.0.14 sheets, dependency-free Node service, Git and GitHub Actions, Homebrew Channel 0.7.3 documented installer/elevation interfaces.

**Spec:** docs/superpowers/specs/2026-11-13-confirmed-upstream-updater-design.md, approved by Mo in msg_01M79AVVVXE3ANZ2EQ35JFPXCX.

## Global Constraints

- Node 8.12 minimum for shipped service/helper code; no new runtime dependencies. Preserve SDL 2.0.14 support.
- No TV changes or deployment in this implementation approval; no firmware writes, altered audio/inputs/privacy, automatic installs or automatic branch merges/force-pushes.
- Keep current build persistently suppresses the chosen upstream version across restart, changed build status and reissued assets. Only a strictly newer upstream version prompts again; manual status never clears suppression.
- Conflict/check failure offers Keep current build or explicit upstream installation dropping custom code.
- Exact source equivalence may be dropped; ambiguous changes/conflicts/failed checks stop the custom build. Unknown installed edits are never silently merged.
- Backups must be verified, private and reinstallable before replacing the package. Independent recovery must survive replacement/power loss and retain LG Home access.
- Fixed packaged update repositories, HTTPS validation, bounded data/time, authenticated actions, fresh version/hash confirmation and one transaction only.
- Keep workers' ownership separate; main owns integration/Git. No worker TV access, posts, branch switching, commits or pushes. Continue with authorised build rather than adding another routine execution approval.

## Review Focus

- A fork result changes from conflict to ready after a version was declined: the ignored release stays silent (Task 1).
- An update kills its requesting service or power fails mid-install: the external transaction completes or restores the previous package (Task 1).
- A default-Home guard restores LG Home during replacement: retain recovery and reapply the requested default only after health checks (Task 1).
- Source patches are squashed/rewritten upstream or only partly equivalent: do not silently lose functionality; conflict/ambiguity leaves the current TV build available (Task 2).
- Remote Back/default focus/late API replies or a new foreground app: no accidental install, repeated prompt or focus theft (Task 3).

## Shared contracts

- `service/updates.js` exports `createUpdates(options)` returning `{ state(), act(body), start(), busy(), close() }`. State is synchronous and does no network. Actions are asynchronous but check/install preparation returns progress promptly; background work never blocks normal Home entry.
- `GET /api/updates` returns `{returnValue,currentVersion,upstreamVersion,phase,currentSource,automatic,prompt,ignoredVersion,confirmation,title,summary,notes,warning,message,canInstallPreserved,canInstallUpstream,progress}`. Phases: idle/checking/ready/conflict/failed/installing/recovering/completed/error. Strings are plain text; missing candidate values are empty, installation choices false by default.
- `POST /api/updates` accepts `{action:"check"}`, `{action:"keep",version}`, `{action:"automatic",enabled}`, or `{action:"install",version,confirmation,target:"preserve"|"upstream"}`. Returns the current state. Installation requires the exact current confirmation/version/target; keeping an older visible version must not suppress a newer release.
- `createSettings` gains optional `updates` controller dependency; backend service.js passes it, UI owner edits settings.js. About's automatic row calls `updates.act`; Check for Updates opens native page `updates`. Existing fallback works without an injected updater.
- Package file `update-info.json` in the application contains schemaVersion1, upstream `{repository,version,tag,commit}`, source `{repository,commit,base}`, customised boolean, customisations array, updater boolean and fileHashes keyed `application/<relative>` / `service/<relative>`. Exclude itself and generated api-token/cache files from its inventory. Unknown provenance warns rather than guessing.
- Fork result asset `com.github.pbalazaar.homeplus.update.json` on tag `upstream-v<VERSION>` contains schemaVersion1, upstream/source objects as above, status ready/conflict/failed, message, conflicts array, customisations and optional artifact `{url,sha256,size,version}`. Result must match the exact official upstream release; ready with no runtime customisations may use its official artifact. Backend and workflow owners coordinate any required adjustment before coding either boundary.

---

### Task 1: Update controller and independent installation/recovery

**Owner:** backend owner. **Files:** create service/updates.js and focused helper/archive modules under service as needed, update service/service.js; own test/updates*.test.js and relevant focused API coverage. Do not edit settings.js, native files, tools/build.mjs or workflow files.

**Consumes:** Existing network, private config/tmp/fs helpers, documented HBC installer/elevation APIs, packaged update-info/result schemas. **Produces:** Shared controller/API contract above and an independent, recoverable installation path.

- [ ] Add failing checks for numeric versions, malformed metadata/hosts/hash, exact candidate consent, suppression across restart and same-version result changes, manual checks, automatic off and unknown/edited builds.
- [ ] Run focused checks and confirm failures identify missing updater behaviour. Implement the controller with dependency-injected release fetching/clock/worker launch for fixtures, validated fixed repositories and bounded production network paths. Wire authenticated API and existing root/default-Home/removal transaction boundaries.
- [ ] Add meaningful worker/real-file fixtures for app/service backup, archive identity/path validation, package replacement killing the parent, install/elevation/health failure, duplicate requests and interrupted persisted transactions. Assert untouched account/layout/input state and LG Home fallback. A mocked install acknowledgement is not success proof.
- [ ] Implement independent backup/install/recovery using shipped code copied outside the package; preflight actual tools/permissions, durable lock/state/hook and verified reinstallable backup. Use existing default-Home compatibility restoration after a healthy install. Refuse unsupported platforms clearly. Preserve explicit foreground changes.
- [ ] Run focused checks on normal Node and actual Node8.12 target. Exercise the real service HTTP producer/consumer with a controlled release server and isolated filesystem/installer boundary. Report actual production-network assumptions and any unverified hardware case.
- [ ] Main reviews this slice, preserves ownership for corrections and commits verified changes only. No direct worker Git mutations or TV actions.

### Task 2: Source-preserving builds, provenance and fork feed

**Owner:** fork workflow owner. **Files:** create tools/prepare-fork-update.js and focused tool helpers only if needed, create .github/workflows/fork-updates.yml; modify tools/build.mjs and required Docker/build wrappers, package checks; own test/fork-updates*.test.js and packaging fixtures. Do not edit service or native UI.

**Consumes:** Shared update-info/result schemas. **Produces:** Honest fork result plus checked IPK, exact upstream source/base provenance and installed-file hashes; no changes to ordinary user branches.

- [ ] Add real temporary-Git-repository fixtures for new upstream release with retained source changes, exact changes already upstream, conflicts, failed checks, invalid refs/repositories and replay/deduplication. Verify candidate operations do not mutate main or dirty user work.
- [ ] Implement isolated integration of the managed fork source revision onto the exact upstream release. Reuse Git equivalence/merge behaviour without semantic guesses. Record conflicts and skip publication of broken packages. No automatically resolved conflict or forced branch update.
- [ ] Extend existing packaging with accurate provenance and payload hash inventory, independent of filename/version equality. Unknown desktop build origin remains unknown. Ensure Docker and CI propagate verified source metadata; no secrets or workstation paths ship.
- [ ] Add a scheduled/manual trusted-fork workflow reusing current build/service/native/compatibility checks before publishing a ready result. Publish conflict/failed result honestly; keep publishing permission separate from untrusted code. Use predictable per-upstream-version tags and do not overwrite immutable artifacts silently.
- [ ] Run tool fixtures and package checks; exercise an actual candidate Git integration and ARM/package build before claiming the workflow usable. Validate resulting manifest against Task1 controller. Document any default-branch activation requirement rather than merging without permission.
- [ ] Main reviews/commits verified slice, with owner correcting findings. Do not publish a new upstream PR or modify review comments.

### Task 3: Native confirmation UI, About/QML settings and whole-product checks

**Owner:** native UI owner; main integrates and owns Git. **Files:** create native/src/updates.c; modify home.h, actions.c, settings.c, results.c, input.c, idle.c, main.c/Makefile only for minimal feature hooks; service/settings.js; preview harness/run scenes, relevant native checks and README.

**Consumes:** Controller state/actions and package schemas. **Produces:** Same-themed foreground-only notices, manual updates/automatic toggle, safe conflict choices and persistent Back/keep behaviour through the actual service API.

- [ ] Add a representative real-launcher preview workflow first: check through About, view a candidate, Back/keep it through the real HTTP route, re-enter and confirm no repeat. Preview network/installer fixtures must remain isolated from TV/production accounts.
- [ ] Implement updates.c as the feature owner. Parse bounded strings and choices, default to Keep current build, queue authenticated actions, display a normal sheet, keep busy/progress/error state clear and never auto-confirm. Add only the required shared state/tags/actions/hooks; normal Home entry must remain nonblocking.
- [ ] Wire native About and external QML settings to the same controller. Show conflict/check-failure cause, explicit custom-code replacement warning for stock fallback, and automatic-check switch. Poll only while needed; no prompt over another sheet/editor/setup flow or a new foreground app.
- [ ] Render/inspect ready, conflict, failed, custom/unknown warning, checking/progress and About states with the production launcher/service; exercise remote default focus, Back, explicit install request and stale-response handling. Run focused native checks, then native sanitizer/QML/package/service and Node8 checks at integration.
- [ ] Conduct a fresh independent whole-branch review of code, actual diff, installer/recovery trust and producer-consumer boundaries. Owners fix valid defects and recheck affected paths; do not expand unrelated scope.
- [ ] Update README and private handoff with actual capabilities, recovery and verification limits. Commit and push the verified branch to the fork; report CI and ask before merging. TV installation/physical end-to-end trial and upstream PR publication remain pending separate approval.

## Plan self-review

Spec coverage includes source preparation, fixed feeds/provenance, all popup/suppression choices, independent backup/install/recovery, settings parity, privacy and compatibility. Interfaces and exact phase/action/schema names above are shared by all owners. Each of the five review-focus cases has an owning check. Runtime safety is a delivery gate; do not equate desktop fixtures or package validity with verified TV installation.

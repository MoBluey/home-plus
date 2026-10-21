# Security Policy

Home+ runs a background service as root on rooted LG TVs, so security problems in it matter. Thank you for reporting them privately.

## Supported versions

Only the latest release gets security fixes. Please check that the problem is still there in it before you report.

## What to report

For example:

- anything that lets another app on the TV, a web page, or another device on the network use Home+'s service: its local API on `127.0.0.1:47823` (each request needs a token kept in Home+'s app folder) or its luna methods;
- a way to read or change files as root through the service (it serves app icons and artwork from apps' own folders);
- sign-ins or keys (Stremio, Trakt, Jellyfin, Plex, TMDB) leaking from `/var/lib/homeplus`, from logs or over the network;
- the boot script, the Home button takeover, or Home+'s LG Input Hook binds doing more than they should.

Report these to their own projects instead: rooting itself, the Homebrew Channel, LG Input Hook, PicCap, HyperHDR and webOS. Problems that need root on the TV already aren't in scope.

## How to report

Please don't open a public issue. Use GitHub's private reporting: the repository's **Security** tab, then **Report a vulnerability**. Say which Home+ version it is, the TV model and webOS version, and how to make it happen.

If you can't use that, open an issue that only asks for a private contact, with no details in it.

## What happens next

The maintainer will reply as soon as they can. Once a fix is ready, it ships in a new release, and the release notes credit you unless you'd rather not be named.

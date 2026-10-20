# Top Shelf artwork for apps without a picture

Approved by the owner in conversation on 2026-10-20, as "popular titles + glow, rotating".

## Problem

Many LG apps don't ship real artwork. On the C3, Prime Video, Smart IPTV, YouTube, Apple TV and
Stremio carry LG's generic square-pattern backgrounds, so the Top Shelf behind them is grey or
red squares. tvOS apps always show something: the Apple TV app's Top Shelf shows "What to Watch".

## Evidence

- LG's generic backgrounds are named `<n>_default_background.png` or `*_preview.png`
  (`amazon_preview.png`, `youtube_leanback_preview.png`). Real ones have other names, such as
  Netflix's `launcher_background_image.png`. Splash screens (`*_splash*`) are a logo on a plain
  colour.
- TMDB's v3 API lists streaming services per region (`/watch/providers/movie|tv?watch_region=`).
  It finds what's popular on one of them (`/discover/movie|tv?with_watch_providers=<id>&watch_region=&sort_by=popularity.desc`)
  and gives each title's logo (`/<type>/<id>/images?include_image_language=en,null`).
  - It authenticates with a v3 API key (`api_key=`) or a read-access token (`Authorization: Bearer`).
  - Pictures come from `image.tmdb.org/t/p/<size>`.
- The TV's store country is the `smartServiceCountryCode2` system setting (`IT` on the C3).

## Design

1. **Streaming apps show "Popular on <service>".**
   - An app gets this when its name matches a streaming service TMDB lists for the region.
     The match ignores case and spacing and treats "+" as "plus" (Disney+ is Disney Plus, Prime
     Video is Amazon Prime Video).
   - **Content:** the 10 most popular subscription titles on that service, series and films
     merged by popularity. Each has a backdrop (w1280), a logo (w500) and a line like
     "Series · 2026".
   - **Caching:** the service keeps the lists for 12 hours and proxies the pictures. When TMDB
     fails, it serves the old list.
   - **Look:** the backdrop fills the Top Shelf under the usual scrims, with a "Popular on
     <service>" pill, the title's logo (or its name, big) and the line. The hint underneath is
     unchanged, and OK opens the app.
   - **Rotation:** a new title every 8 s with a crossfade. It only advances while that app is
     focused in the top row, nothing covers the shelf, and the screen isn't dimming. Only the
     title on show and the one fading out keep textures.
   - This takes precedence over the app's own picture, as the Apple TV app's Top Shelf does.
2. **Every other app shows a glow.** Generic backgrounds and splash screens no longer count as
   artwork, so those apps get the glow made from their icon, with their tile large. Streaming
   apps fall back to the same glow without a key, or while their list loads.
3. **Setup:** `luna-send ... tmdbSetup '{"key":"..."}'` over SSH, like `traktSetup`. The key is
   checked against TMDB before it is saved; an empty key removes it. Settings → Accounts shows a
   TMDB row (Set Up / Not Set Up).
4. **Privacy:** only once a key is set does Home+ contact `api.themoviedb.org` and
   `image.tmdb.org`. The README says so.

## API

- `GET /api/featured?app=<id>` returns `{ provider, items: [{ id, name, meta, background, logo }] }`.
  `items` is empty for apps that aren't streaming services, and when there is no key.
- The `tmdb` status (`{ configured, region }`) rides on `/api/continue`, beside the other
  accounts, for Settings.

## Checks

- The preview harness mocks TMDB (`HOMEPLUS_TMDB`, `HOMEPLUS_TMDB_IMAGES`): two services, mixed
  titles, logos.
- Shots: a streaming app with featured art, the crossfade, an app with a generic background (the
  glow), and Settings → Accounts.
- On the TV: once the owner has set a key, real screenshots. Memory stays at two backdrop
  textures.

## Change: one carousel for everything

When Stremio deep links proved impossible, the owner chose to turn Continue Watching into the same
kind of carousel, with no row of cards. Stremio, Jellyfin and Plex rotate through what you're
partway through. Each title shows its logo where there is one, a progress bar and position dots.
Up, then Left and Right, steps through the titles, with the chevron above the dock showing that you
can. OK opens the app and names the title.

The carousel loads only the pictures for the title on show, the one fading out, and the next. A
refresh keeps the title on show by its id and keeps its pictures. A failed refresh keeps the last
list rather than emptying the shelf.

## Change: Trending Now, current titles, region

- **Trending Now:** the owner disliked the glow behind YouTube and Smart IPTV. Apps with no
  artwork of their own (and not a streaming service) now rotate TMDB's trending titles
  (`/trending/all/day`) under "Trending Now". OK just opens the app.
- **Name matching:** "YouTube" matched "YouTube Premium" and showed stale Originals. A service now
  matches only by the same name (or with "Plus"), or by a name ending in the app's ("Amazon Prime
  Video" for "Prime Video").
- **Current titles:** popularity alone favoured long-running daily shows (Law & Order, WWE Raw).
  Lists now take series from the last four years (no news, reality, soap or talk) and films from
  the last two. A service with too few gets a looser list.
- **Region:** the TV's store country can differ from where you watch (this C3: store IT, viewing
  AL). `tmdbSetup '{"region":"AL"}'` sets it, and `""` goes back to the TV's.
- **Not artwork:** LG's Recents images (the app's logo, large) don't count either. Only a real
  `bgImage` does.

## Steps

1. `service/tmdb.js`, the `/api/featured` route, `tmdbSetup`, and generic backgrounds no longer
   counted as art.
2. Launcher: the featured state, its requests and pictures, rotation, drawing and wake-ups.
3. Settings → Accounts row, README (setup, privacy) and the harness mock and shots.

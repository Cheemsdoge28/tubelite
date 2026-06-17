# TubeLite Backend Architecture (`tubed`)

This document describes the backend rewrite: a persistent local service
(`tubed`) that owns all YouTube communication, and a thin C++ client that the
app and the background-audio daemon both use. It is the foundation for
authenticated features (recommended feed, subscriptions, playlists, channels,
comments, likes, subscribes) on top of robust, fast video resolution.

---

## 1. Why this exists

The original backend shelled out a brand-new process for every action —
`yt-dlp` or `curl` via `popen()` — on every search, every hover preview, and
every play. On the RK3326 (quad A35, ~1 GB RAM) that meant:

- **Cold-start tax on every call.** `yt-dlp` is Python; spawning it per request
  costs hundreds of ms to seconds before any network work begins.
- **Fragility.** Output was scraped from stdout; dead public Invidious/Piped
  instances, partial JSON, and stderr noise all caused silent failures.
- **Process storms.** Racing several resolvers per call (and per hover) spawned
  many concurrent `curl`/`yt-dlp` processes, exhausting memory and crashing
  emulators/the app.
- **No path to auth.** Per-call shelling has nowhere to hold cookies/session, so
  the recommended feed, subscriptions, likes, comments, etc. were impossible.

`tubed` fixes all of this by being **one warm, long-lived process** that holds
the session, caches aggressively, and exposes a tiny local API.

---

## 2. High-level shape

```
┌──────────────┐        Unix socket (JSON lines)        ┌────────────────────┐
│  TubeLite    │  ───────────────────────────────────▶  │       tubed         │
│  app (C++)   │  ◀───────────────────────────────────  │  (persistent Py)    │
└──────────────┘                                         │                     │
┌──────────────┐                                         │  • yt-dlp (library) │
│  daemon (C++)│  ───────────────────────────────────▶  │  • InnerTube client │
│  bg audio    │  ◀───────────────────────────────────  │  • disk + RAM cache │
└──────────────┘                                         │  • cookies / OAuth  │
                                                         └─────────┬──────────┘
                                                                   │ HTTPS
                                                                   ▼
                                                            YouTube / InnerTube
```

- **`tubed`** — Python service. Uses `yt-dlp` *as a library* (no subprocess) for
  stream resolution and `youtubei` (InnerTube) calls for browse/feed/actions.
  Single instance, started on demand, persists across requests.
- **C++ client** — `YouTubeAPI` reimplemented as a thin Unix-socket JSON client.
  Same class interface and data structs as today, so `app.cpp`/`daemon.cpp` are
  largely untouched. New methods (feed, channel, playlist, comments, actions)
  are added to the same client as later phases land.
- **Transport** — `AF_UNIX` stream socket at `/dev/shm/tubed.sock`. Newline-
  delimited JSON: one request object per line, one response object per line.
  One short-lived connection per request keeps framing trivial; the *service*
  is what's persistent, so there is no cold-start cost.

Why a sidecar instead of in-process C++: stream-URL resolution requires running
YouTube's player JS to compute the `n`/signature parameters. `yt-dlp` is the
only battle-tested implementation; reimplementing it in C++ would be enormous
and break every time YouTube changes the player. Keeping `yt-dlp` in a warm
Python process gives us its reliability without the per-call spawn cost.

---

## 3. Socket API contract

Each request is a single JSON object terminated by `\n`. Each response is a
single JSON object terminated by `\n`. Every response has a boolean `ok`; on
failure it carries `error` (string).

### Phase 1 (foundation)

**`search`**
```json
→ {"op":"search","query":"lofi beats","page":1}
← {"ok":true,"results":[ <video>, ... ],"finished":true}
```

**`trending` / `feed` (unauthenticated default)**
```json
→ {"op":"trending","page":1}
← {"ok":true,"results":[ <video>, ... ],"finished":true}
```

**`stream`** — resolve a playable URL
```json
→ {"op":"stream","id":"dQw4w9WgXcQ","max_height":360}
← {"ok":true,"url":"https://...","subtitle_url":"https://...","meta": <meta>}
```

### Later phases (reserved, documented now so the contract is stable)

- `home` — authenticated recommended feed: `{"op":"home"}`
- `subscriptions` — `{"op":"subscriptions","page":1}`
- `channel` — `{"op":"channel","id":"UC...","tab":"videos","continuation":null}`
- `playlist` — `{"op":"playlist","id":"PL...","continuation":null}`
- `related` — watch-next for a video: `{"op":"related","id":"..."}`
- `comments` — `{"op":"comments","id":"...","continuation":null}`
- `comment_post` — `{"op":"comment_post","id":"...","text":"..."}`
- `rate` — like/dislike/none: `{"op":"rate","id":"...","rating":"like"}`
- `subscribe` — `{"op":"subscribe","channel_id":"UC...","on":true}`
- `playlist_edit` — add/remove: `{"op":"playlist_edit","playlist_id":"...","video_id":"...","add":true}`
- `auth_status` / `auth_login` / `auth_logout` — see §6.

### Data models

`<video>`
```json
{"id":"...","title":"...","author":"...","author_id":"UC...",
 "duration_seconds":213,"duration_string":"3:33",
 "view_count_string":"1.2M views","uploaded_ago_string":"2 years ago"}
```

`<meta>`
```json
{"description":"...","view_count":0,"like_count":0,"comment_count":0,
 "subscriber_count":0}
```

These map 1:1 onto the existing C++ `YouTubeVideo` and `VideoPlaybackMetadata`
structs, so Phase 1 needs no struct changes. Later phases add `<comment>`,
`<channel>`, `<playlist>` models.

---

## 4. Stream resolution (robust by design)

`tubed` resolves through `yt_dlp.YoutubeDL` with:

- Format selector tuned for the hardware decoder:
  `best[height<=H][vcodec^=avc1] / best[height<=H] / best` — a single **muxed
  H.264** stream that `rkmpp` decodes in hardware. No DASH merging (the device
  can't mux on the fly cheaply).
- `player_client` fallback chain (`ios`, `android`, then `web`/`tv` if needed) so
  a single client breaking doesn't break playback.
- `socket_timeout`, bounded retries, and `cachedir` reuse.
- Cookies applied when present (see §6), which also raises rate limits.

Resolved URLs are cached (see §5) with a TTL safely under YouTube's ~6 h URL
expiry. The daemon and app share this cache via `tubed`, so the "next track" is
typically already warm.

---

## 5. Caching

`tubed` keeps two layers:

- **RAM (LRU)** — hot metadata and recently resolved URLs for instant repeats.
- **Disk** — `~/.cache/tubelite` (or `/roms/tools/tubelite/cache`) as JSON, so a
  restart keeps recent results.

TTLs: resolved stream URLs ~5 h; search/trending ~10 min; channel/playlist
listings ~30 min; video metadata ~6 h. All tunable in one config block.

This is what makes loads feel instant on repeat and lets the daemon prefetch the
next track without re-resolving.

---

## 6. Authentication (cookies now, OAuth/TV-code later)

Authenticated calls go through InnerTube with the user's session.

**Method A — cookies.txt (Phase 2, available immediately):**
Export YouTube cookies from a desktop browser (e.g. a "Get cookies.txt"
extension) and drop the file at `/roms/tools/tubelite/cookies.txt`. `tubed`
detects it, passes it to `yt-dlp` (`cookiefile`) and to its InnerTube requests.
`auth_status` reports whether a valid session is present.

**Method B — in-app OAuth / TV code (later):**
The app shows a code; the user visits `youtube.com/activate` on a phone. `tubed`
runs the device-code flow, stores the refresh token at
`/roms/tools/tubelite/auth.json`, and refreshes access tokens automatically.
Friendlier on a handheld with no keyboard for cookie files.

Both feed the same internal "session" object, so feature code doesn't care which
was used. Write actions (rate/subscribe/comment) require a session; read-only
features degrade gracefully to the unauthenticated feed when none is present.

Tokens/cookies live only on the device, never leave it except to YouTube.

---

## 7. Daemon integration (minimal footprint)

The background-audio daemon no longer spawns `yt-dlp`. It asks `tubed` for a
resolved URL (`stream` op) or reads the shared cache, then feeds the URL to its
audio-only `mpv`. Net effect: the daemon holds no Python/`yt-dlp` processes,
does no per-track process spawning, and benefits from the same caching and
client fallbacks as the app. Combined with the lazy-DRM and 1 Hz idle loop
already in place, the daemon's resident cost is just `mpv` + a thin socket call
at track boundaries.

---

## 8. Lifecycle & deploy

- **Location:** `tubed.py` ships to `/roms/tools/tubelite/tubed/tubed.py`.
- **Start:** the C++ client calls `ensureTubedRunning()` — connect to the socket;
  if absent, fork/exec `python3 tubed.py`, then retry-connect for a few seconds.
  A pidfile (`/dev/shm/tubed.pid`) + socket presence enforce a single instance.
- **Stop:** `tubed` exits on SIGTERM and removes its socket/pidfile. It may also
  self-exit after a long idle with no clients to free memory (configurable).
- **Logs:** `/roms/tools/tubelite/tubed.log`.
- **Deps:** `python3` + `yt-dlp` (already required today) and `requests` for
  InnerTube. No new heavy dependency.

---

## 9. Phased roadmap

- **Phase 1 — Foundation (this change):** `tubed` service + C++ client; move
  search, trending, and stream resolution onto it; remove all `popen` paths from
  app and daemon. Stable, fast, robust base.
- **Phase 2 — Auth + real feeds:** cookies.txt; authenticated `home`
  (recommended) and `subscriptions` feeds; `auth_status`.
- **Phase 3 — Browsing:** `channel` (tabs + continuations), `playlist`,
  `related` (post-video recommendations), search filters.
- **Phase 4 — Interactions:** `rate` (like/dislike), `subscribe`, view `comments`
  with continuations, then `comment_post`.
- **Phase 5 — Library & polish:** watch `history`, `playlist_edit`
  (save/Watch Later), OAuth TV-code sign-in, richer metadata.

Each phase only adds socket ops + client methods + UI; the transport, caching,
auth, and lifecycle established in Phase 1 don't change.

---

## 10. Failure & robustness model

- Every op returns `{"ok":false,"error":...}` rather than crashing; the client
  surfaces a retryable error to the UI (the existing "Press A to retry").
- `tubed` retries transient network/extractor errors internally with the client
  fallback chain before reporting failure.
- The client auto-restarts `tubed` if the socket disappears.
- Read features work signed-out; only write actions require a session.
- No unbounded concurrency: `tubed` serializes/limits heavy `yt-dlp` work with a
  small worker pool so it never storms the device.

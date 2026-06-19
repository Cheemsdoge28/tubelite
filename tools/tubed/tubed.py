#!/usr/bin/env python3
"""
tubed — TubeLite backend service (Phase 1)
==========================================

A persistent, local-only service that owns all YouTube communication for the
TubeLite app and its background-audio daemon. It replaces the old model of
shelling out a fresh yt-dlp/curl process per request (slow cold-starts, fragile
stdout scraping, process storms) with one warm process that resolves streams
reliably via yt-dlp-as-a-library and caches aggressively.

Transport: AF_UNIX stream socket, newline-delimited JSON. One request object
per line in, one response object per line out. Each response has "ok": bool,
and "error": str on failure.

Phase 1 ops:  search · trending · stream
Reserved (later phases): home, subscriptions, channel, playlist, related,
                         comments, comment_post, rate, subscribe, auth_*

See docs/BACKEND.md for the full contract and roadmap.
"""

import os
import sys
import json
import time
import shutil
import signal
import socket
import subprocess
import threading
import socketserver
import urllib.request
import urllib.error
import re
from collections import OrderedDict

# ── Paths / config ──────────────────────────────────────────────────────────

def _base_dir():
    # Prefer the canonical install location if present.
    if os.path.isdir("/roms/tools/tubelite"):
        return "/roms/tools/tubelite"
    # Otherwise derive the app root from this script's location. tubed lives at
    # <app>/tubed/tubed.py, so the app root is the parent of our directory —
    # that's where cookies.txt / logs / cache should sit, alongside the app.
    here = os.path.dirname(os.path.abspath(__file__))
    parent = os.path.dirname(here)
    return parent if os.path.isdir(parent) else here

BASE_DIR    = _base_dir()
SOCK_PATH   = "/dev/shm/tubed.sock"
PID_PATH    = "/dev/shm/tubed.pid"
LOG_PATH    = os.path.join(BASE_DIR, "tubed.log")
CACHE_DIR   = os.path.join(BASE_DIR, "cache")
CACHE_FILE  = os.path.join(CACHE_DIR, "tubed-cache.json")
COOKIES     = os.path.join(BASE_DIR, "cookies.txt")   # Phase 2 auth (optional)

# TTLs (seconds)
TTL_STREAM   = 5 * 3600      # resolved URLs expire ~6h on YouTube's side
TTL_SEARCH   = 10 * 60
TTL_TRENDING = 10 * 60
TTL_META     = 6 * 3600

# Serialize heavy (yt-dlp) work. On the quad-A35 a single yt-dlp already pegs a
# core; running several at once saturates the SoC and stalls the UI. One at a
# time keeps three cores free for the app/daemon. Requests queue briefly behind
# the worker, which is fine because results are cached.
MAX_WORKERS  = 1
# Self-exit after this long with no requests. This is what stops tubed (and any
# yt-dlp it spawned) from lingering after the app/daemon are gone — the bug
# where an orphaned tubed + stray yt-dlp kept running. The background audio
# daemon only hits tubed at track boundaries, so it simply re-spawns on demand.
IDLE_EXIT_SECS = 90

os.makedirs(CACHE_DIR, exist_ok=True)

# ── Logging ─────────────────────────────────────────────────────────────────

_log_lock = threading.Lock()

def log(*args):
    line = "[tubed %s] %s" % (time.strftime("%H:%M:%S"), " ".join(str(a) for a in args))
    with _log_lock:
        try:
            with open(LOG_PATH, "a") as f:
                f.write(line + "\n")
        except Exception:
            pass
    sys.stderr.write(line + "\n")
    sys.stderr.flush()

# ── TTL cache (RAM, with best-effort disk persistence) ──────────────────────

class TTLCache:
    def __init__(self, capacity=512):
        self.cap = capacity
        self.d = OrderedDict()          # key -> (expires_at, value)
        self.lock = threading.Lock()
        self._load()

    def get(self, key):
        now = time.time()
        with self.lock:
            item = self.d.get(key)
            if not item:
                return None
            exp, val = item
            if exp < now:
                self.d.pop(key, None)
                return None
            self.d.move_to_end(key)
            return val

    def set(self, key, value, ttl):
        with self.lock:
            self.d[key] = (time.time() + ttl, value)
            self.d.move_to_end(key)
            while len(self.d) > self.cap:
                self.d.popitem(last=False)
        self._save_soon()

    # Debounced disk save so frequent writes don't thrash the SD card.
    _save_timer = None
    def _save_soon(self):
        with self.lock:
            if self._save_timer is None:
                self._save_timer = threading.Timer(5.0, self._save)
                self._save_timer.daemon = True
                self._save_timer.start()

    def _save(self):
        with self.lock:
            self._save_timer = None
            snapshot = list(self.d.items())
        try:
            with open(CACHE_FILE, "w") as f:
                json.dump(snapshot, f)
        except Exception as e:
            log("cache save failed:", e)

    def _load(self):
        try:
            with open(CACHE_FILE) as f:
                items = json.load(f)
            now = time.time()
            for k, (exp, val) in items:
                if exp > now:
                    self.d[k] = (exp, val)
        except Exception:
            pass

CACHE = TTLCache()
_work_sem = threading.BoundedSemaphore(MAX_WORKERS)

# Play-over-preview preemption.  Hover-previews (preview=True) and real plays
# (preview=False) share the single worker slot.  A preview that's mid-resolve
# would otherwise make the user wait the full yt-dlp cold-start before their
# actual play even starts.  We count pending real plays; an in-flight PREVIEW
# yt-dlp checks this and aborts itself the moment a real play is waiting, so the
# play gets the worker within one poll tick (~0.5 s) instead of ~10 s.
_pending_plays = 0
_pending_plays_lock = threading.Lock()

def _play_pending():
    with _pending_plays_lock:
        return _pending_plays > 0

# Track every live yt-dlp child so we can guarantee none survive us. Each child
# is started in its own session (process group) so we can kill the whole group
# — yt-dlp's standalone binary can fork helpers that a plain kill() would miss.
_active_procs = set()
_active_lock = threading.Lock()

# Per-request liveness hook. The socket Handler stashes a callable here (scoped to
# the handler thread) that reports whether the requesting client is still
# connected. yt-dlp work checks it so we can bail the moment the app gives up on a
# request — e.g. the user scrolled past a card whose preview was mid-resolve —
# instead of grinding a stream nobody is waiting for any more.
_req_ctx = threading.local()

def _client_is_alive():
    fn = getattr(_req_ctx, "is_alive", None)
    if fn is None:
        return True          # no hook (e.g. internal call) — assume wanted
    try:
        return fn()
    except Exception:
        return True

def _kill_proc_group(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass

def _kill_all_children():
    with _active_lock:
        procs = list(_active_procs)
    for p in procs:
        _kill_proc_group(p)

# ── yt-dlp integration ───────────────────────────────────────────────────────

def _have_cookies():
    # A cookies.txt must exist AND be non-trivial.  A 0-byte or header-only
    # file (the Netscape comment lines without any actual cookie rows) means
    # the user hasn't really signed in, so treat it as guest.
    try:
        if not os.path.isfile(COOKIES):
            return False
        with open(COOKIES, "r", errors="replace") as f:
            for line in f:
                s = line.strip()
                if not s or s.startswith("#"):
                    continue
                # A real cookie row is tab-separated with several fields.
                if "\t" in s and len(s.split("\t")) >= 6:
                    return True
        return False
    except OSError:
        return False


def _find_ytdlp():
    """Return the first yt-dlp executable found on disk.

    We no longer probe with --help/--version at startup: the PyInstaller
    one-file binary always extracts its entire bundle to /tmp/_MEI* before
    the bootloader even parses argv, so --version still takes 15-20 s on
    Cortex-A35 + SD card — long enough to timeout and reject a working binary.
    Runtime stderr capture (already in place) is sufficient to surface any
    import errors when yt-dlp is actually used. _ytdlp_env() injects libssl3
    as belt-and-suspenders for the real invocations.
    """
    candidates = [
        "/usr/local/bin/yt-dlp",
        "/usr/bin/yt-dlp",
        os.path.expanduser("~/.local/bin/yt-dlp"),
    ]
    extra = shutil.which("yt-dlp")
    if extra and extra not in candidates:
        candidates.append(extra)

    for p in candidates:
        if p and os.path.isfile(p) and os.access(p, os.X_OK):
            log(f"yt-dlp resolved to {p}")
            return p
    log("WARNING: no yt-dlp found on disk; will try 'yt-dlp' on PATH")
    return "yt-dlp"

# The device ships yt-dlp as a standalone binary (not an importable module), so
# tubed drives that binary. The win over the old design is that tubed is
# persistent: results are cached, work is bounded, and there is exactly one
# resolution path — no per-UI-action process storms.
YT_DLP = _find_ytdlp()

# yt-dlp is the only real CPU hog in the sidecar (Python start-up + extraction +
# signature JS). We can't make the extraction itself cheaper, but we can stop it
# from starving the foreground app/audio: each child runs at low priority and is
# pinned to a single core, so on the quad-A35 it can never monopolise the SoC —
# the other three cores stay free for the UI and the background audio daemon even
# when previews are resolving back-to-back. Falls back to a plain launch if the
# `nice`/`taskset` tools aren't installed.
def _sched_prefix():
    prefix = []
    nice_bin = shutil.which("nice")
    if nice_bin:
        prefix += [nice_bin, "-n", "15"]
    taskset_bin = shutil.which("taskset")
    ncpu = os.cpu_count() or 1
    if taskset_bin and ncpu > 1:
        prefix += [taskset_bin, "-c", str(ncpu - 1)]  # pin to the last core
    return prefix

_SCHED_PREFIX = _sched_prefix()

# Flags shared by every yt-dlp invocation. Mirrors the command that played
# reliably before, plus cookies when present.
def _ytdlp_base_args():
    args = [
        YT_DLP, "--no-config", "--quiet", "--no-warnings", "--no-update",
        "--encoding", "utf-8", "--no-check-certificate", "--force-ipv4",
        "--no-check-formats", "--cache-dir", CACHE_DIR,
    ]
    if _have_cookies():
        args += ["--cookies", COOKIES]
    return args


def _ytdlp_env():
    """Augment the subprocess env so a side-loaded libssl3 / libcrypto3 is
    visible to yt-dlp's PyInstaller bundled Python.

    ArkOS / RG351MP ships only libssl.so.1.1.  The user side-loads libssl3
    + libcrypto3 into a non-system path (commonly /usr/local/lib) — but if
    ldconfig hasn't been run, or /usr/local/lib isn't in /etc/ld.so.conf,
    the loader can't find them when yt-dlp is forked from tubed.  Pinning
    LD_LIBRARY_PATH per-spawn is a belt-and-suspenders guard so the user
    doesn't have to remember the system-wide config step."""
    env = os.environ.copy()
    extra_dirs = []
    for d in ("/usr/local/lib", "/usr/local/lib64", "/roms/tools/tubelite/lib"):
        if os.path.isfile(os.path.join(d, "libssl.so.3")):
            extra_dirs.append(d)
    if extra_dirs:
        cur = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = ":".join(extra_dirs + ([cur] if cur else []))
    # SPEED: the PyInstaller one-file yt-dlp unpacks its entire bundle to
    # $TMPDIR/_MEIxxxx on EVERY invocation.  On ArkOS /tmp is often on the SD
    # card, so that extraction is the single biggest chunk of cold-start
    # latency (several seconds on the A35).  Point it at /dev/shm (RAM tmpfs)
    # so the unpack is memory-speed.  Falls back to default if /dev/shm is
    # somehow absent.
    if os.path.isdir("/dev/shm"):
        env["TMPDIR"] = "/dev/shm"
    return env


def _run_ytdlp(args, timeout, is_preview=False):
    """Run yt-dlp and return stdout text (empty on failure). The child runs in
    its own process group and is force-killed (group-wide) on timeout/error, so
    a hung or slow yt-dlp can never leak into a stray background process.

    is_preview: when True, the run aborts the instant a real play is queued
    (see _pending_plays) so hover-prefetch never delays an actual play."""
    try:
        # nice/taskset (when present) exec into yt-dlp, so the launched process
        # group still ends as yt-dlp and the group-wide kill below still works.
        #
        # stderr is captured (not DEVNULL'd) so when a video fails to play we
        # can log yt-dlp's actual diagnostic — "Sign in to confirm you're not a
        # bot", "Video unavailable", PO-token errors, etc.  Without this the
        # silent-fail path was undebuggable from the user's side.
        p = subprocess.Popen(_SCHED_PREFIX + args, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, start_new_session=True,
                             env=_ytdlp_env())
    except Exception as ex:
        log("yt-dlp launch failed:", ex)
        return ""
    with _active_lock:
        _active_procs.add(p)

    def _emit_stderr(err_bytes):
        if not err_bytes:
            return
        txt = err_bytes.decode("utf-8", "replace").rstrip()
        if not txt:
            return
        # Tag each line so users can grep yt-dlp failures out of tubed.log.
        for line in txt.splitlines():
            log("yt-dlp:", line)

    def _reap(reason):
        log(reason)
        _kill_proc_group(p)
        try:
            _, err = p.communicate(timeout=2)
            _emit_stderr(err)
        except Exception:
            pass
        return ""

    try:
        # Poll instead of one blocking communicate() so we can cut the work short
        # when the client disconnects. Granularity is 0.5s, so an abandoned
        # request is dropped within half a second of the app closing its socket.
        deadline = time.time() + timeout
        while True:
            try:
                out, err = p.communicate(timeout=0.5)
                _emit_stderr(err)
                if p.returncode not in (0, None):
                    log(f"yt-dlp exited {p.returncode} for args={args[:4]}...")
                return out.decode("utf-8", "replace") if out else ""
            except subprocess.TimeoutExpired:
                pass
            if not _client_is_alive():
                return _reap("client disconnected; killing yt-dlp")
            if is_preview and _play_pending():
                return _reap("preempting preview yt-dlp for a pending play")
            if time.time() >= deadline:
                return _reap("yt-dlp timed out; killing process group")
    except Exception as ex:
        log("yt-dlp run failed:", ex)
        _kill_proc_group(p)
        return ""
    finally:
        with _active_lock:
            _active_procs.discard(p)


def _uploaded_ago(info):
    # Prefer epoch timestamp; fall back to YYYYMMDD upload_date.
    ts = info.get("timestamp")
    if ts:
        diff = max(0, int(time.time()) - int(ts))
        days = diff // 86400
        if days >= 365:  y = days // 365;  return f"{y} year{'s' if y!=1 else ''} ago"
        if days >= 30:   m = days // 30;   return f"{m} month{'s' if m!=1 else ''} ago"
        if days >= 7:    w = days // 7;    return f"{w} week{'s' if w!=1 else ''} ago"
        if days >= 1:    return f"{days} day{'s' if days!=1 else ''} ago"
        hrs = diff // 3600
        if hrs >= 1:     return f"{hrs} hour{'s' if hrs!=1 else ''} ago"
        mins = diff // 60
        if mins >= 1:    return f"{mins} minute{'s' if mins!=1 else ''} ago"
        return "today"
    ud = info.get("upload_date") or ""
    if len(ud) == 8:
        try:
            y, m, d = int(ud[:4]), int(ud[4:6]), int(ud[6:8])
            t = time.localtime()
            dy, dm, dd = t.tm_year - y, t.tm_mon - m, t.tm_mday - d
            if dd < 0: dm -= 1; dd += 30
            if dm < 0: dy -= 1; dm += 12
            if dy > 0: return f"{dy} year{'s' if dy!=1 else ''} ago"
            if dm > 0: return f"{dm} month{'s' if dm!=1 else ''} ago"
            if dd > 7: w = dd // 7; return f"{w} week{'s' if w!=1 else ''} ago"
            if dd > 0: return f"{dd} day{'s' if dd!=1 else ''} ago"
            return "today"
        except Exception:
            return ""
    return ""


def _views_str(n):
    n = int(n or 0)
    if n > 1_000_000: return f"{n // 1_000_000}M views"
    if n > 1_000:     return f"{n // 1_000}K views"
    return f"{n} views"


def _video_from_entry(e):
    if not e:
        return None
    vid = e.get("id") or ""
    if not vid:
        return None
    dur = int(e.get("duration") or 0)
    author = e.get("uploader") or e.get("channel") or ""
    return {
        "id": vid,
        "title": e.get("title") or "",
        "author": author,
        "author_id": e.get("channel_id") or e.get("uploader_id") or "",
        "duration_seconds": dur,
        "duration_string": f"{dur // 60}:{dur % 60:02d}",
        "view_count_string": _views_str(e.get("view_count")),
        "uploaded_ago_string": _uploaded_ago(e),
    }


def _best_subtitle_url(info):
    def pick(track_list):
        if not isinstance(track_list, list) or not track_list:
            return ""
        for want in ("vtt", "srt"):
            for it in track_list:
                if it.get("ext") == want and it.get("url"):
                    return it["url"]
        for it in track_list:
            if it.get("ext") != "json3" and it.get("url"):
                return it["url"]
        for it in track_list:
            if it.get("url"):
                return it["url"]
        return ""

    url = ""
    subs = info.get("subtitles") or {}
    caps = info.get("automatic_captions") or {}
    if isinstance(subs, dict):
        if "en" in subs:
            url = pick(subs["en"])
        if not url:
            for v in subs.values():
                url = pick(v)
                if url:
                    break
    if not url and isinstance(caps, dict):
        if "en" in caps:
            url = pick(caps["en"])
        if not url:
            for v in caps.values():
                url = pick(v)
                if url:
                    break
    if url and "fmt=json3" in url:
        url = url.replace("fmt=json3", "fmt=vtt")
    return url


def op_search(req):
    query = (req.get("query") or "").strip()
    page = int(req.get("page") or 1)
    trending = req.get("_trending", False)
    if not query and not trending:
        return {"ok": False, "error": "empty query"}
    if trending and not query:
        query = "trending"

    ck = ("trending" if trending else "search") + f":{query}:{page}"
    cached = CACHE.get(ck)
    if cached is not None:
        return {"ok": True, "results": cached, "finished": True}

    start = (page - 1) * 15 + 1
    end = page * 15
    spec = f"ytsearch{end}:{query}"

    args = _ytdlp_base_args() + [
        "--flat-playlist", "--dump-json",
        # Ask the tab extractor for approximate upload dates so the cards can
        # show "x years ago" (flat extraction omits exact dates otherwise).
        "--extractor-args", "youtubetab:approximate_date",
        "--playlist-start", str(start), "--playlist-end", str(end),
        spec,
    ]
    results = []
    with _work_sem:
        out = _run_ytdlp(args, timeout=30)
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            v = _video_from_entry(json.loads(line))
            if v:
                results.append(v)
        except Exception:
            continue

    if not results:
        return {"ok": False, "error": "no results"}
    CACHE.set(ck, results, TTL_TRENDING if trending else TTL_SEARCH)
    return {"ok": True, "results": results, "finished": True}


def op_trending(req):
    req = dict(req)
    req["_trending"] = True
    return op_search(req)


# Client fallback chain — depends on whether we're authenticated (cookies).
#
# UNAUTHENTICATED (no cookies.txt): YouTube bot-walls the high-quality clients.
# `tv` and `web` return "Sign in to confirm you're not a bot", so trying them
# just burns ~10s per video before falling back.  The ONLY client that resolves
# reliably without sign-in is `android`, but without a PO token it yields just
# the muxed itag-18 (360p) progressive stream (audio baked in, no DASH).  So
# unauthenticated we go android-only: reliable, fast, 360p.  This is the
# "old android mux until the user signs in" path.
#
# AUTHENTICATED (cookies.txt present): with cookies the `web` client returns the
# COMPLETE format set (DASH video+audio ladder up to 1080p+) reliably.  We tried
# `tv` first for speed but it returns DASH video-only WITHOUT a matching audio
# track unless a PO token is present, so the merge fails with "Requested format
# is not available" — 15s wasted per video.  So authed = web only.  web does the
# nsig signature dance so it's slower (~15-20s first play) but it actually works;
# `android` stays as the 360p muxed safety net if web ever fails.
_CLIENT_CHAIN_AUTHED   = [["web"], ["android"]]
_CLIENT_CHAIN_NOAUTH   = [["android"]]

def _client_chain(authed):
    return _CLIENT_CHAIN_AUTHED if authed else _CLIENT_CHAIN_NOAUTH


def _info_height(info):
    """Best-effort resolved video height from a yt-dlp --dump-json blob.
    Handles both merged (requested_formats) and single-format results."""
    h = info.get("height")
    if h:
        return int(h)
    reqd = info.get("requested_formats") or info.get("requested_downloads")
    if isinstance(reqd, list):
        for f in reqd:
            fh = f.get("height")
            if fh:
                return int(fh)
    return 0

def _parse_stream_info(info):
    """Returns (video_url, audio_url, subtitle_url, meta).
    audio_url is non-empty only when yt-dlp picked a DASH adaptive format
    (separate video+audio); for muxed progressive it's "" and the audio is
    baked into video_url.
    """
    stream_url = ""
    audio_url  = ""

    # Preferred path: yt-dlp filled in requested_formats (a list).  For a DASH
    # selector like `bestvideo+bestaudio` this is [video_format, audio_format].
    # For muxed it's typically a single entry.
    reqd = info.get("requested_formats") or info.get("requested_downloads")
    if isinstance(reqd, list) and reqd:
        # Disambiguate: a format with vcodec != "none" is the video track.
        vid_f = None
        aud_f = None
        for f in reqd:
            vc = (f.get("vcodec") or "").lower()
            ac = (f.get("acodec") or "").lower()
            has_v = vc and vc != "none"
            has_a = ac and ac != "none"
            if has_v and has_a:
                # Muxed entry — use it as the sole stream.
                vid_f = f
                aud_f = None
                break
            if has_v and not vid_f:
                vid_f = f
            elif has_a and not aud_f:
                aud_f = f
        if vid_f:
            stream_url = vid_f.get("url") or ""
        if aud_f:
            audio_url = aud_f.get("url") or ""

    # Fallback: top-level url (older yt-dlp / single-format selection).
    if not stream_url:
        stream_url = info.get("url") or ""

    # Last resort: walk formats[] for any URL.
    if not stream_url:
        fmts = info.get("formats") or []
        if fmts:
            stream_url = fmts[-1].get("url") or ""

    if not stream_url:
        return None

    meta = {
        "description": info.get("description") or "",
        "view_count": int(info.get("view_count") or 0),
        "like_count": int(info.get("like_count") or 0),
        "comment_count": int(info.get("comment_count") or 0),
        "subscriber_count": int(info.get("channel_follower_count") or 0),
    }
    return stream_url, audio_url, _best_subtitle_url(info), meta


# ── Fast public-API resolvers (Invidious / Piped) ─────────────────────────────
#
# These mirror the c707db7 design: try a curl-equivalent HTTP GET against
# public Invidious / Piped instances FIRST.  Each instance returns a direct
# googlevideo.com stream URL plus metadata in ~200 ms.  We only fall back to
# yt-dlp when every instance has failed.  This is what the user remembers
# working 100% — yt-dlp was the rare last resort, not the common path.
#
# Instance lists rot fast.  These are seeded from publicly-tracked uptime
# lists; if every one starts failing simultaneously, update them or add a
# refresh mechanism (api.invidious.io / piped-instances.kavin.rocks).
_INVIDIOUS_INSTANCES = [
    "https://inv.tux.pizza",
    "https://invidious.privacyredirect.com",
    "https://iv.ggtyler.dev",
    "https://invidious.protokolla.fi",
    "https://invidious.incogniweb.net",
    "https://yewtu.be",
    "https://inv.nadeko.net",
]
_PIPED_INSTANCES = [
    "https://pipedapi.tokhmi.xyz",
    "https://piped-api.privacy.com.de",
    "https://pipedapi.kavin.rocks",
    "https://pipedapi.adminforge.de",
]

# Per-session instance health cache.  Any instance that returns a clear
# failure (auth error, HTTP 4xx/5xx, DNS gone, empty body) is blacklisted
# for the lifetime of this tubed process.  This means dead instances cost
# us at most once per session — subsequent stream resolves skip straight to
# yt-dlp instead of retrying every corpse.
_instance_failures: dict = {}
_instance_lock = threading.Lock()

def _instance_ok(instance: str) -> bool:
    with _instance_lock:
        return _instance_failures.get(instance, 0) == 0

def _instance_fail(instance: str) -> None:
    with _instance_lock:
        if _instance_failures.get(instance, 0) == 0:
            log(f"blacklisting {instance} for this session")
        _instance_failures[instance] = _instance_failures.get(instance, 0) + 1


def _http_get_json(url, timeout=3, label=""):
    """Minimal urllib GET that returns a parsed JSON dict or None.

    label: optional prefix for error log lines (e.g. "invidious yewtu.be").
    """
    def _err(msg):
        if label:
            log(f"{label}: {msg}")
    try:
        req = urllib.request.Request(
            url,
            headers={"User-Agent": "Mozilla/5.0 (tubelite)"},
        )
        with urllib.request.urlopen(req, timeout=timeout) as r:
            if r.status != 200:
                _err(f"HTTP {r.status}")
                return None
            body = r.read(2 * 1024 * 1024)  # 2 MB cap
            return json.loads(body.decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        _err(f"HTTP {e.code}")
        return None
    except socket.timeout:
        _err("timeout")
        return None
    except urllib.error.URLError as e:
        _err(f"URLError: {e.reason}")
        return None
    except (ConnectionError, OSError) as e:
        _err(f"connection error: {e}")
        return None
    except (ValueError, Exception) as e:
        _err(f"parse error: {e}")
        return None


def _fetch_invidious(video_id, max_height):
    """Returns (video_url, audio_url, subtitle_url, meta_dict) or None."""
    for instance in _INVIDIOUS_INSTANCES:
        if not _client_is_alive():
            return None
        if not _instance_ok(instance):
            continue
        api = f"{instance}/api/v1/videos/{video_id}"
        j = _http_get_json(api, timeout=2, label=f"invidious {instance}")
        if not j or not isinstance(j, dict):
            _instance_fail(instance)
            continue
        streams = j.get("formatStreams") or []
        best_url = ""
        best_height = 0
        for s in streams:
            res = s.get("resolution", "")
            m = re.match(r"^(\d+)x(\d+)$", res)
            if not m:
                continue
            h = int(m.group(2))
            if h <= 0 or h > max_height:
                continue
            container = s.get("container", "")
            prefer = (container == "mp4")
            if prefer or not best_url or h > best_height:
                u = s.get("url", "")
                if u:
                    best_url = u
                    best_height = h
        if not best_url:
            log(f"invidious {instance}: no usable stream for {video_id} @ <={max_height}p (streams={len(streams)})")
            # Don't blacklist — video-not-indexed is instance-independent
            continue
        meta = {
            "description":      j.get("description", ""),
            "view_count":       int(j.get("viewCount") or 0),
            "like_count":       int(j.get("likeCount") or 0),
            "subscriber_count": int(j.get("subCount") or 0),
            "comment_count":    0,  # invidious doesn't expose this
        }
        # Invidious "captions" → first English/auto track if present.
        sub = ""
        for c in (j.get("captions") or []):
            lc = (c.get("language_code") or "").lower()
            if lc.startswith("en"):
                u = c.get("url", "")
                if u:
                    # captions URLs from Invidious are relative — prefix host
                    sub = u if u.startswith("http") else (instance + u)
                    break
        log(f"resolved via invidious={instance} for {video_id} @ {best_height}p")
        # Fast path is progressive (muxed) only — audio is baked in, so audio_url="".
        return (best_url, "", sub, meta)
    return None


def _fetch_piped(video_id, max_height):
    """Returns (video_url, audio_url, subtitle_url, meta_dict) or None."""
    for instance in _PIPED_INSTANCES:
        if not _client_is_alive():
            return None
        if not _instance_ok(instance):
            continue
        api = f"{instance}/streams/{video_id}"
        j = _http_get_json(api, timeout=2, label=f"piped {instance}")
        if not j or not isinstance(j, dict):
            _instance_fail(instance)
            continue
        streams = j.get("videoStreams") or []
        best_url = ""
        best_height = 0
        for s in streams:
            if s.get("videoOnly", False):
                continue
            q = s.get("quality", "")
            m = re.match(r"^(\d+)p", q)
            if not m:
                continue
            h = int(m.group(1))
            if h <= 0 or h > max_height:
                continue
            codec = s.get("codec", "")
            prefer = ("avc1" in codec)
            if prefer or not best_url or h > best_height:
                u = s.get("url", "")
                if u:
                    best_url = u
                    best_height = h
        if not best_url:
            log(f"piped {instance}: no usable stream for {video_id} @ <={max_height}p (streams={len(streams)})")
            continue
        meta = {
            "description":      j.get("description", ""),
            "view_count":       int(j.get("views") or 0),
            "like_count":       int(j.get("likes") or 0),
            "subscriber_count": 0,
            "comment_count":    0,
        }
        sub = ""
        for c in (j.get("subtitles") or []):
            code = (c.get("code") or "").lower()
            if code.startswith("en"):
                sub = c.get("url", "") or ""
                if sub:
                    break
        log(f"resolved via piped={instance} for {video_id} @ {best_height}p")
        # Fast path is progressive (muxed) only — audio is baked in, so audio_url="".
        return (best_url, "", sub, meta)
    return None


def _extract_stream(video_id, max_height, preview=False):
    # FAST PATH (Invidious / Piped): disabled.  As of 2026-06, every public
    # instance we've tracked is either auth-walled (401/403), DNS-gone, or
    # 5xx-down.  Cycling all of them takes ~26 s on the first request — long
    # enough to blow past the C++-side stream timeout and KILL the yt-dlp
    # fallback before it even gets to run.  Yt-dlp is now reliable enough to
    # be the primary path.  Re-enable this by setting FAST_PATH_ENABLED=True
    # if a working instance list is discovered.
    FAST_PATH_ENABLED = False
    if FAST_PATH_ENABLED:
        try:
            res = _fetch_invidious(video_id, max_height)
            if not res:
                res = _fetch_piped(video_id, max_height)
            if res:
                return res
        except Exception as ex:
            log(f"fast-path error for {video_id}: {ex}")
    # FALLBACK PATH: yt-dlp.
    url = f"https://www.youtube.com/watch?v={video_id}"
    # Quality strategy:
    #   max_height <= 360 OR preview → muxed progressive only.  This is the
    #     daemon (background-audio) path and short previews.  YouTube still
    #     serves itag 18 (360p AVC muxed) reliably, so we get a single URL
    #     with audio baked in — no audio-add gymnastics needed downstream.
    #   max_height >  360 → DASH adaptive preferred.  YouTube stopped serving
    #     muxed progressive >360p in 2024+, so for high-res playback we MUST
    #     pick a video-only + audio-only pair and tell mpv to overlay them.
    #     Order:
    #       1. DASH avc1 video + m4a audio — hardware-decoded on RK3326 via
    #          rkmpp, lowest CPU + battery cost.
    #       2. DASH any-codec video + any audio — software fallback for
    #          videos missing avc1 (rare).
    #       3. Muxed progressive — last-ditch 360p when nothing else works.
    if preview or max_height <= 360:
        fmt = (f"best[height<={max_height}][vcodec^=avc1]"
               f"/best[height<={max_height}]/best")
    else:
        fmt = (f"bestvideo[height<={max_height}][vcodec^=avc1]+bestaudio[acodec^=mp4a]"
               f"/bestvideo[height<={max_height}]+bestaudio"
               f"/best[height<={max_height}][vcodec^=avc1]"
               f"/best[height<={max_height}]/best")
    # Previews are best-effort eye-candy: only try the single fastest client with
    # a short timeout, and don't walk the fallback chain.  Real plays use the
    # auth-aware chain: android-only until the user signs in (cookies.txt), then
    # the full DASH ladder.
    chain = [["android"]] if preview else _client_chain(_have_cookies())
    # web (the authed client) does the nsig signature dance and is slower than
    # android; give real plays a generous ceiling so a slow-but-working web
    # resolve isn't killed mid-flight.  Previews stay snappy.
    run_timeout = 10 if preview else 30

    # If a client yields only a low-res muxed stream (the "crushed quality"
    # case) we keep trying later clients for a proper DASH result, but stash
    # the muxed one so a video that genuinely has nothing better still plays.
    fallback_muxed = None

    def _pick(info):
        """Return (parsed, height, is_dash, poor) or None if unusable."""
        parsed = _parse_stream_info(info)
        if not parsed:
            return None
        height = _info_height(info)
        is_dash = bool(parsed[1])      # audio_url present ⇒ DASH pair
        poor = (not preview and max_height > 480
                and not is_dash and height and height < 480)
        return (parsed, height, is_dash, poor)

    for attempt, clients in enumerate(chain):
        # If the requester already gave up (scrolled away), stop before spawning
        # ANOTHER yt-dlp for a result no one will read.  The first attempt always
        # runs: the liveness peek can race the client's blocking read right after
        # the request is sent and spuriously report "gone", which would kill the
        # resolve before it ever started.  Only subsequent attempts gate on it.
        if attempt > 0 and not _client_is_alive():
            raise RuntimeError("client gone")
        # player_skip=webpage trims the extra watch-page fetch.  We do NOT skip
        # `configs` for the web client: it needs the player config to fetch the
        # JS for nsig signature deciphering — skipping it can yield throttled or
        # missing format URLs.  android (muxed, no nsig) keeps the full skip.
        is_android = (clients == ["android"])
        skip = "webpage,configs" if is_android else "webpage"
        ea = f"youtube:player_skip={skip}"
        if clients:
            ea = (f"youtube:player_client={','.join(clients)}"
                  f";player_skip={skip}")
        args = _ytdlp_base_args() + [
            "--no-playlist", "--socket-timeout", "10",
            "--extractor-args", ea, "-f", fmt, "--dump-json", url,
        ]
        out = _run_ytdlp(args, timeout=run_timeout, is_preview=preview)

        got = None
        for line in out.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                info = json.loads(line)
            except Exception:
                continue
            if isinstance(info, dict):
                picked = _pick(info)
                if picked:
                    got = picked
                    break

        if got is None:
            log(f"stream extract (client={clients}) found nothing for {video_id}")
            continue

        parsed, height, is_dash, poor = got
        if poor and attempt < len(chain) - 1:
            log(f"client={clients}: only {height}p muxed for {video_id}; "
                f"trying next client for DASH")
            if fallback_muxed is None:
                fallback_muxed = parsed
            continue

        log(f"resolved {video_id} via client={clients}: "
            f"{height or '?'}p dash={'yes' if is_dash else 'no'}")
        return parsed

    # All clients exhausted.  Use a stashed low-res muxed result if we have one
    # — 360p beats "won't play at all".
    if fallback_muxed is not None:
        log(f"all clients exhausted for {video_id}; using muxed fallback")
        return fallback_muxed
    raise RuntimeError("no playable URL")


def op_stream(req):
    vid = (req.get("id") or "").strip()
    h = int(req.get("max_height") or 360)
    preview = bool(req.get("preview", False))
    if not vid:
        return {"ok": False, "error": "missing id"}

    log(f"op_stream: vid={vid} h={h} preview={preview}")

    # Auth state is part of the cache key: a guest-resolved 360p muxed entry
    # must NOT be served once the user signs in (they should get the DASH
    # ladder), and vice-versa.  'a' = authed (cookies), 'g' = guest.
    authed = _have_cookies()
    ck = f"stream:{vid}:{h}:{'a' if authed else 'g'}"
    cached = CACHE.get(ck)
    if cached is not None:
        log(f"op_stream: cache hit for {vid}")
        return {"ok": True, **cached}

    # Hover-preview, but a real play is already waiting for the worker?  Don't
    # even queue — a speculative preview must never delay an actual play.  The
    # app re-requests the preview later if the card is still focused.
    if preview and _play_pending():
        log(f"op_stream: dropping preview {vid} — play pending")
        return {"ok": False, "error": "deferred for play"}

    # A real play (preview=False) registers itself as pending so any in-flight
    # preview yt-dlp aborts and frees the single worker for us (see _run_ytdlp),
    # and any preview still queued behind the worker bails at the check above.
    global _pending_plays
    if not preview:
        with _pending_plays_lock:
            _pending_plays += 1
    try:
        with _work_sem:
            # Re-check inside the gate; another thread may have just resolved it.
            cached = CACHE.get(ck)
            if cached is not None:
                log(f"op_stream: cache hit (post-gate) for {vid}")
                return {"ok": True, **cached}
            # Last-chance preview bail: a play may have arrived while we waited
            # for the worker slot.
            if preview and _play_pending():
                log(f"op_stream: dropping preview {vid} at gate — play pending")
                return {"ok": False, "error": "deferred for play"}
            # NOTE: we used to bail here with a pre-extract `_client_is_alive()`
            # check, but that peek can spuriously report the client gone the
            # instant after the request line is consumed (before the client's
            # blocking read settles), which killed EVERY resolve with an empty
            # log.  `_extract_stream` already re-checks liveness before each
            # yt-dlp spawn, so dropping the redundant pre-check is safe and fixes
            # the "resolve instantly fails" regression.
            try:
                url, audio_url, sub, meta = _extract_stream(vid, h, preview=preview)
            except Exception as ex:
                log(f"op_stream: extract failed for {vid}: {ex}")
                return {"ok": False, "error": str(ex)}
    finally:
        if not preview:
            with _pending_plays_lock:
                _pending_plays -= 1

    payload = {"url": url, "audio_url": audio_url, "subtitle_url": sub, "meta": meta}
    CACHE.set(ck, payload, TTL_STREAM)
    log(f"op_stream: resolved {vid} (audio={'yes' if audio_url else 'no'})")
    return {"ok": True, **payload}


def op_ping(req):
    return {"ok": True, "version": 1, "authed": _have_cookies()}


def op_auth_status(req):
    """Report sign-in state so the app can show a Guest/Signed-in indicator and
    decide whether to offer the sign-in screen.  Auth is purely cookie-based
    (see the auth notes in _client_chain): a valid cookies.txt = signed in."""
    authed = _have_cookies()
    info = {"ok": True, "authed": authed, "cookies_path": COOKIES}
    if authed:
        try:
            info["cookies_mtime"] = int(os.path.getmtime(COOKIES))
        except OSError:
            pass
    return info


OPS = {
    "ping": op_ping,
    "auth_status": op_auth_status,
    "search": op_search,
    "trending": op_trending,
    "stream": op_stream,
}

# ── Socket server ─────────────────────────────────────────────────────────────

_last_activity = time.time()

class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        global _last_activity
        try:
            line = self.rfile.readline()
            if not line:
                return
            _last_activity = time.time()
            try:
                req = json.loads(line.decode("utf-8", "replace"))
            except Exception as ex:
                self._send({"ok": False, "error": f"bad json: {ex}"})
                return
            op = req.get("op")
            fn = OPS.get(op)
            if not fn:
                self._send({"ok": False, "error": f"unknown op: {op}"})
                return
            # Expose client liveness to the op (and the yt-dlp it runs) so long
            # resolves can bail when the app disconnects. Scoped to this thread.
            _req_ctx.is_alive = self._client_alive
            try:
                resp = fn(req)
            except Exception as ex:
                log("op", op, "crashed:", ex)
                resp = {"ok": False, "error": str(ex)}
            finally:
                _req_ctx.is_alive = None
            self._send(resp)
            _last_activity = time.time()
        except Exception as ex:
            log("handler error:", ex)

    def _client_alive(self):
        # Peek the socket without consuming: b'' means the peer closed the
        # connection; BlockingIOError means it's still open with nothing pending
        # (the normal case while we resolve). The client never sends a second
        # line, so a non-empty peek isn't expected.
        try:
            return self.connection.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT) != b""
        except (BlockingIOError, InterruptedError):
            return True
        except OSError:
            return False

    def _send(self, obj):
        try:
            self.wfile.write((json.dumps(obj) + "\n").encode("utf-8"))
            self.wfile.flush()
        except Exception:
            pass


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True


def _ping_existing():
    """True if a live tubed is already answering on the socket."""
    if not os.path.exists(SOCK_PATH):
        return False
    try:
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.settimeout(1.0)
        c.connect(SOCK_PATH)
        c.sendall(b'{"op":"ping"}\n')
        alive = bool(c.recv(64))
        c.close()
        return alive
    except Exception:
        return False


def _single_instance_or_exit():
    if _ping_existing():
        log("another tubed is already running; exiting")
        sys.exit(0)
    # Stale socket left by a crashed instance — clear it.
    try:
        os.unlink(SOCK_PATH)
    except OSError:
        pass


def _make_server():
    """Create the server, tolerating a simultaneous-start race between the app
    and the daemon both trying to spawn tubed."""
    try:
        return Server(SOCK_PATH, Handler)
    except OSError:
        if _ping_existing():
            log("lost the start race to another tubed; exiting")
            sys.exit(0)
        try:
            os.unlink(SOCK_PATH)
        except OSError:
            pass
        return Server(SOCK_PATH, Handler)


def _idle_watchdog(server):
    if IDLE_EXIT_SECS <= 0:
        return
    while True:
        time.sleep(30)
        if time.time() - _last_activity > IDLE_EXIT_SECS:
            log("idle timeout — shutting down")
            server.shutdown()
            return


def main():
    _single_instance_or_exit()

    server = _make_server()
    try:
        os.chmod(SOCK_PATH, 0o660)
    except OSError:
        pass

    with open(PID_PATH, "w") as f:
        f.write(str(os.getpid()))

    def _shutdown(*_):
        log("signal received — shutting down")
        try:
            server.shutdown()
        except Exception:
            pass
    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    # Last-resort safety net: never leave a yt-dlp child behind, no matter how
    # we exit.
    import atexit
    atexit.register(_kill_all_children)

    log(f"listening on {SOCK_PATH} (cookies={'yes' if _have_cookies() else 'no'})")
    log("yt-dlp launch prefix:", " ".join(_SCHED_PREFIX) if _SCHED_PREFIX else "(none)")
    wd = threading.Thread(target=_idle_watchdog, args=(server,), daemon=True)
    wd.start()
    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        _kill_all_children()   # reap any in-flight yt-dlp before we go
        for p in (SOCK_PATH, PID_PATH):
            try:
                os.unlink(p)
            except OSError:
                pass
        log("stopped")


if __name__ == "__main__":
    main()

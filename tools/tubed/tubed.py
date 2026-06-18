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
    return os.path.isfile(COOKIES)


def _find_ytdlp():
    """Pick the first yt-dlp on disk that actually RUNS.

    Just checking is_file + executable isn't enough: the standalone PyInstaller
    yt-dlp at /usr/local/bin/yt-dlp ships a bundled Python whose _ssl module
    can link against libssl.so.3 — which ArkOS / RG351MP doesn't have, so the
    binary dies at startup with an ImportError before main() runs.  When that
    happens we want to silently fall through to the next candidate (often a
    pip-installed shebang script that uses the system Python's OpenSSL 1.1).
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
        if not (p and os.path.isfile(p) and os.access(p, os.X_OK)):
            continue
        # Probe with --version: it's intercepted by the PyInstaller bootloader
        # before Python starts, so it's near-instant (<0.5s) even on Cortex-A35
        # where a full Python init takes 15-20s.  We accept the small risk of
        # trusting a binary that might have a broken _ssl (libssl.so.3 missing)
        # — if that happens, runtime stderr will show the ImportError, and
        # Invidious/Piped handle the common case without yt-dlp anyway.
        # _ytdlp_env() injects LD_LIBRARY_PATH as a belt-and-suspenders guard
        # for the real invocations.
        try:
            probe_env = os.environ.copy()
            extra_ssl = [d for d in ("/usr/local/lib", "/usr/local/lib64",
                                     "/roms/tools/tubelite/lib")
                         if os.path.isfile(os.path.join(d, "libssl.so.3"))]
            if extra_ssl:
                cur = probe_env.get("LD_LIBRARY_PATH", "")
                probe_env["LD_LIBRARY_PATH"] = ":".join(extra_ssl + ([cur] if cur else []))
            r = subprocess.run([p, "--version"],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE,
                               timeout=3,
                               env=probe_env)
            if r.returncode == 0:
                log(f"yt-dlp resolved to {p}")
                return p
            err = r.stderr.decode("utf-8", "replace").strip().splitlines()
            tail = err[-1] if err else "no stderr"
            log(f"yt-dlp at {p} unusable (exit {r.returncode}): {tail}")
        except Exception as ex:
            log(f"yt-dlp at {p} unusable: {ex}")
    log("WARNING: no working yt-dlp found; falling back to 'yt-dlp' on PATH")
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
        "--no-call-home", "--no-check-formats", "--cache-dir", CACHE_DIR,
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
    return env


def _run_ytdlp(args, timeout):
    """Run yt-dlp and return stdout text (empty on failure). The child runs in
    its own process group and is force-killed (group-wide) on timeout/error, so
    a hung or slow yt-dlp can never leak into a stray background process."""
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


# Client fallback chain — if one player client is broken, try the next.
# `ios` first and alone: it returns unthrottled progressive URLs plus full
# videoDetails (view counts) in a single player request and is the fastest path.
# Only fall back to heavier clients if it yields nothing.
_CLIENT_CHAIN = [["ios"], ["android"], ["web", "tv"], None]

def _parse_stream_info(info):
    stream_url = info.get("url")
    if not stream_url:
        reqd = info.get("requested_downloads") or info.get("requested_formats")
        if reqd:
            stream_url = reqd[0].get("url")
    if not stream_url:
        fmts = info.get("formats") or []
        if fmts:
            stream_url = fmts[-1].get("url")
    if not stream_url:
        return None
    meta = {
        "description": info.get("description") or "",
        "view_count": int(info.get("view_count") or 0),
        "like_count": int(info.get("like_count") or 0),
        "comment_count": int(info.get("comment_count") or 0),
        "subscriber_count": int(info.get("channel_follower_count") or 0),
    }
    return stream_url, _best_subtitle_url(info), meta


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
    "https://invidious.nerdvpn.de",
    "https://invidious.projectsegfau.lt",
    "https://yewtu.be",
    "https://inv.nadeko.net",
    "https://invidious.flokinet.to",
]
_PIPED_INSTANCES = [
    "https://pipedapi.kavin.rocks",
    "https://pipedapi.adminforge.de",
    "https://piped-api.lunar.icu",
    "https://pipedapi.colt.top",
]


def _http_get_json(url, timeout=3):
    """Minimal urllib GET that returns a parsed JSON dict or None."""
    try:
        req = urllib.request.Request(
            url,
            headers={"User-Agent": "Mozilla/5.0 (tubelite)"},
        )
        with urllib.request.urlopen(req, timeout=timeout) as r:
            if r.status != 200:
                return None
            body = r.read(2 * 1024 * 1024)  # 2 MB cap
            return json.loads(body.decode("utf-8", "replace"))
    except (urllib.error.URLError, urllib.error.HTTPError,
            socket.timeout, ConnectionError, ValueError, OSError):
        return None


def _fetch_invidious(video_id, max_height):
    """Returns (url, subtitle_url, meta_dict) on success, else None."""
    for instance in _INVIDIOUS_INSTANCES:
        if not _client_is_alive():
            return None
        api = f"{instance}/api/v1/videos/{video_id}"
        j = _http_get_json(api, timeout=3)
        if not j or not isinstance(j, dict):
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
        return (best_url, sub, meta)
    return None


def _fetch_piped(video_id, max_height):
    """Returns (url, subtitle_url, meta_dict) on success, else None."""
    for instance in _PIPED_INSTANCES:
        if not _client_is_alive():
            return None
        api = f"{instance}/streams/{video_id}"
        j = _http_get_json(api, timeout=3)
        if not j or not isinstance(j, dict):
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
        return (best_url, sub, meta)
    return None


def _extract_stream(video_id, max_height, preview=False):
    # FAST PATH: try public APIs first.  This was the design that "worked
    # 100%" in the pre-tubed days — yt-dlp was only the rare fallback for
    # videos the public mirrors couldn't resolve.
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
    fmt = (f"best[height<={max_height}][vcodec^=avc1]"
           f"/best[height<={max_height}]"
           f"/best")
    # Previews are best-effort eye-candy: only try the single fastest client with
    # a short timeout, and don't walk the fallback chain. This caps a preview at
    # one quick yt-dlp run (~seconds) instead of up to ~100s of chained attempts
    # the user has usually scrolled past anyway. A real "play" (preview=False)
    # still gets the full reliability chain.
    chain = [["ios"]] if preview else _CLIENT_CHAIN
    run_timeout = 12 if preview else 25
    for clients in chain:
        # If the requester already gave up (scrolled away), stop before spawning
        # another yt-dlp for a result no one will read.
        if not _client_is_alive():
            raise RuntimeError("client gone")
        # player_skip=webpage,configs avoids the extra watch-page + config HTTP
        # round-trips the default path makes — a real chunk of first-play latency.
        ea = "youtube:skip=dash,hls;player_skip=webpage,configs"
        if clients:
            ea = (f"youtube:player_client={','.join(clients)}"
                  f";skip=dash,hls;player_skip=webpage,configs")
        args = _ytdlp_base_args() + [
            "--no-playlist", "--youtube-skip-dash-manifest", "--socket-timeout", "10",
            "--extractor-args", ea, "-f", fmt, "--dump-json", url,
        ]
        out = _run_ytdlp(args, timeout=run_timeout)
        for line in out.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                info = json.loads(line)
            except Exception:
                continue
            if isinstance(info, dict):
                parsed = _parse_stream_info(info)
                if parsed:
                    return parsed
        log(f"stream extract (client={clients}) found nothing for {video_id}")
    raise RuntimeError("no playable URL")


def op_stream(req):
    vid = (req.get("id") or "").strip()
    h = int(req.get("max_height") or 360)
    preview = bool(req.get("preview", False))
    if not vid:
        return {"ok": False, "error": "missing id"}

    ck = f"stream:{vid}:{h}"
    cached = CACHE.get(ck)
    if cached is not None:
        return {"ok": True, **cached}

    with _work_sem:
        # Re-check inside the gate; another thread may have just resolved it.
        cached = CACHE.get(ck)
        if cached is not None:
            return {"ok": True, **cached}
        # The client may have moved on while we waited for the worker slot.
        if not _client_is_alive():
            return {"ok": False, "error": "client gone"}
        try:
            url, sub, meta = _extract_stream(vid, h, preview=preview)
        except Exception as ex:
            return {"ok": False, "error": str(ex)}

    payload = {"url": url, "subtitle_url": sub, "meta": meta}
    CACHE.set(ck, payload, TTL_STREAM)
    return {"ok": True, **payload}


def op_ping(req):
    return {"ok": True, "version": 1, "authed": _have_cookies()}


OPS = {
    "ping": op_ping,
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

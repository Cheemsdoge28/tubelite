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
# Self-exit after this long with no clients (0 = never). Frees RAM when idle.
IDLE_EXIT_SECS = 0

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

# ── yt-dlp integration ───────────────────────────────────────────────────────

def _have_cookies():
    return os.path.isfile(COOKIES)


def _find_ytdlp():
    for p in ("/usr/local/bin/yt-dlp", "/usr/bin/yt-dlp"):
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return shutil.which("yt-dlp") or "yt-dlp"

# The device ships yt-dlp as a standalone binary (not an importable module), so
# tubed drives that binary. The win over the old design is that tubed is
# persistent: results are cached, work is bounded, and there is exactly one
# resolution path — no per-UI-action process storms.
YT_DLP = _find_ytdlp()

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


def _run_ytdlp(args, timeout):
    """Run yt-dlp, return stdout text (empty on failure)."""
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=timeout)
        return p.stdout.decode("utf-8", "replace") if p.stdout else ""
    except subprocess.TimeoutExpired:
        log("yt-dlp timed out:", " ".join(args[-2:]))
        return ""
    except Exception as ex:
        log("yt-dlp run failed:", ex)
        return ""


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
_CLIENT_CHAIN = [["ios", "android"], ["android"], ["web", "tv"], None]

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


def _extract_stream(video_id, max_height):
    url = f"https://www.youtube.com/watch?v={video_id}"
    fmt = (f"best[height<={max_height}][vcodec^=avc1]"
           f"/best[height<={max_height}]"
           f"/best")
    for clients in _CLIENT_CHAIN:
        ea = "youtube:skip=dash,hls"
        if clients:
            ea = f"youtube:player_client={','.join(clients)};skip=dash,hls"
        args = _ytdlp_base_args() + [
            "--no-playlist", "--youtube-skip-dash-manifest", "--socket-timeout", "10",
            "--extractor-args", ea, "-f", fmt, "--dump-json", url,
        ]
        out = _run_ytdlp(args, timeout=25)
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
        try:
            url, sub, meta = _extract_stream(vid, h)
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
            try:
                resp = fn(req)
            except Exception as ex:
                log("op", op, "crashed:", ex)
                resp = {"ok": False, "error": str(ex)}
            self._send(resp)
            _last_activity = time.time()
        except Exception as ex:
            log("handler error:", ex)

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

    log(f"listening on {SOCK_PATH} (cookies={'yes' if _have_cookies() else 'no'})")
    wd = threading.Thread(target=_idle_watchdog, args=(server,), daemon=True)
    wd.start()
    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        for p in (SOCK_PATH, PID_PATH):
            try:
                os.unlink(p)
            except OSError:
                pass
        log("stopped")


if __name__ == "__main__":
    main()

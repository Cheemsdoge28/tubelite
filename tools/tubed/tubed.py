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

TUBED_VERSION = "0.10.5-streaming"      # bump on every meaningful edit so the
                                      # startup log proves which build is live

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
TTL_FEED     = 5 * 60        # personalized feeds change often; short TTL
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
_LOG_MAX_BYTES = 4 * 1024 * 1024   # rotate at 4 MB — never fills a disk

def log(*args):
    line = "[tubed %s] %s" % (time.strftime("%H:%M:%S"), " ".join(str(a) for a in args))
    with _log_lock:
        try:
            # Rotate: if the log exceeds 4 MB, keep only the last 1 MB tail so
            # recent entries are preserved and the file can never grow unbounded.
            try:
                if os.path.getsize(LOG_PATH) > _LOG_MAX_BYTES:
                    with open(LOG_PATH, "rb") as f:
                        f.seek(-1024 * 1024, 2)
                        tail = f.read()
                    with open(LOG_PATH, "wb") as f:
                        f.write(tail)
            except OSError:
                pass
            with open(LOG_PATH, "a") as f:
                f.write(line + "\n")
        except Exception:
            pass
    # Stderr write is intentionally NOT done: the launcher redirects tubed's
    # stderr into the same LOG_PATH (so PyInstaller / Python startup errors
    # are captured), which would double every line we explicitly write to the
    # file.  yt-dlp stderr from child processes is still captured by
    # _emit_stderr() via subprocess.PIPE — it never touches our stderr.

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

# Preview-only proc set so a play can kill in-flight previews INSTANTLY rather
# than waiting for the 0.5s poll inside _run_ytdlp.  Production logs showed a
# preview holding the worker for 24s while a play queued behind it on the
# semaphore — the polled preempt was clearly unreliable under load.  This is
# the hard belt: when a play arrives, op_stream signals here, the preview's
# process group dies, the semaphore is released, the play runs.
_preview_procs = set()
_preview_lock  = threading.Lock()

# ── Resilience: negative cache + yt-dlp circuit breaker ──────────────────────
#
# A misbehaving client (e.g. a hover-preview loop with no backoff) used to be
# able to storm tubed with the SAME failing video hundreds of times a second.
# Each miss spawned a fresh yt-dlp; when /dev/shm filled, yt-dlp died instantly
# and the loop tightened into a runaway that filled the disk and pinned the CPU.
# Two guards stop that dead:
#   1. NEGATIVE CACHE — a vid that just failed is remembered for _FAIL_TTL and
#      its failure returned INSTANTLY (no yt-dlp spawn) until the TTL lapses.
#   2. CIRCUIT BREAKER — if yt-dlp reports it cannot create its temp dir (disk
#      full), we stop spawning it entirely for _BREAKER_COOLDOWN so we don't
#      pour fuel on a full-disk fire; requests fail fast until it clears.
_FAIL_TTL_PLAY     = 8.0      # play fail → short backoff so user-initiated
                              # retry within ~10s actually re-runs yt-dlp.
                              # Previously 30 s, which trapped users behind a
                              # timeout-boundary race for half a minute even
                              # when the next attempt would succeed.
_FAIL_TTL_PREVIEW  = 120.0    # preview fail → long backoff; the C++ side prefetches
                              # the same preview every ~30s on focus, and without
                              # a longer fail TTL each retry spawns a fresh yt-dlp
                              # that times out, looping for many minutes.
_BREAKER_COOLDOWN  = 20.0     # seconds yt-dlp is paused after OOM kill or temp-dir error
_fail_play         = {}       # ck -> fail_time  (set by play failures)
_fail_preview      = {}       # ck -> fail_time  (set by preview failures)
_fail_lock         = threading.Lock()
_breaker_until     = 0.0
_breaker_lock      = threading.Lock()

def _recent_fail(ck, preview):
    """Recent-failure check that does NOT let a preview poison plays.

    Play requests check only the play-fail bucket.  Preview requests check
    both: a recent play fail means the next preview will fail too, so save
    the yt-dlp spawn; a recent preview fail blocks repeat previews."""
    now = time.time()
    with _fail_lock:
        t = _fail_play.get(ck)
        if t is not None:
            if (now - t) < _FAIL_TTL_PLAY:
                return True
            _fail_play.pop(ck, None)
        if preview:
            t = _fail_preview.get(ck)
            if t is not None:
                if (now - t) < _FAIL_TTL_PREVIEW:
                    return True
                _fail_preview.pop(ck, None)
        return False

def _mark_fail(ck, preview):
    with _fail_lock:
        d = _fail_preview if preview else _fail_play
        d[ck] = time.time()
        if len(d) > 256:                                # bound memory per bucket
            oldest = min(d, key=d.get)
            d.pop(oldest, None)

def _breaker_open():
    with _breaker_lock:
        return time.time() < _breaker_until

def _trip_breaker():
    global _breaker_until
    with _breaker_lock:
        if time.time() >= _breaker_until:               # log once per trip
            log(f"yt-dlp circuit breaker TRIPPED ({_BREAKER_COOLDOWN}s) — "
                f"temp dir unavailable (disk full?)")
        _breaker_until = time.time() + _BREAKER_COOLDOWN

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

def _cleanup_mei_dirs():
    """Remove orphaned PyInstaller extraction dirs from our TMPDIR.

    Every yt-dlp invocation unpacks its bundle to $TMPDIR/_MEIxxxxxx (~70 MB).
    When we SIGKILL the process group, Python's atexit cleanup never runs and
    the dir is left behind.  Enough kills fill the tmpfs and PyInstaller can
    no longer mkdir its extraction dir, killing all subsequent resolves with
    `Could not create temporary directory`.

    We find orphans by scanning /proc/*/maps: any _MEI dir that no live
    process has mapped is safe to delete.  Sweep both our dedicated tmpdir
    AND /dev/shm at root (covers _MEIs left by prior tubed versions before
    we moved the extraction location)."""
    import glob
    scan_roots = [_PYI_TMPDIR, "/dev/shm"]
    # dedupe while preserving order
    scan_roots = list(dict.fromkeys(scan_roots))
    in_use: set = set()
    try:
        for pid_entry in os.listdir("/proc"):
            if not pid_entry.isdigit():
                continue
            try:
                with open(f"/proc/{pid_entry}/maps") as fh:
                    for line in fh:
                        for root in scan_roots:
                            tag = f"{root}/_MEI"
                            if tag in line:
                                parts = line.split()
                                if parts:
                                    path = parts[-1]
                                    idx = path.find(tag)
                                    if idx >= 0:
                                        after = path[idx + len(root) + 1:]
                                        in_use.add(os.path.join(root, after.split("/")[0]))
            except OSError:
                pass
    except OSError:
        pass
    cleaned = 0
    for root in scan_roots:
        for d in glob.glob(os.path.join(root, "_MEI*")):
            if not os.path.isdir(d) or d in in_use:
                continue
            try:
                shutil.rmtree(d, ignore_errors=True)
                cleaned += 1
            except OSError:
                pass
    if cleaned:
        log(f"cleaned {cleaned} orphaned _MEI dir(s)")


# Procs we've explicitly killed (preempt, timeout, shutdown).  These exit
# with returncode == -9 just like a kernel OOM kill, so we use this set
# to disambiguate "kernel killed it" from "we killed it" — without that,
# every preempt was being misreported as OOM, tripping the breaker, and
# locking the next 20 s of plays out.
_killed_by_us = set()
_killed_lock  = threading.Lock()

def _kill_proc_group(p):
    with _killed_lock:
        _killed_by_us.add(p.pid)
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass

def _was_killed_by_us(p):
    with _killed_lock:
        return p.pid in _killed_by_us

def _forget_kill(p):
    with _killed_lock:
        _killed_by_us.discard(p.pid)

def _kill_all_children():
    with _active_lock:
        procs = list(_active_procs)
    for p in procs:
        _kill_proc_group(p)

def _kill_preview_procs():
    """Force-kill every in-flight preview yt-dlp and wait for them to be
    fully reaped before returning.  Called by op_stream the instant a real
    play arrives.

    The wait is the important part: yt-dlp's PyInstaller extraction holds
    ~70 MB of anon pages.  If we return before the kernel has reaped the
    zombie and reclaimed those pages, the next yt-dlp (the play's) starts
    its own ~70 MB extraction with the dying preview's RAM still booked —
    on a 640 MB device that flips the OOM killer immediately."""
    with _preview_lock:
        procs = list(_preview_procs)
    if not procs:
        return
    for p in procs:
        _kill_proc_group(p)
    # Wait for each killed process group to actually exit so the kernel
    # releases their pages before we let the play spawn its yt-dlp.
    deadline = time.time() + 2.0
    for p in procs:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        try:
            p.wait(timeout=remaining)
        except Exception:
            pass

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

# Run yt-dlp at mildly reduced priority so it yields CPU to the foreground app,
# but not so low that the OOM killer preferentially targets it.
# taskset is intentionally NOT used: pinning yt-dlp + its deno child to a single
# core makes the Python/deno execution slower, which extends the peak memory
# window and increases the chance of an OOM kill on the RAM-constrained A35.
# nice -n 5 still keeps the UI responsive (foreground tasks get priority) without
# the extended peak-memory duration that -n 15 caused.
def _sched_prefix():
    # Previously prepended `nice -n 5` to keep the UI responsive, but on this
    # RAM/CPU-constrained device the foreground TubeLite app saturates the
    # CPU and nice'd yt-dlp gets scheduling-starved — manifesting as the
    # 9-28 s timeouts with ZERO output (yt-dlp's Python interpreter hadn't
    # even reached its first print() yet).  Match the user's shell exactly:
    # no nice prefix.
    return []

_SCHED_PREFIX = _sched_prefix()

# Flags shared by every yt-dlp invocation.
#
# use_cookies: STREAM resolves pass use_cookies=False.  Counter-intuitively,
# attaching an account (cookies) to the `android_vr` client makes YouTube demand
# extra verification → "Sign in to confirm you're not a bot", whereas the SAME
# client ANONYMOUSLY returns the full DASH ladder (proven: the bare
# `yt-dlp --js-runtimes node -F <video>` test had no cookies and worked).  The
# JS runtime is the unlock, not cookies — so streams go cookie-less.  Cookies are
# still wired up for the future authenticated-feeds path (search/home).
def _ytdlp_base_args(use_cookies=True):
    """Minimum-footprint base args, plus tight network bounds.

    The first two flags mirror the working manual invocation:
    * --no-config — prevent loading /etc/yt-dlp.conf or a user config.
    * --no-update — never hit the GitHub release endpoint at runtime.

    The next three are essential because yt-dlp's defaults will silently
    hang for minutes on a stalled connection — well past our 28 s timeout.
    On the manual command they're not needed because the user can ^C, but
    tubed has hard budgets that must be met:
    * --socket-timeout 8 — cap any single HTTP/TCP wait.
    * --extractor-retries 0 / --retries 0 — single try per call; tubed
      and the C++ side already retry at higher levels.

    Cookies are opt-in via use_cookies; resolve paths pass False because
    the cookieless manual test was the one that worked, and authenticated
    extraction takes a different (and slower) path on this device."""
    args = [
        YT_DLP, "--no-config", "--no-update",
        "--socket-timeout", "8",
        "--extractor-retries", "0",
        "--retries", "0",
        # DIAGNOSTIC: -v makes yt-dlp dump its full bootstrap (Python ver,
        # flags parsed, args resolved) to stderr the instant Python starts.
        # If we still see NO `produced first stderr line` from the drain
        # thread, yt-dlp's Python code isn't running at all — the hang is
        # in the PyInstaller bootloader, not in yt-dlp itself.  Remove this
        # once playback is reliable; the verbose output is noisy.
        "-v",
    ]
    if use_cookies and _have_cookies():
        args += ["--cookies", COOKIES]
    return args


def _ensure_pyi_tmpdir():
    """Find or create a tmpdir PyInstaller can actually write to.

    PyInstaller-bundled yt-dlp extracts its ~70 MB bundle to $TMPDIR/_MEIxxxx
    on every invocation.  If TMPDIR is unset → defaults to /tmp.  On this
    device /tmp lives on the 100 %-full root partition, so PyInstaller dies
    with `[PYI:ERROR] Could not create temporary directory` before any
    Python code ever runs.

    We try each candidate by actually creating a probe directory and
    writing a tiny file — `os.path.isdir()` is not enough, because tmpfs
    that's full passes isdir() and still fails mkdir().  Returns the path
    of the first candidate that survives the probe."""
    candidates = [
        "/dev/shm/tubed-tmp",      # tmpfs in RAM — preferred (fast, isolated)
        "/var/tmp/tubed-tmp",      # disk fallback, usually on a writable partition
        "/tmp/tubed-tmp",          # last resort
    ]
    for path in candidates:
        try:
            os.makedirs(path, exist_ok=True)
            probe = os.path.join(path, ".write_probe")
            with open(probe, "wb") as f:
                f.write(b"x")
            os.unlink(probe)
            return path
        except OSError as ex:
            log(f"tmpdir candidate {path} unusable: {ex}")
            continue
    log("ERROR: no writable tmpdir found — PyInstaller yt-dlp will fail")
    return "/dev/shm"  # let it fail with a clear PYI error

_PYI_TMPDIR = _ensure_pyi_tmpdir()


def _ytdlp_env():
    """Augment env for three essentials on this device:

    * TMPDIR pointing to a tubed-owned, verified-writable directory so the
      PyInstaller bootloader can extract its _MEI bundle reliably.  See
      _ensure_pyi_tmpdir for the candidate list and why we probe.
    * PATH includes BASE_DIR/vendor so yt-dlp can find the vendored deno
      JS runtime.
    * PYTHONUNBUFFERED=1 forces Python (including yt-dlp's bundled
      interpreter) to flush stdout/stderr after every write.  Without
      this, output to a pipe is fully buffered — so yt-dlp's extractor
      messages ('Downloading webpage' etc.) never reach our drain thread
      until the process exits, making mid-run debugging impossible."""
    env = os.environ.copy()
    env["TMPDIR"] = _PYI_TMPDIR
    env["PYTHONUNBUFFERED"] = "1"
    vendor_dir = os.path.join(BASE_DIR, "vendor")
    if os.path.isfile(os.path.join(vendor_dir, "deno")):
        cur = env.get("PATH", "")
        if vendor_dir not in cur.split(":"):
            env["PATH"] = vendor_dir + (":" + cur if cur else "")
    return env




def _run_ytdlp_simple(args, timeout):
    """Simple subprocess.run path used for plays.  This mirrors EXACTLY what
    the user's working manual shell test does and what our successful
    startup `spawn sanity` check uses: one subprocess.run with capture,
    no Popen + thread machinery.

    The Popen + drain-threads path (_run_ytdlp below) silently dropped
    yt-dlp's output in 0.7.x/0.8.0 — manual identical-args invocations
    produced full verbose output instantly, but tubed-spawned ones showed
    zero stderr for 28 s before being killed.  Using subprocess.run avoids
    whatever pipe/thread interaction was breaking that.

    Plays don't need mid-flight preempt (only previews do), so they don't
    need the polling-loop machinery — they just need a yt-dlp call that
    actually works."""
    if _breaker_open():
        return ""
    _cleanup_mei_dirs()
    try:
        log("spawn argv:", " ".join(args))
        env = _ytdlp_env()
        log(f"spawn env: TMPDIR={env.get('TMPDIR','(unset)')} "
            f"PATH={env.get('PATH','(unset)')[:80]} "
            f"PYTHONUNBUFFERED={env.get('PYTHONUNBUFFERED','(unset)')}")
        t0 = time.time()
        result = subprocess.run(
            args, capture_output=True, timeout=timeout,
            stdin=subprocess.DEVNULL, env=env,
        )
        dt = time.time() - t0
        rc = result.returncode
        out = (result.stdout or b"").decode("utf-8", "replace")
        err = (result.stderr or b"").decode("utf-8", "replace").strip()
        log(f"yt-dlp finished in {dt:.2f}s rc={rc} stdout={len(out)}B stderr={len(err)}B")
        if err:
            # Disk-full / temp-dir failure → trip the breaker.
            if ("create temporary directory" in err or "No space left" in err
                    or "Failed to extract" in err
                    or "decompression resulted in return code" in err):
                log("yt-dlp:", err.splitlines()[0] if err else "")
                _trip_breaker()
                return ""
            # Otherwise log every line so we can see what yt-dlp said.
            for line in err.splitlines():
                log("yt-dlp:", line)
        if rc != 0:
            return ""
        return out
    except subprocess.TimeoutExpired as ex:
        log(f"yt-dlp timed out after {timeout}s (run path)")
        # Capture whatever did make it out before the kill.
        if ex.stderr:
            for line in ex.stderr.decode("utf-8", "replace").splitlines():
                log("yt-dlp:", line)
        return ""
    except Exception as ex:
        log(f"yt-dlp run failed: {ex}")
        return ""


def _run_ytdlp_streaming(args, timeout):
    """Run yt-dlp via Popen and yield stdout lines as they arrive.

    Unlike _run_ytdlp_simple (which waits for the full run before returning),
    this generator yields each stdout line the instant yt-dlp writes it.  For
    --flat-playlist --dump-json runs yt-dlp writes one JSON object per video
    as it's fetched, so callers see the first result in ~3-5 s instead of
    waiting for the entire batch.

    The process is killed on timeout, breaker-open, or generator close.
    Stderr is captured and logged line-by-line so diagnostics still appear."""
    if _breaker_open():
        return
    _cleanup_mei_dirs()
    env = _ytdlp_env()
    try:
        log("stream-spawn argv:", " ".join(args))
        p = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            env=env,
        )
        log(f"stream-spawn ok: pid={p.pid}")
    except Exception as ex:
        log("yt-dlp streaming launch failed:", ex)
        return
    with _active_lock:
        _active_procs.add(p)

    def _drain_stderr():
        try:
            for raw in iter(p.stderr.readline, b""):
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if line:
                    if ("create temporary directory" in line
                            or "No space left" in line
                            or "Failed to extract" in line):
                        log("yt-dlp:", line)
                        _trip_breaker()
                    else:
                        log("yt-dlp:", line)
        except Exception as ex:
            log(f"streaming stderr drain crashed: {ex}")
    stderr_thread = threading.Thread(target=_drain_stderr, daemon=True)
    stderr_thread.start()

    try:
        deadline = time.time() + timeout
        while True:
            if time.time() >= deadline:
                log("yt-dlp streaming timed out; killing process group")
                _kill_proc_group(p)
                break
            if not _client_is_alive():
                log("client disconnected; killing streaming yt-dlp")
                _kill_proc_group(p)
                break
            line = p.stdout.readline()
            if not line:
                break
            yield line.decode("utf-8", "replace").strip()
    finally:
        try:
            p.stdout.close()
        except Exception:
            pass
        try:
            p.wait(timeout=2)
        except Exception:
            pass
        stderr_thread.join(timeout=1)
        with _active_lock:
            _active_procs.discard(p)
        _forget_kill(p)


def _run_ytdlp(args, timeout, is_preview=False):
    """Run yt-dlp and return stdout text (empty on failure). The child runs in
    its own process group and is force-killed (group-wide) on timeout/error, so
    a hung or slow yt-dlp can never leak into a stray background process.

    is_preview: when True, the run aborts the instant a real play is queued
    (see _pending_plays) so hover-prefetch never delays an actual play."""
    # Plays route through _run_ytdlp_simple (no preempt needed; the Popen+
    # thread path silently swallowed yt-dlp output).  Only previews still
    # use this path because they need mid-flight preemption.
    if not is_preview:
        return _run_ytdlp_simple(args, timeout)
    # Circuit breaker: if yt-dlp recently couldn't make its temp dir (disk full),
    # don't even spawn it — that only burns CPU and writes more error spam to the
    # very disk that's full.  Fail fast until the cooldown clears.
    if _breaker_open():
        return ""
    _cleanup_mei_dirs()
    try:
        # nice/taskset (when present) exec into yt-dlp, so the launched process
        # group still ends as yt-dlp and the group-wide kill below still works.
        #
        # stderr is captured (not DEVNULL'd) so when a video fails to play we
        # can log yt-dlp's actual diagnostic — "Sign in to confirm you're not a
        # bot", "Video unavailable", PO-token errors, etc.  Without this the
        # silent-fail path was undebuggable from the user's side.
        env = _ytdlp_env()
        full_argv = _SCHED_PREFIX + args
        # Log the EXACT command and TMPDIR so the user can copy-paste it into
        # an SSH shell and verify whether the failure is something tubed-
        # specific (env/cwd/stdin) or an actual yt-dlp problem.
        log("spawn argv:", " ".join(full_argv))
        log(f"spawn env: TMPDIR={env.get('TMPDIR','(unset)')} "
            f"PATH={env.get('PATH','(unset)')[:80]} "
            f"LD_LIBRARY_PATH={env.get('LD_LIBRARY_PATH','(unset)')[:80]}")
        p = subprocess.Popen(full_argv, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             stdin=subprocess.DEVNULL,
                             start_new_session=True, env=env)
        log(f"spawn ok: pid={p.pid}")
    except Exception as ex:
        log("yt-dlp launch failed:", ex)
        return ""
    with _active_lock:
        _active_procs.add(p)
    if is_preview:
        with _preview_lock:
            _preview_procs.add(p)

    def _classify_and_log(txt):
        if not txt:
            return
        # Disk-full / temp-dir failure → trip the breaker.  ALSO log the line
        # so we can see what actually broke (previous behavior swallowed the
        # message, which hid the v0.7.3 "TMPDIR unset → /tmp full" failure
        # behind a generic 'breaker tripped' line and made the bug nearly
        # impossible to diagnose).
        if ("create temporary directory" in txt or "No space left" in txt
                or "Failed to extract" in txt or "decompression resulted in return code" in txt):
            log("yt-dlp:", txt)
            _trip_breaker()
            return
        # SABR-gated video: android client gets formats blocked by YouTube's SABR
        # experiment.  This is normal; _extract_stream will fall through to the
        # default client chain which is NOT SABR-blocked.  Summarise once instead
        # of forwarding the noisy multi-line yt-dlp blurb.
        if "android client https formats have been skipped" in txt:
            log("yt-dlp: android SABR-blocked — falling through to default chain")
            return
        for line in txt.splitlines():
            log("yt-dlp:", line)

    # Drain stdout AND stderr in background threads.  We must NOT mix this
    # with p.communicate(): communicate() spawns its OWN pipe-drain threads
    # internally, which would race ours on the same fds and deadlock — the
    # symptom was zero stderr output and 28 s "timed out" with no diagnostic.
    # The main loop now uses p.poll() to check exit status.
    _stdout_chunks = []
    def _drain_stdout():
        try:
            for raw in iter(p.stdout.readline, b""):
                if not raw:
                    break
                _stdout_chunks.append(raw)
        except Exception:
            pass
    _first_byte_logged = [False]
    def _drain_stderr():
        try:
            for raw in iter(p.stderr.readline, b""):
                if not raw:
                    break
                if not _first_byte_logged[0]:
                    _first_byte_logged[0] = True
                    log(f"yt-dlp pid={p.pid} produced first stderr line")
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if line:
                    _classify_and_log(line)
        except Exception as ex:
            log(f"stderr drain crashed: {ex}")
    _out_thread = threading.Thread(target=_drain_stdout, daemon=True)
    _err_thread = threading.Thread(target=_drain_stderr, daemon=True)
    _out_thread.start()
    _err_thread.start()

    def _collect_stdout():
        # Let drain threads finish flushing any buffered output before reading.
        _out_thread.join(timeout=1.0)
        _err_thread.join(timeout=1.0)
        return b"".join(_stdout_chunks).decode("utf-8", "replace") if _stdout_chunks else ""

    def _reap(reason):
        log(reason)
        _kill_proc_group(p)
        try:
            p.wait(timeout=2)
        except Exception:
            pass
        _collect_stdout()  # drain whatever the drain threads can still read
        return ""

    try:
        deadline = time.time() + timeout
        while True:
            rc = p.poll()
            if rc is not None:
                # Process exited on its own.
                if rc != 0:
                    if rc == -9 and not _was_killed_by_us(p):
                        # SIGKILL we didn't issue — kernel OOM-killer fired.
                        # Don't trip the breaker (that locks ALL videos out
                        # for ~20 s and causes the "stream resolving → back
                        # to browse" flicker).  Sleep briefly so kernel
                        # reclaim catches up before the caller's retry.
                        log("yt-dlp OOM-killed (SIGKILL -9); pausing 1.5s for memory reclaim")
                        time.sleep(1.5)
                    elif rc != -9:
                        log(f"yt-dlp exited {rc} for args={args[:4]}...")
                    # else: we killed it (preempt/timeout/shutdown); _reap
                    # already logged the reason.
                return _collect_stdout()
            if not _client_is_alive():
                return _reap("client disconnected; killing yt-dlp")
            if is_preview and _play_pending():
                return _reap("preempting preview yt-dlp for a pending play")
            if time.time() >= deadline:
                return _reap("yt-dlp timed out; killing process group")
            time.sleep(0.25)   # poll granularity — same order as before
    except Exception as ex:
        log("yt-dlp run failed:", ex)
        _kill_proc_group(p)
        return ""
    finally:
        with _active_lock:
            _active_procs.discard(p)
        if is_preview:
            with _preview_lock:
                _preview_procs.discard(p)
        _forget_kill(p)


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
    # Live detection — accept any of three signals because yt-dlp emits
    # different ones depending on feed type and version:
    #   * is_live: bool (older entries, search results)
    #   * live_status: "is_live" (modern flat-playlist subscriptions)
    #   * concurrent_view_count: int > 0 (only set for currently-streaming)
    # The last one is the most reliable for the subscriptions feed where
    # the first two are sometimes missing entirely.
    live_status = (e.get("live_status") or "").lower()
    concurrent = int(e.get("concurrent_view_count") or 0)
    is_live = (bool(e.get("is_live"))
               or live_status == "is_live"
               or concurrent > 0)

    # Views: prefer concurrent_view_count for live ("12K watching"),
    # otherwise view_count.  Subscriptions feed often omits view_count for
    # recent uploads — leave empty in that case so the card doesn't lie
    # with "0 views".
    views_n = concurrent if is_live else int(e.get("view_count") or 0)
    if is_live and views_n > 0:
        views_str = _views_str(views_n).replace("views", "watching")
    elif views_n > 0:
        views_str = _views_str(views_n)
    else:
        views_str = ""

    return {
        "id": vid,
        "title": e.get("title") or "",
        "author": author,
        "author_id": e.get("channel_id") or e.get("uploader_id") or "",
        "duration_seconds": dur,
        # Live streams have no meaningful duration — leave the string empty
        # so the client renders a LIVE badge instead of "0:00".
        # Live → empty so the card renders LIVE badge.  VOD with 0 dur
        # → empty rather than "0:00" (subscriptions sometimes lack duration
        # on freshly-uploaded videos that haven't been transcoded yet).
        "duration_string": "" if (is_live or dur <= 0) else f"{dur // 60}:{dur % 60:02d}",
        "view_count_string": views_str,
        "uploaded_ago_string": _uploaded_ago(e),
        "is_live": is_live,
    }


def op_search(req, writer=None):
    """Search YouTube for videos, streaming results as they arrive.

    When called with a `writer` callback (streaming mode), each parsed video
    is sent immediately via writer({"ok":True,"result":{...}}) and the
    function returns None.  When called without writer (legacy mode, e.g. from
    op_trending's internal call) it collects and returns all results as before."""
    query = (req.get("query") or "").strip()
    if not query:
        return {"ok": False, "error": "empty query"}

    page = max(1, int(req.get("page") or 1))
    page_size = max(1, min(int(req.get("page_size") or _FEED_PAGE_DEFAULT), 100))

    ck = f"search:{query}:{page}:{page_size}"
    cached = CACHE.get(ck)
    if cached is not None:
        if writer:
            for v in cached:
                writer({"ok": True, "result": v})
            return None
        return {"ok": True, "results": cached, "finished": True}

    start = (page - 1) * page_size + 1
    end   = page * page_size
    spec = f"ytsearch{end}:{query}"

    args = _ytdlp_base_args(use_cookies=_have_cookies()) + [
        "--flat-playlist", "--dump-json",
        "--extractor-args", "youtubetab:approximate_date",
        "--playlist-start", str(start), "--playlist-end", str(end),
        spec,
    ]

    results = []
    with _work_sem:
        for raw_line in _run_ytdlp_streaming(args, timeout=30):
            if not raw_line:
                continue
            try:
                v = _video_from_entry(json.loads(raw_line))
            except (ValueError, TypeError):
                continue
            if not v:
                continue
            results.append(v)
            if writer:
                writer({"ok": True, "result": v})

    log(f"search: {query!r} page {page} → {len(results)} items")

    if not results:
        # Retry once on empty (cold tubed start / Innertube partial response).
        log(f"search: retrying {query!r} (empty first attempt)")
        time.sleep(0.5)
        with _work_sem:
            for raw_line in _run_ytdlp_streaming(args, timeout=30):
                if not raw_line:
                    continue
                try:
                    v = _video_from_entry(json.loads(raw_line))
                except (ValueError, TypeError):
                    continue
                if not v:
                    continue
                results.append(v)
                if writer:
                    writer({"ok": True, "result": v})
        log(f"search: retry {query!r} → {len(results)} items")

    if not results:
        return {"ok": False, "error": "no results"}
    CACHE.set(ck, results, TTL_SEARCH)
    if writer:
        return None
    return {"ok": True, "results": results, "finished": True}


def op_trending(req):
    # Route trending through op_feed(kind="trending") which uses the
    # actual https://www.youtube.com/feed/trending URL extractor.
    # op_feed now streams — first cards appear within ~3-5 s.
    feed_req = {
        "kind":      "trending",
        "page":      req.get("page", 1),
        "page_size": req.get("page_size", _FEED_PAGE_DEFAULT),
    }
    return op_feed(feed_req, req.get("_writer"))



# URLs for feeds, exposed by `op_feed` with kind=<key>.  For subscriptions
# we prefer yt-dlp's `:ytsubs` shortcut which the on-device manual test
# confirmed working — it goes through yt-dlp's dedicated subscriptions
# extractor that paginates correctly (the plain /feed/subscriptions URL
# returns 0 items in some auth states because the guest version of the
# page has nothing to enumerate).  The full URL is kept as a second
# variant in case `:ytsubs` ever breaks.
_FEED_URLS = {
    "subscriptions": [
        ":ytsubs",
        "https://www.youtube.com/feed/subscriptions",
    ],
    "trending":    ["ytsearch:trending"],
    "home":        ["https://www.youtube.com/"],
    "history":     ["https://www.youtube.com/feed/history"],
    "liked":       ["https://www.youtube.com/playlist?list=LL"],
    "watch_later": ["https://www.youtube.com/playlist?list=WL"],
}

# Kinds that require sign-in.  Public feeds (trending) skip the cookie
# check and resolve fine for guests; personal feeds (subscriptions,
# history, liked, watch_later, the home recommendations) need cookies
# to return anything.
_FEED_AUTH_REQUIRED = {"subscriptions", "home", "history", "liked", "watch_later"}

# Default per-page size.  Smaller chunks finish faster — a 20-item page
# typically completes in 6-10 s vs ~18-22 s for a 50-item page, which
# matters because the C++ socket timeout is 30 s and a borderline-slow
# Innertube response with 50 items would time out at the finish line.
# The UI streams cards in as they arrive (see App::loadHomeFeeds), so
# smaller pages actually FEEL faster — first card visible in ~3 s, not
# 15.  Callers can override via `page_size` for a one-shot bulk fetch.
_FEED_PAGE_DEFAULT = 20


def _cookie_summary():
    """Cheap diagnostic: report how many lines the cookies file has and
    whether the key YouTube auth cookies appear in it.  Logged on every
    empty feed result so users see why subscriptions look empty.

    YouTube's signed-in API requests require SAPISID (or one of its
    `__Secure-` variants) — that's what generates the SAPISIDHASH
    Authorization header.  Browser extensions that only export "essential"
    cookies often skip SAPISID, which leaves the user with a file that
    looks valid (has SID) but doesn't authenticate.  We specifically flag
    that case so the diagnostic is actionable instead of a wall of names."""
    try:
        with open(COOKIES, "r", errors="replace") as fh:
            text = fh.read()
        rows = [ln for ln in text.splitlines()
                if ln.strip() and not ln.startswith("#")]
        needles = ("SAPISID", "__Secure-3PAPISID", "__Secure-1PAPISID",
                   "SID", "HSID", "SSID", "LOGIN_INFO", "__Secure-3PSID")
        present = [n for n in needles if n in text]
        # The SAPISIDHASH header is what actually proves identity to
        # YouTube's InnerTube API.  Without one of these we silently get
        # the guest experience.
        sapisid_family = ("SAPISID", "__Secure-3PAPISID", "__Secure-1PAPISID")
        has_sapisid = any(n in text for n in sapisid_family)
        hint = ""
        if not has_sapisid:
            hint = (" — MISSING SAPISID family! YouTube auth will fail. "
                    "Re-export INCLUDING SAPISID, __Secure-3PAPISID and "
                    "HSID — see https://github.com/yt-dlp/yt-dlp/wiki/"
                    "Extractors#exporting-youtube-cookies")
        return (f"cookies.txt: {len(rows)} rows, auth cookies present: "
                f"{','.join(present) or 'NONE'}{hint}")
    except OSError as ex:
        return f"cookies.txt: unreadable ({ex})"


def op_feed(req, writer=None):
    """Fetch a YouTube feed, streaming results as they arrive.

    When `writer` is set (streaming mode from Handler), each parsed video
    is written immediately to the socket as {"ok":True,"result":{...}}.  The
    function still caches the full result list for subsequent cache-hits.
    Without `writer` (legacy / internal call) it falls back to collecting
    everything and returning the old {results:[...]} shape."""
    kind = (req.get("kind") or "subscriptions").strip()
    page = max(1, int(req.get("page") or 1))
    page_size = max(1, min(int(req.get("page_size") or _FEED_PAGE_DEFAULT), 100))
    if kind not in _FEED_URLS:
        return {"ok": False, "error": f"unknown feed kind: {kind}"}
    needs_auth = kind in _FEED_AUTH_REQUIRED
    if needs_auth and not _have_cookies():
        return {"ok": False, "error": "not signed in"}

    ck = f"feed:{kind}:{page}:{page_size}"
    cached = CACHE.get(ck)
    if cached is not None:
        if writer:
            for v in cached:
                writer({"ok": True, "result": v})
            return None
        return {"ok": True, "results": cached, "finished": True}

    start = (page - 1) * page_size + 1
    end   = page * page_size

    results = []
    last_url = ""

    # Timeout budget (must fit within the C++ socket SO_RCVTIMEO of 35 s):
    #   Public feeds (trending): single 25 s yt-dlp run.
    #   Auth feeds (subs etc.):  up to 2 attempts × 15 s = 30 s.
    ytdlp_timeout = 15 if needs_auth else 25
    attempts = (1,) if not needs_auth else (1, 2)

    for url in _FEED_URLS[kind]:
        last_url = url
        for attempt in attempts:
            attempt_results = []
            spec = url
            if url.startswith("ytsearch:"):
                term = url.split(":", 1)[1]
                spec = f"ytsearch{end}:{term}"
            args = _ytdlp_base_args(use_cookies=_have_cookies()) + [
                "--flat-playlist", "--dump-json",
                "--extractor-args", "youtubetab:approximate_date",
                "--playlist-start", str(start), "--playlist-end", str(end),
                spec,
            ]
            with _work_sem:
                for raw_line in _run_ytdlp_streaming(args, timeout=ytdlp_timeout):
                    if not raw_line:
                        continue
                    try:
                        v = _video_from_entry(json.loads(raw_line))
                    except (ValueError, TypeError):
                        continue
                    if not v:
                        continue
                    attempt_results.append(v)
                    if writer:
                        writer({"ok": True, "result": v})
            log(f"feed {kind}: attempt {attempt} via {url} → {len(attempt_results)} items")
            if attempt_results:
                results = attempt_results
                break
            if attempt == 1 and len(attempts) > 1:
                time.sleep(0.5)
        if results:
            break
        log(f"feed {kind}: variant {url} exhausted; trying next")

    if not results:
        if needs_auth:
            summary = _cookie_summary()
            log(f"feed {kind}: ALL variants returned 0 items — {summary}")
            if "MISSING SAPISID" in summary:
                return {"ok": False, "error":
                        "cookies.txt is missing SAPISID — re-export with full "
                        "cookie set (see tubed.log for the wiki link)"}
        else:
            log(f"feed {kind}: ALL variants returned 0 items (public feed; "
                f"likely a transient yt-dlp/Innertube issue)")
        return {"ok": False, "error":
                f"feed empty (last url: {last_url}; cookies may not be "
                f"authenticated — see tubed.log)"}
    CACHE.set(ck, results, TTL_FEED)
    if writer:
        return None
    return {"ok": True, "results": results, "finished": True}





# Field separator for yt-dlp --print output.  Group separator (0x1d) doesn't
# appear in normal YouTube text content; safer than a printable delimiter
# that could collide with title/description content.
_PRINT_SEP = "\x1d"
_PRINT_FIELDS = [
    "%(url)s",                                               # 0 video URL
    "%(subtitles.en.0.url,automatic_captions.en.0.url|)s",   # 1 subtitle URL
    "%(view_count|0)d",                                      # 2
    "%(like_count|0)d",                                      # 3
    "%(comment_count|0)d",                                   # 4
    "%(channel_follower_count|0)d",                          # 5
    "%(height|0)d",                                          # 6
    "%(description|)s",                                      # 7 (last — can be multiline)
]
_PRINT_TEMPLATE = _PRINT_SEP.join(_PRINT_FIELDS)


def _extract_stream(video_id, max_height, preview=False, deadline=None, is_live=False):
    """Resolve playback URLs via yt-dlp `--print` (NOT `--dump-single-json`).

    The --print path is materially faster: yt-dlp emits ONLY the fields we
    ask for, in order, separated by 0x1d.  Output is ~500 bytes instead of
    the 30 KB+ full-info JSON, and there's no json.loads() of a giant
    formats array.  Description goes LAST so any embedded newlines are
    naturally captured by re-joining the trailing fields.

    is_live: caller's hint (from feed/search metadata) that this video is
    a live broadcast.  Live streams have no muxed progressive itag, so the
    default ios/android+skip=dash,hls path returns nothing.  When true we
    instead allow HLS (the standard live transport on YouTube) and pick
    the best HLS variant via mpv-friendly args."""
    if _breaker_open():
        raise RuntimeError("circuit breaker open")

    # Headroom math (android-exclusive 360p muxed path):
    #   PyInstaller bootloader   ~8 s
    #   webpage + android JSON   ~4 s
    #   --print flush            ~1 s
    #   total                    ~13 s typical, 25 s worst case (slow net)
    # Plus possible queue wait ~15 s behind a search → overall_deadline 40 s.
    # Live adds ~5 s for the HLS manifest fetch.  Dropping the ios client
    # round-trip cut about 8 s of typical resolve time vs. the old path.
    overall_deadline = deadline if deadline is not None else time.time() + (15.0 if preview else (45.0 if is_live else 40.0))
    remaining = overall_deadline - time.time()
    if remaining < 4.0:
        raise RuntimeError("deadline exceeded")
    run_timeout = min(remaining - 2.0, 13.0 if preview else (32.0 if is_live else 28.0))
    if run_timeout < 4.0:
        raise RuntimeError("deadline exceeded")

    yt_url = f"https://www.youtube.com/watch?v={video_id}"
    # ANDROID-EXCLUSIVE 360p PROGRESSIVE.
    #
    # Why we dropped ios: every recent yt-dlp version warns
    #   "ios client https formats require a GVS PO Token which was not
    #    provided. They will be skipped..."
    # so the iOS leg returns zero usable formats anyway — pure latency
    # waste.  The android client still serves itag 18 (360p H.264 + AAC,
    # muxed progressive) for all non-SABR videos, which is the canonical
    # path that worked pre-tubed (commit c707db7).
    #
    # We force itag 18 directly instead of `best[height<=N]` so on the
    # rare SABR-only video where android returns multiple muxed formats
    # we always pick the deterministic one mpv knows hardware-decodes
    # cleanly on RK3326.
    if is_live:
        # HLS path: allow yt-dlp to pick the best variant.  We don't try
        # to force itag because live streams use HLS manifests entirely,
        # not progressive itags.
        fmt = f"best[height<={max_height}]/best"
        extractor_args = "youtube:player_client=android;skip=dash"
        log(f"resolving LIVE stream for {video_id}")
    else:
        # itag 18 first (canonical 360p muxed), fall back to any 360p
        # muxed if 18 is missing, then any muxed at all.
        fmt = "18/best[height<=360][vcodec*=avc1]/best[height<=360]/best"
        extractor_args = "youtube:player_client=android;skip=dash,hls"

    # Pass cookies on stream resolves too when they exist.  Previous comment
    # claimed cookies caused bot challenges with DASH+android_vr — but the
    # current path is ios/android (with or without HLS) which doesn't have
    # that interaction, and forcing cookies-off prevented signed-in users
    # from accessing age-restricted / region-locked / private content.
    args = _ytdlp_base_args(use_cookies=True) + [
        "--no-playlist",
        "--skip-download",
        # `--no-call-home` was removed: yt-dlp 2026.x deprecates it (only
        # update checks remain, already disabled via --no-update).  Keeping
        # it printed a deprecation warning + future-removal threat to stderr
        # on every spawn — noise we don't need.
        "--extractor-args", extractor_args,
        "-f", fmt,
        "--print", _PRINT_TEMPLATE,
        yt_url,
    ]
    raw = _run_ytdlp(args, timeout=run_timeout, is_preview=preview)
    if not raw:
        raise RuntimeError("no playable URL")

    parts = raw.split(_PRINT_SEP)
    if len(parts) < len(_PRINT_FIELDS) or not parts[0].strip():
        raise RuntimeError("no playable URL")
    # Re-join any trailing parts in case description (last field) wrapped.
    if len(parts) > len(_PRINT_FIELDS):
        parts = parts[:len(_PRINT_FIELDS) - 1] + [_PRINT_SEP.join(parts[len(_PRINT_FIELDS) - 1:])]

    def _int(s):
        try:
            return int((s or "0").strip())
        except (ValueError, TypeError):
            return 0

    url = parts[0].strip()
    sub_url = parts[1].strip()
    # Subtitle URL post-processing: yt-dlp's first English entry is
    # usually `fmt=json3`, but mpv's subtitle decoder only handles
    # WebVTT/SRT reliably on this device.  YouTube serves both formats
    # from the same endpoint differing only in the `fmt` query param —
    # so we just rewrite it.  Without this, subtitles silently no-op
    # ever since the switch from --dump-single-json + _best_subtitle_url
    # to the leaner --print template.
    if sub_url and "fmt=json3" in sub_url:
        sub_url = sub_url.replace("fmt=json3", "fmt=vtt")
    meta = {
        "view_count":       _int(parts[2]),
        "like_count":       _int(parts[3]),
        "comment_count":    _int(parts[4]),
        "subscriber_count": _int(parts[5]),
        "description":      parts[7].rstrip("\n"),
    }
    height = _int(parts[6])
    log(f"resolved {video_id}: {height or '?'}p muxed")
    return (url, "", sub_url, meta)


def op_stream(req):
    vid = (req.get("id") or "").strip()
    h = int(req.get("max_height") or 360)
    preview = bool(req.get("preview", False))
    is_live = bool(req.get("is_live", False))
    if not vid:
        return {"ok": False, "error": "missing id"}

    log(f"op_stream: vid={vid} h={h} preview={preview} live={is_live}")
    # Anchor the resolve deadline to NOW — before any semaphore wait — so that
    # semaphore wait + yt-dlp time together stay inside the C++ socket budget
    # (preview 14s, play 40s).  _extract_stream receives this deadline so it
    # can't accidentally run over the budget even after a long semaphore wait.
    req_deadline = time.time() + (11.0 if preview else 37.0)

    # Auth state is part of the cache key: a guest-resolved 360p muxed entry
    # must NOT be served once the user signs in (they should get the DASH
    # ladder), and vice-versa.  'a' = authed (cookies), 'g' = guest.
    authed = _have_cookies()
    # Cache key includes the live flag so a stale VOD URL doesn't get
    # served when the same id is later resolved as a live broadcast (and
    # vice versa).  Live URLs in particular have a much shorter TTL — the
    # HLS manifest sometimes rotates within minutes.
    ck = f"stream:{vid}:{h}:{'a' if authed else 'g'}{':live' if is_live else ''}"
    cached = CACHE.get(ck)
    if cached is not None:
        log(f"op_stream: cache hit for {vid}")
        return {"ok": True, **cached}

    # Negative cache: this vid failed very recently — return instantly without
    # spawning yt-dlp.  This is the hard stop for a runaway client (e.g. a
    # hover-preview loop) that would otherwise re-request the same dead video
    # hundreds of times a second.  (Logged sparsely on purpose.)
    if _recent_fail(ck, preview):
        return {"ok": False, "error": "recently failed"}

    # Hover-preview, but a real play is already waiting for the worker?  Don't
    # even queue — a speculative preview must never delay an actual play.  The
    # app re-requests the preview later if the card is still focused.
    if preview and _play_pending():
        log(f"op_stream: dropping preview {vid} — play pending")
        return {"ok": False, "error": "deferred for play"}

    # A real play (preview=False) registers itself as pending so any in-flight
    # preview yt-dlp aborts and frees the single worker for us (see _run_ytdlp),
    # and any preview still queued behind the worker bails at the check above.
    # We also kill the preview's yt-dlp process group OUTRIGHT here — the 0.5s
    # poll inside _run_ytdlp was empirically not preempting fast enough (a
    # preview held the worker for 24s under load while the play queued behind
    # it on the semaphore).  The direct kill makes preempt instant.
    global _pending_plays
    if not preview:
        with _pending_plays_lock:
            _pending_plays += 1
        _kill_preview_procs()
    try:
        with _work_sem:
            # Re-check inside the gate; another thread may have just resolved it.
            cached = CACHE.get(ck)
            if cached is not None:
                log(f"op_stream: cache hit (post-gate) for {vid}")
                return {"ok": True, **cached}
            # Storm guard: a sibling request that started just ahead of us
            # marked this vid as failed while we were waiting on the worker.
            # Without this re-check, 7 duplicate clicks at the C++ side all
            # queue behind the semaphore and each spawns its own yt-dlp.
            if _recent_fail(ck, preview):
                return {"ok": False, "error": "recently failed"}
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
                url, audio_url, sub, meta = _extract_stream(vid, h, preview=preview,
                                                              deadline=req_deadline,
                                                              is_live=is_live)
            except Exception as ex:
                log(f"op_stream: extract failed for {vid}: {ex}")
                # Mark fail in the per-kind bucket: preview fails poison only
                # future PREVIEWS (with a longer TTL — see _FAIL_TTL_PREVIEW —
                # because the C++ side re-prefetches the same preview every
                # ~30s on focus and we want one yt-dlp spawn, not 30).
                _mark_fail(ck, preview)
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
    a valid cookies.txt = signed in."""
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
    "feed": op_feed,
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
            _req_ctx.is_alive = self._client_alive
            try:
                # Streaming ops (search, feed, trending): the op accepts a
                # writer= callback and emits items one-by-one.  We detect
                # streaming support by checking the function signature.
                import inspect
                supports_writer = "writer" in inspect.signature(fn).parameters

                if supports_writer:
                    # Pass a writer that sends one JSON line per result.
                    # On error the op returns a dict (not None); we forward that.
                    err_resp = [None]
                    def _writer(obj):
                        self._send(obj)
                    req["_writer"] = _writer   # not used by op, plumbed via kwarg
                    resp = fn(req, writer=_writer)
                    if resp is not None:
                        # Op returned an error dict instead of streaming.
                        self._send(resp)
                    else:
                        # All items sent; emit the finished sentinel.
                        self._send({"ok": True, "finished": True})
                else:
                    resp = fn(req)
                    self._send(resp)
            except Exception as ex:
                log("op", op, "crashed:", ex)
                self._send({"ok": False, "error": str(ex)})
            finally:
                _req_ctx.is_alive = None
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


_lock_fd = None   # kept open to hold the flock for the lifetime of the process


def _acquire_lock_or_exit():
    """Grab an exclusive flock on PID_PATH so only one tubed runs at a time.
    The kernel releases the lock automatically when the process exits, so this
    is race-free even when two instances start at the exact same millisecond."""
    global _lock_fd
    import fcntl
    try:
        fd = open(PID_PATH, "a+")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fd.seek(0)
        fd.truncate()
        fd.write(str(os.getpid()) + "\n")
        fd.flush()
        _lock_fd = fd  # prevent GC from closing the fd (would release the lock)
    except (IOError, OSError):
        log("another tubed holds the instance lock; exiting")
        sys.exit(0)


def _make_server():
    """Create the server socket. We already hold the exclusive lock so there
    is no simultaneous-start race to handle here."""
    # Remove any stale socket left by a previous crashed instance.
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
    _acquire_lock_or_exit()

    server = _make_server()
    try:
        os.chmod(SOCK_PATH, 0o660)
    except OSError:
        pass

    def _shutdown(*_):
        log("signal received — shutting down")
        # server.shutdown() blocks waiting for serve_forever() to stop.
        # Calling it directly from the signal handler deadlocks because
        # serve_forever() runs in THIS thread (the signal interrupts it).
        # Spawn a thread so the handler returns immediately and
        # serve_forever() can notice the shutdown flag and exit cleanly.
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    # Last-resort safety net: never leave a yt-dlp child behind, no matter how
    # we exit.
    import atexit
    atexit.register(_kill_all_children)

    _cleanup_mei_dirs()   # clear any stale extracts from a previous run
    log(f"tubed v{TUBED_VERSION} starting")
    log(f"PyInstaller TMPDIR: {_PYI_TMPDIR}")

    # Process-state diagnostics: nice value, ulimits, fd count.  If `yt-dlp
    # --version` is slow (sanity check) the cause is usually one of these
    # being inherited from a sandboxed launcher and propagated to children.
    try:
        import resource
        nice_self = os.nice(0)   # returns current value without changing it
        nofile = resource.getrlimit(resource.RLIMIT_NOFILE)
        nproc  = resource.getrlimit(resource.RLIMIT_NPROC)
        cpu_t  = resource.getrlimit(resource.RLIMIT_CPU)
        as_lim = resource.getrlimit(resource.RLIMIT_AS)
        try:
            fd_count = len(os.listdir(f"/proc/{os.getpid()}/fd"))
        except OSError:
            fd_count = -1
        log(f"proc state: nice={nice_self} fds_open={fd_count} "
            f"nofile={nofile} nproc={nproc} cpu={cpu_t} as={as_lim}")
        # If we're niced down (>0) try to renice to 0.  Children inherit
        # the niceness, and a high nice value can make `yt-dlp --version`
        # take many seconds under app CPU load.
        if nice_self > 0:
            try:
                os.nice(-nice_self)
                log(f"reniced from {nice_self} to {os.nice(0)}")
            except OSError as ex:
                log(f"renice failed (needs root): {ex}")
    except Exception as ex:
        log(f"proc state introspection failed: {ex}")

    # Spawn-pathway sanity check: invoke `yt-dlp --version` twice through
    # the same Popen settings we use for real resolves.  Two calls so we can
    # distinguish cold-start PyInstaller extraction (slow) from steady-state
    # spawn (should be sub-second).  If BOTH are slow the issue is
    # scheduling / resource limits, not extraction.
    for label, budget in (("cold", 15), ("warm", 5)):
        try:
            t0 = time.time()
            sanity = subprocess.run(
                [YT_DLP, "--version"],
                capture_output=True, timeout=budget,
                stdin=subprocess.DEVNULL,
                env=_ytdlp_env(),
            )
            dt = time.time() - t0
            ver = (sanity.stdout or b"").decode("utf-8", "replace").strip()
            err = (sanity.stderr or b"").decode("utf-8", "replace").strip()
            log(f"spawn sanity ({label}): yt-dlp --version → '{ver}' in {dt:.2f}s rc={sanity.returncode}")
            if err:
                log(f"spawn sanity ({label}) stderr: {err[:200]}")
        except subprocess.TimeoutExpired:
            log(f"spawn sanity ({label}): yt-dlp --version TIMED OUT (>{budget}s)")
        except Exception as ex:
            log(f"spawn sanity ({label}): yt-dlp --version FAILED — {ex}")

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

#include "youtube_api.hpp"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <ctime>
#include <chrono>
#include <cstring>
#include <filesystem>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#endif

using json = nlohmann::json;

// ── Backend transport ────────────────────────────────────────────────────────
// All YouTube work lives in the persistent `tubed` service (docs/BACKEND.md).
// Here we just speak its newline-delimited JSON protocol over a Unix socket.

namespace {

#ifndef _WIN32
constexpr const char* kSockPath = "/dev/shm/tubed.sock";

int connectTubed(int timeout_ms) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSockPath, sizeof(addr.sun_path) - 1);

    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string locateTubedScript() {
    // The launcher cd's into the install dir before exec, so "tubed/tubed.py"
    // (the packaged layout) is the reliable relative path. Others cover the
    // hardcoded install location and the dev-repo layout.
    for (const char* p : {
            "tubed/tubed.py",                       // installed: <app>/tubed/tubed.py (cwd = <app>)
            "/roms/tools/tubelite/tubed/tubed.py",  // hardcoded install location
            "tools/tubed/tubed.py",                 // dev repo
            "../tools/tubed/tubed.py" }) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return "tubed/tubed.py";
}

// fork/exec a detached python3 tubed. The script path is resolved in the parent
// (before fork); the child only uses async-signal-safe calls before execlp.
void spawnTubed() {
    const std::string script = locateTubedScript();
    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid > 0) return;  // parent

    ::setsid();
    // stdin → /dev/null. stdout/stderr → tubed.log so yt-dlp errors are
    // recoverable.  `tail -f /dev/shm/tubed.log` to watch resolves live.
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) { ::dup2(devnull, 0); ::close(devnull); }
    int logfd = ::open("/dev/shm/tubed.log",
                       O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0) {
        ::dup2(logfd, 1);
        ::dup2(logfd, 2);
        ::close(logfd);
    } else {
        // Worst case: keep tubed's stderr on the parent's so the user can
        // still see SOMETHING.  Better than silent failure.
    }
    ::execlp("python3", "python3", script.c_str(), static_cast<char*>(nullptr));
    ::execlp("python",  "python",  script.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
}

// Throttle spawns so concurrent worker threads don't fork a swarm of services.
std::mutex g_spawn_mtx;
std::chrono::steady_clock::time_point g_last_spawn{};

bool ensureTubedRunning() {
    if (int fd = connectTubed(500); fd >= 0) { ::close(fd); return true; }

    {
        std::lock_guard<std::mutex> lk(g_spawn_mtx);
        auto now = std::chrono::steady_clock::now();
        if (now - g_last_spawn >= std::chrono::seconds(3)) {
            g_last_spawn = now;
            spawnTubed();
        }
    }

    // Wait for the service to come up (first launch loads yt-dlp).
    for (int i = 0; i < 80; ++i) {            // up to ~8s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (int fd = connectTubed(500); fd >= 0) { ::close(fd); return true; }
    }
    return false;
}

// Send one request line, read one response line. `timeout_ms` bounds the whole
// resolve (stream extraction can take a few seconds on first play).
bool tubedRequest(const json& req, json& resp, int timeout_ms) {
    if (!ensureTubedRunning()) return false;
    int fd = connectTubed(timeout_ms);
    if (fd < 0) return false;

    std::string payload = req.dump();
    payload.push_back('\n');
    size_t off = 0;
    while (off < payload.size()) {
        ssize_t w = ::write(fd, payload.data() + off, payload.size() - off);
        if (w <= 0) { ::close(fd); return false; }
        off += static_cast<size_t>(w);
    }

    std::string line;
    char buf[8192];
    bool got_newline = false;
    while (true) {
        ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        line.append(buf, static_cast<size_t>(r));
        if (line.find('\n') != std::string::npos) { got_newline = true; break; }
    }
    ::close(fd);
    if (!got_newline && line.empty()) return false;

    try {
        resp = json::parse(line);
    } catch (...) {
        return false;
    }
    return true;
}
#else  // _WIN32 — desktop dev stub (no Unix socket / fork).
bool tubedRequest(const nlohmann::json&, nlohmann::json&, int) { return false; }
#endif

template <typename T>
T jget(const json& j, const char* key, const T& dflt) {
    if (j.contains(key) && !j[key].is_null()) {
        try { return j[key].get<T>(); } catch (...) {}
    }
    return dflt;
}

YouTubeVideo videoFromJson(const json& j) {
    YouTubeVideo v;
    v.id                  = jget<std::string>(j, "id", "");
    v.title               = jget<std::string>(j, "title", "");
    v.author              = jget<std::string>(j, "author", "");
    v.duration_seconds    = jget<int>(j, "duration_seconds", 0);
    v.duration_string     = jget<std::string>(j, "duration_string", "");
    v.view_count_string   = jget<std::string>(j, "view_count_string", "");
    v.uploaded_ago_string = jget<std::string>(j, "uploaded_ago_string", "");
    return v;
}

} // namespace

YouTubeAPI::YouTubeAPI() {}
YouTubeAPI::~YouTubeAPI() {}

// ── Auth status ────────────────────────────────────────────────────────────────

void YouTubeAPI::refreshAuthStatus() {
    // Coalesce: if a check is already running, skip (the result lands soon).
    bool expected = false;
    if (!auth_inflight_.compare_exchange_strong(expected, true)) return;

    std::thread([this]() {
#ifndef _WIN32
        json req = {{"op", "auth_status"}};
        json resp;
        bool ok = tubedRequest(req, resp, 4000);
        if (ok && resp.value("ok", false)) {
            authed_.store(resp.value("authed", false), std::memory_order_relaxed);
        } else {
            // tubed unreachable — leave the last known value, just mark checked.
        }
#endif
        auth_checked_.store(true, std::memory_order_relaxed);
        auth_inflight_.store(false, std::memory_order_relaxed);
    }).detach();
}

// ── Search ────────────────────────────────────────────────────────────────────

void YouTubeAPI::search(const std::string& query, int page,
    std::function<void(const std::vector<YouTubeVideo>& results, bool finished)> callback) {

    int req_id = ++current_search_request_id_;
    tele_.searches_inflight.fetch_add(1, std::memory_order_relaxed);
    auto t0 = std::chrono::steady_clock::now();

    std::thread([this, query, page, callback, req_id, t0]() {
        auto finish = [this, t0]() {
            uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            tele_.last_search_ms.store(ms, std::memory_order_relaxed);
            ema_update(tele_.ema_search_ms_x10, ms);
            tele_.searches_total.fetch_add(1, std::memory_order_relaxed);
            tele_.tubed_wait_ms_total.fetch_add(ms, std::memory_order_relaxed);
            tele_.searches_inflight.fetch_sub(1, std::memory_order_relaxed);
        };

        if (req_id != current_search_request_id_) { callback({}, true); finish(); return; }

        json req = {{"op", "search"}, {"query", query}, {"page", page}};
        json resp;
        bool ok = tubedRequest(req, resp, 20000);

        if (req_id != current_search_request_id_) { callback({}, true); finish(); return; }

        if (ok && resp.value("ok", false) && resp.contains("results")) {
            for (const auto& item : resp["results"]) {
                if (req_id != current_search_request_id_) { callback({}, true); finish(); return; }
                YouTubeVideo v = videoFromJson(item);
                if (!v.id.empty()) callback({v}, false);
            }
        }
        callback({}, true);
        finish();
    }).detach();
}

// ── Stream resolution ──────────────────────────────────────────────────────────

void YouTubeAPI::getStreamUrl(const std::string& video_id, int max_height,
    std::function<void(bool success, const std::string& url, const std::string& subtitle_url, const std::string& audio_url, const VideoPlaybackMetadata& meta)> callback,
    bool isPreview,
    const std::string& parent_focus_id) {

    int req_id = 0;
    if (isPreview) {
        if (!parent_focus_id.empty()) {
            std::lock_guard<std::mutex> lock(preview_mutex_);
            current_preview_focus_id_ = parent_focus_id;
        }
        tele_.previews_inflight.fetch_add(1, std::memory_order_relaxed);
    } else {
        req_id = ++current_stream_request_id_;
        tele_.streams_inflight.fetch_add(1, std::memory_order_relaxed);
    }
    auto t0 = std::chrono::steady_clock::now();

    std::thread([this, video_id, max_height, callback, req_id, isPreview, parent_focus_id, t0]() {
        auto stillWanted = [this, req_id, isPreview, parent_focus_id]() -> bool {
            if (isPreview) {
                if (parent_focus_id.empty()) return true;
                std::lock_guard<std::mutex> lock(preview_mutex_);
                return current_preview_focus_id_ == parent_focus_id;
            }
            return req_id == current_stream_request_id_;
        };

        auto finish = [this, isPreview, t0](bool success, bool cancelled) {
            uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            tele_.tubed_wait_ms_total.fetch_add(ms, std::memory_order_relaxed);
            if (isPreview) {
                tele_.last_preview_ms.store(ms, std::memory_order_relaxed);
                ema_update(tele_.ema_preview_ms_x10, ms);
                tele_.previews_total.fetch_add(1, std::memory_order_relaxed);
                if (cancelled) tele_.previews_cancelled.fetch_add(1, std::memory_order_relaxed);
                tele_.previews_inflight.fetch_sub(1, std::memory_order_relaxed);
            } else {
                tele_.last_stream_ms.store(ms, std::memory_order_relaxed);
                ema_update(tele_.ema_stream_ms_x10, ms);
                tele_.streams_total.fetch_add(1, std::memory_order_relaxed);
                if (!success && !cancelled) tele_.streams_failed.fetch_add(1, std::memory_order_relaxed);
                tele_.streams_inflight.fetch_sub(1, std::memory_order_relaxed);
            }
        };

        if (!stillWanted()) { callback(false, "", "", "", VideoPlaybackMetadata()); finish(false, true); return; }

        json req = {{"op", "stream"}, {"id", video_id}, {"max_height", max_height}};
        if (isPreview) req["preview"] = true;
        json resp;
        // Previews use a shorter ceiling so a stale one releases its socket
        // quickly; tubed sees the disconnect and kills the underlying yt-dlp
        // instead of resolving a stream the user already scrolled past.
        // Budget covers tubed's android play resolve (~10-15s, 20s ceiling)
        // plus slack.  (If the DASH ladder / web client is ever re-enabled via
        // a PO-token provider, bump this back to ~38s for web's nsig dance.)
        bool ok = tubedRequest(req, resp, isPreview ? 14000 : 28000);

        if (!stillWanted()) { callback(false, "", "", "", VideoPlaybackMetadata()); finish(false, true); return; }

        if (!ok || !resp.value("ok", false)) {
            callback(false, "", "", "", VideoPlaybackMetadata());
            finish(false, false);
            return;
        }

        std::string url    = resp.value("url", std::string());
        std::string sub    = resp.value("subtitle_url", std::string());
        std::string audio  = resp.value("audio_url", std::string());
        VideoPlaybackMetadata meta;
        if (resp.contains("meta") && resp["meta"].is_object()) {
            const auto& m = resp["meta"];
            meta.description      = m.value("description", std::string());
            meta.view_count       = m.value("view_count", 0LL);
            meta.like_count       = m.value("like_count", 0LL);
            meta.comment_count    = m.value("comment_count", 0LL);
            meta.subscriber_count = m.value("subscriber_count", 0LL);
        }

        if (url.empty()) { callback(false, "", "", "", VideoPlaybackMetadata()); finish(false, false); return; }
        callback(true, url, sub, audio, meta);
        finish(true, false);
    }).detach();
}

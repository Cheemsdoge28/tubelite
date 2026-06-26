#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

// Data models — unchanged wire-compatible structs the rest of the app uses.
// These are now populated from the `tubed` backend service (see docs/BACKEND.md)
// instead of from per-request yt-dlp/curl subprocesses.

struct YouTubeVideo {
    std::string id;
    std::string title;
    std::string author;
    std::string duration_string;
    std::string view_count_string;
    std::string uploaded_ago_string;
    int duration_seconds = 0;
    // True when yt-dlp reports the entry as a live broadcast.  Live videos
    // resolve through a different yt-dlp path (HLS allowed; muxed-only
    // ios/android returns nothing) and render with a LIVE badge instead of
    // a duration pill.  Cards also skip the autoplay-next behaviour on end
    // because live streams don't really "end" in a meaningful sense.
    bool is_live = false;
};

struct VideoPlaybackMetadata {
    long long like_count = 0;
    long long comment_count = 0;
    long long view_count = 0;
    long long subscriber_count = 0;
    std::string description;
};

// YouTubeAPI is now a thin client for the local `tubed` service. The public
// surface is identical to before so app.cpp / daemon.cpp need no changes; all
// network and extraction work happens in the persistent backend process.
class YouTubeAPI {
public:
    YouTubeAPI();
    ~YouTubeAPI();

    // Async search with pagination. page is 1-indexed. Results are delivered
    // one-by-one (results, finished=false) then a final (empty, finished=true),
    // matching the previous streaming contract.
    void search(const std::string& query, int page,
        std::function<void(const std::vector<YouTubeVideo>& results, bool finished)> callback);

    // Fetch a personalized feed (e.g. "subscriptions", "home", "history",
    // "liked", "watch_later") via tubed `feed` op.  Requires a valid
    // cookies.txt on the device; without one tubed returns "not signed in"
    // and the callback fires with (empty, true).  Streaming contract is
    // identical to search().
    void fetchFeed(const std::string& kind, int page,
        std::function<void(const std::vector<YouTubeVideo>& results, bool finished)> callback);

    // Resolve a direct playback URL for libmpv via tubed.
    // `audio_url` is non-empty when tubed picks a DASH adaptive stream
    // (separate video + audio tracks). The caller passes it to mpv via
    // audio-add so >360p video has sound — YouTube no longer serves
    // muxed progressive above 360p.
    // isPreview=true uses a separate request token so preview prefetches don't
    // cancel main playback resolutions and vice versa.
    // is_live tells tubed to take the HLS path (live streams have no muxed
    // progressive itag, so the default ios/android+skip=hls path returns
    // nothing).  Caller passes the flag from the YouTubeVideo metadata.
    void getStreamUrl(const std::string& video_id, int max_height,
        std::function<void(bool success, const std::string& url, const std::string& subtitle_url, const std::string& audio_url, const VideoPlaybackMetadata& meta)> callback,
        bool isPreview = false,
        bool isLive = false,
        const std::string& parent_focus_id = "");

    // ── Auth (cookie-based sign-in) ──────────────────────────────────────────
    // Stream auth is purely cookie-based: a valid cookies.txt on the device =
    // signed in (unlocks the DASH quality ladder + lifts the bot-wall).  We
    // can't block the UI on a tubed round-trip, so the status is fetched on a
    // background thread and cached in atomics the UI reads cheaply.
    void refreshAuthStatus();              // async; pings tubed `auth_status`
    bool isAuthed()    const { return authed_.load(std::memory_order_relaxed); }
    bool authChecked() const { return auth_checked_.load(std::memory_order_relaxed); }

    // ── Telemetry (thread-safe; read from any thread including the UI) ────────
    // In-flight = currently running requests of that kind.
    // total      = ever-issued counter (since process start).
    // last_ms    = latency of the most recently completed request.
    // ema_ms_x10 = exponentially-weighted average latency, multiplied by 10 so
    //              we can store it in an integer atomic without locks.
    struct Telemetry {
        std::atomic<int>      searches_inflight{0};
        std::atomic<int>      streams_inflight{0};
        std::atomic<int>      previews_inflight{0};
        std::atomic<uint64_t> searches_total{0};
        std::atomic<uint64_t> streams_total{0};
        std::atomic<uint64_t> previews_total{0};
        std::atomic<uint64_t> streams_failed{0};
        std::atomic<uint64_t> previews_cancelled{0};
        std::atomic<uint32_t> last_search_ms{0};
        std::atomic<uint32_t> last_stream_ms{0};
        std::atomic<uint32_t> last_preview_ms{0};
        std::atomic<uint32_t> ema_search_ms_x10{0};
        std::atomic<uint32_t> ema_stream_ms_x10{0};
        std::atomic<uint32_t> ema_preview_ms_x10{0};
        std::atomic<uint64_t> tubed_wait_ms_total{0};   // sum of all tubed I/O
    };
    const Telemetry& telemetry() const { return tele_; }

private:
    // Separate tokens so preview and main stream requests don't interfere.
    std::atomic<int> current_stream_request_id_{0};
    std::atomic<int> current_search_request_id_{0};

    std::mutex preview_mutex_;
    std::string current_preview_focus_id_;

    std::atomic<bool> authed_{false};
    std::atomic<bool> auth_checked_{false};
    std::atomic<bool> auth_inflight_{false};

    Telemetry tele_;

    // EMA helper: blend the new sample into the rolling average at weight 0.2.
    static void ema_update(std::atomic<uint32_t>& slot, uint32_t new_ms) {
        uint32_t cur = slot.load(std::memory_order_relaxed);
        uint32_t sample_x10 = new_ms * 10u;
        // smoothing: 80% old + 20% new
        uint32_t next = (cur == 0) ? sample_x10
                                    : (cur * 8u + sample_x10 * 2u) / 10u;
        slot.store(next, std::memory_order_relaxed);
    }
};

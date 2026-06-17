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

    // Resolve a direct playback URL for libmpv via tubed.
    // isPreview=true uses a separate request token so preview prefetches don't
    // cancel main playback resolutions and vice versa.
    void getStreamUrl(const std::string& video_id, int max_height,
        std::function<void(bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& meta)> callback,
        bool isPreview = false,
        const std::string& parent_focus_id = "");

private:
    // Separate tokens so preview and main stream requests don't interfere.
    std::atomic<int> current_stream_request_id_{0};
    std::atomic<int> current_search_request_id_{0};

    std::mutex preview_mutex_;
    std::string current_preview_focus_id_;
};

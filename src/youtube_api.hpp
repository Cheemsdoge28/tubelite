#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <optional>
#include <sstream>
#include <mutex>


struct YouTubeVideo {
    std::string id;
    std::string title;
    std::string author;
    std::string duration_string;
    std::string view_count_string;
    std::string uploaded_ago_string;
    int duration_seconds = 0;
};

class YouTubeAPI {
public:
    YouTubeAPI();
    ~YouTubeAPI();

    // Async search with pagination. page is 1-indexed.
    void search(const std::string& query, int page,
        std::function<void(const std::vector<YouTubeVideo>& results, bool finished)> callback);

    // Get direct playback URL for libmpv.
    // isPreview=true uses a separate request counter so preview prefetches
    // don't cancel main playback resolutions and vice versa.
    void getStreamUrl(const std::string& video_id, int max_height,
        std::function<void(bool success, const std::string& url, const std::string& subtitle_url)> callback,
        bool isPreview = false,
        const std::string& parent_focus_id = "");

private:
    std::string executeCommand(const std::string& cmd);
    static std::string sanitizeShellText(const std::string& value);

    // Separate counters so preview and main stream requests don't interfere.
    std::atomic<int> current_stream_request_id_{0};
    std::atomic<int> current_search_request_id_{0};

    std::mutex preview_mutex_;
    std::string current_preview_focus_id_;
};

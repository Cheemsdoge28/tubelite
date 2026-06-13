#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <optional>
#include "json.hpp"

struct YouTubeVideo {
    std::string id;
    std::string title;
    std::string author;
    std::string duration_string;
    std::string view_count_string;
    int duration_seconds = 0;
};

class YouTubeAPI {
public:
    YouTubeAPI();
    ~YouTubeAPI();

    // Async search with pagination. page is 1-indexed.
    void search(const std::string& query, int page, std::function<void(const std::vector<YouTubeVideo>& results, bool finished)> callback);

    // Get direct playback URL for libmpv.
    void getStreamUrl(const std::string& video_id, int max_height, std::function<void(bool success, const std::string& url)> callback);

private:
    std::string executeCommand(const std::string& cmd);
    static std::string sanitizeShellText(const std::string& value);
    std::atomic<int> current_stream_request_id_{0};
    std::atomic<int> current_search_request_id_{0};
};

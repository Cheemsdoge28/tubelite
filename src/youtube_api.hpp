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
    std::string thumbnail_url;
    std::string view_count_string;
    int duration_seconds = 0;
};

class YouTubeAPI {
public:
    YouTubeAPI();
    ~YouTubeAPI();

    // Async search. Callback is invoked on a background thread.
    void search(const std::string& query, std::function<void(bool success, const std::vector<YouTubeVideo>& results)> callback);

    // Get direct playback URL for libmpv.
    void getStreamUrl(const std::string& video_id, std::function<void(bool success, const std::string& url)> callback);

private:
    std::string executeCommand(const std::string& cmd);
};

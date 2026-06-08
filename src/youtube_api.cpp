#include "youtube_api.hpp"
#include <iostream>
#include <memory>
#include <array>
#include <stdexcept>

// Since nlohmann/json is a single-header library, we include it here.
using json = nlohmann::json;

YouTubeAPI::YouTubeAPI() {
}

YouTubeAPI::~YouTubeAPI() {
}

std::string YouTubeAPI::executeCommand(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

void YouTubeAPI::search(const std::string& query, int page, std::function<void(bool success, const std::vector<YouTubeVideo>& results)> callback) {
    std::thread([this, query, page, callback]() {
        try {
            std::string safeQuery = query;
            for (char& c : safeQuery) {
                if (c == '"' || c == '\\' || c == '$' || c == '`') c = ' ';
            }
            
            int startIdx = (page - 1) * 15 + 1;
            int endIdx = page * 15;
            
            std::string cmd = "yt-dlp --no-check-certificate --force-ipv4 --flat-playlist --dump-json \"ytsearch" + std::to_string(endIdx) + ":" + safeQuery + "\" --playlist-start " + std::to_string(startIdx) + " --playlist-end " + std::to_string(endIdx) + " 2>> yt-dlp-error.log";
            std::string output = executeCommand(cmd);
            
            std::vector<YouTubeVideo> results;
            
            // yt-dlp outputs one JSON object per line when dumping JSON for multiple results
            size_t start_pos = 0;
            while (start_pos < output.length()) {
                size_t end_pos = output.find('\n', start_pos);
                if (end_pos == std::string::npos) {
                    end_pos = output.length();
                }
                
                std::string line = output.substr(start_pos, end_pos - start_pos);
                start_pos = end_pos + 1;
                
                if (line.empty()) continue;
                
                try {
                    auto j = json::parse(line);
                    YouTubeVideo video;
                    video.id = j.value("id", "");
                    video.title = j.value("title", "");
                    video.author = j.value("uploader", "");
                    video.duration_seconds = j.value("duration", 0);
                    
                    int m = video.duration_seconds / 60;
                    int s = video.duration_seconds % 60;
                    video.duration_string = std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
                    
                    video.thumbnail_url = j.value("thumbnail", "");
                    
                    int views = j.value("view_count", 0);
                    if (views > 1000000) {
                        video.view_count_string = std::to_string(views / 1000000) + "M views";
                    } else if (views > 1000) {
                        video.view_count_string = std::to_string(views / 1000) + "K views";
                    } else {
                        video.view_count_string = std::to_string(views) + " views";
                    }
                    
                    results.push_back(video);
                } catch (const std::exception& e) {
                    // Ignore parse errors for single lines
                    std::cerr << "JSON parse error on line: " << e.what() << std::endl;
                }
            }
            
            // Kick off asynchronous downloads of thumbnails using curl
            std::string mkdirCmd = "mkdir -p /tmp/tubelite_thumbs";
            int ret = system(mkdirCmd.c_str());
            (void)ret;
            for (auto& v : results) {
                // yt-dlp sometimes gives WebP, which stb_image doesn't support.
                // We construct the guaranteed JPEG thumbnail URL manually.
                v.thumbnail_url = "https://i.ytimg.com/vi/" + v.id + "/hqdefault.jpg";
                std::string dl = "curl -s -o /tmp/tubelite_thumbs/" + v.id + ".jpg \"" + v.thumbnail_url + "\" &";
                int ret = system(dl.c_str());
                (void)ret;
            }
            
            callback(true, results);
        } catch (...) {
            callback(false, {});
        }
    }).detach();
}

void YouTubeAPI::getStreamUrl(const std::string& video_id, int max_height, std::function<void(bool success, const std::string& url)> callback) {
    std::thread([this, video_id, max_height, callback]() {
        try {
            // Get best format that is <= max_height, or worst if not available
            std::string cmd = "yt-dlp --no-check-certificate --force-ipv4 -f \"best[height<=" + std::to_string(max_height) + "]/worst\" --get-url \"https://www.youtube.com/watch?v=" + video_id + "\" 2>> yt-dlp-error.log";
            std::string url = executeCommand(cmd);
            
            // Trim whitespace
            if (!url.empty() && url.back() == '\n') {
                url.pop_back();
            }
            if (!url.empty() && url.back() == '\r') {
                url.pop_back();
            }
            
            if (url.empty()) {
                callback(false, "");
            } else {
                callback(true, url);
            }
        } catch (...) {
            callback(false, "");
        }
    }).detach();
}

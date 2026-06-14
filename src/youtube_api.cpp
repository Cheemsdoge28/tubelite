#include "youtube_api.hpp"
#include <iostream>
#include <memory>
#include <array>
#include <stdexcept>
#include <fstream>
#include <ctime>
#include <filesystem>

// Since nlohmann/json is a single-header library, we include it here.
#include "json.hpp"
using json = nlohmann::json;

YouTubeAPI::YouTubeAPI() {
}

YouTubeAPI::~YouTubeAPI() {
}

std::string YouTubeAPI::sanitizeShellText(const std::string& value) {
    std::string safe = value;
    for (char& c : safe) {
        if (c == '"' || c == '\\' || c == '$' || c == '`' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return safe;
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

static std::string getLogPath() {
#ifdef _WIN32
    return "yt-dlp-error.log";
#else
    if (std::filesystem::exists("/roms/tools/tubelite")) {
        return "/roms/tools/tubelite/yt-dlp-error.log";
    }
    return "yt-dlp-error.log";
#endif
}

static void appendLog(const std::string& title, const std::string& body) {
    std::ofstream ofs(getLogPath(), std::ios::app);
    if (!ofs) return;
    std::time_t t = std::time(nullptr);
    ofs << "===== " << title << " =====\n";
    ofs << "Time: " << std::asctime(std::localtime(&t));
    ofs << body << "\n\n";
}

static std::string calculateUploadedAgo(const std::string& upload_date) {
    if (upload_date.length() != 8) return "";
    try {
        int year = std::stoi(upload_date.substr(0, 4));
        int month = std::stoi(upload_date.substr(4, 2));
        int day = std::stoi(upload_date.substr(6, 2));

        std::time_t t = std::time(nullptr);
        std::tm* now = std::localtime(&t);
        if (!now) return "";
        int cur_year = now->tm_year + 1900;
        int cur_month = now->tm_mon + 1;
        int cur_day = now->tm_mday;

        int diff_years = cur_year - year;
        int diff_months = cur_month - month;
        int diff_days = cur_day - day;

        if (diff_days < 0) {
            diff_months -= 1;
            diff_days += 30; // approximate month length
        }
        if (diff_months < 0) {
            diff_years -= 1;
            diff_months += 12;
        }

        if (diff_years > 0) {
            return std::to_string(diff_years) + (diff_years == 1 ? " year ago" : " years ago");
        }
        if (diff_months > 0) {
            return std::to_string(diff_months) + (diff_months == 1 ? " month ago" : " months ago");
        }
        if (diff_days > 7) {
            int weeks = diff_days / 7;
            return std::to_string(weeks) + (weeks == 1 ? " week ago" : " weeks ago");
        }
        if (diff_days > 0) {
            return std::to_string(diff_days) + (diff_days == 1 ? " day ago" : " days ago");
        }
        return "today";
    } catch (...) {
        return "";
    }
}

static std::string calculateUploadedAgoFromTimestamp(long long timestamp) {
    if (timestamp <= 0) return "";
    try {
        std::time_t now = std::time(nullptr);
        long long diff = now - timestamp;
        if (diff < 0) diff = 0;
        
        long long minutes = diff / 60;
        long long hours = minutes / 60;
        long long days = hours / 24;
        long long weeks = days / 7;
        long long months = days / 30;
        long long years = days / 365;
        
        if (years > 0) {
            return std::to_string(years) + (years == 1 ? " year ago" : " years ago");
        }
        if (months > 0) {
            return std::to_string(months) + (months == 1 ? " month ago" : " months ago");
        }
        if (weeks > 0) {
            return std::to_string(weeks) + (weeks == 1 ? " week ago" : " weeks ago");
        }
        if (days > 0) {
            return std::to_string(days) + (days == 1 ? " day ago" : " days ago");
        }
        if (hours > 0) {
            return std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
        }
        if (minutes > 0) {
            return std::to_string(minutes) + (minutes == 1 ? " minute ago" : " minutes ago");
        }
        return "today";
    } catch (...) {
        return "";
    }
}

template<typename T>
static T safeGet(const nlohmann::json& j, const std::string& key, const T& default_val) {
    if (j.contains(key) && !j[key].is_null()) {
        try {
            return j[key].get<T>();
        } catch (...) {
            return default_val;
        }
    }
    return default_val;
}

static YouTubeVideo parseVideoJson(const nlohmann::json& j) {
    YouTubeVideo video;
    video.id = safeGet<std::string>(j, "id", "");
    video.title = safeGet<std::string>(j, "title", "");
    
    std::string uploader = safeGet<std::string>(j, "uploader", "");
    if (uploader.empty()) {
        uploader = safeGet<std::string>(j, "channel", "");
    }
    video.author = uploader;
    
    video.duration_seconds = safeGet<int>(j, "duration", 0);

    int m = video.duration_seconds / 60;
    int s = video.duration_seconds % 60;
    video.duration_string = std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);

    int views = safeGet<int>(j, "view_count", 0);
    if (views > 1000000) {
        video.view_count_string = std::to_string(views / 1000000) + "M views";
    } else if (views > 1000) {
        video.view_count_string = std::to_string(views / 1000) + "K views";
    } else {
        video.view_count_string = std::to_string(views) + " views";
    }

    std::string upload_date = safeGet<std::string>(j, "upload_date", "");
    if (!upload_date.empty()) {
        video.uploaded_ago_string = calculateUploadedAgo(upload_date);
    } else {
        long long ts = safeGet<long long>(j, "timestamp", 0LL);
        if (ts > 0) {
            video.uploaded_ago_string = calculateUploadedAgoFromTimestamp(ts);
        } else {
            video.uploaded_ago_string = "";
        }
    }
    return video;
}

void YouTubeAPI::search(const std::string& query, int page,
    std::function<void(const std::vector<YouTubeVideo>& results, bool finished)> callback) {
    int req_id = ++current_search_request_id_;
    std::thread([this, query, page, callback, req_id]() {
        try {
            if (req_id != current_search_request_id_) { callback({}, true); return; }

            std::string safeQuery = sanitizeShellText(query);
            int startIdx = (page - 1) * 15 + 1;
            int endIdx   = page * 15;

            // Always use ytsearch – it's the only reliably pageable source
            // without requiring YouTube cookies. Map special "home" markers
            // to a broad trending search so the home feed always loads.
            std::string searchTerm = safeQuery;
            if (query.find("http") == 0) {
                // URL-based feed → fall back to ytsearch with broad term
                searchTerm = "trending";
            }

            std::string cmd =
                "yt-dlp --no-config --quiet --no-warnings --no-update --encoding utf-8 "
                "--no-check-certificate --force-ipv4 --no-check-formats --no-call-home "
                "--cache-dir \"build/cache\" "
                "--extractor-arg \"youtubetab:approximate_date\" "
                "--flat-playlist --dump-json \"ytsearch" + std::to_string(endIdx) + ":" + searchTerm +
                "\" --playlist-start " + std::to_string(startIdx) +
                " --playlist-end "   + std::to_string(endIdx) + " 2>/dev/null";

#ifdef _WIN32
            FILE* pipe = _popen(cmd.c_str(), "r");
#else
            FILE* pipe = popen(cmd.c_str(), "r");
#endif
            if (!pipe) {
                callback({}, true);
                return;
            }

            char buffer[4096];
            std::string current_line;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                if (req_id != current_search_request_id_) {
#ifdef _WIN32
                    _pclose(pipe);
#else
                    pclose(pipe);
#endif
                    callback({}, true);
                    return;
                }
                current_line += buffer;
                size_t pos;
                while ((pos = current_line.find('\n')) != std::string::npos) {
                    std::string line = current_line.substr(0, pos);
                    current_line.erase(0, pos + 1);
                    if (line.empty()) continue;
                    try {
                        auto j = json::parse(line);
                        YouTubeVideo video = parseVideoJson(j);
                        if (!video.id.empty()) callback({video}, false);
                    } catch (...) {}
                }
            }

            // Flush any remaining partial line
            if (!current_line.empty()) {
                try {
                    auto j = json::parse(current_line);
                    YouTubeVideo video = parseVideoJson(j);
                    if (!video.id.empty()) callback({video}, false);
                } catch (...) {}
            }

#ifdef _WIN32
            _pclose(pipe);
#else
            pclose(pipe);
#endif
            callback({}, true);
        } catch (...) {
            callback({}, true);
        }
    }).detach();
}

static std::string extractSubtitleUrl(const json& j) {
    // Helper lambda to find the best url in a language track list (preferring vtt/srt over json3)
    auto findBestSubUrl = [](const json& track_list) -> std::string {
        if (!track_list.is_array() || track_list.empty()) return "";
        
        // 1. First pass: look for vtt or srt
        for (const auto& item : track_list) {
            if (item.is_object()) {
                std::string ext = item.value("ext", "");
                if (ext == "vtt" || ext == "srt") {
                    std::string url = item.value("url", "");
                    if (!url.empty()) return url;
                }
            }
        }
        
        // 2. Second pass: fallback to any format that is not json3 (e.g. ttml, srv1, srv2, srv3)
        for (const auto& item : track_list) {
            if (item.is_object()) {
                std::string ext = item.value("ext", "");
                if (ext != "json3") {
                    std::string url = item.value("url", "");
                    if (!url.empty()) return url;
                }
            }
        }
        
        // 3. Third pass: last resort, return whatever is available
        for (const auto& item : track_list) {
            if (item.is_object()) {
                std::string url = item.value("url", "");
                if (!url.empty()) return url;
            }
        }
        return "";
    };

    std::string url = "";

    // 1. Manual English subtitles
    if (j.contains("subtitles")) {
        const auto& subs = j["subtitles"];
        if (subs.is_object() && subs.contains("en")) {
            url = findBestSubUrl(subs["en"]);
        }
    }

    // 2. Any other manual subtitles
    if (url.empty() && j.contains("subtitles")) {
        const auto& subs = j["subtitles"];
        if (subs.is_object()) {
            for (auto it = subs.begin(); it != subs.end(); ++it) {
                url = findBestSubUrl(it.value());
                if (!url.empty()) break;
            }
        }
    }

    // 3. Automatic English captions
    if (url.empty() && j.contains("automatic_captions")) {
        const auto& caps = j["automatic_captions"];
        if (caps.is_object() && caps.contains("en")) {
            url = findBestSubUrl(caps["en"]);
        }
    }

    // 4. Any other automatic captions
    if (url.empty() && j.contains("automatic_captions")) {
        const auto& caps = j["automatic_captions"];
        if (caps.is_object()) {
            for (auto it = caps.begin(); it != caps.end(); ++it) {
                url = findBestSubUrl(it.value());
                if (!url.empty()) break;
            }
        }
    }

    if (!url.empty()) {
        size_t pos = url.find("fmt=json3");
        if (pos != std::string::npos) {
            url.replace(pos, 9, "fmt=vtt");
        }
    }

    return url;
}

void YouTubeAPI::getStreamUrl(const std::string& video_id, int max_height,
    std::function<void(bool success, const std::string& url, const std::string& subtitle_url)> callback,
    bool isPreview) {

    // Use separate counters so preview prefetches and main playback
    // do NOT cancel each other.
    std::atomic<int>& id_counter = isPreview ? current_preview_request_id_
                                              : current_stream_request_id_;
    int req_id = ++id_counter;

    std::thread([this, video_id, max_height, callback, req_id, isPreview]() {
        std::atomic<int>& id_counter2 = isPreview ? current_preview_request_id_
                                                   : current_stream_request_id_;
        try {
            if (req_id != id_counter2) { callback(false, "", ""); return; }

            const std::string safeId  = sanitizeShellText(video_id);
            const std::string watchUrl = "https://www.youtube.com/watch?v=" + safeId;

            // Use muxed-only format selectors (no bestvideo+bestaudio) because
            // skip=dash,hls means DASH merging is unavailable. We want a single
            // H.264 muxed stream for reliable hardware decode on ARM.
            const std::string fmtMain =
                "best[height<=" + std::to_string(max_height) + "][vcodec^=avc1]"
                "/best[height<=" + std::to_string(max_height) + "]"
                "/best";

            const std::string cmd =
                "yt-dlp --no-config --quiet --no-warnings --no-update --encoding utf-8 "
                "--no-check-certificate --force-ipv4 --no-playlist --no-call-home --no-check-formats "
                "--youtube-skip-dash-manifest "
                "--cache-dir \"build/cache\" "
                "--extractor-args \"youtube:player_client=android;skip=dash,hls\" "
                "-f \"" + fmtMain + "\" --dump-json \"" + watchUrl + "\" 2>&1";

            std::string url;
            std::string subtitle_url;

            if (req_id == id_counter2) {
                const std::string output = executeCommand(cmd);

                if (req_id == id_counter2) {
                    std::istringstream iss(output);
                    std::string line;
                    while (std::getline(iss, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.empty()) continue;
                        try {
                            auto j = json::parse(line);
                            if (j.is_object() && j.contains("url")) {
                                url = safeGet<std::string>(j, "url", "");
                                subtitle_url = extractSubtitleUrl(j);
                                if (!url.empty()) break;
                            }
                        } catch (...) {
                            // ignore
                        }
                    }
                }
            }

            if (!url.empty()) {
                appendLog("Selected URL", url);
                appendLog("Selected Subtitle URL", subtitle_url);
            }

            if (url.empty()) {
                appendLog("yt-dlp: no URL found", "No playable URL was extracted.");
                callback(false, "", "");
            } else {
                callback(true, url, subtitle_url);
            }
        } catch (...) {
            callback(false, "", "");
        }
    }).detach();
}

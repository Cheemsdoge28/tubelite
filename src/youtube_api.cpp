#include "youtube_api.hpp"
#include <iostream>
#include <memory>
#include <array>
#include <stdexcept>
#include <fstream>
#include <ctime>

// Since nlohmann/json is a single-header library, we include it here.
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

static void appendLog(const std::string& title, const std::string& body) {
    std::ofstream ofs("yt-dlp-error.log", std::ios::app);
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

static YouTubeVideo parseVideoJson(const nlohmann::json& j) {
    YouTubeVideo video;
    video.id = j.value("id", "");
    video.title = j.value("title", "");
    video.author = j.value("uploader", j.value("channel", ""));
    video.duration_seconds = j.value("duration", 0);

    int m = video.duration_seconds / 60;
    int s = video.duration_seconds % 60;
    video.duration_string = std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);

    int views = j.value("view_count", 0);
    if (views > 1000000) {
        video.view_count_string = std::to_string(views / 1000000) + "M views";
    } else if (views > 1000) {
        video.view_count_string = std::to_string(views / 1000) + "K views";
    } else {
        video.view_count_string = std::to_string(views) + " views";
    }

    std::string upload_date = j.value("upload_date", "");
    video.uploaded_ago_string = calculateUploadedAgo(upload_date);
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
                "--no-check-certificate --force-ipv4 --no-check-formats "
                "--extractor-args \"youtube:player_client=android,ios;skip=dash,hls\" "
                "--flat-playlist --dump-json \"ytsearch" + std::to_string(endIdx) + ":" + searchTerm +
                "\" --playlist-start " + std::to_string(startIdx) +
                " --playlist-end "   + std::to_string(endIdx) + " 2>> yt-dlp-error.log";

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

void YouTubeAPI::getStreamUrl(const std::string& video_id, int max_height,
    std::function<void(bool success, const std::string& url)> callback,
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
            if (req_id != id_counter2) { callback(false, ""); return; }

            const std::string safeId  = sanitizeShellText(video_id);
            const std::string watchUrl = "https://www.youtube.com/watch?v=" + safeId;

            // Use muxed-only format selectors (no bestvideo+bestaudio) because
            // skip=dash,hls means DASH merging is unavailable. We want a single
            // H.264 muxed stream for reliable hardware decode on ARM.
            const std::string fmtMain =
                "best[height<=" + std::to_string(max_height) + "][vcodec^=avc1]"
                "/best[height<=" + std::to_string(max_height) + "]"
                "/best";

            const std::vector<std::string> commands = {
                // 1st: android client – most reliable muxed source
                "yt-dlp --no-config --quiet --no-warnings --no-update --encoding utf-8 "
                "--no-check-certificate --force-ipv4 --no-playlist "
                "--extractor-args \"youtube:player_client=android;skip=dash,hls\" "
                "-f \"" + fmtMain + "\" --get-url \"" + watchUrl + "\" 2>&1",

                // 2nd: ios client fallback
                "yt-dlp --no-config --quiet --no-warnings --no-update --encoding utf-8 "
                "--no-check-certificate --force-ipv4 --no-playlist "
                "--extractor-args \"youtube:player_client=ios;skip=dash,hls\" "
                "-f \"" + fmtMain + "\" --get-url \"" + watchUrl + "\" 2>&1",

                // 3rd: web client, no format restriction – last resort
                "yt-dlp --no-config --quiet --no-warnings --no-update --encoding utf-8 "
                "--no-check-certificate --force-ipv4 --no-playlist "
                "--get-url \"" + watchUrl + "\" 2>&1"
            };

            std::string url;
            for (const auto& cmd : commands) {
                if (req_id != id_counter2) { callback(false, ""); return; }

                const std::string output = executeCommand(cmd);

                if (req_id != id_counter2) { callback(false, ""); return; }

                appendLog(std::string("getStreamUrl cmd"), output);

                // The output may be multiple lines if yt-dlp found multiple streams.
                // Take the FIRST line that starts with http – we want the muxed video URL,
                // not a second audio-only URL that sometimes appears on line 2.
                std::istringstream iss(output);
                std::string line;
                while (std::getline(iss, line)) {
                    // Strip trailing \r
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0) {
                        url = line;
                        break;
                    }
                }

                if (!url.empty()) {
                    appendLog("Selected URL", url);
                    break;
                }
            }

            if (url.empty()) {
                appendLog("yt-dlp: no URL found", "No playable URL was extracted.");
                callback(false, "");
            } else {
                callback(true, url);
            }
        } catch (...) {
            callback(false, "");
        }
    }).detach();
}

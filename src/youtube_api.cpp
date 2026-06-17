#include "youtube_api.hpp"
#include <iostream>
#include <memory>
#include <array>
#include <stdexcept>
#include <fstream>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <condition_variable>
#include <chrono>

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

bool YouTubeAPI::fetchFromInvidious(const std::string& video_id, int max_height, std::string& out_url, VideoPlaybackMetadata& out_meta) {
    std::vector<std::string> instances = {
        "https://invidious.projectsegfau.lt",
        "https://yewtu.be",
        "https://invidious.flokinet.to"
    };
    
    for (const auto& instance : instances) {
        std::string cmd = "curl -k -s -m 3 \"" + instance + "/api/v1/videos/" + video_id + "\"";
        try {
            std::string response = executeCommand(cmd);
            if (response.empty()) continue;
            
            auto j = json::parse(response);
            if (j.is_object() && j.contains("formatStreams")) {
                std::string best_url;
                int best_height = 0;
                for (const auto& s : j["formatStreams"]) {
                    std::string res = s.value("resolution", "");
                    int w = 0, h = 0;
                    if (std::sscanf(res.c_str(), "%dx%d", &w, &h) == 2) {
                        if (h > 0 && h <= max_height) {
                            std::string container = s.value("container", "");
                            if (container == "mp4" || best_url.empty() || h > best_height) {
                                best_url = s.value("url", "");
                                best_height = h;
                            }
                        }
                    }
                }
                
                if (!best_url.empty()) {
                    out_url = best_url;
                    out_meta.description = j.value("description", "");
                    out_meta.view_count = j.value("viewCount", 0LL);
                    out_meta.like_count = j.value("likeCount", 0LL);
                    out_meta.subscriber_count = j.value("subCount", 0LL);
                    return true;
                }
            }
        } catch (...) {
            // try next
        }
    }
    return false;
}

bool YouTubeAPI::fetchFromPiped(const std::string& video_id, int max_height, std::string& out_url, VideoPlaybackMetadata& out_meta) {
    std::vector<std::string> instances = {
        "https://pipedapi.kavin.rocks",
        "https://piped-api.lunar.icu",
        "https://pipedapi.colt.top"
    };
    
    for (const auto& instance : instances) {
        std::string cmd = "curl -k -s -m 3 \"" + instance + "/streams/" + video_id + "\"";
        try {
            std::string response = executeCommand(cmd);
            if (response.empty()) continue;
            
            auto j = json::parse(response);
            if (j.is_object() && j.contains("videoStreams")) {
                std::string best_url;
                int best_height = 0;
                for (const auto& s : j["videoStreams"]) {
                    if (s.value("videoOnly", false)) continue;
                    std::string quality = s.value("quality", "");
                    int height = 0;
                    if (std::sscanf(quality.c_str(), "%dp", &height) == 1) {
                        if (height > 0 && height <= max_height) {
                            std::string codec = s.value("codec", "");
                            if (codec.find("avc1") != std::string::npos || best_url.empty() || height > best_height) {
                                best_url = s.value("url", "");
                                best_height = height;
                            }
                        }
                    }
                }
                
                if (!best_url.empty()) {
                    out_url = best_url;
                    out_meta.description = j.value("description", "");
                    out_meta.view_count = j.value("views", 0LL);
                    out_meta.like_count = j.value("likes", 0LL);
                    return true;
                }
            }
        } catch (...) {
            // try next
        }
    }
    return false;
}

// ── Parallel resolve plumbing ───────────────────────────────────────────────
// Run a shell command and capture stdout (free function so worker threads
// don't share mutable state on the API object).
static std::string execCapture(const std::string& cmd) {
    std::array<char, 256> buffer{};
    std::string result;
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif
    if (!pipe) return result;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    return result;
}

// Resolve against ONE Invidious instance. Returns true on a usable muxed URL.
static bool fetchInvidiousInstance(const std::string& instance, const std::string& video_id,
                                   int max_height, std::string& out_url, VideoPlaybackMetadata& out_meta) {
    std::string cmd = "curl -k -s -m 4 \"" + instance + "/api/v1/videos/" + video_id + "\"";
    std::string response = execCapture(cmd);
    if (response.empty()) return false;
    try {
        auto j = json::parse(response);
        if (!j.is_object() || !j.contains("formatStreams")) return false;
        std::string best_url; int best_height = 0;
        for (const auto& s : j["formatStreams"]) {
            std::string res = s.value("resolution", "");
            int w = 0, h = 0;
            if (std::sscanf(res.c_str(), "%dx%d", &w, &h) == 2 && h > 0 && h <= max_height) {
                std::string container = s.value("container", "");
                if (container == "mp4" || best_url.empty() || h > best_height) {
                    best_url = s.value("url", "");
                    best_height = h;
                }
            }
        }
        if (best_url.empty()) return false;
        out_url = best_url;
        out_meta.description      = j.value("description", "");
        out_meta.view_count       = j.value("viewCount", 0LL);
        out_meta.like_count       = j.value("likeCount", 0LL);
        out_meta.subscriber_count = j.value("subCount", 0LL);
        return true;
    } catch (...) { return false; }
}

// Resolve against ONE Piped instance. Returns true on a usable muxed URL.
static bool fetchPipedInstance(const std::string& instance, const std::string& video_id,
                               int max_height, std::string& out_url, VideoPlaybackMetadata& out_meta) {
    std::string cmd = "curl -k -s -m 4 \"" + instance + "/streams/" + video_id + "\"";
    std::string response = execCapture(cmd);
    if (response.empty()) return false;
    try {
        auto j = json::parse(response);
        if (!j.is_object() || !j.contains("videoStreams")) return false;
        std::string best_url; int best_height = 0;
        for (const auto& s : j["videoStreams"]) {
            if (s.value("videoOnly", false)) continue;
            std::string quality = s.value("quality", "");
            int height = 0;
            if (std::sscanf(quality.c_str(), "%dp", &height) == 1 && height > 0 && height <= max_height) {
                std::string codec = s.value("codec", "");
                if (codec.find("avc1") != std::string::npos || best_url.empty() || height > best_height) {
                    best_url = s.value("url", "");
                    best_height = height;
                }
            }
        }
        if (best_url.empty()) return false;
        out_url = best_url;
        out_meta.description = j.value("description", "");
        out_meta.view_count  = j.value("views", 0LL);
        out_meta.like_count  = j.value("likes", 0LL);
        return true;
    } catch (...) { return false; }
}

// Resolve via local yt-dlp. Slow and heavy (spawns a Python process), so it is
// only ever used once per request — never fanned out, never on hover.
static bool runYtDlp(const std::string& watchUrl, int max_height,
                     std::string& out_url, std::string& out_sub, VideoPlaybackMetadata& out_meta) {
    const std::string fmtMain =
        "best[height<=" + std::to_string(max_height) + "][vcodec^=avc1]"
        "/best[height<=" + std::to_string(max_height) + "]"
        "/best";
    const std::string cmd =
        "yt-dlp --no-config --quiet --no-warnings --no-update --encoding utf-8 "
        "--no-check-certificate --force-ipv4 --no-playlist --no-call-home --no-check-formats "
        "--youtube-skip-dash-manifest --socket-timeout 10 "
        "--cache-dir \"build/cache\" "
        "--extractor-args \"youtube:player_client=ios,android;skip=dash,hls\" "
        "-f \"" + fmtMain + "\" --dump-json \"" + watchUrl + "\" 2>/dev/null";
    const std::string output = execCapture(cmd);
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            if (j.is_object() && j.contains("url")) {
                out_url = safeGet<std::string>(j, "url", "");
                out_sub = extractSubtitleUrl(j);
                out_meta.like_count       = j.value("like_count", 0LL);
                out_meta.comment_count    = j.value("comment_count", 0LL);
                out_meta.view_count       = j.value("view_count", 0LL);
                out_meta.subscriber_count = j.value("channel_follower_count", 0LL);
                if (out_meta.subscriber_count == 0LL) out_meta.subscriber_count = j.value("subscriber_count", 0LL);
                out_meta.description      = j.value("description", "");
                if (!out_url.empty()) return true;
            }
        } catch (...) {}
    }
    return false;
}

static const std::vector<std::string> kInvidious = {
    "https://invidious.projectsegfau.lt", "https://yewtu.be", "https://invidious.flokinet.to"
};
static const std::vector<std::string> kPiped = {
    "https://pipedapi.kavin.rocks", "https://piped-api.lunar.icu", "https://pipedapi.colt.top"
};

// Shared result for the resolver race. The first worker to produce a usable
// URL wins; the rest finish and harmlessly discard their work. Held by a
// shared_ptr so it outlives any worker that is still in flight.
namespace {
struct ResolveRace {
    std::mutex m;
    std::condition_variable cv;
    bool   done    = false;   // a winner has been recorded
    bool   success = false;
    int    remaining = 0;     // workers still running
    std::string url;
    std::string subtitle_url;
    VideoPlaybackMetadata meta;

    void finish(bool ok, const std::string& u, const std::string& sub, const VideoPlaybackMetadata& mt) {
        std::lock_guard<std::mutex> lk(m);
        if (ok && !u.empty() && !done) {
            done = true; success = true; url = u; subtitle_url = sub; meta = mt;
        }
        --remaining;
        cv.notify_all();
    }
};
} // namespace

void YouTubeAPI::getStreamUrl(const std::string& video_id, int max_height,
    std::function<void(bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& meta)> callback,
    bool isPreview,
    const std::string& parent_focus_id) {

    int req_id = 0;
    if (isPreview) {
        if (!parent_focus_id.empty()) {
            std::lock_guard<std::mutex> lock(preview_mutex_);
            current_preview_focus_id_ = parent_focus_id;
        }
    } else {
        req_id = ++current_stream_request_id_;
    }

    std::thread([this, video_id, max_height, callback, req_id, isPreview, parent_focus_id]() {
      try {
        // Cancellation predicate: a newer request (or a different focused card
        // for previews) supersedes this one.
        auto stillWanted = [this, req_id, isPreview, parent_focus_id]() -> bool {
            if (isPreview) {
                if (parent_focus_id.empty()) return true;
                std::lock_guard<std::mutex> lock(preview_mutex_);
                return current_preview_focus_id_ == parent_focus_id;
            }
            return req_id == current_stream_request_id_;
        };

        if (!stillWanted()) { callback(false, "", "", VideoPlaybackMetadata()); return; }

        const std::string safeId   = sanitizeShellText(video_id);
        const std::string watchUrl = "https://www.youtube.com/watch?v=" + safeId;

        std::string url, subtitle_url;
        VideoPlaybackMetadata meta;

        // ── Preview path (hover/focus prefetch) ───────────────────────────────
        // Must stay cheap and cancellable: ONE process at a time, bailing the
        // moment focus moves. Never fan out — rapid scrolling would otherwise
        // launch a storm of curl/yt-dlp processes and exhaust the device.
        if (isPreview) {
            for (const auto& inst : kInvidious) {
                if (!stillWanted()) { callback(false, "", "", VideoPlaybackMetadata()); return; }
                if (fetchInvidiousInstance(inst, safeId, max_height, url, meta)) break;
            }
            if (url.empty()) {
                for (const auto& inst : kPiped) {
                    if (!stillWanted()) { callback(false, "", "", VideoPlaybackMetadata()); return; }
                    if (fetchPipedInstance(inst, safeId, max_height, url, meta)) break;
                }
            }
            // yt-dlp only as a last resort, and only if this preview is still
            // wanted — so a scroll-past bails before the heavy process spawns.
            if (url.empty() && stillWanted()) {
                runYtDlp(watchUrl, max_height, url, subtitle_url, meta);
            }
            if (!stillWanted() || url.empty()) { callback(false, "", "", VideoPlaybackMetadata()); return; }
            callback(true, url, subtitle_url, meta);
            return;
        }

        // ── Playback path (explicit, user-initiated, infrequent) ──────────────
        // Race every resolver concurrently; first usable muxed URL wins. The
        // burst of processes is bounded to a single play action.
        auto race = std::make_shared<ResolveRace>();
        auto spawn = [race](std::function<bool(std::string&, std::string&, VideoPlaybackMetadata&)> fn) {
            { std::lock_guard<std::mutex> lk(race->m); ++race->remaining; }
            std::thread([race, fn]() {
                std::string u, sub; VideoPlaybackMetadata m;
                bool ok = false;
                try { ok = fn(u, sub, m); } catch (...) { ok = false; }
                race->finish(ok, u, sub, m);
            }).detach();
        };

        for (const auto& inst : kInvidious) {
            spawn([inst, safeId, max_height](std::string& u, std::string& s, VideoPlaybackMetadata& m) {
                (void)s; return fetchInvidiousInstance(inst, safeId, max_height, u, m);
            });
        }
        for (const auto& inst : kPiped) {
            spawn([inst, safeId, max_height](std::string& u, std::string& s, VideoPlaybackMetadata& m) {
                (void)s; return fetchPipedInstance(inst, safeId, max_height, u, m);
            });
        }
        spawn([watchUrl, max_height](std::string& u, std::string& s, VideoPlaybackMetadata& m) {
            return runYtDlp(watchUrl, max_height, u, s, m);
        });

        // Wait for the first winner, or until everyone has finished — capped so
        // a stalled network can't wedge the request forever.
        bool ok = false;
        {
            std::unique_lock<std::mutex> lk(race->m);
            race->cv.wait_for(lk, std::chrono::seconds(20),
                              [&]{ return race->done || race->remaining == 0; });
            ok           = race->success;
            url          = race->url;
            subtitle_url = race->subtitle_url;
            meta         = race->meta;
        }

        if (!ok || url.empty()) {
            appendLog("resolve: no URL found", "All resolvers failed or were superseded.");
            callback(false, "", "", VideoPlaybackMetadata());
            return;
        }
        if (!stillWanted()) { callback(false, "", "", VideoPlaybackMetadata()); return; }

        appendLog("Selected URL", url);
        appendLog("Selected Subtitle URL", subtitle_url);
        callback(true, url, subtitle_url, meta);
      } catch (...) {
        callback(false, "", "", VideoPlaybackMetadata());
      }
    }).detach();
}

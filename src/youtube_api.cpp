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

void YouTubeAPI::search(const std::string& query, int page, std::function<void(bool success, const std::vector<YouTubeVideo>& results)> callback) {
    std::thread([this, query, page, callback]() {
        try {
            std::string safeQuery = sanitizeShellText(query);
            
            int startIdx = (page - 1) * 15 + 1;
            int endIdx = page * 15;
            
            std::string cmd;
            if (query.find("http") == 0) {
                cmd =
                    "yt-dlp --quiet --no-warnings --no-update --encoding utf-8 --no-check-certificate --force-ipv4 "
                    "--no-check-formats --extractor-args \"youtube:player_client=android,ios;skip=dash,hls\" "
                    "--flat-playlist --dump-json \"" + safeQuery +
                    "\" --playlist-start " + std::to_string(startIdx) +
                    " --playlist-end " + std::to_string(endIdx) + " 2>> yt-dlp-error.log";
            } else {
                cmd =
                    "yt-dlp --quiet --no-warnings --no-update --encoding utf-8 --no-check-certificate --force-ipv4 "
                    "--no-check-formats --extractor-args \"youtube:player_client=android,ios;skip=dash,hls\" "
                    "--flat-playlist --dump-json \"ytsearch" + std::to_string(endIdx) + ":" + safeQuery +
                    "\" --playlist-start " + std::to_string(startIdx) +
                    " --playlist-end " + std::to_string(endIdx) + " 2>> yt-dlp-error.log";
            }
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
            
            callback(true, results);
        } catch (...) {
            callback(false, {});
        }
    }).detach();
}

void YouTubeAPI::getStreamUrl(const std::string& video_id, int max_height, std::function<void(bool success, const std::string& url)> callback) {
    std::thread([this, video_id, max_height, callback]() {
        try {
            const std::string safeId = sanitizeShellText(video_id);
            const std::string watchUrl = "https://www.youtube.com/watch?v=" + safeId;

            const std::vector<std::string> commands = {
                "yt-dlp --quiet --no-warnings --no-update --encoding utf-8 --no-check-certificate --force-ipv4 --no-playlist "
                "--extractor-args \"youtube:player_client=android,ios;skip=dash,hls\" "
                "-f \"bestvideo[height<=" + std::to_string(max_height) + "][vcodec^=avc1]+bestaudio/best[height<=" +
                std::to_string(max_height) + "]/best\" --get-url \"" + watchUrl + "\" 2>&1",
                "yt-dlp --quiet --no-warnings --no-update --encoding utf-8 --no-check-certificate --force-ipv4 --no-playlist "
                "--extractor-args \"youtube:player_client=android,web;skip=dash,hls\" "
                "-f \"best[height<=" + std::to_string(max_height) + "]/best\" --get-url \"" + watchUrl + "\" 2>&1",
                "yt-dlp --quiet --no-warnings --no-update --encoding utf-8 --no-check-certificate --force-ipv4 --no-playlist "
                "--extractor-args \"youtube:skip=dash,hls\" "
                "--get-url \"" + watchUrl + "\" 2>&1"
            };

            std::string url;
            for (const auto& cmd : commands) {
                const std::string output = executeCommand(cmd);
                appendLog(std::string("Command: ") + cmd, output);
                size_t lineEnd = output.find_last_not_of("\r\n \t");
                if (lineEnd == std::string::npos) continue;
                size_t lineStart = output.find_last_of("\r\n", lineEnd);
                lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
                std::string candidate = output.substr(lineStart, lineEnd - lineStart + 1);
                if (candidate.rfind("http://", 0) == 0 || candidate.rfind("https://", 0) == 0) {
                    url = candidate;
                    appendLog("Selected URL", url);
                    break;
                }
            }

            if (url.empty()) {
                appendLog("yt-dlp: no URL found", "No playable URL was extracted by yt-dlp probes.");
                callback(false, "");
            } else {
                callback(true, url);
            }
        } catch (...) {
            callback(false, "");
        }
    }).detach();
}

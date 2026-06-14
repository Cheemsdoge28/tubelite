#include "storyboard.hpp"
#include "stb_image.h"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <cstring>

StoryboardManager::StoryboardManager() {
}

StoryboardManager::~StoryboardManager() {
    stop();
}

void StoryboardManager::start(const std::string& stream_url, int duration_seconds) {
    stop();
    cancel_extract_ = false;
    extract_thread_ = std::thread(&StoryboardManager::runExtraction, this, stream_url, duration_seconds);
}

static std::string getTmpDir() {
#ifdef _WIN32
    return "build/tmp";
#else
    if (std::filesystem::exists("/roms/tools/tubelite")) {
        return "/roms/tools/tubelite/build/tmp";
    }
    return "build/tmp";
#endif
}

void StoryboardManager::stop() {
    cancel_extract_ = true;
#ifdef _WIN32
    std::system("taskkill /F /IM ffmpeg.exe >NUL 2>&1");
#else
    // Kill only the ffmpeg instance that is writing to our unique tmp directory.
    // Using the specific output path avoids killing unrelated ffmpeg processes on the device.
    std::string tmpDir = getTmpDir();
    std::string killCmd = "pkill -9 -f \"" + tmpDir + "/preview_\" >/dev/null 2>&1";
    std::system(killCmd.c_str());
#endif

    if (extract_thread_.joinable()) {
        extract_thread_.join();
    }
    clearFrames();
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    last_texture_frame_idx_ = -1;
}

bool StoryboardManager::hasFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !frames_.empty();
}

void StoryboardManager::clearFrames() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
}



void StoryboardManager::runExtraction(std::string stream_url, int duration_seconds) {
    if (duration_seconds <= 0) return;

    std::string tmpDir = getTmpDir();
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(tmpDir, ec)) {
        if (entry.path().filename().string().find("preview_") == 0) {
            std::filesystem::remove(entry.path(), ec);
        }
    }

    // Dynamic interval to target ~100 frames
    int interval = std::max(1, duration_seconds / 100);

    char cmd[2048];
#ifdef _WIN32
    std::snprintf(cmd, sizeof(cmd), "ffmpeg -y -threads 1 -i \"%s\" -vf \"fps=1/%d,scale=160:90\" -q:v 6 \"%s/preview_%%03d.jpg\" >NUL 2>&1", stream_url.c_str(), interval, tmpDir.c_str());
#else
    std::snprintf(cmd, sizeof(cmd), "ffmpeg -y -threads 1 -i \"%s\" -vf \"fps=1/%d,scale=160:90\" -q:v 6 \"%s/preview_%%03d.jpg\" >/dev/null 2>&1", stream_url.c_str(), interval, tmpDir.c_str());
#endif

    int res = std::system(cmd);
    (void)res;

    for (int i = 1; !cancel_extract_; ++i) {
        char filename[512];
        std::snprintf(filename, sizeof(filename), "%s/preview_%03d.jpg", tmpDir.c_str(), i);

        if (!std::filesystem::exists(filename)) {
            break;
        }

        int w = 0, h = 0, channels = 0;
        unsigned char* data = stbi_load(filename, &w, &h, &channels, 3);
        if (data) {
            std::vector<uint16_t> pixels(w * h);
            for (int p = 0; p < w * h; ++p) {
                uint8_t r = data[p * 3 + 0];
                uint8_t g = data[p * 3 + 1];
                uint8_t b = data[p * 3 + 2];
                uint16_t r5 = (r >> 3) & 0x1F;
                uint16_t g6 = (g >> 2) & 0x3F;
                uint16_t b5 = (b >> 3) & 0x1F;
                pixels[p] = (r5 << 11) | (g6 << 5) | b5;
            }
            stbi_image_free(data);

            StoryboardFrame frame;
            frame.timestamp = (i - 0.5) * interval;
            frame.pixels = std::move(pixels);

            std::lock_guard<std::mutex> lock(mutex_);
            width_ = w;
            height_ = h;
            frames_.push_back(std::move(frame));
        }

        std::filesystem::remove(filename, ec);
    }

    // Cleanup remaining files in build/tmp
    for (const auto& entry : std::filesystem::directory_iterator(tmpDir, ec)) {
        if (entry.path().filename().string().find("preview_") == 0) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

SDL_Texture* StoryboardManager::getTexture(SDL_Renderer* renderer, double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) return nullptr;

    int best_idx = 0;
    double min_diff = std::abs(frames_[0].timestamp - seconds);
    for (size_t i = 1; i < frames_.size(); ++i) {
        double diff = std::abs(frames_[i].timestamp - seconds);
        if (diff < min_diff) {
            min_diff = diff;
            best_idx = i;
        }
    }

    if (texture_ == nullptr) {
        texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, width_, height_);
        last_texture_frame_idx_ = -1;
    }

    if (texture_ && last_texture_frame_idx_ != best_idx) {
        void* pixels = nullptr;
        int pitch = 0;
        if (SDL_LockTexture(texture_, nullptr, &pixels, &pitch) == 0) {
            const auto& frame_pixels = frames_[best_idx].pixels;
            uint8_t* dst = static_cast<uint8_t*>(pixels);
            const uint8_t* src = reinterpret_cast<const uint8_t*>(frame_pixels.data());
            int row_size = width_ * 2;
            for (int y = 0; y < height_; ++y) {
                std::memcpy(dst + y * pitch, src + y * row_size, row_size);
            }
            SDL_UnlockTexture(texture_);
            last_texture_frame_idx_ = best_idx;
        }
    }

    return texture_;
}

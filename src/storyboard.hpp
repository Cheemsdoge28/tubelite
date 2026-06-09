#pragma once
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <SDL2/SDL.h>

struct StoryboardFrame {
    double timestamp;
    std::vector<uint16_t> pixels; // RGB565 format
};

class StoryboardManager {
public:
    StoryboardManager();
    ~StoryboardManager();

    // Start extraction process for a new video
    void start(const std::string& stream_url, int duration_seconds);
    
    // Stop and clean up extraction thread and cached frames
    void stop();

    // Get the texture containing the closest frame.
    // This must be called from the main thread (thread running the SDL_Renderer).
    SDL_Texture* getTexture(SDL_Renderer* renderer, double seconds);

    // Check if storyboard has any loaded frames
    bool hasFrames() const;

private:
    void runExtraction(std::string stream_url, int duration_seconds);
    void clearFrames();

    std::vector<StoryboardFrame> frames_;
    mutable std::mutex mutex_;

    std::thread extract_thread_;
    std::atomic<bool> cancel_extract_{false};

    SDL_Texture* texture_ = nullptr;
    int last_texture_frame_idx_ = -1;
    
    int width_ = 160;
    int height_ = 90;
};

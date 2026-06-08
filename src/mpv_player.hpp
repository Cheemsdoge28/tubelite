#pragma once

#include <string>
#include <functional>
#include <mpv/client.h>

struct SDL_Window;
struct SDL_Renderer;

class MpvPlayer {
public:
    MpvPlayer();
    ~MpvPlayer();

    bool initialize(SDL_Window* window, SDL_Renderer* renderer);
    void shutdown();

    void play(const std::string& url);
    void pause();
    void resume();
    void stop();
    void setVolume(int volume);
    void seek(int seconds);
    void toggleSubtitles();

    void update(); // Pump events
    
    bool isPlaying() const { return is_playing_; }
    double getPlaybackTime() const { return playback_time_; }
    double getDuration() const { return duration_; }

private:
    mpv_handle* mpv_ = nullptr;
    bool is_playing_ = false;
    double playback_time_ = 0.0;
    double duration_ = 0.0;
    
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

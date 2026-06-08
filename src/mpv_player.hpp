#pragma once
#include <string>
#include <mpv/client.h>
#include <mpv/render.h>    // MPV_RENDER_API_TYPE_SW, MPV_RENDER_PARAM_SW_*

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

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
    void cycleSubtitleTrack();
    void setMute(bool mute);
    void setGeometry(int x, int y, int w, int h);
    void resetGeometry();
    void setSpeed(double speed);
    void adjustSpeed(double delta);
    double getSpeed() const;
    void showText(const std::string& text, int duration_ms = 1400);
    void showProgress();
    void cycleStatsOverlay();

    // Render the current video frame into the SDL scene at the configured rect.
    // Call after SDL_RenderClear and before SDL_RenderPresent.
    void render(int winWidth, int winHeight);

    // Pump mpv events. Returns true if a new video frame is ready (caller should re-render).
    bool update();

    bool   isPlaying()       const { return is_playing_; }
    double getPlaybackTime() const { return playback_time_; }
    double getDuration()     const { return duration_; }

private:
    void destroyTexture();

    mpv_handle*         mpv_        = nullptr;
    mpv_render_context* mpv_render_ = nullptr;

    // Video texture (recreated whenever the target rect size changes)
    SDL_Texture*  video_texture_ = nullptr;
    int           tex_w_         = 0;
    int           tex_h_         = 0;

    bool   is_playing_   = false;
    double playback_time_ = 0.0;
    double duration_      = 0.0;

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    int  target_x_ = 0;
    int  target_y_ = 0;
    int  target_w_ = 0;
    int  target_h_ = 0;
    bool has_custom_geometry_ = false;
};

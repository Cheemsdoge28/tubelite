#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <GLES2/gl2.h>

// MpvPlayer manages a libmpv instance and its GLES render context.
//
// Architecture:
//   SDL_CreateRenderer(opengles2) creates the EGL context on the main thread.
//   mpv_render_context_create uses THAT same EGL context (already current).
//   mpv renders directly to FBO=0 (the display framebuffer) via glViewport/scissor.
//   SDL draws 2D UI overlays after mpv — no GPU→CPU→GPU roundtrip.
#include "layer.hpp"

class MpvPlayer {
public:
    MpvPlayer()  = default;
    ~MpvPlayer() { shutdown(); }

    // Call AFTER SDL_CreateRenderer. Primes the EGL context then inits mpv.
    bool initialize(SDL_Window* window, SDL_Renderer* renderer);
    void shutdown();

    // Render the latest video frame by rendering it offscreen to texture and copying it to the screen.
    void render(int winWidth, int winHeight);

    // Pump mpv events + check for new frame. Returns true if redraw needed.
    bool update();

    // ── Playback controls ─────────────────────────────────────────────────────
    void play(const std::string& url, const std::string& subtitle_url = "");
    void pause();
    void resume();
    void stop();
    void setVolume(int volume);
    void seek(int seconds);
    void seekAbsoluteKeyframes(double seconds);
    void seekAbsoluteExact(double seconds);
    void toggleSubtitles();
    void cycleSubtitleTrack();
    void cycleAudioTrack();
    std::string getSubtitleTrackName();
    std::string getAudioTrackName();
    void setMute(bool mute);
    SDL_Texture* renderToTexture(SDL_Renderer* renderer, int w, int h);
    void destroyPreviewTexture();
    void setSpeed(double speed);
    void adjustSpeed(double delta);
    double getSpeed() const;
    void showText(const std::string& text, int duration_ms = 1400);
    void showProgress();
    void cycleStatsOverlay();

    // ── State ─────────────────────────────────────────────────────────────────
    bool   isPlaying()       const { return is_playing_; }
    double getPlaybackTime() const { return playback_time_; }
    double getDuration()     const { return duration_; }
    int64_t getPropertyInt(const std::string& name) const;
    bool checkAndClearEnded();


private:
    mpv_handle*         mpv_       = nullptr;
    mpv_render_context* mpv_gl_    = nullptr;

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    void* egl_display_{nullptr};
    void* egl_draw_{nullptr};
    void* egl_read_{nullptr};
    void* egl_context_{nullptr};

    bool   is_playing_   = false;
    double playback_time_ = 0.0;
    double duration_      = 0.0;

    Layer        video_layer_;
    bool         has_new_frame_{false};

    std::string pending_subtitle_url_;
    bool file_ended_ = false;

};

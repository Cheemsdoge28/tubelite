#pragma once
#include <string>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <GLES2/gl2.h>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

// MpvPlayer manages a libmpv instance and its GLES render context.
//
// Architecture:
//   SDL_CreateRenderer(opengles2) creates the EGL context on the main thread.
//   mpv_render_context_create uses THAT same EGL context (already current).
//   mpv renders directly to FBO=0 (the display framebuffer) via glViewport/scissor.
//   SDL draws 2D UI overlays after mpv — no GPU→CPU→GPU roundtrip.
class MpvPlayer {
public:
    MpvPlayer()  = default;
    ~MpvPlayer() { shutdown(); }

    // Call AFTER SDL_CreateRenderer. Primes the EGL context then inits mpv.
    bool initialize(SDL_Window* window, SDL_Renderer* renderer);
    void shutdown();

    // Render the latest video frame directly into the current framebuffer.
    // Internally: SDL_RenderFlush → mpv → reset viewport.
    // Call before any SDL overlays that should appear above video.
    void render(int winWidth, int winHeight);

    // Pump mpv events + check for new frame. Returns true if redraw needed.
    bool update();

    // ── Playback controls ─────────────────────────────────────────────────────
    void play(const std::string& url);
    void pause();
    void resume();
    void stop();
    void setVolume(int volume);
    void seek(int seconds);
    void seekAbsoluteKeyframes(double seconds);
    void seekAbsoluteExact(double seconds);
    void toggleSubtitles();
    void cycleSubtitleTrack();
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

private:
    mpv_handle*         mpv_       = nullptr;
    mpv_render_context* mpv_gl_    = nullptr;

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_GLContext gl_context_{nullptr};

    bool   is_playing_   = false;
    double playback_time_ = 0.0;
    double duration_      = 0.0;

    SDL_Texture* preview_tex_{nullptr};
    int          preview_tex_w_{0};
    int          preview_tex_h_{0};
};

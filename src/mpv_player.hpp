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
    bool initializeAudioOnly();
    void shutdown();

    // Render the latest video frame by rendering it offscreen to texture and copying it to the screen.
    void render(int winWidth, int winHeight);

    // Pump mpv events + check for new frame. Returns true if redraw needed.
    bool update();

    // ── Playback controls ─────────────────────────────────────────────────────
    void play(const std::string& url, const std::string& subtitle_url = "",
              const std::string& audio_url = "");
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

    // The video FBO is rendered at full window size (4:3 on RG351MP) with
    // mpv's `keepaspect=yes`, which letterboxes/pillarboxes the actual
    // video inside the texture.  When a UI element (miniplayer, preview)
    // needs to RenderCopy that texture into a destination of a different
    // aspect ratio, it MUST pass this rect as the source — otherwise the
    // black bars get stretched into the destination too.  Returns the
    // full texture rect when the video aspect matches the FBO (or when
    // unknown) so it's always safe to pass through.
    SDL_Rect getVideoRect() const;

    // Call once at the start of every frame on the rendering thread.
    // Lets renderToTexture skip an extra mpv_render pass if multiple UI elements
    // (preview card + miniplayer) consume the video in the same frame.
    void beginFrame() { ++current_frame_id_; }
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
    // For properties whose native type is double (e.g. "volume",
    // "speed").  Calling getPropertyInt on a double-native property
    // returns 0/garbage depending on mpv version because the format
    // conversion at the C-API boundary is lossy or rejected.
    double  getPropertyDouble(const std::string& name) const;
    bool checkAndClearEnded();
    // True once (then cleared) if mpv ended the current file with an ERROR
    // reason — i.e. the stream URL failed to load. Distinct from EOF.
    bool checkAndClearLoadFailed();
    void setPendingSeekPosition(double pos) { pending_seek_position_ = pos; }


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
    // Monotonic frame counter for the per-frame renderToTexture dedup guard.
    uint64_t     current_frame_id_{0};
    uint64_t     last_rendered_frame_id_{0};

    std::string pending_subtitle_url_;
    std::string pending_audio_url_;
    bool file_ended_ = false;
    bool load_failed_ = false;
    double pending_seek_position_ = -1.0;

};

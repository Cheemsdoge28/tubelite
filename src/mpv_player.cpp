#include "mpv_player.hpp"
#include <iostream>
#include <SDL2/SDL.h>
#include <cmath>

MpvPlayer::MpvPlayer() {}

MpvPlayer::~MpvPlayer() {
    shutdown();
}

bool MpvPlayer::initialize(SDL_Window* window, SDL_Renderer* renderer) {
    window_   = window;
    renderer_ = renderer;

    mpv_ = mpv_create();
    if (!mpv_) {
        std::cerr << "Failed to create mpv context" << std::endl;
        return false;
    }

    // Optimise for weak ARM hardware (RK3326 / Cortex-A35)
    mpv_set_option_string(mpv_, "hwdec",                 "no");   // SW decode; hwdec readback to CPU is slow on Mali
    mpv_set_option_string(mpv_, "profile",               "fast");
    mpv_set_option_string(mpv_, "ao",                    "alsa");
    mpv_set_option_string(mpv_, "keepaspect",            "yes");
    mpv_set_option_string(mpv_, "osc",                   "no");
    mpv_set_option_string(mpv_, "input-default-bindings","no");
    mpv_set_option_string(mpv_, "input-vo-keyboard",     "no");
    mpv_set_option_string(mpv_, "osd-level",             "0");   // OSD unsupported in SW render path
    mpv_set_option_string(mpv_, "sub-auto",              "fuzzy");
    mpv_set_option_string(mpv_, "cache",                 "yes");
    mpv_set_option_string(mpv_, "demuxer-max-bytes",     "16MiB");
    // Keep video small so SW render stays cheap on the weak CPU
    mpv_set_option_string(mpv_, "vf",                    "scale=640:480");

    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "Failed to initialize mpv" << std::endl;
        return false;
    }

    // SW render context: mpv outputs frames to a CPU pixel buffer.
    // This avoids any GL/EGL context and any DRM plane ownership conflict.
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
        {MPV_RENDER_PARAM_INVALID,  nullptr}
    };

    int err = mpv_render_context_create(&mpv_render_, mpv_, params);
    if (err < 0) {
        std::cerr << "Failed to create mpv SW render context: "
                  << mpv_error_string(err) << std::endl;
        // Non-fatal: fall back to no video rendering
        mpv_render_ = nullptr;
    }

    mpv_observe_property(mpv_, 0, "time-pos",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause",     MPV_FORMAT_FLAG);

    return true;
}

void MpvPlayer::shutdown() {
    destroyTexture();
    if (mpv_render_) {
        mpv_render_context_free(mpv_render_);
        mpv_render_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
}

void MpvPlayer::destroyTexture() {
    if (video_texture_) {
        SDL_DestroyTexture(video_texture_);
        video_texture_ = nullptr;
        tex_w_ = 0;
        tex_h_ = 0;
    }
}

// ── Playback controls ────────────────────────────────────────────────────────

void MpvPlayer::play(const std::string& url) {
    if (!mpv_) return;
    const char* cmd[] = {"loadfile", url.c_str(), NULL};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::pause() {
    if (!mpv_) return;
    int v = 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &v);
}

void MpvPlayer::resume() {
    if (!mpv_) return;
    int v = 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &v);
}

void MpvPlayer::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", NULL};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::setVolume(int volume) {
    if (!mpv_) return;
    double vol = volume;
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void MpvPlayer::seek(int seconds) {
    if (!mpv_) return;
    std::string val = std::to_string(seconds);
    const char* cmd[] = {"seek", val.c_str(), "relative", NULL};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::toggleSubtitles() {
    if (!mpv_) return;
    char* sv = mpv_get_property_string(mpv_, "sub-visibility");
    std::string cur = sv ? sv : "yes";
    if (sv) mpv_free(sv);
    mpv_set_property_string(mpv_, "sub-visibility", (cur == "yes") ? "no" : "yes");
}

void MpvPlayer::cycleSubtitleTrack() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "sub", NULL};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::setMute(bool mute) {
    if (!mpv_) return;
    int val = mute ? 1 : 0;
    mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &val);
}

void MpvPlayer::setGeometry(int x, int y, int w, int h) {
    target_x_ = x; target_y_ = y;
    target_w_ = w; target_h_ = h;
    has_custom_geometry_ = true;
}

void MpvPlayer::resetGeometry() {
    target_x_ = target_y_ = target_w_ = target_h_ = 0;
    has_custom_geometry_ = false;
    destroyTexture(); // recreate at new size next render
}

void MpvPlayer::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
}

void MpvPlayer::adjustSpeed(double delta) {
    if (!mpv_) return;
    double s = getSpeed() + delta;
    if (s < 0.25) s = 0.25;
    if (s > 2.0)  s = 2.0;
    setSpeed(s);
}

double MpvPlayer::getSpeed() const {
    if (!mpv_) return 1.0;
    double s = 1.0;
    mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &s);
    return s;
}

void MpvPlayer::showText(const std::string& text, int duration_ms) {
    if (!mpv_) return;
    // OSD not available in SW render mode; print to stderr for debug
    (void)duration_ms;
    std::cerr << "[mpv] " << text << std::endl;
}

void MpvPlayer::showProgress() {
    if (!mpv_) return;
    // no-op in SW render mode
}

void MpvPlayer::cycleStatsOverlay() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "stats-display", NULL};
    mpv_command(mpv_, cmd);
}

// ── Per-frame update (pump mpv events) ──────────────────────────────────────

bool MpvPlayer::update() {
    if (!mpv_) return false;
    bool needs_redraw = false;

    // Check if mpv has a new decoded frame ready
    if (mpv_render_) {
        uint64_t flags = mpv_render_context_update(mpv_render_);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            needs_redraw = true;
        }
    }

    while (mpv_event* ev = mpv_wait_event(mpv_, 0)) {
        if (ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* prop = static_cast<mpv_event_property*>(ev->data);
            if (!prop || !prop->name) continue;
            const std::string name = prop->name;
            if (name == "time-pos") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                    playback_time_ = *static_cast<double*>(prop->data);
                    needs_redraw = true;
                } else {
                    playback_time_ = 0.0;
                }
            } else if (name == "duration") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                    duration_ = *static_cast<double*>(prop->data);
                else
                    duration_ = 0.0;
            } else if (name == "pause") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data)
                    is_playing_ = !(*static_cast<int*>(prop->data));
                else
                    is_playing_ = false;
            }
        }
    }
    return needs_redraw;
}

// ── SW render → SDL_Texture → blit at target rect ───────────────────────────

void MpvPlayer::render(int winWidth, int winHeight) {
    if (!mpv_render_ || !renderer_) return;

    // Determine destination rect
    SDL_Rect dest;
    if (has_custom_geometry_) {
        dest = {target_x_, target_y_, target_w_, target_h_};
    } else {
        dest = {0, 0, winWidth, winHeight};
    }

    if (dest.w <= 0 || dest.h <= 0) return;

    // (Re)create texture if size changed
    if (!video_texture_ || tex_w_ != dest.w || tex_h_ != dest.h) {
        destroyTexture();
        // ARGB8888 in SDL = [B][G][R][A] in memory on little-endian ARM,
        // which matches mpv SW format "bgra".
        video_texture_ = SDL_CreateTexture(renderer_,
                                           SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           dest.w, dest.h);
        if (!video_texture_) {
            std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << std::endl;
            return;
        }
        tex_w_ = dest.w;
        tex_h_ = dest.h;
    }

    // Lock texture — pass the pixel buffer directly to mpv SW renderer
    void* pixels = nullptr;
    int   pitch  = 0;
    if (SDL_LockTexture(video_texture_, nullptr, &pixels, &pitch) != 0) return;

    int    sw_size[2]  = {dest.w, dest.h};
    size_t sw_stride   = static_cast<size_t>(pitch);
    const char* sw_fmt = "bgra";   // matches ARGB8888 on little-endian ARM

    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_SW_SIZE,    sw_size},
        {MPV_RENDER_PARAM_SW_FORMAT,  (void*)sw_fmt},
        {MPV_RENDER_PARAM_SW_STRIDE,  &sw_stride},
        {MPV_RENDER_PARAM_SW_POINTER, pixels},
        {MPV_RENDER_PARAM_INVALID,    nullptr}
    };

    int err = mpv_render_context_render(mpv_render_, rp);
    SDL_UnlockTexture(video_texture_);

    if (err < 0) {
        // Frame not ready yet — don't blit stale garbage
        return;
    }

    SDL_RenderCopy(renderer_, video_texture_, nullptr, &dest);
}

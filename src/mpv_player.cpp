#include "mpv_player.hpp"
#include <iostream>
#include <cmath>
#include <cstring>
#include <SDL2/SDL.h>

// ── Lifecycle ─────────────────────────────────────────────────────────────────

bool MpvPlayer::initialize(SDL_Window* window, SDL_Renderer* renderer) {
    window_   = window;
    renderer_ = renderer;

    // ── Prime the EGL context ────────────────────────────────────────────────
    std::cerr << "[mpv] priming SDL GL context...\n";
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderFlush(renderer_);
    std::cerr << "[mpv] SDL_GL_GetCurrentContext = "
              << reinterpret_cast<void*>(SDL_GL_GetCurrentContext()) << "\n";

    // ── Create mpv ───────────────────────────────────────────────────────────
    std::cerr << "[mpv] mpv_create...\n";
    mpv_ = mpv_create();
    if (!mpv_) {
        std::cerr << "[mpv] mpv_create failed\n";
        return false;
    }

    std::cerr << "[mpv] setting options...\n";
    mpv_set_option_string(mpv_, "hwdec",                  "no");   // "auto" probes RKVDEC and can crash
    mpv_set_option_string(mpv_, "profile",                "fast");
    mpv_set_option_string(mpv_, "ao",                     "alsa");
    mpv_set_option_string(mpv_, "keepaspect",             "yes");
    mpv_set_option_string(mpv_, "osc",                    "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard",      "no");
    mpv_set_option_string(mpv_, "osd-level",              "1");
    mpv_set_option_string(mpv_, "sub-auto",               "fuzzy");
    mpv_set_option_string(mpv_, "cache",                  "yes");
    mpv_set_option_string(mpv_, "demuxer-max-bytes",      "16MiB");

    std::cerr << "[mpv] mpv_initialize...\n";
    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "[mpv] mpv_initialize failed\n";
        return false;
    }

    // ── Create GLES render context ────────────────────────────────────────────
    std::cerr << "[mpv] building gl_params...\n";
    mpv_opengl_init_params gl_params;
    gl_params.get_proc_address = [](void*, const char* name) -> void* {
        void* fn = reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
        if (!fn)
            std::cerr << "[mpv] WARNING: SDL_GL_GetProcAddress(\"" << name << "\") = null\n";
        return fn;
    };
    gl_params.get_proc_address_ctx = nullptr;
    gl_params.extra_exts           = nullptr;

    std::cerr << "[mpv] calling mpv_render_context_create...\n";
    int adv_ctrl = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE,           const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL,   reinterpret_cast<void*>(static_cast<intptr_t>(adv_ctrl))},
        {MPV_RENDER_PARAM_INVALID,            nullptr}
    };

    int err = mpv_render_context_create(&mpv_gl_, mpv_, params);
    if (err < 0) {
        std::cerr << "[mpv] mpv_render_context_create failed: "
                  << mpv_error_string(err) << "\n"
                  << "     Video will not render (audio only).\n";
        mpv_gl_ = nullptr;
    } else {
        std::cerr << "[mpv] GLES render context OK\n";
    }

    std::cerr << "[mpv] observing properties...\n";
    mpv_observe_property(mpv_, 0, "time-pos",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause",     MPV_FORMAT_FLAG);

    std::cerr << "[mpv] initialize complete\n";
    return true;
}


void MpvPlayer::shutdown() {
    if (mpv_gl_) { mpv_render_context_free(mpv_gl_); mpv_gl_ = nullptr; }
    if (mpv_)    { mpv_terminate_destroy(mpv_);       mpv_    = nullptr; }
}

// ── Per-frame update ──────────────────────────────────────────────────────────

bool MpvPlayer::update() {
    if (!mpv_) return false;
    bool needs_redraw = false;

    if (mpv_gl_) {
        uint64_t flags = mpv_render_context_update(mpv_gl_);
        if (flags & MPV_RENDER_UPDATE_FRAME) needs_redraw = true;
    }

    while (true) {
        mpv_event* ev = mpv_wait_event(mpv_, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* prop = static_cast<mpv_event_property*>(ev->data);
            if (!prop || !prop->name) continue;

            if (std::strcmp(prop->name, "time-pos") == 0) {
                playback_time_ = (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                                 ? *static_cast<double*>(prop->data) : 0.0;
                needs_redraw = true;
            } else if (std::strcmp(prop->name, "duration") == 0) {
                duration_     = (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                                ? *static_cast<double*>(prop->data) : 0.0;
            } else if (std::strcmp(prop->name, "pause") == 0) {
                is_playing_   = (prop->format == MPV_FORMAT_FLAG && prop->data)
                                ? !(*static_cast<int*>(prop->data)) : false;
            }
        }
    }
    return needs_redraw;
}

// ── Video render: SDL_RenderFlush → mpv → restore GL state ───────────────────

void MpvPlayer::render(int winWidth, int winHeight) {
    if (!mpv_gl_) return;

    // Determine where the video goes (fullscreen or constrained thumbnail rect).
    int rx, ry, rw, rh;
    if (has_custom_geometry_) {
        rx = target_x_; ry = target_y_;
        rw = target_w_; rh = target_h_;
    } else {
        rx = 0; ry = 0; rw = winWidth; rh = winHeight;
    }
    if (rw <= 0 || rh <= 0) return;

    // Flush any pending SDL draw commands before we touch GL state directly.
    SDL_RenderFlush(renderer_);

    // GL viewport/scissor: SDL Y=0 is at the top; GL Y=0 is at the bottom.
    // Flip the rect vertically.
    int gl_x = rx;
    int gl_y = winHeight - ry - rh;
    glViewport(gl_x, gl_y, rw, rh);
    glScissor (gl_x, gl_y, rw, rh);
    glEnable(GL_SCISSOR_TEST);

    // Render mpv into FBO=0 (the real framebuffer / SDL's EGL surface).
    // flip_y=1: compensates for GL's Y-flip so the video is right-side-up.
    mpv_opengl_fbo fbo{};
    fbo.fbo = 0;
    fbo.w   = rw;        // tell mpv the target logical size
    fbo.h   = rh;
    fbo.internal_format = 0;

    int flip_y = 1;
    mpv_render_param rparams[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y,     &flip_y},
        {MPV_RENDER_PARAM_INVALID,    nullptr}
    };
    mpv_render_context_render(mpv_gl_, rparams);

    // Restore full-window viewport and disable scissor so SDL draws correctly.
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, winWidth, winHeight);
}

// ── Playback controls ─────────────────────────────────────────────────────────

void MpvPlayer::play(const std::string& url) {
    if (!mpv_) return;
    const char* cmd[] = {"loadfile", url.c_str(), nullptr};
    mpv_command(mpv_, cmd);
}
void MpvPlayer::pause() {
    if (!mpv_) return;
    int v = 1; mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &v);
}
void MpvPlayer::resume() {
    if (!mpv_) return;
    int v = 0; mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &v);
}
void MpvPlayer::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr}; mpv_command(mpv_, cmd);
}
void MpvPlayer::setVolume(int volume) {
    if (!mpv_) return;
    double v = volume; mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &v);
}
void MpvPlayer::seek(int seconds) {
    if (!mpv_) return;
    std::string s = std::to_string(seconds);
    const char* cmd[] = {"seek", s.c_str(), "relative", nullptr};
    mpv_command(mpv_, cmd);
}
void MpvPlayer::toggleSubtitles() {
    if (!mpv_) return;
    char* sv = mpv_get_property_string(mpv_, "sub-visibility");
    std::string cur = sv ? sv : "yes"; if (sv) mpv_free(sv);
    mpv_set_property_string(mpv_, "sub-visibility", cur == "yes" ? "no" : "yes");
}
void MpvPlayer::cycleSubtitleTrack() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "sub", nullptr}; mpv_command(mpv_, cmd);
}
void MpvPlayer::setMute(bool mute) {
    if (!mpv_) return;
    int v = mute ? 1 : 0; mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &v);
}
void MpvPlayer::setGeometry(int x, int y, int w, int h) {
    target_x_ = x; target_y_ = y; target_w_ = w; target_h_ = h;
    has_custom_geometry_ = true;
}
void MpvPlayer::resetGeometry() {
    target_x_ = target_y_ = target_w_ = target_h_ = 0;
    has_custom_geometry_ = false;
}
void MpvPlayer::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
}
void MpvPlayer::adjustSpeed(double delta) {
    if (!mpv_) return;
    double s = getSpeed() + delta;
    s = std::max(0.25, std::min(2.0, s));
    setSpeed(s);
}
double MpvPlayer::getSpeed() const {
    if (!mpv_) return 1.0;
    double s = 1.0; mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &s);
    return s;
}
void MpvPlayer::showText(const std::string& text, int duration_ms) {
    if (!mpv_) return;
    std::string d = std::to_string(duration_ms);
    const char* cmd[] = {"show-text", text.c_str(), d.c_str(), nullptr};
    mpv_command(mpv_, cmd);
}
void MpvPlayer::showProgress() {
    if (!mpv_) return;
    const char* cmd[] = {"show-progress", nullptr}; mpv_command(mpv_, cmd);
}
void MpvPlayer::cycleStatsOverlay() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "stats-display", nullptr}; mpv_command(mpv_, cmd);
}

#include "mpv_player.hpp"
#include <iostream>
#include <SDL2/SDL.h>
#include <cmath>
#include <GLES2/gl2.h>

MpvPlayer::MpvPlayer() {
}

MpvPlayer::~MpvPlayer() {
    shutdown();
}

bool MpvPlayer::initialize(SDL_Window* window, SDL_Renderer* renderer) {
    window_ = window;
    renderer_ = renderer;
    
    mpv_ = mpv_create();
    if (!mpv_) {
        std::cerr << "Failed to create mpv context" << std::endl;
        return false;
    }

    // Optimize for weak hardware
    mpv_set_option_string(mpv_, "hwdec", "auto");
    mpv_set_option_string(mpv_, "profile", "fast");
    mpv_set_option_string(mpv_, "ao", "alsa");
    mpv_set_option_string(mpv_, "keepaspect", "yes");
    mpv_set_option_string(mpv_, "osc", "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv_, "osd-level", "1");
    mpv_set_option_string(mpv_, "sub-auto", "fuzzy");
    mpv_set_option_string(mpv_, "cache", "yes");
    mpv_set_option_string(mpv_, "demuxer-max-bytes", "16MiB");

    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "Failed to initialize mpv" << std::endl;
        return false;
    }

    // Initialize mpv render context for GLES2
    mpv_opengl_init_params gl_init_params{
        [](void*, const char* name) -> void* {
            return (void*)SDL_GL_GetProcAddress(name);
        },
        nullptr
    };

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_API_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, (void*)(intptr_t)1},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    int err = mpv_render_context_create(&mpv_gl_, mpv_, params);
    if (err < 0) {
        std::cerr << "Failed to create mpv GL render context: " << mpv_error_string(err) << std::endl;
        return false;
    }

    mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);

    return true;
}

void MpvPlayer::shutdown() {
    if (mpv_gl_) {
        mpv_render_context_free(mpv_gl_);
        mpv_gl_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
}

void MpvPlayer::play(const std::string& url) {
    if (!mpv_) return;
    const char* cmd[] = {"loadfile", url.c_str(), NULL};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::pause() {
    if (!mpv_) return;
    int pause = 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
}

void MpvPlayer::resume() {
    if (!mpv_) return;
    int pause = 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
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
    char* sub_vis = mpv_get_property_string(mpv_, "sub-visibility");
    std::string current = sub_vis ? sub_vis : "yes";
    if (sub_vis) mpv_free(sub_vis);
    
    std::string next = (current == "yes") ? "no" : "yes";
    mpv_set_property_string(mpv_, "sub-visibility", next.c_str());
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
    target_x_ = x;
    target_y_ = y;
    target_w_ = w;
    target_h_ = h;
    has_custom_geometry_ = true;
}

void MpvPlayer::resetGeometry() {
    target_x_ = 0;
    target_y_ = 0;
    target_w_ = 0;
    target_h_ = 0;
    has_custom_geometry_ = false;
}

void MpvPlayer::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
}

void MpvPlayer::adjustSpeed(double delta) {
    if (!mpv_) return;
    double speed = getSpeed() + delta;
    if (speed < 0.25) speed = 0.25;
    if (speed > 2.0) speed = 2.0;
    setSpeed(speed);
}

double MpvPlayer::getSpeed() const {
    if (!mpv_) return 1.0;
    double speed = 1.0;
    mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
    return speed;
}

void MpvPlayer::showText(const std::string& text, int duration_ms) {
    if (!mpv_) return;
    std::string duration = std::to_string(duration_ms);
    const char* cmd[] = {"show-text", text.c_str(), duration.c_str(), NULL};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::showProgress() {
    if (!mpv_) return;
    const char* cmd[] = {"show-progress", NULL};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::cycleStatsOverlay() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "stats-display", NULL};
    mpv_command(mpv_, cmd);
}

bool MpvPlayer::update() {
    if (!mpv_) return false;
    bool needs_redraw = false;
    if (mpv_gl_) {
        uint64_t flags = mpv_render_context_update(mpv_gl_);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            needs_redraw = true;
        }
    }
    while (mpv_event* event = mpv_wait_event(mpv_, 0)) {
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* prop = static_cast<mpv_event_property*>(event->data);
            if (!prop || !prop->name) {
                continue;
            }

            const std::string name = prop->name;
            if (name == "time-pos") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                    playback_time_ = *static_cast<double*>(prop->data);
                } else {
                    playback_time_ = 0.0;
                }
            } else if (name == "duration") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                    duration_ = *static_cast<double*>(prop->data);
                } else {
                    duration_ = 0.0;
                }
            } else if (name == "pause") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    is_playing_ = !(*static_cast<int*>(prop->data));
                } else {
                    is_playing_ = false;
                }
            }
        }
    }
    return needs_redraw;
}

void MpvPlayer::render(int winWidth, int winHeight) {
    if (!mpv_gl_) return;

    int rx = 0, ry = 0, rw = winWidth, rh = winHeight;
    if (has_custom_geometry_) {
        rx = target_x_;
        ry = target_y_;
        rw = target_w_;
        rh = target_h_;
    }

    // Standard OpenGL viewport has y=0 at bottom.
    // Flip y coordinate from SDL's top-left to GL's bottom-left.
    int gl_x = rx;
    int gl_y = winHeight - (ry + rh);
    int gl_w = rw;
    int gl_h = rh;

    glViewport(gl_x, gl_y, gl_w, gl_h);
    glScissor(gl_x, gl_y, gl_w, gl_h);
    glEnable(GL_SCISSOR_TEST);

    mpv_opengl_fbo fbo{
        .fbo = 0,
        .w = winWidth,
        .h = winHeight,
        .internal_format = 0
    };

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(mpv_gl_, params);

    // Reset standard GLES states
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, winWidth, winHeight);
}

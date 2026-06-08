#include "mpv_player.hpp"
#include <iostream>
#include <SDL2/SDL.h>

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
    mpv_set_option_string(mpv_, "vo", "gpu,sdl,x11,drm");

    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "Failed to initialize mpv" << std::endl;
        return false;
    }

    mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);

    return true;
}

void MpvPlayer::shutdown() {
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

void MpvPlayer::update() {
    if (!mpv_) return;
    while (mpv_event* event = mpv_wait_event(mpv_, 0)) {
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property* prop = (mpv_event_property*)event->data;
            if (std::string(prop->name) == "time-pos" && prop->format == MPV_FORMAT_DOUBLE) {
                playback_time_ = *(double*)prop->data;
            } else if (std::string(prop->name) == "duration" && prop->format == MPV_FORMAT_DOUBLE) {
                duration_ = *(double*)prop->data;
            } else if (std::string(prop->name) == "pause" && prop->format == MPV_FORMAT_FLAG) {
                is_playing_ = !(*(int*)prop->data);
            }
        }
    }
}

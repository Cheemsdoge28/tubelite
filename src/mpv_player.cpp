#include "mpv_player.hpp"
#include <iostream>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <vector>
#include <SDL2/SDL.h>


// ── Lifecycle ─────────────────────────────────────────────────────────────────

// Resolve GL/EGL function pointers via eglGetProcAddress (dlopen, no headers needed).
// Fallback to dlsym on libGLESv2 for core functions not exported by eglGetProcAddress.
// This is thread-safe and independent of SDL's context management.
static void* gl_get_proc_addr(void* /*ctx*/, const char* name) {
    // ── eglGetProcAddress ────────────────────────────────────────────────────
    using PFN_eglGPA = void*(*)(const char*);
    static PFN_eglGPA egl_gpa = []() -> PFN_eglGPA {
        void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
        return lib ? reinterpret_cast<PFN_eglGPA>(dlsym(lib, "eglGetProcAddress")) : nullptr;
    }();

    if (egl_gpa) {
        if (void* fn = reinterpret_cast<void*>(egl_gpa(name))) return fn;
    }

    // ── dlsym fallback (core GLES2 functions) ────────────────────────────────
    static void* gles_lib = []() -> void* {
        void* lib = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_GLOBAL);
        return lib;
    }();

    void* fn = gles_lib ? dlsym(gles_lib, name) : nullptr;
    if (!fn) std::cerr << "[mpv] unresolved GL symbol: " << name << "\n";
    return fn;
}

using EGLDisplay = void*;
using EGLSurface = void*;
using EGLContext = void*;

#define EGL_DRAW 0x3059
#define EGL_READ 0x305A

typedef EGLDisplay (*PFN_eglGetCurrentDisplay)(void);
typedef EGLSurface (*PFN_eglGetCurrentSurface)(int re);
typedef EGLContext (*PFN_eglGetCurrentContext)(void);
typedef int (*PFN_eglMakeCurrent)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);

static bool restore_egl_context(void* dpy, void* draw, void* read, void* ctx) {
    static auto egl_get_current_context = []() -> PFN_eglGetCurrentContext {
        void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
        return lib ? reinterpret_cast<PFN_eglGetCurrentContext>(dlsym(lib, "eglGetCurrentContext")) : nullptr;
    }();
    static auto egl_make_current = []() -> PFN_eglMakeCurrent {
        void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
        return lib ? reinterpret_cast<PFN_eglMakeCurrent>(dlsym(lib, "eglMakeCurrent")) : nullptr;
    }();

    if (egl_get_current_context && egl_get_current_context() == ctx) {
        return true;
    }

    if (egl_make_current && dpy && ctx) {
        return egl_make_current(dpy, draw, read, ctx) != 0;
    }
    return false;
}

bool MpvPlayer::initialize(SDL_Window* window, SDL_Renderer* renderer) {
    window_   = window;
    renderer_ = renderer;

    // ── Ensure the EGL context is truly current on this thread ────────────────
    // SDL's opengles2 renderer on KMSDRM may release the EGL context after
    // SDL_RenderFlush (eglMakeCurrent(dpy, NO_SURFACE, NO_SURFACE, NO_CONTEXT)).
    // We must explicitly re-bind it before mpv probes GL functions.
    std::cerr << "[mpv] priming EGL context...\n";
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderFlush(renderer_);

    static auto egl_get_current_display = []() -> PFN_eglGetCurrentDisplay {
        void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
        return lib ? reinterpret_cast<PFN_eglGetCurrentDisplay>(dlsym(lib, "eglGetCurrentDisplay")) : nullptr;
    }();
    static auto egl_get_current_surface = []() -> PFN_eglGetCurrentSurface {
        void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
        return lib ? reinterpret_cast<PFN_eglGetCurrentSurface>(dlsym(lib, "eglGetCurrentSurface")) : nullptr;
    }();
    static auto egl_get_current_context = []() -> PFN_eglGetCurrentContext {
        void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
        return lib ? reinterpret_cast<PFN_eglGetCurrentContext>(dlsym(lib, "eglGetCurrentContext")) : nullptr;
    }();

    if (egl_get_current_display && egl_get_current_surface && egl_get_current_context) {
        egl_display_ = egl_get_current_display();
        egl_draw_    = egl_get_current_surface(EGL_DRAW);
        egl_read_    = egl_get_current_surface(EGL_READ);
        egl_context_ = egl_get_current_context();
        std::cerr << "[mpv] Saved EGL Context: " << egl_context_
                  << ", Display: " << egl_display_
                  << ", Draw: " << egl_draw_
                  << ", Read: " << egl_read_ << "\n";
        
        restore_egl_context(egl_display_, egl_draw_, egl_read_, egl_context_);
    } else {
        std::cerr << "[mpv] WARNING: no GL/EGL context query pointers found!\n";
    }

    // ── Create mpv ───────────────────────────────────────────────────────────
    std::cerr << "[mpv] mpv_create...\n";
    mpv_ = mpv_create();
    if (!mpv_) { std::cerr << "[mpv] mpv_create failed\n"; return false; }

    mpv_set_option_string(mpv_, "hwdec",                  "rkmpp,auto");
    mpv_set_option_string(mpv_, "profile",                "fast");
    mpv_set_option_string(mpv_, "ao",                     "alsa");
    mpv_set_option_string(mpv_, "audio-pitch-correction", "no");
    mpv_set_option_string(mpv_, "video-sync",             "audio");
    mpv_set_option_string(mpv_, "keepaspect",             "yes");
    mpv_set_option_string(mpv_, "osc",                    "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard",      "no");
    mpv_set_option_string(mpv_, "osd-level",              "1");
    mpv_set_option_string(mpv_, "sub-auto",               "fuzzy");
    mpv_set_option_string(mpv_, "cache",                  "yes");
    mpv_set_option_string(mpv_, "network-timeout",        "5");
    mpv_set_option_string(mpv_, "demuxer-max-bytes",      "50MiB");
    mpv_set_option_string(mpv_, "demuxer-readahead-secs", "20");
    mpv_set_option_string(mpv_, "vd-lavc-threads",        "4");
    mpv_set_option_string(mpv_, "vd-lavc-skiploopfilter", "nonkey");
    mpv_set_option_string(mpv_, "vd-lavc-fast",           "yes");
    mpv_set_option_string(mpv_, "tls-verify",             "no");
    mpv_set_option_string(mpv_, "ytdl-raw-options",       "no-check-certificate=");

    // Use included Atkinson Hyperlegible font for subtitles
    std::vector<std::string> fontDirs = {
        "res/fonts",
        "../res/fonts",
        "/roms/tools/tubelite/res/fonts",
    };
    std::string foundFontDir = "";
    for (const auto& d : fontDirs) {
        if (std::filesystem::exists(d)) {
            foundFontDir = d;
            break;
        }
    }
    if (!foundFontDir.empty()) {
        mpv_set_option_string(mpv_, "sub-fonts-dir", foundFontDir.c_str());
    }
    mpv_set_option_string(mpv_, "sub-font", "Atkinson Hyperlegible");
    mpv_set_option_string(mpv_, "sub-ass-override", "force");


    std::cerr << "[mpv] mpv_initialize...\n";
    if (mpv_initialize(mpv_) < 0) { std::cerr << "[mpv] mpv_initialize failed\n"; return false; }

    // ── Create GLES render context ────────────────────────────────────────────
    // Use eglGetProcAddress via dlopen — NOT SDL_GL_GetProcAddress.
    // SDL's wrapper can return bad pointers when called outside its render loop.
    std::cerr << "[mpv] mpv_render_context_create...\n";
    mpv_opengl_init_params gl_params;
    gl_params.get_proc_address     = gl_get_proc_addr;
    gl_params.get_proc_address_ctx = nullptr;
    gl_params.extra_exts           = nullptr;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE,           const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_params},
        {MPV_RENDER_PARAM_INVALID,            nullptr}
    };

    int err = mpv_render_context_create(&mpv_gl_, mpv_, params);
    if (err < 0) {
        std::cerr << "[mpv] mpv_render_context_create failed: "
                  << mpv_error_string(err) << " (audio only)\n";
        mpv_gl_ = nullptr;
    } else {
        std::cerr << "[mpv] GLES render context OK\n";
    }

    mpv_observe_property(mpv_, 0, "time-pos",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause",     MPV_FORMAT_FLAG);

    std::cerr << "[mpv] initialize complete\n";
    return true;
}



void MpvPlayer::shutdown() {
    restore_egl_context(egl_display_, egl_draw_, egl_read_, egl_context_);
    video_layer_.destroy();
    if (mpv_gl_) {
        restore_egl_context(egl_display_, egl_draw_, egl_read_, egl_context_);
        mpv_render_context_free(mpv_gl_);
        mpv_gl_ = nullptr;
    }
    if (mpv_)    { mpv_terminate_destroy(mpv_);       mpv_    = nullptr; }
}

// ── Per-frame update ──────────────────────────────────────────────────────────

bool MpvPlayer::update() {
    if (!mpv_) return false;
    bool needs_redraw = false;

    if (mpv_gl_) {
        void* old_display = nullptr;
        void* old_draw = nullptr;
        void* old_read = nullptr;
        void* old_context = nullptr;

        static auto egl_get_current_display = []() -> PFN_eglGetCurrentDisplay {
            void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
            if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
            return lib ? reinterpret_cast<PFN_eglGetCurrentDisplay>(dlsym(lib, "eglGetCurrentDisplay")) : nullptr;
        }();
        static auto egl_get_current_surface = []() -> PFN_eglGetCurrentSurface {
            void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
            if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
            return lib ? reinterpret_cast<PFN_eglGetCurrentSurface>(dlsym(lib, "eglGetCurrentSurface")) : nullptr;
        }();
        static auto egl_get_current_context = []() -> PFN_eglGetCurrentContext {
            void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
            if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
            return lib ? reinterpret_cast<PFN_eglGetCurrentContext>(dlsym(lib, "eglGetCurrentContext")) : nullptr;
        }();

        if (egl_get_current_display && egl_get_current_surface && egl_get_current_context) {
            old_display = egl_get_current_display();
            old_draw    = egl_get_current_surface(EGL_DRAW);
            old_read    = egl_get_current_surface(EGL_READ);
            old_context = egl_get_current_context();
        }

        restore_egl_context(egl_display_, egl_draw_, egl_read_, egl_context_);
        uint64_t flags = mpv_render_context_update(mpv_gl_);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            needs_redraw = true;
            has_new_frame_ = true;
        }

        if (old_display && old_context) {
            restore_egl_context(old_display, old_draw, old_read, old_context);
        }
    }

    while (true) {
        mpv_event* ev = mpv_wait_event(mpv_, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            if (!pending_subtitle_url_.empty()) {
                const char* subCmd[] = {"sub-add", pending_subtitle_url_.c_str(), "select", nullptr};
                mpv_command_async(mpv_, 0, subCmd);
                pending_subtitle_url_.clear();
            }
        }

        if (ev->event_id == MPV_EVENT_END_FILE) {
            auto* end_file = static_cast<mpv_event_end_file*>(ev->data);
            if (end_file && end_file->reason == MPV_END_FILE_REASON_EOF) {
                file_ended_ = true;
            }
        }

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
    if (winWidth <= 0 || winHeight <= 0) return;

    SDL_Texture* tex = renderToTexture(renderer_, winWidth, winHeight);
    if (tex) {
        SDL_RenderCopy(renderer_, tex, nullptr, nullptr);
    }
}

SDL_Texture* MpvPlayer::renderToTexture(SDL_Renderer* renderer, int w, int h) {
    if (!mpv_gl_) return nullptr;
    if (w <= 0 || h <= 0) return nullptr;

    bool texture_just_recreated = false;
    if (!video_layer_.getTexture() || video_layer_.getWidth() != w || video_layer_.getHeight() != h) {
        video_layer_.init(renderer, w, h, {0, 0, w, h});
        texture_just_recreated = true;
    }

    if (!has_new_frame_ && !texture_just_recreated) {
        return video_layer_.getTexture();
    }
    has_new_frame_ = false;

    video_layer_.renderGLES(renderer, egl_display_, egl_draw_, egl_read_, egl_context_, [this, w, h](unsigned int fbo) {
        mpv_opengl_fbo mpv_fbo{};
        mpv_fbo.fbo = fbo;
        mpv_fbo.w   = w;
        mpv_fbo.h   = h;
        mpv_fbo.internal_format = GL_RGBA;

        int flip_y = 0; // standard EGL texture alignment inside SDL
        mpv_render_param rparams[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
            {MPV_RENDER_PARAM_FLIP_Y,     &flip_y},
            {MPV_RENDER_PARAM_INVALID,    nullptr}
        };
        mpv_render_context_render(mpv_gl_, rparams);
    });

    return video_layer_.getTexture();
}

void MpvPlayer::destroyPreviewTexture() {
    video_layer_.destroy();
}

// ── Playback controls ─────────────────────────────────────────────────────────

void MpvPlayer::play(const std::string& url, const std::string& subtitle_url) {
    if (!mpv_) return;
    file_ended_ = false;
    has_new_frame_ = true;
    restore_egl_context(egl_display_, egl_draw_, egl_read_, egl_context_);
    pending_subtitle_url_ = subtitle_url;
    const char* cmd[] = {"loadfile", url.c_str(), nullptr};
    mpv_command_async(mpv_, 0, cmd);
    resume();
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
    restore_egl_context(egl_display_, egl_draw_, egl_read_, egl_context_);
    pending_subtitle_url_.clear();
    const char* cmdStop[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, cmdStop);
    const char* cmdClear[] = {"playlist-clear", nullptr};
    mpv_command_async(mpv_, 0, cmdClear);
}
void MpvPlayer::setVolume(int volume) {
    if (!mpv_) return;
    double v = volume; mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &v);
}
void MpvPlayer::seek(int seconds) {
    if (!mpv_) return;
    has_new_frame_ = true;
    std::string s = std::to_string(seconds);
    const char* cmd[] = {"seek", s.c_str(), "relative", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
void MpvPlayer::seekAbsoluteKeyframes(double seconds) {
    if (!mpv_) return;
    has_new_frame_ = true;
    std::string s = std::to_string(seconds);
    const char* cmd[] = {"seek", s.c_str(), "absolute", "keyframes", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
void MpvPlayer::seekAbsoluteExact(double seconds) {
    if (!mpv_) return;
    has_new_frame_ = true;
    std::string s = std::to_string(seconds);
    const char* cmd[] = {"seek", s.c_str(), "absolute", "exact", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
void MpvPlayer::toggleSubtitles() {
    if (!mpv_) return;
    char* sv = mpv_get_property_string(mpv_, "sub-visibility");
    std::string cur = sv ? sv : "yes"; if (sv) mpv_free(sv);
    mpv_set_property_string(mpv_, "sub-visibility", cur == "yes" ? "no" : "yes");
}
void MpvPlayer::cycleSubtitleTrack() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "sub", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
void MpvPlayer::cycleAudioTrack() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "audio", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
void MpvPlayer::setMute(bool mute) {
    if (!mpv_) return;
    int v = mute ? 1 : 0; mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &v);
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
int64_t MpvPlayer::getPropertyInt(const std::string& name) const {
    if (!mpv_) return 0;
    int64_t val = 0;
    mpv_get_property(mpv_, name.c_str(), MPV_FORMAT_INT64, &val);
    return val;
}
void MpvPlayer::showText(const std::string& text, int duration_ms) {
    if (!mpv_) return;
    std::string d = std::to_string(duration_ms);
    const char* cmd[] = {"show-text", text.c_str(), d.c_str(), nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
void MpvPlayer::showProgress() {
    if (!mpv_) return;
    const char* cmd[] = {"show-progress", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}
void MpvPlayer::cycleStatsOverlay() {
    if (!mpv_) return;
    const char* cmd[] = {"cycle", "stats-display", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}

std::string MpvPlayer::getSubtitleTrackName() {
    if (!mpv_) return "None";
    char* sid = mpv_get_property_string(mpv_, "sid");
    if (!sid) return "None";
    std::string s(sid);
    mpv_free(sid);
    if (s == "no" || s.empty()) return "None";
    
    int64_t count = 0;
    mpv_get_property(mpv_, "track-list/count", MPV_FORMAT_INT64, &count);
    for (int64_t i = 0; i < count; ++i) {
        char* type = mpv_get_property_string(mpv_, ("track-list/" + std::to_string(i) + "/type").c_str());
        if (type) {
            std::string t(type);
            mpv_free(type);
            if (t == "sub") {
                int64_t id = 0;
                mpv_get_property(mpv_, ("track-list/" + std::to_string(i) + "/id").c_str(), MPV_FORMAT_INT64, &id);
                if (std::to_string(id) == s) {
                    char* lang = mpv_get_property_string(mpv_, ("track-list/" + std::to_string(i) + "/lang").c_str());
                    char* title = mpv_get_property_string(mpv_, ("track-list/" + std::to_string(i) + "/title").c_str());
                    std::string res = "Track " + s;
                    if (lang && strlen(lang) > 0) {
                        res += " [" + std::string(lang) + "]";
                    }
                    if (title && strlen(title) > 0) {
                        res += " (" + std::string(title) + ")";
                    }
                    if (lang) mpv_free(lang);
                    if (title) mpv_free(title);
                    return res;
                }
            }
        }
    }
    return "Track " + s;
}

std::string MpvPlayer::getAudioTrackName() {
    if (!mpv_) return "None";
    char* aid = mpv_get_property_string(mpv_, "aid");
    if (!aid) return "None";
    std::string a(aid);
    mpv_free(aid);
    if (a == "no" || a.empty()) return "None";
    
    int64_t count = 0;
    mpv_get_property(mpv_, "track-list/count", MPV_FORMAT_INT64, &count);
    for (int64_t i = 0; i < count; ++i) {
        char* type = mpv_get_property_string(mpv_, ("track-list/" + std::to_string(i) + "/type").c_str());
        if (type) {
            std::string t(type);
            mpv_free(type);
            if (t == "audio") {
                int64_t id = 0;
                mpv_get_property(mpv_, ("track-list/" + std::to_string(i) + "/id").c_str(), MPV_FORMAT_INT64, &id);
                if (std::to_string(id) == a) {
                    char* lang = mpv_get_property_string(mpv_, ("track-list/" + std::to_string(i) + "/lang").c_str());
                    char* title = mpv_get_property_string(mpv_, ("track-list/" + std::to_string(i) + "/title").c_str());
                    std::string res = "Track " + a;
                    if (lang && strlen(lang) > 0) {
                        res += " [" + std::string(lang) + "]";
                    }
                    if (title && strlen(title) > 0) {
                        res += " (" + std::string(title) + ")";
                    }
                    if (lang) mpv_free(lang);
                    if (title) mpv_free(title);
                    return res;
                }
            }
        }
    }
    return "Track " + a;
}

bool MpvPlayer::checkAndClearEnded() {
    bool ended = file_ended_;
    file_ended_ = false;
    return ended;
}

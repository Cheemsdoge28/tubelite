#include "mpv_player.hpp"
#include "profiler.hpp"
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
    // Route through ALSA's `plug:dmix` so the codec is shared, not
    // grabbed exclusively.  Background:
    //   * `default` and `dmix:CARD=...` both end up opening the
    //     RK817 hw exclusively on this device (verified via
    //     `fuser -v /dev/snd/*` showing `F...m` mmap, and via
    //     `speaker-test -D default` returning -16 EBUSY while
    //     tubelite is running).
    //   * Raw `dmix` fails too: `speaker-test -D dmix` returns
    //     "Sample format not available" because the RK817 doesn't
    //     advertise dmix's default S16_LE@48000 natively.
    //   * `plug:dmix` interposes a format/rate conversion layer
    //     in front of dmix.  This was confirmed working on this
    //     hardware: `speaker-test -D plug:dmix` plays, two
    //     simultaneous `speaker-test -D plug:dmix` invocations
    //     play together, and RetroArch with
    //     `audio_device = "plug:dmix"` coexists with another
    //     `plug:dmix` client.
    // We set this BEFORE mpv_initialize because mpv applies
    // audio-device at AO-open time, and pre-init the option
    // string is the lowest-friction path (no need to probe
    // audio-device-list afterwards).  The literal `plug:dmix`
    // has no nested colon, so ALSA's PCM-name parser accepts it
    // (unlike the `plug:dmix:CARD=...` form we tried earlier).
    mpv_set_option_string(mpv_, "audio-device",           "alsa/plug:dmix");
    // Color-correctness fix for the rkmpp → GL pipeline on RK3326.
    //
    // YouTube videos are encoded with BT.709 primaries and a LIMITED
    // (16-235) range.  Without `video-output-levels=full`, mpv ships
    // limited-range RGB to the GL_RGBA FBO; SDL's RenderCopy sampler
    // then treats those bytes as full-range and the result is the
    // classic "washed out / low-contrast / wrong skin tones" YouTube
    // look.  Forcing full-range output makes mpv apply the 16→0, 235→255
    // remap before writing, so the texture is colorimetrically correct
    // by the time SDL stretches it to the display.
    mpv_set_option_string(mpv_, "video-output-levels",    "full");
    // Dither would normally cover the small precision loss from the
    // range conversion, but profile=fast already disabled it.  At 360p
    // on an LCD handheld the banding is not visible — keep it off for
    // perf.
    mpv_set_option_string(mpv_, "audio-pitch-correction", "no");
    mpv_set_option_string(mpv_, "video-sync",             "audio");
    mpv_set_option_string(mpv_, "keepaspect",             "yes");
    mpv_set_option_string(mpv_, "osc",                    "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard",      "no");
    mpv_set_option_string(mpv_, "osd-level",              "1");
    // Push mpv's OSD messages (our playback toasts: Volume, Paused,
    // Speed, screen-off prompt, …) DOWN into the lower-middle of the
    // frame.  The default top-left position sat directly under our
    // HUD's title/stats bar, which composites on top of mpv's FBO and
    // obscured the toast.  Bottom-aligned with a large margin clears
    // the top chrome while staying above the bottom hint pill row.
    mpv_set_option_string(mpv_, "osd-align-x",            "left");
    mpv_set_option_string(mpv_, "osd-align-y",            "top");
    mpv_set_option_string(mpv_, "osd-margin-y",           "60");
    mpv_set_option_string(mpv_, "sub-auto",               "fuzzy");
    mpv_set_option_string(mpv_, "cache",                  "yes");
    mpv_set_option_string(mpv_, "network-timeout",        "5");
    // Trimmed from 50MiB/20s: the device was memory-pressured (kswapd/mmcqd
    // active, ~235MB RSS). A 32MiB / 12s readahead is plenty for 360p and
    // meaningfully reduces RAM pressure and swapping.
    mpv_set_option_string(mpv_, "demuxer-max-bytes",      "32MiB");
    mpv_set_option_string(mpv_, "demuxer-readahead-secs", "12");
    mpv_set_option_string(mpv_, "vd-lavc-threads",        "4");
    mpv_set_option_string(mpv_, "vd-lavc-skiploopfilter", "nonkey");
    mpv_set_option_string(mpv_, "vd-lavc-fast",           "yes");
    mpv_set_option_string(mpv_, "tls-verify",             "no");
    mpv_set_option_string(mpv_, "user-agent",             "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
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

    // PERF: disable libass's system font provider (fontconfig).  Atkinson is a
    // Latin-only font; the instant a subtitle contains a glyph it lacks (a
    // non-Latin char, an auto-caption artifact, a smart quote, emoji) libass
    // asks fontconfig to find a fallback font that HAS the glyph — which scans
    // font files on the SD card, per missing glyph.  On RK3326 with a cold
    // fontconfig cache that scan is the source of the subtitle lag, and the
    // tofu box (□) is the visible symptom of the same missing-glyph event.
    // "none" makes libass use ONLY our bundled fonts dir + any embedded fonts,
    // so Latin subtitles still render via Atkinson with zero scanning; non-Latin
    // shows boxes but no longer stalls the device.  (If we ever want real
    // multilingual coverage, bundle a glyph-complete font like GO Noto Universal
    // into res/fonts and set it as sub-font instead — but it's ~megabytes.)
    mpv_set_option_string(mpv_, "sub-font-provider", "none");


    std::cerr << "[mpv] mpv_initialize...\n";
    if (mpv_initialize(mpv_) < 0) { std::cerr << "[mpv] mpv_initialize failed\n"; return false; }

    // (audio-device is set BEFORE mpv_initialize — to `alsa/plug:dmix`
    // — so no post-init device picking is needed here.  The dynamic
    // picker that used to live here selected `alsa/dmix:CARD=...`,
    // which failed `snd_pcm_dmix_open: unable to open slave` on this
    // codec.  `plug:dmix` is the only PCM name that opens cleanly AND
    // shares the device with other clients.)
    //
    // Diagnostic: read back the *actual* audio-device and AO mpv ended
    // up using.  If mpv silently falls back (e.g. plug:dmix open fails
    // and mpv reopens on a different device), this is the only signal
    // we have without an mpv IPC socket.  Cheap one-time call.
    {
        char* dev = nullptr;
        char* ao  = nullptr;
        if (mpv_get_property(mpv_, "audio-device",
                             MPV_FORMAT_STRING, &dev) >= 0 && dev) {
            std::cerr << "[mpv] audio-device (requested): " << dev << "\n";
            mpv_free(dev);
        }
        if (mpv_get_property(mpv_, "current-ao",
                             MPV_FORMAT_STRING, &ao) >= 0 && ao) {
            std::cerr << "[mpv] current-ao: " << ao << "\n";
            mpv_free(ao);
        }
    }

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

bool MpvPlayer::initializeAudioOnly() {
    std::cerr << "[mpv] mpv_create (audio only)...\n";
    mpv_ = mpv_create();
    if (!mpv_) { std::cerr << "[mpv] mpv_create failed\n"; return false; }

    mpv_set_option_string(mpv_, "video",                  "no");
    // Use ALSA so the daemon shares the audio device with other apps.
    mpv_set_option_string(mpv_, "ao",                     "alsa");
    // Same `plug:dmix` rationale as the foreground initialize() path
    // — see the long comment there.  Without this, the daemon would
    // grab the codec exclusively (`F...m` mmap) and block both
    // tubelite-foreground and RetroArch from playing audio.
    mpv_set_option_string(mpv_, "audio-device",           "alsa/plug:dmix");
    // Buffer slightly larger to smooth over scheduling jitter in a background process
    mpv_set_option_string(mpv_, "audio-buffer",           "0.5");
    mpv_set_option_string(mpv_, "gapless-audio",          "no");
    mpv_set_option_string(mpv_, "audio-pitch-correction", "no");
    mpv_set_option_string(mpv_, "osc",                    "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard",      "no");
    mpv_set_option_string(mpv_, "cache",                  "yes");
    mpv_set_option_string(mpv_, "network-timeout",        "5");
    mpv_set_option_string(mpv_, "demuxer-max-bytes",      "50MiB");
    mpv_set_option_string(mpv_, "demuxer-readahead-secs", "20");
    mpv_set_option_string(mpv_, "user-agent",             "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    mpv_set_option_string(mpv_, "ytdl-raw-options",       "no-check-certificate=");

    std::cerr << "[mpv] mpv_initialize...\n";
    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "[mpv] mpv_initialize failed\n";
        return false;
    }

    // (audio-device is set BEFORE mpv_initialize — to `alsa/plug:dmix`
    // — so no post-init device picking is needed here.  The dynamic
    // picker that used to live here selected `alsa/dmix:CARD=...`,
    // which failed `snd_pcm_dmix_open: unable to open slave` on this
    // codec.  `plug:dmix` is the only PCM name that opens cleanly AND
    // shares the device with other clients.)
    //
    // Diagnostic: read back the *actual* audio-device and AO mpv ended
    // up using.  If mpv silently falls back (e.g. plug:dmix open fails
    // and mpv reopens on a different device), this is the only signal
    // we have without an mpv IPC socket.  Cheap one-time call.
    {
        char* dev = nullptr;
        char* ao  = nullptr;
        if (mpv_get_property(mpv_, "audio-device",
                             MPV_FORMAT_STRING, &dev) >= 0 && dev) {
            std::cerr << "[mpv] audio-device (requested): " << dev << "\n";
            mpv_free(dev);
        }
        if (mpv_get_property(mpv_, "current-ao",
                             MPV_FORMAT_STRING, &ao) >= 0 && ao) {
            std::cerr << "[mpv] current-ao: " << ao << "\n";
            mpv_free(ao);
        }
    }

    mpv_observe_property(mpv_, 0, "time-pos",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration",  MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause",     MPV_FORMAT_FLAG);

    std::cerr << "[mpv] audio-only initialize complete\n";
    is_playing_ = true;
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
        // mpv_render_context_update is documented thread-safe and does NOT
        // require a current GL/EGL context.  The full EGL save/restore round
        // trip that used to wrap this call (4 eglGetCurrent* + 2 eglMakeCurrent
        // per frame) was pure overhead — on the RK3326 it accounted for a
        // meaningful slice of idle-frame CPU.  Just poll the flags directly.
        uint64_t flags = mpv_render_context_update(mpv_gl_);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            needs_redraw = true;
            has_new_frame_ = true;
        }
    }

    while (true) {
        mpv_event* ev = mpv_wait_event(mpv_, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            if (!pending_audio_url_.empty()) {
                // DASH adaptive: the main file is the video-only track; overlay
                // the separate audio track. "select" makes it the active audio
                // stream immediately.
                const char* audioCmd[] = {"audio-add", pending_audio_url_.c_str(), "select", nullptr};
                mpv_command_async(mpv_, 0, audioCmd);
                pending_audio_url_.clear();
            }
            if (!pending_subtitle_url_.empty()) {
                const char* subCmd[] = {"sub-add", pending_subtitle_url_.c_str(), "select", nullptr};
                mpv_command_async(mpv_, 0, subCmd);
                pending_subtitle_url_.clear();
            }
            if (pending_seek_position_ >= 0.0) {
                seekAbsoluteExact(pending_seek_position_);
                pending_seek_position_ = -1.0;
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
        // For fullscreen playback the destination matches the FBO aspect
        // (both 4:3 on this device), so passing the whole texture and
        // letting SDL stretch to whole render target is correct — the
        // letterbox bars from mpv land exactly on the screen's bars.
        SDL_RenderCopy(renderer_, tex, nullptr, nullptr);
    }
}

SDL_Rect MpvPlayer::getVideoRect() const {
    // Default: whole texture, conservatively safe (no cropping).  Used
    // when mpv isn't loaded or we can't read the video params.
    int winW = 0, winH = 0;
    if (window_) SDL_GetWindowSize(window_, &winW, &winH);
    if (winW <= 0 || winH <= 0) return SDL_Rect{0, 0, 1, 1};
    SDL_Rect full{0, 0, winW, winH};
    if (!mpv_) return full;

    // mpv exposes the on-screen video dimensions via dwidth/dheight
    // (display width after SAR/PAR correction).  When keepaspect=yes and
    // the FBO doesn't match the video aspect, mpv centers the video and
    // pads with black on the two opposite sides.  Compute that inner
    // rect so callers can pass it as the source to RenderCopy.
    int64_t dw = 0, dh = 0;
    if (mpv_get_property(const_cast<mpv_handle*>(mpv_), "dwidth",
                         MPV_FORMAT_INT64, &dw) < 0 || dw <= 0) return full;
    if (mpv_get_property(const_cast<mpv_handle*>(mpv_), "dheight",
                         MPV_FORMAT_INT64, &dh) < 0 || dh <= 0) return full;

    const double videoAspect = static_cast<double>(dw) / static_cast<double>(dh);
    const double fboAspect   = static_cast<double>(winW) / static_cast<double>(winH);

    // Within ~1% just use the whole texture — avoids 1-px rounding cracks.
    if (std::abs(videoAspect - fboAspect) / fboAspect < 0.01) return full;

    if (videoAspect > fboAspect) {
        // Video wider than FBO — letterboxed (black bars top + bottom).
        const int innerH = static_cast<int>(winW / videoAspect + 0.5);
        const int innerY = (winH - innerH) / 2;
        return SDL_Rect{0, innerY, winW, innerH};
    } else {
        // Video taller than FBO — pillarboxed (black bars left + right).
        const int innerW = static_cast<int>(winH * videoAspect + 0.5);
        const int innerX = (winW - innerW) / 2;
        return SDL_Rect{innerX, 0, innerW, winH};
    }
}

SDL_Texture* MpvPlayer::renderToTexture(SDL_Renderer* renderer, int w, int h) {
    PROFILE_SCOPE("mpv_renderToTexture");
    if (!mpv_gl_) return nullptr;
    if (w <= 0 || h <= 0) return nullptr;

    // Per-frame dedup: if multiple UI elements (preview card + miniplayer)
    // both consume the video in the same frame, only the FIRST call drives a
    // new mpv render — subsequent callers re-use whatever was just produced.
    if (last_rendered_frame_id_ == current_frame_id_ && video_layer_.getTexture()) {
        PROFILE_COUNT("mpv_render_dedup_hit");
        return video_layer_.getTexture();
    }

    // RENDER AT FULL WINDOW SIZE, ALWAYS.  This was the 04fe884 fix for the
    // continuous Mali-G31 flicker.  Reverting it (to "honour caller w/h" for
    // perf) re-introduced the regression — every renderToTexture call from
    // a different caller (preview 160×90, miniplayer 240×135, fullscreen
    // 640×480) thrashes the FBO each frame.  On a tile-based GPU each FBO
    // resize forces a tile flush which is visible as flicker.
    //
    // The fragment-work cost of rendering 640×480 for a 240×135 preview is
    // real but bounded (~12× more pixels).  Callers do the downscale on the
    // *SDL blit* (theme::RenderCopy stretch), which is a single textured
    // quad — cheap on Mali-G31 vs. another mpv shader pass.
    int targetW = w;
    int targetH = h;
    if (window_) {
        int winW = 0, winH = 0;
        SDL_GetWindowSize(window_, &winW, &winH);
        if (winW > 0 && winH > 0) {
            targetW = winW;
            targetH = winH;
        }
    }

    bool texture_just_recreated = false;
    if (!video_layer_.getTexture() || video_layer_.getWidth() != targetW || video_layer_.getHeight() != targetH) {
        video_layer_.init(renderer, targetW, targetH, {0, 0, targetW, targetH});
        texture_just_recreated = true;
    }

    if (!has_new_frame_ && !texture_just_recreated) {
        PROFILE_COUNT("mpv_render_no_new_frame");
        last_rendered_frame_id_ = current_frame_id_;
        return video_layer_.getTexture();
    }
    has_new_frame_ = false;
    last_rendered_frame_id_ = current_frame_id_;
    PROFILE_SCOPE("mpv_renderGLES");

    video_layer_.renderGLES(renderer, egl_display_, egl_draw_, egl_read_, egl_context_, [this, targetW, targetH](unsigned int fbo) {
        mpv_opengl_fbo mpv_fbo{};
        mpv_fbo.fbo = fbo;
        mpv_fbo.w   = targetW;
        mpv_fbo.h   = targetH;
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

void MpvPlayer::play(const std::string& url, const std::string& subtitle_url,
                     const std::string& audio_url) {
    if (!mpv_) return;
    file_ended_ = false;
    has_new_frame_ = true;
    pending_seek_position_ = -1.0;          // stale seek from a prior file → drop
    restore_egl_context(egl_display_, egl_draw_, egl_read_, egl_context_);
    pending_subtitle_url_ = subtitle_url;
    pending_audio_url_    = audio_url;

    // Explicit synchronous stop BEFORE loadfile so the previous file's
    // demuxer cache (up to 32 MiB readahead), decoder state, hwdec
    // surfaces and audio filter chain are torn down deterministically
    // BEFORE the new file allocates its own.  Without this, on
    // back-to-back track changes the old + new state are briefly both
    // resident — a real memory spike on the 640 MB device that
    // occasionally tripped the OOM killer for yt-dlp running alongside.
    // The sync stop costs ~50 ms of silence at the transition; that's
    // imperceptible against the network latency of the new loadfile.
    const char* cmdStop[] = {"stop", nullptr};
    mpv_command(mpv_, cmdStop);
    const char* cmdLoad[] = {"loadfile", url.c_str(), "replace", nullptr};
    mpv_command_async(mpv_, 0, cmdLoad);
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
    pending_audio_url_.clear();
    pending_seek_position_ = -1.0;
    // Sync stop so the demuxer cache + decoder state are gone before
    // we return — callers (track-change, app-shutdown, reabsorption)
    // rely on this for clean memory hand-off.  playlist-clear after to
    // make sure nothing auto-advances.
    const char* cmdStop[] = {"stop", nullptr};
    mpv_command(mpv_, cmdStop);
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
double MpvPlayer::getPropertyDouble(const std::string& name) const {
    if (!mpv_) return 0.0;
    double val = 0.0;
    mpv_get_property(mpv_, name.c_str(), MPV_FORMAT_DOUBLE, &val);
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

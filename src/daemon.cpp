#include "daemon.hpp"
#include "mpv_player.hpp"
#include "youtube_api.hpp"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <mutex>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <poll.h>
#include <linux/input.h>
#endif

#include <ft2build.h>
#include FT_FREETYPE_H

enum class DaemonStatus { Idle, Resolving, Playing, Paused, Error };

struct DaemonVideo {
    std::string id;
    std::string title;
    std::string author;
    int duration_seconds = 0;
    std::string duration_string;
    std::string stream_url;
    std::string subtitle_url;
};

static std::vector<DaemonVideo> daemon_playlist;
static int daemon_current_index = 0;
static double daemon_start_position = 0.0;
static std::atomic<bool> daemon_running{true};

static std::atomic<DaemonStatus> daemon_status{DaemonStatus::Idle};
static std::atomic<bool> daemon_request_finished{false};
static std::atomic<bool> daemon_request_success{false};
static std::string daemon_resolved_url;
static std::string daemon_subtitle_url;
static std::mutex daemon_resolved_mutex;

static float overlay_alpha = 0.0f;
static bool overlay_active = false;
static float daemon_overlay_timer = 0.0f;

static const int card_w = 320;
static const int card_h = 96;
static const int card_y = 12;  // vertical position on screen

static inline uint8_t fade(uint8_t a)
{
    return (uint8_t)(a * overlay_alpha);
}

#ifndef _WIN32
#define DRM_OVERLAY_PLANE_ID  61
#define DRM_CRTC_ID           60

static int       drm_fd     = -1;
static uint32_t  drm_fb_id  = 0;
static uint32_t  drm_handle = 0;
static uint32_t  drm_pitch  = 0;
static uint64_t  drm_size   = 0;
static uint32_t* drm_map    = nullptr;
static int       drm_screen_w = 640;
static int       drm_screen_h = 480;
#endif

static FT_Library ft_lib;
static FT_Face    ft_face;
static bool       ft_ok = false;

// JoystickEvent struct removed as we migrated to standard evdev input_event

static std::string getAppDataPath(const std::string& filename) {
#ifdef _WIN32
    return filename;
#else
    if (std::filesystem::exists("/roms/tools/tubelite"))
        return "/roms/tools/tubelite/" + filename;
    return filename;
#endif
}

static std::string formatTime(double s) {
    if (s < 0) s = 0;
    int tot = (int)s;
    int h = tot / 3600, m = (tot % 3600) / 60, sec = tot % 60;
    char buf[16];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else        snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

static bool loadDaemonQueue() {
    daemon_playlist.clear();
    try {
        std::ifstream ifs(getAppDataPath("daemon_queue.json"));
        if (!ifs) return false;
        nlohmann::json j;
        ifs >> j;
        if (j.contains("videos") && j["videos"].is_array()) {
            for (const auto& item : j["videos"]) {
                DaemonVideo v;
                v.id               = item.value("id", "");
                v.title            = item.value("title", "");
                v.author           = item.value("author", "");
                v.duration_seconds = item.value("duration_seconds", 0);
                v.duration_string  = item.value("duration_string", "");
                v.stream_url       = item.value("stream_url", "");
                v.subtitle_url     = item.value("subtitle_url", "");
                daemon_playlist.push_back(v);
            }
        }
        daemon_current_index  = j.value("current_index", 0);
        daemon_start_position = j.value("current_position", 0.0);
        return !daemon_playlist.empty();
    } catch (...) {
        return false;
    }
}

static void handleSignal(int sig) {
    if (sig == SIGTERM || sig == SIGINT)
        daemon_running = false;
}

// ─── DRM overlay ─────────────────────────────────────────────────────────────

#ifndef _WIN32
static bool initDrmOverlay() {
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("[daemon] open card0"); return false; }

    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    drmModeRes* res = drmModeGetResources(drm_fd);
    if (res) {
        for (int i = 0; i < res->count_crtcs; i++) {
            drmModeCrtc* crtc = drmModeGetCrtc(drm_fd, res->crtcs[i]);
            if (crtc && crtc->mode_valid && crtc->crtc_id == DRM_CRTC_ID) {
                drm_screen_w = crtc->mode.hdisplay;
                drm_screen_h = crtc->mode.vdisplay;
                drmModeFreeCrtc(crtc);
                break;
            }
            if (crtc) drmModeFreeCrtc(crtc);
        }
        drmModeFreeResources(res);
    }

    struct drm_mode_create_dumb creq = {};
    creq.width  = card_w;
    creq.height = card_h;
    creq.bpp    = 32;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("[daemon] CREATE_DUMB"); return false;
    }
    drm_handle = creq.handle;
    drm_pitch  = creq.pitch;
    drm_size   = creq.size;

    uint32_t handles[4] = { drm_handle };
    uint32_t pitches[4] = { drm_pitch  };
    uint32_t offsets[4] = { 0 };
    if (drmModeAddFB2(drm_fd, card_w, card_h,
                      DRM_FORMAT_ARGB8888,
                      handles, pitches, offsets,
                      &drm_fb_id, 0) < 0) {
        perror("[daemon] AddFB2"); return false;
    }

    struct drm_mode_map_dumb mreq = {};
    mreq.handle = drm_handle;
    drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    drm_map = (uint32_t*)mmap(nullptr, drm_size,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED, drm_fd, mreq.offset);
    if (drm_map == MAP_FAILED) { perror("[daemon] mmap drm"); return false; }

    memset(drm_map, 0, drm_size);
    std::cerr << "[daemon] DRM overlay ready. Screen: "
              << drm_screen_w << "x" << drm_screen_h << "\n";
    return true;
}

static void closeDrmOverlay() {
    if (drm_fd < 0) return;
    drmModeSetPlane(drm_fd, DRM_OVERLAY_PLANE_ID, DRM_CRTC_ID,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (drm_map && drm_map != MAP_FAILED) munmap(drm_map, drm_size);
    if (drm_fb_id)  drmModeRmFB(drm_fd, drm_fb_id);
    if (drm_handle) {
        struct drm_mode_destroy_dumb dreq = {};
        dreq.handle = drm_handle;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    close(drm_fd);
    drm_fd = -1;
}

static void commitOverlay() {
    if (drm_fd < 0) return;
    int dest_x = (drm_screen_w - card_w) / 2;
    int dest_y = card_y;
    drmModeSetPlane(drm_fd, DRM_OVERLAY_PLANE_ID, DRM_CRTC_ID,
                    drm_fb_id, 0,
                    dest_x, dest_y, card_w,       card_h,
                    0,      0,      card_w << 16, card_h << 16);
}

static void hideOverlay() {
    if (drm_fd < 0) return;
    drmModeSetPlane(drm_fd, DRM_OVERLAY_PLANE_ID, DRM_CRTC_ID,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
#else
static bool initDrmOverlay() { return true; }
static void closeDrmOverlay() {}
static void commitOverlay() {}
static void hideOverlay() {}
#endif

// ─── Drawing ─────────────────────────────────────────────────────────────────
//
// KEY FIX: drmWritePixel does NOT software-blend against the existing buffer.
// The hardware plane composites this buffer over the primary plane using the
// alpha channel. Reading back and blending inside the buffer would double-apply
// alpha and cause the red flicker seen previously. Instead we write the final
// ARGB value directly. For text glyphs the glyph alpha modulates the pixel's
// A channel so the hardware blends it correctly against the game beneath.

static inline void drmPutPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifndef _WIN32
    if (!drm_map || (unsigned)x >= (unsigned)card_w || (unsigned)y >= (unsigned)card_h) return;
    drm_map[y * (drm_pitch / 4) + x] = ((uint32_t)a << 24) |
                                         ((uint32_t)r << 16) |
                                         ((uint32_t)g <<  8) |
                                          (uint32_t)b;
#else
    (void)x; (void)y; (void)r; (void)g; (void)b; (void)a;
#endif
}

static void drawRect(int rx, int ry, int rw, int rh,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = ry; y < ry + rh; ++y)
        for (int x = rx; x < rx + rw; ++x)
            drmPutPixel(x, y, r, g, b, a);
}

static void drawRoundedRect(int rx, int ry, int rw, int rh, int radius,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = ry; y < ry + rh; ++y) {
        for (int x = rx; x < rx + rw; ++x) {
            int dx = 0, dy = 0;
            if      (x < rx + radius)        dx = rx + radius - x;
            else if (x >= rx + rw - radius)  dx = x - (rx + rw - radius - 1);
            if      (y < ry + radius)        dy = ry + radius - y;
            else if (y >= ry + rh - radius)  dy = y - (ry + rh - radius - 1);

            if (dx > 0 && dy > 0) {
                float dist = std::sqrt((float)(dx*dx + dy*dy));
                if (dist > radius) continue;
                // Anti-alias the corner edge by scaling alpha
                uint8_t aa = (dist > radius - 1.0f)
                             ? (uint8_t)(a * (radius - dist))
                             : a;
                drmPutPixel(x, y, r, g, b, aa);
            } else {
                drmPutPixel(x, y, r, g, b, a);
            }
        }
    }
}

static void initFreetype() {
    if (FT_Init_FreeType(&ft_lib) != 0) return;
    for (const auto& p : {
            "res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "../res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "/roms/tools/tubelite/res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" }) {
        if (std::filesystem::exists(p) && FT_New_Face(ft_lib, p, 0, &ft_face) == 0) {
            ft_ok = true;
            break;
        }
    }
}

static void drawText(const std::string& text, int x, int y, int fontSize,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ft_ok) return;
    FT_Set_Pixel_Sizes(ft_face, 0, fontSize);

    int pen_x = x;
    int pen_y = y + fontSize;

    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = (uint8_t)text[i];
        if (cp & 0x80) {
            if ((cp & 0xE0) == 0xC0 && i+1 < text.size()) {
                cp = ((cp & 0x1F) << 6) | ((uint8_t)text[++i] & 0x3F);
            } else if ((cp & 0xF0) == 0xE0 && i+2 < text.size()) {
                cp = ((cp & 0x0F) << 12)
                   | (((uint8_t)text[i+1] & 0x3F) << 6)
                   |  ((uint8_t)text[i+2] & 0x3F);
                i += 2;
            }
        }
        if (FT_Load_Char(ft_face, cp, FT_LOAD_RENDER) != 0) continue;

        FT_GlyphSlot gl = ft_face->glyph;
        int gx = pen_x + gl->bitmap_left;
        int gy = pen_y - gl->bitmap_top;

        for (unsigned row = 0; row < gl->bitmap.rows; ++row) {
            for (unsigned col = 0; col < gl->bitmap.width; ++col) {
                uint8_t glyph_a = gl->bitmap.buffer[row * gl->bitmap.pitch + col];
                if (glyph_a == 0) continue;
                // Scale the pixel's alpha by both the glyph coverage and the
                // caller's alpha — hardware composites the result over the game.
                uint8_t final_a = (uint8_t)((uint16_t)glyph_a * a / 255);
                drmPutPixel(gx + col, gy + row, r, g, b, final_a);
            }
        }
        pen_x += gl->advance.x >> 6;
    }
}

static std::string truncateText(const std::string& t, size_t maxLen) {
    if (t.size() <= maxLen) return t;
    return t.substr(0, maxLen - 3) + "...";
}

// ─── Playback ─────────────────────────────────────────────────────────────────

static void playCurrentTrack(MpvPlayer& mpv, YouTubeAPI& yt) {
    if (daemon_current_index < 0 ||
        daemon_current_index >= (int)daemon_playlist.size()) return;

    auto& video = daemon_playlist[daemon_current_index];
    daemon_overlay_timer = 5.0f;
    overlay_active = true;

    if (!video.stream_url.empty()) {
        std::cerr << "[daemon] Using pre-resolved URL for " << video.id << "\n";
        mpv.play(video.stream_url, video.subtitle_url);
        if (daemon_start_position > 0.0) {
            mpv.setPendingSeekPosition(daemon_start_position);
            daemon_start_position = 0.0;
        }
        daemon_status = DaemonStatus::Playing;
        video.stream_url.clear();
        video.subtitle_url.clear();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
        daemon_status            = DaemonStatus::Resolving;
        daemon_request_finished  = false;
        daemon_request_success   = false;
        daemon_resolved_url      = "";
        daemon_subtitle_url      = "";
    }

    yt.getStreamUrl(video.id, 360,
        [](bool ok, const std::string& url, const std::string& sub,
           const VideoPlaybackMetadata&) {
            std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
            daemon_resolved_url     = url;
            daemon_subtitle_url     = sub;
            daemon_request_success  = ok;
            daemon_request_finished = true;
        });
}

// ─── Card render ──────────────────────────────────────────────────────────────
// All coordinates are local to the 320×76 plane buffer.
// Hardware composites this plane over the primary at commitOverlay() time.

static void renderCard(MpvPlayer& mpv) {
#ifndef _WIN32
    if (drm_map) memset(drm_map, 0, drm_size);  // fully transparent baseline
#endif

    // Drop shadow (offset 2px down-right, slightly inset)
    drawRoundedRect(2, 2, card_w - 2, card_h - 2, 8,  0,  0,  0,  fade(80));

    // Red border
    drawRoundedRect(0, 0, card_w,     card_h,     9, 255, 48, 48, fade(200));

    // Dark card fill (inset 1px from border)
    drawRoundedRect(1, 1, card_w - 2, card_h - 2, 8,  26, 28, 32, fade(245));

    // Thin red top accent
    drawRect(2, 2, card_w - 4, 2, 255, 48, 48, fade(160));

    if (daemon_current_index < 0 ||
        daemon_current_index >= (int)daemon_playlist.size()) {
        commitOverlay();
        return;
    }
    const auto& video = daemon_playlist[daemon_current_index];

    // Title
    drawText(truncateText(video.title,  32), 10, 8,  12, 240, 242, 245, fade(255));
    // Author
    drawText(truncateText(video.author, 38), 10, 23, 10, 154, 165, 184, fade(255));

    // Status badge (top-right)
    std::string statStr = "PLAYING";
    uint8_t sr = 64, sg = 214, sb = 96;
    if      (daemon_status == DaemonStatus::Resolving) { statStr="LOADING"; sr=64;  sg=148; sb=255; }
    else if (daemon_status == DaemonStatus::Paused)    { statStr="PAUSED";  sr=255; sg=214; sb=64;  }
    else if (daemon_status == DaemonStatus::Error)     { statStr="ERROR";   sr=255; sg=48;  sb=48;  }
    drawText(statStr, card_w - 65, 8, 10, sr, sg, sb, fade(255));

    // Progress bar
    double pos  = mpv.getPlaybackTime();
    double dur  = mpv.getDuration() > 0.0 ? mpv.getDuration()
                                           : (double)video.duration_seconds;
    double frac = (dur > 0.0) ? std::max(0.0, std::min(1.0, pos / dur)) : 0.0;

    const int barX = 10, barY = 41, barW = card_w - 20, barH = 4;
    drawRect(barX, barY, barW, barH, 42, 48, 56, fade(200));
    if (frac > 0.0)
        drawRect(barX, barY, (int)(barW * frac), barH, 255, 48, 48, fade(255));

    // Timestamp
    std::string timeStr = formatTime(pos) + " / " +
        (video.duration_string.empty() ? formatTime(dur) : video.duration_string);
    drawText(timeStr, 10, 49, 9, 220, 220, 232, fade(255));

    // Button hint row 1
    drawText("L3+A Pause      L3+B Exit",
             10, 63, 9, 160, 160, 172, fade(255));

    // Button hint row 2
    drawText("L3+L1 Prev      L3+R1 Next",
             10, 77, 9, 160, 160, 172, fade(255));

    commitOverlay();
}

// ─── Daemon loop ──────────────────────────────────────────────────────────────

void runDaemon() {
    std::cerr << "[daemon] Initializing...\n";
    killExistingDaemon();

    {
        std::ofstream ofs("/dev/shm/tubelite_daemon.pid");
        if (ofs) ofs << getpid() << "\n";
    }

    if (!loadDaemonQueue()) {
        std::cerr << "[daemon] Empty queue. Exiting.\n";
        unlink("/dev/shm/tubelite_daemon.pid");
        return;
    }

#ifndef _WIN32
    struct sigaction sa;
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
#endif

    MpvPlayer mpv;
    if (!mpv.initializeAudioOnly()) {
        std::cerr << "[daemon] MPV init failed.\n";
        unlink("/dev/shm/tubelite_daemon.pid");
        return;
    }

    YouTubeAPI yt;
    initFreetype();
    initDrmOverlay();

    daemon_overlay_timer = 5.0f;
    overlay_active = true;

    int js_fd = -1;
#ifndef _WIN32
    js_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    if (js_fd < 0) std::cerr << "[daemon] Warning: no event2\n";
#endif

    playCurrentTrack(mpv, yt);

    auto last_tick    = std::chrono::steady_clock::now();
    bool l3_held      = false;
    bool dpad_up_held = false;

    // Track last rendered progress to avoid unnecessary redraws while hidden
    // or when nothing changed.
    double last_render_pos = -1.0;

    std::cerr << "[daemon] Loop started.\n";

    while (daemon_running) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;

        mpv.update();

        // Async resolve
        if (daemon_status == DaemonStatus::Resolving) {
            bool finished, success;
            std::string url, sub;
            {
                std::lock_guard<std::mutex> lk(daemon_resolved_mutex);
                finished = daemon_request_finished;
                success  = daemon_request_success;
                url      = daemon_resolved_url;
                sub      = daemon_subtitle_url;
            }
            if (finished) {
                if (success) {
                    mpv.play(url, sub);
                    if (daemon_start_position > 0.0) {
                        mpv.setPendingSeekPosition(daemon_start_position);
                        daemon_start_position = 0.0;
                    }
                    daemon_status = DaemonStatus::Playing;
                } else {
                    daemon_status = DaemonStatus::Error;
                }
                last_render_pos = -1.0;  // force redraw on status change
            }
        }

        // Track end → auto-advance
        if (daemon_status == DaemonStatus::Playing && mpv.checkAndClearEnded()) {
            daemon_current_index =
                (daemon_current_index + 1) % (int)daemon_playlist.size();
            playCurrentTrack(mpv, yt);
            last_render_pos = -1.0;
        }

#ifndef _WIN32
        if (js_fd >= 0) {
            struct input_event ev;
            while (read(js_fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY) {
                    bool down = (ev.value != 0);

                    // Track L3 state (code 706)
                    if (ev.code == 706) {
                        l3_held = down;
                    }

                    // Hotkeys on button down (value == 1)
                    if (down && l3_held) {
                        if (ev.code == 305) { // A → pause/resume
                            if (daemon_status == DaemonStatus::Playing) {
                                mpv.pause();
                                daemon_status = DaemonStatus::Paused;
                            } else if (daemon_status == DaemonStatus::Paused) {
                                mpv.resume();
                                daemon_status = DaemonStatus::Playing;
                            }
                            daemon_overlay_timer = 5.0f;
                            overlay_active = true;
                            last_render_pos = -1.0;
                        } else if (ev.code == 304) { // B → exit
                            daemon_running = false;
                        } else if (ev.code == 310) { // L1 → prev
                            daemon_current_index =
                                (daemon_current_index - 1 + (int)daemon_playlist.size())
                                % (int)daemon_playlist.size();
                            playCurrentTrack(mpv, yt);
                            last_render_pos = -1.0;
                        } else if (ev.code == 311) { // R1 → next
                            daemon_current_index =
                                (daemon_current_index + 1) % (int)daemon_playlist.size();
                            playCurrentTrack(mpv, yt);
                            last_render_pos = -1.0;
                        }
                    }
                } else if (ev.type == EV_ABS) {
                    if (ev.code == 17) { // ABS_HAT0Y (DPAD vertical)
                        bool new_dpad_up = (ev.value < 0);
                        if (new_dpad_up && !dpad_up_held) { // transition to pressed
                            if (l3_held) {
                                daemon_overlay_timer = 5.0f;
                                overlay_active = true;
                                last_render_pos = -1.0;
                            }
                        }
                        dpad_up_held = new_dpad_up;
                    }
                }
            }
        }
#endif

        // Update overlay alpha based on active state
        if (overlay_active) {
            overlay_alpha += dt * 5.0f; // 200ms fade in time
            if (overlay_alpha > 1.0f) overlay_alpha = 1.0f;
        } else {
            overlay_alpha -= dt * 5.0f; // 200ms fade out time
            if (overlay_alpha < 0.0f) overlay_alpha = 0.0f;
        }

        // Timer logic
        if (daemon_overlay_timer > 0.0f) {
            daemon_overlay_timer -= dt;
            if (daemon_overlay_timer <= 0.0f) {
                overlay_active = false;
            }
        }

        // Rendering logic
        bool animating = overlay_alpha > 0.0f && overlay_alpha < 1.0f;
        double cur_pos = mpv.getPlaybackTime();
        bool pos_changed = std::abs(cur_pos - last_render_pos) >= 1.0;
        bool forced_redraw = (last_render_pos < 0.0);

        if (overlay_alpha > 0.0f) {
            if (animating || pos_changed || forced_redraw) {
                renderCard(mpv);
                last_render_pos = cur_pos;
            }
        } else {
            if (!overlay_active && overlay_alpha <= 0.01f) {
                hideOverlay();
                overlay_alpha = 0.0f;
                last_render_pos = -1.0;
            }
        }

        // Poll waiting strategy
        int timeout = 500; // default hidden idle state
        if (overlay_alpha > 0.0f && overlay_alpha < 1.0f) {
            timeout = 16; // smooth fade animation
        } else if (overlay_active) {
            timeout = 250; // overlay visible
        }

#ifndef _WIN32
        if (js_fd >= 0) {
            struct pollfd pfd{};
            pfd.fd = js_fd;
            pfd.events = POLLIN;
            poll(&pfd, 1, timeout);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
#endif
    }

    std::cerr << "[daemon] Stopping...\n";
    mpv.shutdown();
    closeDrmOverlay();
#ifndef _WIN32
    if (js_fd >= 0) close(js_fd);
#endif
    unlink("/dev/shm/tubelite_daemon.pid");
    std::cerr << "[daemon] Done.\n";
}

void killExistingDaemon() {
#ifndef _WIN32
    std::ifstream ifs("/dev/shm/tubelite_daemon.pid");
    if (ifs) {
        pid_t pid = 0;
        ifs >> pid;
        if (pid > 0) {
            std::cerr << "[daemon] Killing PID " << pid << "\n";
            kill(pid, SIGTERM);
            for (int i = 0; i < 25; ++i) {
                if (kill(pid, 0) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
    unlink("/dev/shm/tubelite_daemon.pid");
#endif
}

void spawnDaemon() {
#ifndef _WIN32
    std::string exec_path = "./tubelite";
    try {
        if (std::filesystem::exists("/proc/self/exe"))
            exec_path = std::filesystem::read_symlink("/proc/self/exe").string();
    } catch (...) {}

    pid_t pid = fork();
    if (pid < 0) { std::cerr << "[daemon] fork failed\n"; return; }
    if (pid > 0) return;

    if (setsid() < 0) exit(1);
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);

    close(0); close(1); close(2);
    open("/dev/null", O_RDONLY);

    int log_fd = open("tubelite_daemon.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd >= 0) { dup2(log_fd, 1); dup2(log_fd, 2); close(log_fd); }
    else             { open("/dev/null", O_WRONLY); open("/dev/null", O_WRONLY); }

    char* args[] = { (char*)exec_path.c_str(), (char*)"--daemon", nullptr };
    execvp(args[0], args);
    exit(1);
#endif
}
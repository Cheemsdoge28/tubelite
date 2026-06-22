#include "daemon.hpp"
#include "mpv_player.hpp"
#include "youtube_api.hpp"
#include "json.hpp"
#include "theme.hpp"   // unified design system (SDL-free in this TU)

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdint>
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
#include <dirent.h>
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
    std::string audio_url;
};

static std::vector<DaemonVideo> daemon_playlist;
static int daemon_current_index = 0;
static double daemon_start_position = 0.0;
static std::atomic<bool> daemon_running{true};

static std::atomic<DaemonStatus> daemon_status{DaemonStatus::Idle};
static std::atomic<bool> daemon_request_finished{false};
static std::atomic<bool> daemon_request_success{false};
static std::atomic<uint64_t> daemon_request_serial{0};
static std::string daemon_resolved_url;
static std::string daemon_subtitle_url;
static std::string daemon_audio_url;
static std::mutex daemon_resolved_mutex;

// Predictive prefetch state.  When the current track has < 90 s left,
// we kick off a background resolve for the NEXT-in-queue and write the
// result into its playlist slot.  On track end / skip, playCurrentTrack
// sees the pre-resolved stream_url and starts mpv instantly with no
// audible gap.  Only one prefetch can be in flight at a time —
// gate is the slot index we're currently prefetching (-1 = none).
// Slot index is captured by the lambda; this gate just prevents
// stacking duplicate resolves for the same target.
static std::atomic<int> daemon_prefetch_inflight_idx{-1};

static float overlay_alpha = 0.0f;
static bool  overlay_active = false;
static float daemon_overlay_timer = 0.0f;

// ── Card dimensions ───────────────────────────────────────────────────────────
static const int card_w = 380;
static const int card_h = 88;
static const int card_y = 10;

static inline uint8_t fade(uint8_t a) {
    return (uint8_t)(a * overlay_alpha);
}

// ── Design tokens ─────────────────────────────────────────────────────────────
// All pulled from the shared theme so the daemon's now-playing card is the
// exact same palette as the in-app cards, HUD and status bar. The C_* aliases
// are kept so the rasteriser call sites below stay unchanged.
static const uint8_t C_BG_R  = theme::BG.r,     C_BG_G  = theme::BG.g,     C_BG_B  = theme::BG.b;      // background
static const uint8_t C_SF_R  = theme::RAISED.r, C_SF_G  = theme::RAISED.g, C_SF_B  = theme::RAISED.b;  // surface lift
static const uint8_t C_AC_R  = theme::ACCENT.r, C_AC_G  = theme::ACCENT.g, C_AC_B  = theme::ACCENT.b;  // accent red
static const uint8_t C_TT_R  = theme::TEXT.r,   C_TT_G  = theme::TEXT.g,   C_TT_B  = theme::TEXT.b;     // title
static const uint8_t C_AU_R  = theme::TEXT_2.r, C_AU_G  = theme::TEXT_2.g, C_AU_B  = theme::TEXT_2.b;   // author
static const uint8_t C_TM_R  = theme::TEXT_ON.r,C_TM_G  = theme::TEXT_ON.g,C_TM_B  = theme::TEXT_ON.b;  // time
static const uint8_t C_HN_R  = theme::TEXT_MUTED.r, C_HN_G = theme::TEXT_MUTED.g, C_HN_B = theme::TEXT_MUTED.b; // hints
static const uint8_t C_TR_R  = theme::TRACK.r,  C_TR_G  = theme::TRACK.g,  C_TR_B  = theme::TRACK.b;    // progress track
static const uint8_t C_GR_R  = theme::GREEN.r,  C_GR_G  = theme::GREEN.g,  C_GR_B  = theme::GREEN.b;    // green
static const uint8_t C_YL_R  = theme::YELLOW.r, C_YL_G  = theme::YELLOW.g, C_YL_B  = theme::YELLOW.b;   // yellow
static const uint8_t C_BL_R  = theme::BLUE.r,   C_BL_G  = theme::BLUE.g,   C_BL_B  = theme::BLUE.b;     // blue

#ifndef _WIN32
#define DRM_OVERLAY_PLANE_ID  61
#define DRM_CRTC_ID           60

static int       drm_fd      = -1;
static uint32_t  drm_fb_id   = 0;
static uint32_t  drm_handle  = 0;
static uint32_t  drm_pitch   = 0;
static uint64_t  drm_size    = 0;
static uint32_t* drm_map     = nullptr;
static int       drm_screen_w = 640;
static int       drm_screen_h = 480;
#endif

static FT_Library ft_lib;
static FT_Face    ft_face;
static bool       ft_ok = false;

static uint32_t card_backbuffer[card_w * card_h];

// ── Helpers ───────────────────────────────────────────────────────────────────

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
                v.audio_url        = item.value("audio_url", "");
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

// ── DRM overlay ───────────────────────────────────────────────────────────────

#ifndef _WIN32
// Fully releases card0 and all DRM resources. Safe to call when already closed.
static void closeDrmOverlay();

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
        perror("[daemon] CREATE_DUMB"); closeDrmOverlay(); return false;
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
        perror("[daemon] AddFB2"); closeDrmOverlay(); return false;
    }

    struct drm_mode_map_dumb mreq = {};
    mreq.handle = drm_handle;
    drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    drm_map = (uint32_t*)mmap(nullptr, drm_size,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED, drm_fd, mreq.offset);
    if (drm_map == MAP_FAILED) { perror("[daemon] mmap drm"); drm_map = nullptr; closeDrmOverlay(); return false; }

    memset(drm_map, 0, drm_size);
    std::cerr << "[daemon] DRM overlay ready. Screen: "
              << drm_screen_w << "x" << drm_screen_h << "\n";
    return true;
}

static void closeDrmOverlay() {
    if (drm_fd < 0) return;
    // Disable the overlay plane, then release every resource and the device fd
    // itself. Closing card0 is what lets an emulator's KMS modeset take the
    // CRTC cleanly — a pinned FB on a lingering plane is what crashed games.
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
    drm_fd     = -1;
    drm_map    = nullptr;
    drm_fb_id  = 0;
    drm_handle = 0;
    drm_pitch  = 0;
    drm_size   = 0;
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

// Returns true when a game/emulator owns the display, so the daemon must not
// touch DRM. Two signals, cheapest first:
//   1. An explicit suspend sentinel a launch hook may drop (100% reliable if
//      wired into runcommand: `touch /dev/shm/tubelite_suspend_overlay` on
//      game start, `rm` on exit).
//   2. Autonomous fallback: a known emulator process is running. This is only
//      consulted when the daemon is about to (re)acquire DRM — i.e. rarely —
//      so the /proc scan cost is irrelevant.
static bool foregroundGameActive() {
    if (access("/dev/shm/tubelite_suspend_overlay", F_OK) == 0) return true;

    static const char* kEmu[] = {
        "retroarch", "retroarch32", "ra32", "drastic", "PPSSPPSDL", "ppsspp",
        "standalone", "flycast", "duckstation", "melonDS", "mupen64plus",
        "snes9x", "pcsx_rearmed", "dosbox", "scummvm", "mednafen", "openbor",
        "gpsp", "yabasanshiro", "easyrpg", "mgba", "vbam", "fbneo", nullptr
    };

    DIR* d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;  // pid dirs only
        char path[300];  // sized for "/proc/" + max 255-byte name + "/comm"
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char comm[128] = {0};
        if (fgets(comm, sizeof(comm), f)) {
            size_t n = strlen(comm);
            if (n && comm[n - 1] == '\n') comm[n - 1] = '\0';
            for (int i = 0; kEmu[i]; ++i) {
                if (strstr(comm, kEmu[i])) { found = true; break; }
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

// Lazily (re)acquire the overlay only when it is safe to do so. Returns false
// — and ensures DRM is fully released — whenever a game is in the foreground.
static bool ensureDrmReady() {
    if (foregroundGameActive()) {
        if (drm_fd >= 0) closeDrmOverlay();
        return false;
    }
    if (drm_fd >= 0) return true;
    return initDrmOverlay();
}
#else
static bool initDrmOverlay() { return true; }
static void closeDrmOverlay() {}
static void commitOverlay() {}
static bool foregroundGameActive() { return false; }
static bool ensureDrmReady() { return true; }
#endif

// ── Drawing primitives ────────────────────────────────────────────────────────
// Backbuffer uses Porter-Duff "src over dst" so layers composite correctly
// (shadow → card body → gradient → text). Hardware plane then composites the
// finished buffer over the primary plane via the alpha channel.

static inline void drmPutPixel(int x, int y,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if ((unsigned)x >= (unsigned)card_w || (unsigned)y >= (unsigned)card_h) return;
    int idx = y * card_w + x;
    uint32_t dst   = card_backbuffer[idx];
    uint8_t  dst_a = (dst >> 24) & 0xff;
    uint8_t  dst_r = (dst >> 16) & 0xff;
    uint8_t  dst_g = (dst >>  8) & 0xff;
    uint8_t  dst_b =  dst        & 0xff;

    uint32_t inv   = 255 - a;
    uint32_t out_a = a + (uint32_t)dst_a * inv / 255;
    if (out_a == 0) return;
    uint8_t out_r  = ((uint32_t)r * a + (uint32_t)dst_r * inv) / 255;
    uint8_t out_g  = ((uint32_t)g * a + (uint32_t)dst_g * inv) / 255;
    uint8_t out_b  = ((uint32_t)b * a + (uint32_t)dst_b * inv) / 255;

    card_backbuffer[idx] = (uint32_t)(out_a > 255 ? 255 : out_a) << 24 |
                           (uint32_t)out_r << 16 |
                           (uint32_t)out_g <<  8 |
                           (uint32_t)out_b;
}

[[maybe_unused]] static void fillRect(int rx, int ry, int rw, int rh,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int x2 = std::min(rx + rw, card_w);
    int y2 = std::min(ry + rh, card_h);
    for (int y = std::max(ry, 0); y < y2; ++y)
        for (int x = std::max(rx, 0); x < x2; ++x)
            drmPutPixel(x, y, r, g, b, a);
}

static void fillRoundedRect(int rx, int ry, int rw, int rh, int radius,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = ry; y < ry + rh && y < card_h; ++y) {
        for (int x = rx; x < rx + rw && x < card_w; ++x) {
            int dx = 0, dy = 0;
            if      (x < rx + radius)       dx = rx + radius - x;
            else if (x >= rx + rw - radius) dx = x - (rx + rw - radius - 1);
            if      (y < ry + radius)       dy = ry + radius - y;
            else if (y >= ry + rh - radius) dy = y - (ry + rh - radius - 1);
            if (dx > 0 && dy > 0) {
                float dist = std::sqrt((float)(dx*dx + dy*dy));
                if (dist >= radius) continue;
                uint8_t aa = dist > radius - 1.0f
                             ? (uint8_t)(a * (radius - dist)) : a;
                drmPutPixel(x, y, r, g, b, aa);
            } else {
                drmPutPixel(x, y, r, g, b, a);
            }
        }
    }
}

// Top-to-bottom gradient within a rectangle
static void fillGradientV(int rx, int ry, int rw, int rh,
                           uint8_t r0, uint8_t g0, uint8_t b0, uint8_t a0,
                           uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1) {
    for (int y = ry; y < ry + rh && y < card_h; ++y) {
        float t = (rh > 1) ? (float)(y - ry) / (float)(rh - 1) : 0.f;
        uint8_t r = (uint8_t)(r0 + t * ((int)r1 - r0));
        uint8_t g = (uint8_t)(g0 + t * ((int)g1 - g0));
        uint8_t b = (uint8_t)(b0 + t * ((int)b1 - b0));
        uint8_t a = (uint8_t)(a0 + t * ((int)a1 - a0));
        for (int x = std::max(rx, 0); x < rx + rw && x < card_w; ++x)
            drmPutPixel(x, y, r, g, b, a);
    }
}

// Left-to-right gradient — for accent glow
static void fillGradientH(int rx, int ry, int rw, int rh,
                           uint8_t r0, uint8_t g0, uint8_t b0, uint8_t a0,
                           uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1) {
    for (int x = rx; x < rx + rw && x < card_w; ++x) {
        float t = (rw > 1) ? (float)(x - rx) / (float)(rw - 1) : 0.f;
        uint8_t r = (uint8_t)(r0 + t * ((int)r1 - r0));
        uint8_t g = (uint8_t)(g0 + t * ((int)g1 - g0));
        uint8_t b = (uint8_t)(b0 + t * ((int)b1 - b0));
        uint8_t a = (uint8_t)(a0 + t * ((int)a1 - a0));
        for (int y = std::max(ry, 0); y < ry + rh && y < card_h; ++y)
            drmPutPixel(x, y, r, g, b, a);
    }
}

static void drawHRule(int rx, int ry, int rw,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int x = rx; x < rx + rw && x < card_w; ++x)
        drmPutPixel(x, ry, r, g, b, a);
}

// ── FreeType ──────────────────────────────────────────────────────────────────

// Coverage gamma LUT. Small anti-aliased text on a dark surface renders thin
// and washed-out because linear AA coverage under-weights partially-covered
// edge pixels. A sub-1.0 gamma lifts those mid values, fattening strokes just
// enough to read crisply at 9-14px. Built once; a plain table lookup at draw.
static uint8_t cov_lut[256];
static void initCoverageLut() {
    for (int i = 0; i < 256; ++i) {
        float c = i / 255.0f;
        c = powf(c, 0.70f);                 // < 1.0 => brighten/thicken edges
        int v = (int)(c * 255.0f + 0.5f);
        cov_lut[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static void initFreetype() {
    initCoverageLut();
    if (FT_Init_FreeType(&ft_lib) != 0) return;
    for (const auto& p : {
            "res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "../res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "/roms/tools/tubelite/res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" }) {
        if (std::filesystem::exists(p) && FT_New_Face(ft_lib, p, 0, &ft_face) == 0) {
            ft_ok = true; break;
        }
    }
}

static int measureText(const std::string& text, int fontSize) {
    if (!ft_ok) return 0;
    FT_Set_Pixel_Sizes(ft_face, 0, fontSize);
    int w = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = (uint8_t)text[i];
        if (cp & 0x80) {
            if ((cp & 0xE0) == 0xC0 && i+1 < text.size())
                cp = ((cp & 0x1F) << 6) | ((uint8_t)text[++i] & 0x3F);
            else if ((cp & 0xF0) == 0xE0 && i+2 < text.size()) {
                cp = ((cp & 0x0F) << 12)
                   | (((uint8_t)text[i+1] & 0x3F) << 6)
                   |  ((uint8_t)text[i+2] & 0x3F);
                i += 2;
            }
        }
        if (FT_Load_Char(ft_face, cp, FT_LOAD_ADVANCE_ONLY) != 0) continue;
        w += ft_face->glyph->advance.x >> 6;
    }
    return w;
}

static void drawText(const std::string& text, int x, int y, int fontSize,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                     int clip_x2 = card_w) {
    if (!ft_ok) return;
    FT_Set_Pixel_Sizes(ft_face, 0, fontSize);
    int pen_x = x, pen_y = y + fontSize;

    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = (uint8_t)text[i];
        if (cp & 0x80) {
            if ((cp & 0xE0) == 0xC0 && i+1 < text.size())
                cp = ((cp & 0x1F) << 6) | ((uint8_t)text[++i] & 0x3F);
            else if ((cp & 0xF0) == 0xE0 && i+2 < text.size()) {
                cp = ((cp & 0x0F) << 12)
                   | (((uint8_t)text[i+1] & 0x3F) << 6)
                   |  ((uint8_t)text[i+2] & 0x3F);
                i += 2;
            }
        }
        // Light autohint target keeps small glyphs crisp without over-snapping.
        if (FT_Load_Char(ft_face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) continue;
        FT_GlyphSlot gl = ft_face->glyph;
        int gx = pen_x + gl->bitmap_left;
        int gy = pen_y - gl->bitmap_top;
        for (unsigned row = 0; row < gl->bitmap.rows; ++row)
            for (unsigned col = 0; col < gl->bitmap.width; ++col) {
                int px = gx + (int)col;
                if (px >= clip_x2) continue;
                uint8_t ga = cov_lut[gl->bitmap.buffer[row * gl->bitmap.pitch + col]];
                if (ga == 0) continue;
                drmPutPixel(px, gy + (int)row, r, g, b,
                            (uint8_t)((uint16_t)ga * a / 255));
            }
        pen_x += gl->advance.x >> 6;
    }
}

static void drawTextRight(const std::string& text, int right_x, int y,
                           int fontSize,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int w = measureText(text, fontSize);
    drawText(text, right_x - w, y, fontSize, r, g, b, a);
}

static std::string truncateText(const std::string& t, size_t maxLen) {
    if (t.size() <= maxLen) return t;
    // Unicode ellipsis U+2026
    return t.substr(0, maxLen - 1) + "\xE2\x80\xA6";
}

// ── Playback ──────────────────────────────────────────────────────────────────

static void playCurrentTrack(MpvPlayer& mpv, YouTubeAPI& yt) {
    if (daemon_current_index < 0 ||
        daemon_current_index >= (int)daemon_playlist.size()) return;

    // Snapshot URLs + id under the mutex.  The prefetch callback writes
    // to playlist[next_idx] fields from a worker thread; without this
    // lock we could read a half-written std::string and crash or play
    // garbage.  Local copies decouple the (potentially blocking) mpv
    // calls from the mutex hold.
    std::string vid_id, stream_url, subtitle_url, audio_url;
    {
        std::lock_guard<std::mutex> lk(daemon_resolved_mutex);
        const auto& video = daemon_playlist[daemon_current_index];
        vid_id       = video.id;
        stream_url   = video.stream_url;
        subtitle_url = video.subtitle_url;
        audio_url    = video.audio_url;
    }

    // Bump serial BEFORE mpv.stop() and BEFORE the new getStreamUrl call
    // so any in-flight callback from the PREVIOUS track is guaranteed to
    // see a mismatch and silently drop.  Without this ordering, a rapid
    // skip can race: old callback fires, sees the old serial still active
    // (the bump hasn't happened yet), writes ok=false → ERROR status is
    // shown for the new track even though its resolve hasn't started yet.
    const uint64_t request_serial = ++daemon_request_serial;
    daemon_overlay_timer = 5.0f;
    overlay_active = true;
    std::cerr << "[daemon] playCurrentTrack idx=" << daemon_current_index
              << " id=" << vid_id
              << " cached=" << (!stream_url.empty() ? "yes" : "no") << "\n";
    mpv.stop();

    if (!stream_url.empty()) {
        // Pre-resolved path — either the queue was loaded with cached
        // URLs from the app, or the previous track's prefetch wrote
        // here.  Either way: instant transition, no extractor call.
        std::cerr << "[daemon] Using pre-resolved URL for " << vid_id << "\n";
        mpv.play(stream_url, subtitle_url, audio_url);
        if (daemon_start_position > 0.0) {
            mpv.setPendingSeekPosition(daemon_start_position);
            daemon_start_position = 0.0;
        }
        daemon_status = DaemonStatus::Playing;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
        // Always reset to Resolving immediately so the overlay never
        // flashes ERROR between a skip and the new resolve starting,
        // even if the previous track's callback arrives late with a
        // failure result.  The serial bump above ensures that callback
        // will be discarded; this reset makes the visual correct too.
        daemon_status            = DaemonStatus::Resolving;
        daemon_request_finished  = false;
        daemon_request_success   = false;
        daemon_resolved_url      = "";
        daemon_subtitle_url      = "";
        daemon_audio_url         = "";
    }

    yt.getStreamUrl(vid_id, 360,
        [request_serial](bool ok, const std::string& url, const std::string& sub,
           const std::string& audio_arg,
           const VideoPlaybackMetadata&) {
            // Check serial FIRST, outside any lock, to cheaply discard
            // the majority of stale callbacks from rapid track switches
            // without contending on the mutex at all.
            if (daemon_request_serial.load(std::memory_order_acquire) != request_serial) {
                std::cerr << "[daemon] Ignoring stale resolve serial=" << request_serial << "\n";
                return;
            }
            std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
            // Re-check under the lock — another skip may have bumped the
            // serial between our relaxed load above and taking the lock.
            if (daemon_request_serial.load(std::memory_order_relaxed) != request_serial) {
                std::cerr << "[daemon] Ignoring stale resolve (post-lock) serial=" << request_serial << "\n";
                return;
            }
            daemon_resolved_url     = url;
            daemon_subtitle_url     = sub;
            daemon_audio_url        = audio_arg;
            daemon_request_success  = ok;
            daemon_request_finished = true;
        });
}

// Predictive prefetch: when the current track has < 90 s remaining, kick
// off a background resolve for the NEXT-in-queue.  Caches the URL into
// the next playlist slot so the eventual track change is gap-free.
//
// Strategy notes (matches the user's spec):
//   * Triggered on remaining-time, not percent — short clips don't waste
//     a prefetch on 5-second windows, long clips don't sit idle.
//   * Skips when:
//       - playlist <= 1 entry (no "next")
//       - next slot already resolved (cache hit from a previous play)
//       - a prefetch is already in flight (avoids stacking)
//       - resolve work is already in flight for the CURRENT track
//         (don't compete with our own foreground call)
//   * The resolve runs on a worker thread (YouTubeAPI::getStreamUrl
//     dispatches one).  The callback writes back under daemon_resolved_mutex
//     so the main thread can read it safely.
//   * Caller (the main loop) checks remaining_time via mpv.getPlaybackTime/
//     getDuration — this helper does everything else.
static void maybePrefetchNext(YouTubeAPI& yt, double remaining_seconds) {
    if (remaining_seconds <= 0.0 || remaining_seconds >= 90.0) return;
    if (daemon_status != DaemonStatus::Playing) return;
    if (daemon_request_serial.load(std::memory_order_relaxed) > 0 &&
        !daemon_request_finished.load(std::memory_order_relaxed)) {
        // A foreground resolve is in flight (current track) — don't add
        // network/CPU contention on top.
        return;
    }
    const int n = static_cast<int>(daemon_playlist.size());
    if (n <= 1) return;
    if (daemon_current_index < 0 || daemon_current_index >= n) return;

    const int next_idx = (daemon_current_index + 1) % n;

    std::string next_id;
    bool already_resolved = false;
    {
        std::lock_guard<std::mutex> lk(daemon_resolved_mutex);
        already_resolved = !daemon_playlist[next_idx].stream_url.empty();
        next_id          = daemon_playlist[next_idx].id;
    }
    if (already_resolved || next_id.empty()) return;

    // Try to claim the prefetch slot.  CAS so two callers racing into
    // this function can't both kick the same resolve.
    int expected = -1;
    if (!daemon_prefetch_inflight_idx.compare_exchange_strong(
            expected, next_idx, std::memory_order_acq_rel)) {
        return; // already prefetching something
    }

    std::cerr << "[daemon] prefetch: resolving next track idx=" << next_idx
              << " id=" << next_id
              << " (current has " << remaining_seconds << "s left)\n";

    yt.getStreamUrl(next_id, 360,
        [next_idx, next_id](bool ok, const std::string& url, const std::string& sub,
                            const std::string& audio,
                            const VideoPlaybackMetadata&) {
            // Always clear the inflight gate, even on failure (so the
            // next opportunity isn't blocked forever).
            daemon_prefetch_inflight_idx.store(-1, std::memory_order_release);
            if (!ok || url.empty()) {
                std::cerr << "[daemon] prefetch: failed for " << next_id << "\n";
                return;
            }
            // Verify the playlist hasn't been mutated under us — the
            // user might have skipped, paused, or replaced the queue
            // in the interim.  Writing to a stale index would corrupt
            // an unrelated entry.
            std::lock_guard<std::mutex> lk(daemon_resolved_mutex);
            if (next_idx < 0 || next_idx >= (int)daemon_playlist.size()) return;
            if (daemon_playlist[next_idx].id != next_id) return;
            daemon_playlist[next_idx].stream_url   = url;
            daemon_playlist[next_idx].subtitle_url = sub;
            daemon_playlist[next_idx].audio_url    = audio;
            std::cerr << "[daemon] prefetch: cached for idx=" << next_idx
                      << " id=" << next_id << "\n";
        });
}

// ── Card render ───────────────────────────────────────────────────────────────
//
// Layout (380 × 88px):
//
//  ┌────────────────────────────────────────────────────────────────┐
//  │▌ Title 14px                                    [STATUS badge] │ y=10
//  │▌ Author 11px                                   00:00 / 00:00 │ y=27
//  │  ──────── hairline rule ───────────────────────────────────── │ y=42
//  │  ████████████░░░░░░░░░░░░░░░░░●░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │ y=48 bar
//  │  FN+A Pause · FN+L/R Skip · FN+B Exit           2 / 5        │ y=62
//  └────────────────────────────────────────────────────────────────┘
//   ↑ 4px accent bar + glow

static void renderCard(MpvPlayer& mpv) {
    const uint8_t fa = fade(255);
    if (fa == 0) return;

    // Acquire DRM lazily and bail if a game owns the display. Done before we
    // spend any CPU rasterising, and it guarantees we never commit the overlay
    // plane onto a CRTC an emulator is driving.
    if (!ensureDrmReady()) return;

    memset(card_backbuffer, 0, sizeof(card_backbuffer));

    const int ML = 16;                  // left margin (after accent bar + gap)
    const int MR = 12;                  // right margin
    const int R  = theme::RADIUS_CARD;  // card corner radius (shared token)

    // ── 1. Diffuse shadow ─────────────────────────────────────────────────────
    // Two-pass soft shadow: larger rect, low alpha, slightly offset down
    fillRoundedRect(2,  4, card_w - 2, card_h,     R, 0, 0, 0, fade(50));
    fillRoundedRect(1,  2, card_w - 1, card_h + 1, R, 0, 0, 0, fade(30));

    // ── 2. Card body ──────────────────────────────────────────────────────────
    fillRoundedRect(0, 0, card_w, card_h, R, C_BG_R, C_BG_G, C_BG_B, fa);

    // Subtle top-half gradient: slightly lighter surface at top
    fillGradientV(1, 1, card_w - 2, card_h / 2,
                  C_SF_R, C_SF_G, C_SF_B, fade(55),
                  C_SF_R, C_SF_G, C_SF_B, 0);

    // ── 3. Left accent bar + glow ─────────────────────────────────────────────
    // 4px solid bar, full card height with matching corner radius
    fillRoundedRect(0, 0, 4, card_h, 2, C_AC_R, C_AC_G, C_AC_B, fa);
    // Glow: short horizontal gradient fading right from bar
    fillGradientH(4, 0, 32, card_h,
                  C_AC_R, C_AC_G, C_AC_B, fade(28),
                  C_AC_R, C_AC_G, C_AC_B, 0);

    // ── 4. Hairline rules (top + bottom edges, inside card) ───────────────────
    drawHRule(4, 0,          card_w - 4, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, fade(70));
    drawHRule(4, card_h - 1, card_w - 4, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, fade(35));

    // ── Guard ─────────────────────────────────────────────────────────────────
    if (daemon_current_index < 0 ||
        daemon_current_index >= (int)daemon_playlist.size()) {
#ifndef _WIN32
        if (drm_map)
            for (int y = 0; y < card_h; ++y)
                memcpy(drm_map + y * (drm_pitch / 4),
                       card_backbuffer + y * card_w, card_w * 4);
#endif
        commitOverlay();
        return;
    }
    const auto& video = daemon_playlist[daemon_current_index];

    // ── 5. Status badge (top-right) ───────────────────────────────────────────
    uint8_t sr, sg, sb;
    std::string statStr;
    switch ((DaemonStatus)daemon_status) {
        case DaemonStatus::Resolving:
            statStr="LOADING"; sr=C_BL_R; sg=C_BL_G; sb=C_BL_B; break;
        case DaemonStatus::Paused:
            statStr="PAUSED";  sr=C_YL_R; sg=C_YL_G; sb=C_YL_B; break;
        case DaemonStatus::Error:
            statStr="ERROR";   sr=C_AC_R; sg=C_AC_G; sb=C_AC_B;  break;
        default:
            statStr="PLAYING"; sr=C_GR_R; sg=C_GR_G; sb=C_GR_B;  break;
    }
    int bw = measureText(statStr, 9) + 10;
    int bx = card_w - MR - bw;
    // Pill tint behind badge text
    fillRoundedRect(bx, 7, bw, 14, 4, sr, sg, sb, fade(30));
    drawText(statStr, bx + 5, 9, 9, sr, sg, sb, fade(220));

    // ── 6. Title (14px, clipped before badge) ────────────────────────────────
    drawText(truncateText(video.title, 34), ML, 10, 14,
             C_TT_R, C_TT_G, C_TT_B, fa, bx - 6);

    // ── 7. Author (11px) + timestamp right-aligned (same baseline) ───────────
    drawText(truncateText(video.author, 40), ML, 27, 11,
             C_AU_R, C_AU_G, C_AU_B, fade(230));

    double pos  = mpv.getPlaybackTime();
    double dur  = mpv.getDuration() > 0.0 ? mpv.getDuration()
                                           : (double)video.duration_seconds;
    double frac = (dur > 0.0) ? std::max(0.0, std::min(1.0, pos / dur)) : 0.0;

    std::string timeStr = formatTime(pos) + " / " +
        (video.duration_string.empty() ? formatTime(dur) : video.duration_string);
    drawTextRight(timeStr, card_w - MR, 27, 10,
                  C_TM_R, C_TM_G, C_TM_B, fade(215));

    // ── 8. Separator hairline above progress bar ──────────────────────────────
    drawHRule(ML, 42, card_w - ML - MR, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, fade(90));

    // ── 9. Progress bar (5px pill, y=48) ─────────────────────────────────────
    const int barX = ML, barY = 48, barW = card_w - ML - MR, barH = 5;

    // Track pill
    fillRoundedRect(barX, barY, barW, barH, barH / 2,
                    C_TR_R, C_TR_G, C_TR_B, fade(220));

    // Fill pill
    int fillW = (int)(barW * frac);
    if (fillW > 1) {
        fillRoundedRect(barX, barY, fillW, barH, barH / 2,
                        C_AC_R, C_AC_G, C_AC_B, fa);

        // Thumb dot — 7px diameter circle at leading edge
        int tx = barX + fillW - 1;
        int ty = barY + barH / 2;
        const int TR = 4;
        for (int dy = -TR; dy <= TR; ++dy) {
            for (int dx = -TR; dx <= TR; ++dx) {
                float d = std::sqrt((float)(dx*dx + dy*dy));
                if (d > TR) continue;
                // Soft edge
                uint8_t aa = d > TR - 1.0f ? (uint8_t)(fa * (TR - d)) : fa;
                // White core → accent red at rim
                float t = d / (float)TR;
                uint8_t tr = (uint8_t)(255 - t * (255 - C_AC_R));
                uint8_t tg = (uint8_t)(255 * (1.f - t) + C_AC_G * t);
                uint8_t tb = (uint8_t)(255 * (1.f - t) + C_AC_B * t);
                drmPutPixel(tx + dx, ty + dy, tr, tg, tb, aa);
            }
        }
    }

    // ── 10. Hint row + track index ────────────────────────────────────────────
    // Single line with middle-dot separators; track index right-aligned.
    // Use plain "L/R" for the shoulder-button skip (the font has no ←/→ arrow
    // glyphs, which previously rendered as tofu). U+00B7 middle dot is present.
    const std::string hints =
        "FN+A Pause  \xC2\xB7  FN+L/R Skip  \xC2\xB7  FN+B Exit";
    drawText(hints, ML, 62, 9, C_HN_R, C_HN_G, C_HN_B, fade(205));

    if ((int)daemon_playlist.size() > 1) {
        // Plain ASCII slash — the font lacks the U+2044 fraction slash glyph.
        std::string idx = std::to_string(daemon_current_index + 1)
                        + " / "
                        + std::to_string(daemon_playlist.size());
        drawTextRight(idx, card_w - MR, 62, 9,
                      C_HN_R, C_HN_G, C_HN_B, fade(175));
    }

    // ── 11. Flush to DRM dumb buffer ─────────────────────────────────────────
#ifndef _WIN32
    if (drm_map)
        for (int y = 0; y < card_h; ++y)
            memcpy(drm_map + y * (drm_pitch / 4),
                   card_backbuffer + y * card_w, card_w * 4);
#endif
    commitOverlay();
}

// ── Daemon loop ───────────────────────────────────────────────────────────────

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
    // DRM is acquired lazily (ensureDrmReady) the first time the card actually
    // needs to draw, and fully released whenever it fades out — so the daemon
    // holds no display resources while sitting in the background.

    daemon_overlay_timer = 5.0f;
    overlay_active = true;

    int js_fd = -1;
#ifndef _WIN32
    js_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    if (js_fd < 0) std::cerr << "[daemon] Warning: no event2\n";
#endif

    playCurrentTrack(mpv, yt);

    auto last_tick    = std::chrono::steady_clock::now();
    bool fn_held      = false;
    bool dpad_up_held = false;
    double last_render_pos = -1.0;

    // Seamless-handoff signal: the app keeps its own audio playing (fading out)
    // until this flag appears, so playback never goes silent. Clear any stale
    // one, then write it the moment our audio is actually flowing.
    ::unlink("/dev/shm/tubelite_daemon_audio.live");
    bool audio_live_signalled = false;

    // Audio fade-out helper.  Ramps mpv volume from current → 0 over
    // `dur_ms` milliseconds, then sets daemon_running=false so the main
    // loop exits cleanly on next iteration.  Used by FN+B (user-initiated
    // exit) and by the reabsorption signal file (app-initiated exit) so
    // BOTH exit paths produce a smooth audible fade instead of an
    // abrupt audio cut.  Called from the main loop's own thread, so it
    // shares the same mpv pointer without a synchronization concern.
    auto fadeOutAndExit = [&](int dur_ms = 800) {
        // Snapshot current effective volume (mpv exposes "volume" 0-100).
        const int64_t cur = mpv.getPropertyInt("volume");
        if (cur <= 0) { daemon_running = false; return; }
        using namespace std::chrono;
        const auto start = steady_clock::now();
        while (true) {
            const float t = duration<float>(steady_clock::now() - start).count();
            const float frac = std::min(1.0f, t * 1000.0f / dur_ms);
            int v = static_cast<int>(cur * (1.0f - frac));
            if (v < 0) v = 0;
            mpv.setVolume(v);
            if (frac >= 1.0f) break;
            mpv.update();
            std::this_thread::sleep_for(milliseconds(20));
        }
        daemon_running = false;
    };

    // For reabsorption: snapshot current track + position into a small
    // JSON.  When TubeLite re-launches it reads this file BEFORE killing
    // the daemon, transfers playback into its own mpv at the same
    // offset, and opens the miniplayer.  Throttled to ~2 s — the user
    // can't perceive a 2 s position skip on resume, and halving the
    // write rate halves the filesystem wake-ups that prevent the
    // device from entering deeper sleep states.
    auto last_state_write = std::chrono::steady_clock::now();
    auto writeState = [&]() {
        // Skip when there's nothing playing — no point rewriting the
        // same idle snapshot every 2 s, and a missing file is a clean
        // signal to the app that there's nothing to reabsorb.
        if (daemon_current_index < 0 ||
            daemon_current_index >= (int)daemon_playlist.size()) return;
        if (daemon_status != DaemonStatus::Playing &&
            daemon_status != DaemonStatus::Paused) return;

        std::string vid, title, author, stream_url, subtitle_url, audio_url;
        {
            std::lock_guard<std::mutex> lk(daemon_resolved_mutex);
            const auto& v = daemon_playlist[daemon_current_index];
            vid = v.id;
            title = v.title;
            author = v.author;
            stream_url = v.stream_url;
            subtitle_url = v.subtitle_url;
            audio_url = v.audio_url;
        }

        std::ofstream ofs("/dev/shm/tubelite_daemon_state.json");
        if (!ofs) return;
        ofs << "{"
            << "\"id\":\""        << vid        << "\","
            << "\"title\":\""     << title     << "\","
            << "\"author\":\""    << author    << "\","
            << "\"position\":"    << mpv.getPlaybackTime() << ","
            << "\"duration\":"    << mpv.getDuration()     << ","
            << "\"playing\":"     << (daemon_status == DaemonStatus::Playing ? "true" : "false") << ","
            << "\"stream_url\":\"" << stream_url << "\","
            << "\"subtitle_url\":\"" << subtitle_url << "\","
            << "\"audio_url\":\"" << audio_url << "\""
            << "}";
    };

    std::cerr << "[daemon] Loop started.\n";

    while (daemon_running) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;

        mpv.update();

        if (!audio_live_signalled &&
            daemon_status == DaemonStatus::Playing &&
            mpv.getPlaybackTime() > 0.05) {
            std::ofstream(("/dev/shm/tubelite_daemon_audio.live")) << "1";
            audio_live_signalled = true;
        }

        // Reabsorption-state snapshot: refresh every ~2 s.  Cheap (~200
        // byte file write) but throttled so we don't keep the CPU + I/O
        // path warm in pure-audio idle mode.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_state_write).count() >= 2000) {
            writeState();
            last_state_write = now;
        }

        // Predictive prefetch of the next track.  Cheap to call every
        // loop iteration: the helper bails immediately when remaining
        // time is high, a prefetch is already in flight, the queue
        // has one entry, etc.  When all conditions align, it dispatches
        // a single async resolve and the callback writes the URL back
        // into the next playlist slot — so the next playCurrentTrack()
        // is gap-free instead of waiting on yt-dlp.
        {
            const double dur = mpv.getDuration();
            const double pos = mpv.getPlaybackTime();
            if (dur > 0.0 && pos > 0.0) {
                maybePrefetchNext(yt, dur - pos);
            }
        }

        // Reabsorption fade-out trigger.  When the app re-launches it
        // creates this flag file, then begins fading its own mpv UP
        // from volume 0.  We respond by fading our mpv DOWN over the
        // same window and exiting cleanly — net effect is a symmetric
        // crossfade with no perceptible audio gap.  Cleanup of the flag
        // happens on exit (unlink at the bottom of main()).
        if (access("/dev/shm/tubelite_daemon_fadeout", F_OK) == 0) {
            ::unlink("/dev/shm/tubelite_daemon_fadeout");
            fadeOutAndExit(800);
            continue;   // fall through to the loop-exit on next iter
        }

        // ── Async resolve ─────────────────────────────────────────────────
        if (daemon_status == DaemonStatus::Resolving) {
            bool finished, success;
            std::string url, sub, audio;
            {
                std::lock_guard<std::mutex> lk(daemon_resolved_mutex);
                finished = daemon_request_finished;
                success  = daemon_request_success;
                url      = daemon_resolved_url;
                sub      = daemon_subtitle_url;
                audio    = daemon_audio_url;
            }
            if (finished) {
                if (success) {
                    std::string vid = (daemon_current_index >= 0 &&
                                       daemon_current_index < (int)daemon_playlist.size())
                        ? daemon_playlist[daemon_current_index].id
                        : std::string("<out-of-range>");
                    std::cerr << "[daemon] Resolve success idx=" << daemon_current_index
                              << " id=" << vid
                              << " audio=" << (!audio.empty() ? "yes" : "no") << "\n";
                    // Cache the resolved URL into the playlist slot under
                    // the same mutex the prefetch path uses — these can
                    // race on adjacent slots in the playlist vector.
                    {
                        std::lock_guard<std::mutex> lk(daemon_resolved_mutex);
                        if (daemon_current_index >= 0 &&
                            daemon_current_index < (int)daemon_playlist.size()) {
                            auto& video = daemon_playlist[daemon_current_index];
                            video.stream_url   = url;
                            video.subtitle_url = sub;
                            video.audio_url    = audio;
                        }
                    }
                    mpv.play(url, sub, audio);
                    if (daemon_start_position > 0.0) {
                        mpv.setPendingSeekPosition(daemon_start_position);
                        daemon_start_position = 0.0;
                    }
                    daemon_status = DaemonStatus::Playing;
                } else {
                    std::string vid = (daemon_current_index >= 0 &&
                                       daemon_current_index < (int)daemon_playlist.size())
                        ? daemon_playlist[daemon_current_index].id
                        : std::string("<out-of-range>");
                    std::cerr << "[daemon] Resolve failed idx=" << daemon_current_index
                              << " id=" << vid << "\n";
                    daemon_status = DaemonStatus::Error;
                }
                last_render_pos = -1.0;
            }
        }

        // ── Track end ─────────────────────────────────────────────────────
        if (daemon_status == DaemonStatus::Playing && mpv.checkAndClearEnded()) {
            daemon_current_index =
                (daemon_current_index + 1) % (int)daemon_playlist.size();
            playCurrentTrack(mpv, yt);
            last_render_pos = -1.0;
        }

        // ── Input ─────────────────────────────────────────────────────────
#ifndef _WIN32
        if (js_fd >= 0) {
            struct input_event ev;
            while (read(js_fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY) {
                    bool down = (ev.value != 0);
                    if (ev.code == 708) { fn_held = down; }

                    if (down && fn_held) {
                        if (ev.code == 310) {
                            constexpr double kRestartThresholdSec = 3.0;
                            double cur = mpv.getPlaybackTime();
                            bool restartCurrent =
                                (daemon_status == DaemonStatus::Playing ||
                                 daemon_status == DaemonStatus::Paused) &&
                                cur > kRestartThresholdSec;
                            if (restartCurrent) {
                                mpv.seekAbsoluteExact(0.0);
                            } else {
                                daemon_current_index =
                                    (daemon_current_index - 1 + (int)daemon_playlist.size())
                                    % (int)daemon_playlist.size();
                                playCurrentTrack(mpv, yt);
                            }
                            daemon_overlay_timer = 5.0f;
                            overlay_active = true;
                            last_render_pos = -1.0;
                            continue;
                        }
                        if (ev.code == 311) {
                            daemon_current_index =
                                (daemon_current_index + 1) % (int)daemon_playlist.size();
                            playCurrentTrack(mpv, yt);
                            daemon_overlay_timer = 5.0f;
                            overlay_active = true;
                            last_render_pos = -1.0;
                            continue;
                        }
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
                        } else if (ev.code == 304) { // B → exit (with fade)
                            // Fade audio out over 800 ms before exiting
                            // so the speaker doesn't pop or cut mid-track.
                            // Drains the input queue first — we don't want
                            // late events queueing while we're mid-fade.
                            fadeOutAndExit(800);
                        } else if (ev.code == 103 || ev.code == 544) { // UP → show
                            daemon_overlay_timer = 5.0f;
                            overlay_active = true;
                            last_render_pos = -1.0;
                        }
                    }
                } else if (ev.type == EV_ABS && ev.code == 17) {
                    bool new_dpad_up = (ev.value < 0);
                    if (new_dpad_up && !dpad_up_held && fn_held) {
                        daemon_overlay_timer = 5.0f;
                        overlay_active = true;
                        last_render_pos = -1.0;
                    }
                    dpad_up_held = new_dpad_up;
                }
            }
        }
#endif

        // ── Fade ──────────────────────────────────────────────────────────
        if (overlay_active) {
            overlay_alpha += dt * 5.0f;
            if (overlay_alpha > 1.0f) overlay_alpha = 1.0f;
        } else {
            overlay_alpha -= dt * 5.0f;
            if (overlay_alpha < 0.0f) overlay_alpha = 0.0f;
        }

        // ── Timer ─────────────────────────────────────────────────────────
        if (daemon_overlay_timer > 0.0f) {
            daemon_overlay_timer -= dt;
            if (daemon_overlay_timer <= 0.0f)
                overlay_active = false;
        }

        // ── Render ────────────────────────────────────────────────────────
        bool animating   = overlay_alpha > 0.0f && overlay_alpha < 1.0f;
        double cur_pos   = mpv.getPlaybackTime();
        bool pos_changed = std::abs(cur_pos - last_render_pos) >= 1.0;
        bool forced      = (last_render_pos < 0.0);

        if (overlay_alpha > 0.0f) {
            if (animating || pos_changed || forced) {
                renderCard(mpv);
                last_render_pos = cur_pos;
            }
        } else {
            if (!overlay_active && overlay_alpha <= 0.01f) {
                // Fully release card0 so a game can take the display cleanly.
                closeDrmOverlay();
                overlay_alpha = 0.0f;
                last_render_pos = -1.0;
            }
        }

        // ── Poll / sleep ──────────────────────────────────────────────────
        // Idle (audio only, no card): wake once a second — just enough to pump
        // mpv and catch input, at a fraction of the previous CPU cost.
        int timeout = 1000;
        if (overlay_alpha > 0.0f && overlay_alpha < 1.0f) timeout = 16;
        else if (overlay_active) timeout = 100;

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
#ifndef _WIN32
    unlink("/dev/shm/tubelite_daemon_audio.live");
    unlink("/dev/shm/tubelite_daemon_state.json");
    unlink("/dev/shm/tubelite_daemon_fadeout");
    // Background playback is over and the app isn't running, so nothing needs
    // the tubed backend — stop it so neither it nor any yt-dlp child lingers.
    {
        std::ifstream tp("/dev/shm/tubed.pid");
        pid_t tpid = 0;
        if (tp) tp >> tpid;
        if (tpid > 0) kill(tpid, SIGTERM);
    }
#endif
    std::cerr << "[daemon] Done.\n";
}

// ── Process management ────────────────────────────────────────────────────────

void killExistingDaemon() {
#ifndef _WIN32
    std::ifstream ifs("/dev/shm/tubelite_daemon.pid");
    if (ifs) {
        pid_t pid = 0; ifs >> pid;
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

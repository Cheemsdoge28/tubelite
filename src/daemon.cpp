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
#include <linux/fb.h>
#include <signal.h>
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
    // Pre-resolved stream URL from the app — avoids yt-dlp re-resolve on startup.
    std::string stream_url;
    std::string subtitle_url;
};

// State variables
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

static float daemon_overlay_timer = 0.0f;

// Framebuffer parameters
#ifndef _WIN32
static int fb_fd = -1;
static uint8_t* fb_ptr = (uint8_t*)MAP_FAILED;
static long int screensize = 0;
static int fb_width = 0;
static int fb_height = 0;
static int fb_bpp = 0;
static int fb_line_len = 0;
static int fb_red_offset = 0;
static int fb_green_offset = 0;
static int fb_blue_offset = 0;
static int fb_alpha_offset = -1;
#else
static int fb_width = 640;
static int fb_height = 480;
static int fb_bpp = 32;
#endif

// Card overlay position (top-left corner of the overlay card drawn on /dev/fb0)
static const int card_x = 160;
static const int card_y = 12;
static const int card_w = 320;
static const int card_h = 76;

// FreeType
static FT_Library ft_lib;
static FT_Face ft_face;
static bool ft_ok = false;

#ifndef _WIN32
struct JoystickEvent {
    uint32_t time;
    int16_t value;
    uint8_t type;
    uint8_t number;
};
#endif

static std::string getAppDataPath(const std::string& filename) {
#ifdef _WIN32
    return filename;
#else
    if (std::filesystem::exists("/roms/tools/tubelite")) {
        return "/roms/tools/tubelite/" + filename;
    }
    return filename;
#endif
}

static std::string formatTime(double s) {
    if (s < 0) s = 0;
    int tot = static_cast<int>(s);
    int h = tot / 3600, m = (tot % 3600) / 60, sec = tot % 60;
    char buf[16];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else       snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
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
                v.id = item.value("id", "");
                v.title = item.value("title", "");
                v.author = item.value("author", "");
                v.duration_seconds = item.value("duration_seconds", 0);
                v.duration_string = item.value("duration_string", "");
                v.stream_url = item.value("stream_url", "");
                v.subtitle_url = item.value("subtitle_url", "");
                daemon_playlist.push_back(v);
            }
        }
        daemon_current_index = j.value("current_index", 0);
        daemon_start_position = j.value("current_position", 0.0);
        return !daemon_playlist.empty();
    } catch (...) {
        return false;
    }
}

static void handleSignal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        daemon_running = false;
    }
}

static bool initFramebuffer() {
#ifndef _WIN32
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        std::cerr << "[daemon] Failed to open /dev/fb0\n";
        return false;
    }
    
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        std::cerr << "[daemon] Failed to get screen info\n";
        close(fb_fd);
        fb_fd = -1;
        return false;
    }
    
    fb_width = vinfo.xres;
    fb_height = vinfo.yres;
    fb_bpp = vinfo.bits_per_pixel;
    fb_line_len = finfo.line_length;
    fb_red_offset = vinfo.red.offset;
    fb_green_offset = vinfo.green.offset;
    fb_blue_offset = vinfo.blue.offset;
    fb_alpha_offset = vinfo.transp.offset;
    
    screensize = vinfo.yres_virtual * finfo.line_length;
    fb_ptr = (uint8_t*)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_ptr == MAP_FAILED) {
        std::cerr << "[daemon] Failed to mmap /dev/fb0\n";
        close(fb_fd);
        fb_fd = -1;
        return false;
    }
    return true;
#else
    return false;
#endif
}

// Wait for vertical blank before drawing to avoid tearing.
// FBIO_WAITFORVSYNC = _IOW('F', 0x20, __u32) — defined in linux/fb.h but
// not always present in older NDKs, so we define it manually if needed.
#ifndef FBIO_WAITFORVSYNC
#  define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif
static void fbWaitVsync() {
#ifndef _WIN32
    if (fb_fd < 0) return;
    uint32_t dummy = 0;
    ioctl(fb_fd, FBIO_WAITFORVSYNC, &dummy);
#endif
}

static void closeFramebuffer() {
#ifndef _WIN32
    if (fb_ptr != MAP_FAILED) {
        munmap(fb_ptr, screensize);
        fb_ptr = (uint8_t*)MAP_FAILED;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
#endif
}

// backupBackground / restoreBackground have been removed.
// The daemon no longer tries to save/restore framebuffer pixels because
// other apps (e.g. EmulationStation) continuously redraw the framebuffer at
// their own rate, making any saved copy immediately stale and causing flicker.
// Instead, the overlay is redrawn on top every loop iteration while it is
// active; when it expires the other app's next redraw naturally erases it.

static void writePixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifndef _WIN32
    if (fb_ptr == (uint8_t*)MAP_FAILED) return;
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
    if (a == 0) return;
    
    int bytes_per_pixel = fb_bpp / 8;
    uint8_t* dst = fb_ptr + y * fb_line_len + x * bytes_per_pixel;
    
    if (fb_bpp == 32) {
        if (a == 255) {
            dst[fb_red_offset/8] = r;
            dst[fb_green_offset/8] = g;
            dst[fb_blue_offset/8] = b;
            if (fb_alpha_offset >= 0) dst[fb_alpha_offset/8] = 255;
        } else {
            uint8_t cur_r = dst[fb_red_offset/8];
            uint8_t cur_g = dst[fb_green_offset/8];
            uint8_t cur_b = dst[fb_blue_offset/8];
            
            dst[fb_red_offset/8] = (r * a + cur_r * (255 - a)) / 255;
            dst[fb_green_offset/8] = (g * a + cur_g * (255 - a)) / 255;
            dst[fb_blue_offset/8] = (b * a + cur_b * (255 - a)) / 255;
            if (fb_alpha_offset >= 0) dst[fb_alpha_offset/8] = 255;
        }
    } else if (fb_bpp == 16) {
        uint16_t* dst16 = (uint16_t*)dst;
        if (a == 255) {
            *dst16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        } else {
            uint16_t cur = *dst16;
            uint8_t cur_r = ((cur >> 11) & 0x1F) << 3;
            uint8_t cur_g = ((cur >> 5) & 0x3F) << 2;
            uint8_t cur_b = (cur & 0x1F) << 3;
            
            uint8_t new_r = (r * a + cur_r * (255 - a)) / 255;
            uint8_t new_g = (g * a + cur_g * (255 - a)) / 255;
            uint8_t new_b = (b * a + cur_b * (255 - a)) / 255;
            
            *dst16 = ((new_r >> 3) << 11) | ((new_g >> 2) << 5) | (new_b >> 3);
        }
    }
#endif
}

static void drawRect(int rx, int ry, int rw, int rh, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = ry; y < ry + rh; ++y) {
        for (int x = rx; x < rx + rw; ++x) {
            writePixel(x, y, r, g, b, a);
        }
    }
}

static void drawRoundedRect(int rx, int ry, int rw, int rh, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = ry; y < ry + rh; ++y) {
        for (int x = rx; x < rx + rw; ++x) {
            int dx = 0;
            if (x < rx + radius) dx = rx + radius - x;
            else if (x >= rx + rw - radius) dx = x - (rx + rw - radius - 1);
            
            int dy = 0;
            if (y < ry + radius) dy = ry + radius - y;
            else if (y >= ry + rh - radius) dy = y - (ry + rh - radius - 1);
            
            if (dx > 0 && dy > 0) {
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > radius) continue;
                else if (dist > radius - 1.0f) {
                    float alpha_factor = radius - dist;
                    writePixel(x, y, r, g, b, static_cast<uint8_t>(a * alpha_factor));
                } else {
                    writePixel(x, y, r, g, b, a);
                }
            } else {
                writePixel(x, y, r, g, b, a);
            }
        }
    }
}

static void initFreetype() {
    if (FT_Init_FreeType(&ft_lib) == 0) {
        std::vector<std::string> paths = {
            "res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "../res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "/roms/tools/tubelite/res/fonts/AtkinsonHyperlegible-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        };
        for (const auto& p : paths) {
            if (std::filesystem::exists(p)) {
                if (FT_New_Face(ft_lib, p.c_str(), 0, &ft_face) == 0) {
                    ft_ok = true;
                    break;
                }
            }
        }
    }
}

static void drawText(const std::string& text, int x, int y, int fontSize, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ft_ok) return;
    FT_Set_Pixel_Sizes(ft_face, 0, fontSize);
    
    int pen_x = x;
    int pen_y = y + fontSize;
    
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t codepoint = static_cast<uint8_t>(text[i]);
        if ((codepoint & 0x80) != 0) {
            if ((codepoint & 0xE0) == 0xC0) {
                if (i + 1 < text.size()) {
                    codepoint = ((codepoint & 0x1F) << 6) | (static_cast<uint8_t>(text[i+1]) & 0x3F);
                    i += 1;
                }
            } else if ((codepoint & 0xF0) == 0xE0) {
                if (i + 2 < text.size()) {
                    codepoint = ((codepoint & 0x0F) << 12) | ((static_cast<uint8_t>(text[i+1]) & 0x3F) << 6) | (static_cast<uint8_t>(text[i+2]) & 0x3F);
                    i += 2;
                }
            }
        }
        
        if (FT_Load_Char(ft_face, codepoint, FT_LOAD_RENDER) != 0) continue;
        
        FT_GlyphSlot glyph = ft_face->glyph;
        int glyph_x = pen_x + glyph->bitmap_left;
        int glyph_y = pen_y - glyph->bitmap_top;
        
        for (unsigned int row = 0; row < glyph->bitmap.rows; ++row) {
            for (unsigned int col = 0; col < glyph->bitmap.width; ++col) {
                uint8_t alpha = glyph->bitmap.buffer[row * glyph->bitmap.pitch + col];
                if (alpha > 0) {
                    uint16_t blend_alpha = (static_cast<uint16_t>(alpha) * a) / 255;
                    writePixel(glyph_x + col, glyph_y + row, r, g, b, blend_alpha);
                }
            }
        }
        pen_x += glyph->advance.x >> 6;
    }
}

static std::string truncateText(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen) return text;
    return text.substr(0, maxLen - 3) + "...";
}

static void playCurrentTrack(MpvPlayer& mpv, YouTubeAPI& yt) {
    if (daemon_current_index < 0 || daemon_current_index >= static_cast<int>(daemon_playlist.size())) return;
    auto& video = daemon_playlist[daemon_current_index];

    daemon_overlay_timer = 5.0f;

    // Fast path: use pre-resolved URL written by the app into daemon_queue.json.
    // This avoids a full yt-dlp re-resolve on startup and gives instant audio.
    if (!video.stream_url.empty()) {
        std::cerr << "[daemon] Using pre-resolved URL for " << video.id << "\n";
        mpv.play(video.stream_url, video.subtitle_url);
        if (daemon_start_position > 0.0) {
            mpv.setPendingSeekPosition(daemon_start_position);
            daemon_start_position = 0.0;
        }
        daemon_status = DaemonStatus::Playing;
        // Clear so next/prev tracks go through the normal resolve path.
        video.stream_url.clear();
        video.subtitle_url.clear();
        return;
    }

    // Slow path: resolve via yt-dlp (for tracks other than the currently-playing one).
    {
        std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
        daemon_status = DaemonStatus::Resolving;
        daemon_request_finished = false;
        daemon_request_success = false;
        daemon_resolved_url = "";
        daemon_subtitle_url = "";
    }

    yt.getStreamUrl(video.id, 360, [](bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& /*meta*/) {
        std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
        daemon_resolved_url = url;
        daemon_subtitle_url = subtitle_url;
        daemon_request_success = success;
        daemon_request_finished = true;
    });
}

static void renderCard(MpvPlayer& mpv) {
    // 1. Drop shadow
    drawRoundedRect(card_x + 3, card_y + 3, card_w, card_h, 8,  0,  0,  0, 90);

    // 2. Red border (matches miniplayer outline in compositor)
    drawRoundedRect(card_x - 1, card_y - 1, card_w + 2, card_h + 2, 9, 255, 48, 48, 200);

    // 3. Dark card fill — same {26,28,32} as compositor miniplayer strip
    drawRoundedRect(card_x, card_y, card_w, card_h, 8, 26, 28, 32, 245);

    // 4. Thin red top accent inside the card
    drawRect(card_x + 1, card_y + 1, card_w - 2, 2, 255, 48, 48, 160);

    // Guard: need valid playlist entry
    if (daemon_current_index < 0 || daemon_current_index >= static_cast<int>(daemon_playlist.size())) return;
    const auto& video = daemon_playlist[daemon_current_index];

    // 5. Title — {240,242,245} near-white (matches compositor title text)
    drawText(truncateText(video.title, 32), card_x + 10, card_y + 8, 13, 240, 242, 245, 255);

    // 6. Author — {154,165,184} muted blue-grey (matches compositor author text)
    drawText(truncateText(video.author, 38), card_x + 10, card_y + 26, 11, 154, 165, 184, 255);

    // 7. Status badge (top-right)
    std::string statStr = "PLAYING";
    uint8_t sr = 64, sg = 214, sb = 96;  // green
    if (daemon_status == DaemonStatus::Resolving) {
        statStr = "LOADING";
        sr = 64;  sg = 148; sb = 255;   // blue
    } else if (daemon_status == DaemonStatus::Paused) {
        statStr = "PAUSED";
        sr = 255; sg = 214; sb = 64;   // yellow
    } else if (daemon_status == DaemonStatus::Error) {
        statStr = "ERROR";
        sr = 255; sg = 48;  sb = 48;   // red
    }
    drawText(statStr, card_x + card_w - 72, card_y + 8, 10, sr, sg, sb, 255);

    // 8. Progress bar — same style as compositor (track + red fill)
    double pos  = mpv.getPlaybackTime();
    double dur  = mpv.getDuration() > 0.0 ? mpv.getDuration() : static_cast<double>(video.duration_seconds);
    double frac = (dur > 0.0) ? std::max(0.0, std::min(1.0, pos / dur)) : 0.0;

    const int barX = card_x + 10;
    const int barY = card_y + 46;
    const int barW = card_w - 20;
    const int barH = 4;

    drawRect(barX, barY, barW, barH, 42, 48, 56, 200);   // track
    if (frac > 0.0)
        drawRect(barX, barY, static_cast<int>(barW * frac), barH, 255, 48, 48, 255); // fill

    // 9. Time — {220,220,232} matches compositor timestamp color
    std::string timeStr = formatTime(pos) + " / " + (video.duration_string.empty() ? formatTime(dur) : video.duration_string);
    drawText(timeStr, card_x + 10, card_y + 55, 9, 220, 220, 232, 255);

    // 10. Key hints — {160,160,172} matches compositor hint text
    drawText("SEL+A:PLAY  SEL+B:EXIT  SEL+\xE2\x96\xB6:NEXT", card_x + card_w - 196, card_y + 55, 9, 160, 160, 172, 255);
}

void runDaemon() {
    std::cerr << "[daemon] Initializing background daemon...\n";
    
    // Kill existing daemon first
    killExistingDaemon();
    
    // Write PID file
    {
        std::ofstream ofs("/dev/shm/tubelite_daemon.pid");
        if (ofs) {
            ofs << getpid() << "\n";
        }
    }
    
    if (!loadDaemonQueue()) {
        std::cerr << "[daemon] Empty playlist queue. Exiting.\n";
        unlink("/dev/shm/tubelite_daemon.pid");
        return;
    }
    
    // Setup signal handlers
#ifndef _WIN32
    struct sigaction sa;
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
#endif

    MpvPlayer mpv;
    if (!mpv.initializeAudioOnly()) {
        std::cerr << "[daemon] MPV initialization failed.\n";
        unlink("/dev/shm/tubelite_daemon.pid");
        return;
    }
    
    YouTubeAPI yt;
    initFreetype();
    initFramebuffer();

    // Show startup overlay immediately so user knows daemon is active.
    // FB is now open; set timer here so the first loop iteration draws the card.
    daemon_overlay_timer = 5.0f;

    // Open controller input
    int js_fd = -1;
#ifndef _WIN32
    js_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
    if (js_fd < 0) {
        std::cerr << "[daemon] Warning: Could not open /dev/input/js0\n";
    }
#endif

    // Start playing
    playCurrentTrack(mpv, yt);
    
    auto last_tick = std::chrono::steady_clock::now();
    bool select_held = false;
    bool overlay_visible = false;
    
    std::cerr << "[daemon] Daemon loop started.\n";
    
    while (daemon_running) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;
        
        // Update mpv events
        mpv.update();
        
        // Resolve tracks asynchronously
        if (daemon_status == DaemonStatus::Resolving) {
            bool finished = false;
            bool success = false;
            std::string url, sub_url;
            {
                std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
                finished = daemon_request_finished;
                success = daemon_request_success;
                url = daemon_resolved_url;
                sub_url = daemon_subtitle_url;
            }
            if (finished) {
                if (success) {
                    mpv.play(url, sub_url);
                    if (daemon_start_position > 0.0) {
                        mpv.setPendingSeekPosition(daemon_start_position);
                        daemon_start_position = 0.0;
                    }
                    daemon_status = DaemonStatus::Playing;
                } else {
                    daemon_status = DaemonStatus::Error;
                }
            }
        }
        
        // Handle track end
        if (daemon_status == DaemonStatus::Playing && mpv.checkAndClearEnded()) {
            daemon_current_index = (daemon_current_index + 1) % daemon_playlist.size();
            playCurrentTrack(mpv, yt);
        }
        
        // Read joystick events
#ifndef _WIN32
        if (js_fd >= 0) {
            JoystickEvent ev;
            while (read(js_fd, &ev, sizeof(ev)) > 0) {
                // Strip the JS_EVENT_INIT flag so init-state events
                // are treated the same as real events.
                uint8_t ev_type = ev.type & ~0x80;

                if (ev_type == 1) { // JS_EVENT_BUTTON
                    bool down = (ev.value != 0);
                    // SELECT button — common mappings across ArkOS RG351 variants:
                    //   js button 6  (most RG351 ArkOS builds)
                    //   js button 8  (some gamepad remaps)
                    //   js button 12 / 16 (legacy)
                    if (ev.number == 6 || ev.number == 8 ||
                        ev.number == 12 || ev.number == 16) {
                        select_held = down;
                    }
                    if (down) {
                        if (select_held) {
                            if (ev.number == 0 || ev.number == 1) { // A (east) -> Play/Pause
                                if (daemon_status == DaemonStatus::Playing) {
                                    mpv.pause();
                                    daemon_status = DaemonStatus::Paused;
                                } else if (daemon_status == DaemonStatus::Paused) {
                                    mpv.resume();
                                    daemon_status = DaemonStatus::Playing;
                                }
                                daemon_overlay_timer = 5.0f;
                            } else if (ev.number == 1 || ev.number == 2) { // B (south) -> Exit
                                daemon_running = false;
                            } else if (ev.number == 5) { // R1 -> Next
                                daemon_current_index = (daemon_current_index + 1) % daemon_playlist.size();
                                playCurrentTrack(mpv, yt);
                            } else if (ev.number == 4) { // L1 -> Prev
                                daemon_current_index = (daemon_current_index - 1 + daemon_playlist.size()) % daemon_playlist.size();
                                playCurrentTrack(mpv, yt);
                            }
                        }
                        // Show overlay on any button press (convenience)
                        daemon_overlay_timer = 5.0f;
                    }
                } else if (ev_type == 2) { // JS_EVENT_AXIS — DPAD on RG351MP
                    // On RG351MP with ArkOS, the D-pad is reported as hat axes:
                    //   Axis 6: horizontal  (-32767=left,  0=center, +32767=right)
                    //   Axis 7: vertical    (-32767=up,    0=center, +32767=down)
                    // Threshold to avoid accidental analog-stick triggering:
                    const int16_t DPAD_THRESH = 16384;

                    // Only act if SELECT is held
                    if (select_held && std::abs(ev.value) >= DPAD_THRESH) {
                        if (ev.number == 6) { // Horizontal hat
                            if (ev.value > 0) { // DPAD Right -> Next
                                daemon_current_index = (daemon_current_index + 1) % daemon_playlist.size();
                                playCurrentTrack(mpv, yt);
                            } else { // DPAD Left -> Prev
                                daemon_current_index = (daemon_current_index - 1 + daemon_playlist.size()) % daemon_playlist.size();
                                playCurrentTrack(mpv, yt);
                            }
                        } else if (ev.number == 7) { // Vertical hat
                            if (ev.value < 0) { // DPAD Up -> Show overlay
                                daemon_overlay_timer = 5.0f;
                            } else { // DPAD Down -> Play/Pause toggle
                                if (daemon_status == DaemonStatus::Playing) {
                                    mpv.pause();
                                    daemon_status = DaemonStatus::Paused;
                                } else if (daemon_status == DaemonStatus::Paused) {
                                    mpv.resume();
                                    daemon_status = DaemonStatus::Playing;
                                }
                                daemon_overlay_timer = 5.0f;
                            }
                        }
                    } else if (std::abs(ev.value) >= DPAD_THRESH) {
                        // Any DPAD movement without SELECT -> just show overlay
                        daemon_overlay_timer = 5.0f;
                    }
                }
            }
        }
#endif

        // Overlay management: redraw unconditionally every tick while the timer
        // is active (spam-mode). We don't attempt save/restore — ES redraws
        // continuously and will erase the card whenever overlay is gone.
        if (daemon_overlay_timer > 0.0f) {
            daemon_overlay_timer -= dt;
            overlay_visible = true;
            fbWaitVsync();
            renderCard(mpv);   // unconditional — win the FB race every frame
        } else {
            overlay_visible = false;  // ES next frame will paint over naturally
        }

        // Adaptive sleep: go full crackhead (4 ms) while card is on screen,
        // drop to 50 ms when hidden to save CPU in the idle case.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(overlay_visible ? 4 : 50));
    }
    
    std::cerr << "[daemon] Stopping daemon...\n";
    
    mpv.shutdown();
    closeFramebuffer();
#ifndef _WIN32
    if (js_fd >= 0) close(js_fd);
#endif
    unlink("/dev/shm/tubelite_daemon.pid");
    std::cerr << "[daemon] Daemon stopped.\n";
}

void killExistingDaemon() {
#ifndef _WIN32
    std::ifstream ifs("/dev/shm/tubelite_daemon.pid");
    if (ifs) {
        pid_t pid = 0;
        ifs >> pid;
        if (pid > 0) {
            std::cerr << "[daemon] Terminating existing daemon with PID " << pid << "\n";
            kill(pid, SIGTERM);
            // Wait up to 500ms for clean termination and framebuffer restoration
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
        if (std::filesystem::exists("/proc/self/exe")) {
            exec_path = std::filesystem::read_symlink("/proc/self/exe").string();
        }
    } catch (...) {
        // Fallback to "./tubelite"
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[daemon] fork failed\n";
        return;
    }
    if (pid > 0) {
        // Parent process
        return;
    }
    
    // First child process daemonizes
    if (setsid() < 0) exit(1);
    
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0); // Exit first child
    
    // Close standard files
    close(0);
    close(1);
    close(2);
    
    open("/dev/null", O_RDONLY); // stdin
    
    int log_fd = open("tubelite_daemon.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, 1); // stdout
        dup2(log_fd, 2); // stderr
        close(log_fd);
    } else {
        open("/dev/null", O_WRONLY); // stdout
        open("/dev/null", O_WRONLY); // stderr
    }
    
    char* args[] = { (char*)exec_path.c_str(), (char*)"--daemon", nullptr };
    execvp(args[0], args);
    exit(1);
#endif
}

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
#include <algorithm>
#include <cctype>

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

enum class OverlayState { Closed, FadingIn, Open, FadingOut };
static OverlayState overlay_state = OverlayState::Closed;
static float overlay_fade_progress = 0.0f; // 0.0 to 1.0
static int overlay_selection_index = 0;
static const float FADE_DURATION = 0.2f; // 200ms

static uint8_t* original_screenshot_buffer = nullptr;
static uint8_t* blurred_screenshot_buffer = nullptr;
static uint8_t* temp_blur_buffer = nullptr;
static long int screenshot_buffer_size = 0;

static std::vector<pid_t> suspended_pids;

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

// Full-screen overlay layout constants (for a 640x480 display)
// Overlay card dimensions
static const int OVL_MARGIN  = 24;
static const int OVL_HEADER_H = 44;
static const int OVL_FOOTER_H = 36;
static const int OVL_CARD_H   = 86;
static const int OVL_QUEUE_ROW = 26; // height per queue row

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
    if (original_screenshot_buffer) { delete[] original_screenshot_buffer; original_screenshot_buffer = nullptr; }
    if (blurred_screenshot_buffer) { delete[] blurred_screenshot_buffer; blurred_screenshot_buffer = nullptr; }
    if (temp_blur_buffer) { delete[] temp_blur_buffer; temp_blur_buffer = nullptr; }
    screenshot_buffer_size = 0;
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

static const std::vector<std::string> SUSPEND_NAMES = {
    "emulationstatio",
    "retroarch",
    "ppsspp",
    "pico8",
    "openbor",
    "dosbox",
    "flycast",
    "drastic",
    "mupen64plus",
    "recast",
    "scummvm",
    "love",
    "mono",
    "pcsx",
    "pcsxrearmed",
    "duckstation"
};

static std::vector<pid_t> findProcessesToSuspend() {
    std::vector<pid_t> pids;
#ifndef _WIN32
    pid_t my_pid = getpid();
    
    try {
        for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
            if (!entry.is_directory()) continue;
            std::string filename = entry.path().filename().string();
            
            bool is_pid = !filename.empty() && std::all_of(filename.begin(), filename.end(), ::isdigit);
            if (!is_pid) continue;
            
            pid_t pid = std::stoi(filename);
            if (pid == my_pid) continue;
            
            // Read comm name
            std::string comm;
            std::ifstream comm_file("/proc/" + filename + "/comm");
            if (comm_file >> comm) {
                bool match = false;
                for (const auto& name : SUSPEND_NAMES) {
                    if (comm == name) {
                        match = true;
                        break;
                    }
                }
                if (match) {
                    pids.push_back(pid);
                    continue;
                }
            }
            
            // Check open file descriptors for display devices
            try {
                std::string fd_dir = "/proc/" + filename + "/fd";
                if (std::filesystem::exists(fd_dir)) {
                    for (const auto& fd_entry : std::filesystem::directory_iterator(fd_dir)) {
                        if (fd_entry.is_symlink()) {
                            std::string target = std::filesystem::read_symlink(fd_entry.path()).string();
                            if (target == "/dev/dri/card0" || target == "/dev/fb0") {
                                pids.push_back(pid);
                                break;
                            }
                        }
                    }
                }
            } catch (...) {}
        }
    } catch (...) {}
#endif
    return pids;
}

static void suspendForegroundApps() {
#ifndef _WIN32
    suspended_pids = findProcessesToSuspend();
    for (pid_t pid : suspended_pids) {
        std::cerr << "[daemon] Suspending PID " << pid << "\n";
        kill(pid, SIGSTOP);
    }
#endif
}

static void resumeForegroundApps() {
#ifndef _WIN32
    for (pid_t pid : suspended_pids) {
        std::cerr << "[daemon] Resuming PID " << pid << "\n";
        kill(pid, SIGCONT);
    }
    suspended_pids.clear();
#endif
}

static void initScreenshotBuffers() {
#ifndef _WIN32
    if (screenshot_buffer_size != screensize) {
        if (original_screenshot_buffer) delete[] original_screenshot_buffer;
        if (blurred_screenshot_buffer) delete[] blurred_screenshot_buffer;
        if (temp_blur_buffer) delete[] temp_blur_buffer;
        
        screenshot_buffer_size = screensize;
        original_screenshot_buffer = new uint8_t[screenshot_buffer_size];
        blurred_screenshot_buffer = new uint8_t[screenshot_buffer_size];
        temp_blur_buffer = new uint8_t[screenshot_buffer_size];
    }
#endif
}

static void captureAndPrepareScreenshot() {
#ifndef _WIN32
    initScreenshotBuffers();
    if (!original_screenshot_buffer || !blurred_screenshot_buffer) return;
    
    // Copy current framebuffer
    std::memcpy(original_screenshot_buffer, fb_ptr, screensize);
    
    // Perform 3x3 separable box blur + 20% dimming (multiply color channels by 0.8)
    if (fb_bpp == 32) {
        // Horizontal pass: original -> temp
        for (int y = 0; y < fb_height; ++y) {
            for (int x = 0; x < fb_width; ++x) {
                int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx;
                    if (nx >= 0 && nx < fb_width) {
                        uint8_t* p = original_screenshot_buffer + y * fb_line_len + nx * 4;
                        r_sum += p[fb_red_offset/8];
                        g_sum += p[fb_green_offset/8];
                        b_sum += p[fb_blue_offset/8];
                        count++;
                    }
                }
                uint8_t* out = temp_blur_buffer + y * fb_line_len + x * 4;
                out[fb_red_offset/8] = r_sum / count;
                out[fb_green_offset/8] = g_sum / count;
                out[fb_blue_offset/8] = b_sum / count;
                if (fb_alpha_offset >= 0) out[fb_alpha_offset/8] = 255;
            }
        }
        
        // Vertical pass: temp -> blurred (with dimming)
        for (int x = 0; x < fb_width; ++x) {
            for (int y = 0; y < fb_height; ++y) {
                int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = y + dy;
                    if (ny >= 0 && ny < fb_height) {
                        uint8_t* p = temp_blur_buffer + ny * fb_line_len + x * 4;
                        r_sum += p[fb_red_offset/8];
                        g_sum += p[fb_green_offset/8];
                        b_sum += p[fb_blue_offset/8];
                        count++;
                    }
                }
                uint8_t* out = blurred_screenshot_buffer + y * fb_line_len + x * 4;
                // Dimming by 20% (multiplied by 8/10)
                out[fb_red_offset/8] = ((r_sum / count) * 8) / 10;
                out[fb_green_offset/8] = ((g_sum / count) * 8) / 10;
                out[fb_blue_offset/8] = ((b_sum / count) * 8) / 10;
                if (fb_alpha_offset >= 0) out[fb_alpha_offset/8] = 255;
            }
        }
    } else if (fb_bpp == 16) {
        int stride = fb_line_len / 2;
        uint16_t* src = (uint16_t*)original_screenshot_buffer;
        uint16_t* temp = (uint16_t*)temp_blur_buffer;
        uint16_t* dst = (uint16_t*)blurred_screenshot_buffer;
        
        // Horizontal pass
        for (int y = 0; y < fb_height; ++y) {
            for (int x = 0; x < fb_width; ++x) {
                int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx;
                    if (nx >= 0 && nx < fb_width) {
                        uint16_t p = src[y * stride + nx];
                        r_sum += (p >> 11) & 0x1F;
                        g_sum += (p >> 5) & 0x3F;
                        b_sum += p & 0x1F;
                        count++;
                    }
                }
                temp[y * stride + x] = ((r_sum / count) << 11) | ((g_sum / count) << 5) | (b_sum / count);
            }
        }
        
        // Vertical pass (with dimming)
        for (int x = 0; x < fb_width; ++x) {
            for (int y = 0; y < fb_height; ++y) {
                int r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = y + dy;
                    if (ny >= 0 && ny < fb_height) {
                        uint16_t p = temp[ny * stride + x];
                        r_sum += (p >> 11) & 0x1F;
                        g_sum += (p >> 5) & 0x3F;
                        b_sum += p & 0x1F;
                        count++;
                    }
                }
                uint8_t r = ((r_sum / count) * 8) / 10;
                uint8_t g = ((g_sum / count) * 8) / 10;
                uint8_t b = ((b_sum / count) * 8) / 10;
                dst[y * stride + x] = (r << 11) | (g << 5) | b;
            }
        }
    }
#endif
}

static void drawTransitionBackground(float progress) {
#ifndef _WIN32
    if (!original_screenshot_buffer || !blurred_screenshot_buffer) return;
    if (progress >= 1.0f) {
        std::memcpy(fb_ptr, blurred_screenshot_buffer, screensize);
        return;
    }
    if (progress <= 0.0f) {
        std::memcpy(fb_ptr, original_screenshot_buffer, screensize);
        return;
    }
    
    int factor = static_cast<int>(progress * 256.0f);
    int inv_factor = 256 - factor;
    
    if (fb_bpp == 32) {
        uint8_t* src_orig = original_screenshot_buffer;
        uint8_t* src_blur = blurred_screenshot_buffer;
        uint8_t* dst = fb_ptr;
        for (long int i = 0; i < screensize; i += 4) {
            dst[i + fb_red_offset/8]   = (src_orig[i + fb_red_offset/8]   * inv_factor + src_blur[i + fb_red_offset/8]   * factor) >> 8;
            dst[i + fb_green_offset/8] = (src_orig[i + fb_green_offset/8] * inv_factor + src_blur[i + fb_green_offset/8] * factor) >> 8;
            dst[i + fb_blue_offset/8]  = (src_orig[i + fb_blue_offset/8]  * inv_factor + src_blur[i + fb_blue_offset/8]  * factor) >> 8;
            if (fb_alpha_offset >= 0) dst[i + fb_alpha_offset/8] = 255;
        }
    } else if (fb_bpp == 16) {
        uint16_t* src_orig = (uint16_t*)original_screenshot_buffer;
        uint16_t* src_blur = (uint16_t*)blurred_screenshot_buffer;
        uint16_t* dst = (uint16_t*)fb_ptr;
        long int num_pixels = screensize / 2;
        for (long int i = 0; i < num_pixels; ++i) {
            uint16_t o = src_orig[i];
            uint16_t b = src_blur[i];
            
            uint8_t r = (((o >> 11) & 0x1F) * inv_factor + ((b >> 11) & 0x1F) * factor) >> 8;
            uint8_t g = (((o >> 5) & 0x3F) * inv_factor + ((b >> 5) & 0x3F) * factor) >> 8;
            uint8_t bl = ((o & 0x1F) * inv_factor + (b & 0x1F) * factor) >> 8;
            
            dst[i] = (r << 11) | (g << 5) | bl;
        }
    }
#endif
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
        uint32_t codepoint = text[i];
        if ((codepoint & 0x80) != 0) {
            if ((codepoint & 0xE0) == 0xC0) {
                if (i + 1 < text.size()) {
                    codepoint = ((codepoint & 0x1F) << 6) | (text[i+1] & 0x3F);
                    i += 1;
                }
            } else if ((codepoint & 0xF0) == 0xE0) {
                if (i + 2 < text.size()) {
                    codepoint = ((codepoint & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F);
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

    // Fast path: use pre-resolved URL written by the app into daemon_queue.json.
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

    // Slow path: resolve via yt-dlp.
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

// renderOverlay: draws the full interactive overlay on top of the blurred screenshot.
// The background has already been written by drawTransitionBackground().
// alpha_scale [0..255] fades the entire overlay UI in/out during transitions.
static void renderOverlay(MpvPlayer& mpv, uint8_t alpha_scale) {
    if (daemon_playlist.empty()) return;

    int W = fb_width;
    int H = fb_height;
    int inner_w = W - OVL_MARGIN * 2;

    // ── Header bar ───────────────────────────────────────────────────────────
    drawRect(0, 0, W, OVL_HEADER_H, 10, 12, 16, static_cast<uint8_t>(210 * alpha_scale / 255));
    // Red left accent
    drawRect(0, 0, 3, OVL_HEADER_H, 255, 48, 48, alpha_scale);
    // Bottom separator
    drawRect(0, OVL_HEADER_H - 1, W, 1, 255, 48, 48, static_cast<uint8_t>(80 * alpha_scale / 255));
    drawText("TubeLite", OVL_MARGIN, 10, 20, 255, 52, 52, alpha_scale);
    drawText("Background Audio", OVL_MARGIN + 106, 16, 11, 160, 160, 180, static_cast<uint8_t>(180 * alpha_scale / 255));

    // ── Currently playing card ────────────────────────────────────────────────
    const auto& cur = daemon_playlist[daemon_current_index];
    int cx = OVL_MARGIN;
    int cy = OVL_HEADER_H + 10;
    int cw = inner_w;
    int ch = OVL_CARD_H;

    // Card shadow
    drawRoundedRect(cx + 2, cy + 2, cw, ch, 8, 0, 0, 0, static_cast<uint8_t>(80 * alpha_scale / 255));
    // Card base
    drawRoundedRect(cx, cy, cw, ch, 8, 18, 20, 26, static_cast<uint8_t>(230 * alpha_scale / 255));
    // Red top accent
    drawRect(cx, cy, cw, 3, 255, 48, 48, alpha_scale);

    // Status badge
    std::string statStr = "PLAYING";
    uint8_t sr = 64, sg = 214, sb = 96;
    if (daemon_status == DaemonStatus::Resolving) { statStr = "RESOLVING"; sr = 64; sg = 148; sb = 255; }
    else if (daemon_status == DaemonStatus::Paused)  { statStr = "PAUSED";    sr = 255; sg = 214; sb = 64;  }
    else if (daemon_status == DaemonStatus::Error)   { statStr = "ERROR";     sr = 255; sg = 48;  sb = 48;  }
    drawText(statStr, cx + cw - 80, cy + 8, 10, sr, sg, static_cast<uint8_t>(sb * alpha_scale / 255), alpha_scale);

    // Title and author
    drawText(truncateText(cur.title,  52), cx + 10, cy + 6,  14, 255, 255, 255, alpha_scale);
    drawText(truncateText(cur.author, 60), cx + 10, cy + 26, 11, 140, 140, 158, static_cast<uint8_t>(220 * alpha_scale / 255));

    // Progress bar
    double pos = mpv.getPlaybackTime();
    double dur = mpv.getDuration() > 0.0 ? mpv.getDuration() : static_cast<double>(cur.duration_seconds);
    double frac = (dur > 0.0) ? std::max(0.0, std::min(1.0, pos / dur)) : 0.0;
    int barX = cx + 10, barY = cy + 46, barW = cw - 20, barH = 5;
    drawRect(barX, barY, barW, barH, 42, 48, 56, static_cast<uint8_t>(180 * alpha_scale / 255));
    if (frac > 0.0)
        drawRect(barX, barY, static_cast<int>(barW * frac), barH, 255, 48, 48, alpha_scale);

    // Timestamps
    std::string timeStr = formatTime(pos) + " / " + (cur.duration_string.empty() ? formatTime(dur) : cur.duration_string);
    drawText(timeStr, cx + 10, cy + 57, 10, 180, 180, 190, static_cast<uint8_t>(200 * alpha_scale / 255));

    // ── Queue list ────────────────────────────────────────────────────────────
    int qx = OVL_MARGIN;
    int qy = cy + ch + 12;
    int qw = inner_w;
    int queue_area_h = H - OVL_FOOTER_H - qy - 4;
    int visible_rows = queue_area_h / OVL_QUEUE_ROW;
    if (visible_rows < 1) visible_rows = 1;

    // Compute scroll so selected row stays visible
    int n = static_cast<int>(daemon_playlist.size());
    int scroll_top = 0;
    if (overlay_selection_index >= visible_rows)
        scroll_top = overlay_selection_index - visible_rows + 1;
    scroll_top = std::max(0, std::min(scroll_top, n - visible_rows));

    // Queue background panel
    drawRoundedRect(qx, qy, qw, queue_area_h, 6, 12, 14, 18, static_cast<uint8_t>(200 * alpha_scale / 255));
    // Queue top separator
    drawRect(qx, qy, qw, 1, 255, 48, 48, static_cast<uint8_t>(60 * alpha_scale / 255));

    // Queue label
    drawText("QUEUE", qx + 10, qy + 4, 9, 100, 108, 130, static_cast<uint8_t>(180 * alpha_scale / 255));

    for (int i = 0; i < visible_rows && (scroll_top + i) < n; ++i) {
        int idx = scroll_top + i;
        int ry = qy + 16 + i * OVL_QUEUE_ROW;
        const auto& v = daemon_playlist[idx];

        bool is_playing  = (idx == daemon_current_index);
        bool is_selected = (idx == overlay_selection_index);

        // Row highlight
        if (is_selected && !is_playing)
            drawRect(qx + 4, ry, qw - 8, OVL_QUEUE_ROW - 2, 38, 44, 56, static_cast<uint8_t>(200 * alpha_scale / 255));
        else if (is_playing)
            drawRect(qx + 4, ry, qw - 8, OVL_QUEUE_ROW - 2, 60, 12, 12, static_cast<uint8_t>(180 * alpha_scale / 255));

        // Track number
        char num[4]; snprintf(num, sizeof(num), "%2d", idx + 1);
        uint8_t num_r = is_playing ? 255 : 80,
                num_g = is_playing ? 48  : 88,
                num_b = is_playing ? 48  : 110;
        drawText(num, qx + 8, ry + 6, 10, num_r, num_g, num_b, alpha_scale);

        // Title
        std::string row_title = truncateText(v.title, 58);
        uint8_t tr = is_playing ? 255 : (is_selected ? 240 : 200);
        uint8_t tg = is_playing ? 210 : (is_selected ? 240 : 200);
        uint8_t tb = is_playing ? 210 : (is_selected ? 240 : 200);
        drawText(row_title, qx + 30, ry + 6, 11, tr, tg, tb, alpha_scale);

        // Duration on the right
        if (!v.duration_string.empty())
            drawText(v.duration_string, qx + qw - 50, ry + 6, 10, 100, 108, 130, static_cast<uint8_t>(180 * alpha_scale / 255));

        // Playing indicator
        if (is_playing) {
            drawRect(qx + 4, ry + (OVL_QUEUE_ROW/2) - 4, 3, 8, 255, 48, 48, alpha_scale);
        }
    }

    // Scrollbar
    if (n > visible_rows) {
        int sb_h = std::max(6, queue_area_h * visible_rows / n);
        int sb_range = queue_area_h - sb_h;
        int sb_y = qy + (n > 1 ? (scroll_top * sb_range / (n - visible_rows)) : 0);
        drawRect(qx + qw - 5, qy, 3, queue_area_h, 30, 34, 44, static_cast<uint8_t>(160 * alpha_scale / 255));
        drawRect(qx + qw - 5, sb_y, 3, sb_h, 255, 48, 48, static_cast<uint8_t>(200 * alpha_scale / 255));
    }

    // ── Footer hint bar ───────────────────────────────────────────────────────
    int fy = H - OVL_FOOTER_H;
    drawRect(0, fy, W, OVL_FOOTER_H, 10, 12, 16, static_cast<uint8_t>(220 * alpha_scale / 255));
    drawRect(0, fy, W, 1, 255, 48, 48, static_cast<uint8_t>(60 * alpha_scale / 255));
    drawText("A:PLAY  B:CLOSE  L/R:VOL  SEL+B:EXIT",
             OVL_MARGIN, fy + 11, 10, 130, 138, 160, static_cast<uint8_t>(210 * alpha_scale / 255));
}

void runDaemon() {
    std::cerr << "[daemon] Initializing background daemon...\n";

    killExistingDaemon();

    {
        std::ofstream ofs("/dev/shm/tubelite_daemon.pid");
        if (ofs) ofs << getpid() << "\n";
    }

    if (!loadDaemonQueue()) {
        std::cerr << "[daemon] Empty playlist queue. Exiting.\n";
        unlink("/dev/shm/tubelite_daemon.pid");
        return;
    }

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

    int js_fd = -1;
#ifndef _WIN32
    js_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
    if (js_fd < 0)
        std::cerr << "[daemon] Warning: Could not open /dev/input/js0\n";
#endif

    playCurrentTrack(mpv, yt);

    auto last_tick = std::chrono::steady_clock::now();
    bool select_held = false;
    overlay_state = OverlayState::Closed;
    overlay_fade_progress = 0.0f;
    overlay_selection_index = daemon_current_index;

    std::cerr << "[daemon] Daemon loop started.\n";

    while (daemon_running) {
        auto now_tp = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now_tp - last_tick).count();
        if (dt > 0.1f) dt = 0.1f; // clamp large gaps
        last_tick = now_tp;

        // ── MPV event pump ────────────────────────────────────────────────────
        mpv.update();

        // ── Async URL resolution ──────────────────────────────────────────────
        if (daemon_status == DaemonStatus::Resolving) {
            bool finished = false, success = false;
            std::string url, sub_url;
            {
                std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
                finished = daemon_request_finished;
                success  = daemon_request_success;
                url      = daemon_resolved_url;
                sub_url  = daemon_subtitle_url;
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

        // ── Track auto-advance ────────────────────────────────────────────────
        if (daemon_status == DaemonStatus::Playing && mpv.checkAndClearEnded()) {
            daemon_current_index = (daemon_current_index + 1) % static_cast<int>(daemon_playlist.size());
            if (overlay_state == OverlayState::Open || overlay_state == OverlayState::FadingIn)
                overlay_selection_index = daemon_current_index;
            playCurrentTrack(mpv, yt);
        }

        // ── Controller input ──────────────────────────────────────────────────
#ifndef _WIN32
        if (js_fd >= 0) {
            JoystickEvent ev;
            while (read(js_fd, &ev, sizeof(ev)) > 0) {
                uint8_t ev_type = ev.type & ~0x80;
                int n_tracks = static_cast<int>(daemon_playlist.size());

                if (ev_type == 1) { // JS_EVENT_BUTTON
                    bool down = (ev.value != 0);

                    // Track SELECT held state
                    if (ev.number == 6 || ev.number == 8 ||
                        ev.number == 12 || ev.number == 16) {
                        select_held = down;
                    }

                    if (down) {
                        if (overlay_state == OverlayState::Open) {
                            // ── In-overlay controls ───────────────────────────
                            if (ev.number == 0 || ev.number == 1) {
                                // A -> play selected track
                                daemon_current_index = overlay_selection_index;
                                playCurrentTrack(mpv, yt);
                            } else if (ev.number == 2 || ev.number == 3) {
                                // B -> close overlay (FadeOut)
                                overlay_state = OverlayState::FadingOut;
                            } else if (ev.number == 5) {
                                // R1 -> next
                                daemon_current_index = (daemon_current_index + 1) % n_tracks;
                                overlay_selection_index = daemon_current_index;
                                playCurrentTrack(mpv, yt);
                            } else if (ev.number == 4) {
                                // L1 -> previous
                                daemon_current_index = (daemon_current_index - 1 + n_tracks) % n_tracks;
                                overlay_selection_index = daemon_current_index;
                                playCurrentTrack(mpv, yt);
                            } else if (select_held && (ev.number == 2 || ev.number == 3)) {
                                // SELECT + B -> exit daemon
                                daemon_running = false;
                            }
                        } else if (overlay_state == OverlayState::Closed) {
                            // SELECT + B while closed -> exit daemon
                            if (select_held && (ev.number == 2 || ev.number == 3)) {
                                daemon_running = false;
                            }
                        }
                    }
                } else if (ev_type == 2) { // JS_EVENT_AXIS (D-pad as hat on RG351)
                    const int16_t DPAD_THRESH = 16384;
                    bool big = std::abs(ev.value) >= DPAD_THRESH;
                    int n_tracks = static_cast<int>(daemon_playlist.size());

                    if (overlay_state == OverlayState::Closed && ev.number == 7 && ev.value < -DPAD_THRESH && select_held) {
                        // SELECT + DPAD_UP -> open overlay
                        overlay_state = OverlayState::FadingIn;
                        overlay_selection_index = daemon_current_index;
                    } else if (overlay_state == OverlayState::Open && big) {
                        if (ev.number == 7) {
                            if (ev.value < 0) { // DPAD_UP
                                overlay_selection_index = (overlay_selection_index - 1 + n_tracks) % n_tracks;
                            } else { // DPAD_DOWN
                                overlay_selection_index = (overlay_selection_index + 1) % n_tracks;
                            }
                        } else if (ev.number == 6) {
                            // DPAD_LEFT/RIGHT -> volume
                            if (ev.value < 0) mpv.setVolume(std::max(0,   (int)mpv.getPropertyInt("volume") - 5));
                            else              mpv.setVolume(std::min(130,  (int)mpv.getPropertyInt("volume") + 5));
                        }
                    }
                }
            }
        }
#endif

        // ── Overlay state machine ─────────────────────────────────────────────
#ifndef _WIN32
        if (overlay_state == OverlayState::FadingIn) {
            if (overlay_fade_progress == 0.0f) {
                // First frame of fade-in: capture screenshot and suspend apps
                captureAndPrepareScreenshot();
                suspendForegroundApps();
                std::cerr << "[daemon] Overlay opening.\n";
            }
            overlay_fade_progress += dt / FADE_DURATION;
            if (overlay_fade_progress >= 1.0f) {
                overlay_fade_progress = 1.0f;
                overlay_state = OverlayState::Open;
            }
            fbWaitVsync();
            drawTransitionBackground(overlay_fade_progress);
            uint8_t alpha = static_cast<uint8_t>(overlay_fade_progress * 255.0f);
            renderOverlay(mpv, alpha);

        } else if (overlay_state == OverlayState::Open) {
            // Fully open: re-draw the blurred background every frame then the UI
            fbWaitVsync();
            std::memcpy(fb_ptr, blurred_screenshot_buffer ? blurred_screenshot_buffer : fb_ptr, screensize);
            renderOverlay(mpv, 255);

        } else if (overlay_state == OverlayState::FadingOut) {
            overlay_fade_progress -= dt / FADE_DURATION;
            if (overlay_fade_progress <= 0.0f) {
                overlay_fade_progress = 0.0f;
                overlay_state = OverlayState::Closed;
                resumeForegroundApps();
                // Restore original framebuffer so app paints over from clean state
                if (original_screenshot_buffer)
                    std::memcpy(fb_ptr, original_screenshot_buffer, screensize);
                std::cerr << "[daemon] Overlay closed.\n";
            } else {
                fbWaitVsync();
                drawTransitionBackground(overlay_fade_progress);
                uint8_t alpha = static_cast<uint8_t>(overlay_fade_progress * 255.0f);
                renderOverlay(mpv, alpha);
            }
        }
        // OverlayState::Closed: do nothing (framebuffer belongs to the foreground app)
#endif

        // Sleep to target ~50 Hz loop when open, ~10 Hz when closed to be gentle
        int sleep_ms = (overlay_state == OverlayState::Closed) ? 100 : 20;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }

    // Clean shutdown
    if (overlay_state != OverlayState::Closed) {
        resumeForegroundApps();
        if (original_screenshot_buffer)
            std::memcpy(fb_ptr, original_screenshot_buffer, screensize);
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

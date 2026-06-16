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
static bool daemon_needs_redraw = false;

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

// Background backup
static const int card_x = 160;
static const int card_y = 12;
static const int card_w = 320;
static const int card_h = 76;
static std::vector<uint8_t> bg_backup;

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

static void backupBackground() {
#ifndef _WIN32
    if (fb_ptr == (uint8_t*)MAP_FAILED) return;
    int bytes_per_pixel = fb_bpp / 8;
    bg_backup.resize(card_w * card_h * bytes_per_pixel);
    
    for (int y = 0; y < card_h; ++y) {
        int fb_y = card_y + y;
        if (fb_y < 0 || fb_y >= fb_height) continue;
        uint8_t* src = fb_ptr + fb_y * fb_line_len + card_x * bytes_per_pixel;
        uint8_t* dst = bg_backup.data() + y * card_w * bytes_per_pixel;
        std::memcpy(dst, src, card_w * bytes_per_pixel);
    }
#endif
}

static void restoreBackground() {
#ifndef _WIN32
    if (fb_ptr == (uint8_t*)MAP_FAILED || bg_backup.empty()) return;
    int bytes_per_pixel = fb_bpp / 8;
    
    for (int y = 0; y < card_h; ++y) {
        int fb_y = card_y + y;
        if (fb_y < 0 || fb_y >= fb_height) continue;
        uint8_t* dst = fb_ptr + fb_y * fb_line_len + card_x * bytes_per_pixel;
        uint8_t* src = bg_backup.data() + y * card_w * bytes_per_pixel;
        std::memcpy(dst, src, card_w * bytes_per_pixel);
    }
#endif
}

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
    
    {
        std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
        daemon_status = DaemonStatus::Resolving;
        daemon_request_finished = false;
        daemon_request_success = false;
        daemon_resolved_url = "";
        daemon_subtitle_url = "";
    }
    
    daemon_overlay_timer = 5.0f;
    daemon_needs_redraw = true;
    
    yt.getStreamUrl(video.id, 360, [](bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& /*meta*/) {
        std::lock_guard<std::mutex> lock(daemon_resolved_mutex);
        daemon_resolved_url = url;
        daemon_subtitle_url = subtitle_url;
        daemon_request_success = success;
        daemon_request_finished = true;
    });
}

static void renderCard(MpvPlayer& mpv) {
    // 1. Draw shadow
    drawRoundedRect(card_x + 2, card_y + 2, card_w, card_h, 8, 0, 0, 0, 80);
    
    // 2. Draw card base
    drawRoundedRect(card_x, card_y, card_w, card_h, 8, 16, 18, 22, 220);
    
    // 3. Top accent line
    drawRect(card_x, card_y, card_w, 3, 255, 48, 48, 220);
    
    // Get metadata of current index
    if (daemon_current_index < 0 || daemon_current_index >= static_cast<int>(daemon_playlist.size())) return;
    const auto& video = daemon_playlist[daemon_current_index];
    
    // 4. Render title & author
    drawText(truncateText(video.title, 32), card_x + 10, card_y + 8, 13, 255, 255, 255, 255);
    drawText(truncateText(video.author, 36), card_x + 10, card_y + 26, 11, 140, 140, 158, 255);
    
    // 5. Draw status tag at top right
    std::string statStr = "PLAYING";
    uint8_t sr = 64, sg = 214, sb = 96; // green
    if (daemon_status == DaemonStatus::Resolving) {
        statStr = "RESOLVING";
        sr = 64; sg = 148; sb = 255; // blue
    } else if (daemon_status == DaemonStatus::Paused) {
        statStr = "PAUSED";
        sr = 255; sg = 214; sb = 64; // yellow
    } else if (daemon_status == DaemonStatus::Error) {
        statStr = "ERROR";
        sr = 255; sg = 48; sb = 48; // red
    }
    drawText(statStr, card_x + card_w - 75, card_y + 8, 10, sr, sg, sb, 255);
    
    // 6. Draw progress bar
    double pos = mpv.getPlaybackTime();
    double dur = mpv.getDuration() > 0.0 ? mpv.getDuration() : static_cast<double>(video.duration_seconds);
    double frac = (dur > 0.0) ? std::max(0.0, std::min(1.0, pos / dur)) : 0.0;
    
    int barX = card_x + 10;
    int barY = card_y + 45;
    int barW = card_w - 20;
    int barH = 4;
    
    // Progress bg
    drawRect(barX, barY, barW, barH, 42, 48, 56, 180);
    // Progress fill
    if (frac > 0.0) {
        drawRect(barX, barY, static_cast<int>(barW * frac), barH, 255, 48, 48, 255);
    }
    
    // 7. Time string
    std::string timeStr = formatTime(pos) + " / " + (video.duration_string.empty() ? formatTime(dur) : video.duration_string);
    drawText(timeStr, card_x + 10, card_y + 54, 9, 180, 180, 190, 255);
    
    // 8. Key hints at bottom right
    drawText("SEL+A: PLAY  SEL+B: EXIT  SEL+R1: NEXT", card_x + card_w - 200, card_y + 54, 9, 120, 120, 135, 255);
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
                        mpv.seekAbsoluteExact(daemon_start_position);
                        daemon_start_position = 0.0;
                    }
                    daemon_status = DaemonStatus::Playing;
                } else {
                    daemon_status = DaemonStatus::Error;
                }
                daemon_needs_redraw = true;
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
                if (ev.type == 1) { // Button
                    bool down = (ev.value != 0);
                    if (ev.number == 12 || ev.number == 16 || ev.number == 6) {
                        select_held = down;
                    }
                    if (down) {
                        if (select_held) {
                            if (ev.number == 1) { // A -> Play/Pause
                                if (daemon_status == DaemonStatus::Playing) {
                                    mpv.pause();
                                    daemon_status = DaemonStatus::Paused;
                                } else if (daemon_status == DaemonStatus::Paused) {
                                    mpv.resume();
                                    daemon_status = DaemonStatus::Playing;
                                }
                                daemon_overlay_timer = 5.0f;
                                daemon_needs_redraw = true;
                            } else if (ev.number == 0) { // B -> Exit
                                daemon_running = false;
                            } else if (ev.number == 11 || ev.number == 5) { // DPAD_RIGHT / R1 -> Next
                                daemon_current_index = (daemon_current_index + 1) % daemon_playlist.size();
                                playCurrentTrack(mpv, yt);
                            } else if (ev.number == 10 || ev.number == 4) { // DPAD_LEFT / L1 -> Prev
                                daemon_current_index = (daemon_current_index - 1 + daemon_playlist.size()) % daemon_playlist.size();
                                playCurrentTrack(mpv, yt);
                            } else if (ev.number == 8) { // DPAD_UP -> Show
                                daemon_overlay_timer = 5.0f;
                                daemon_needs_redraw = true;
                            }
                        }
                    }
                }
            }
        }
#endif

        // Manage overlay visibility and rendering
        if (daemon_overlay_timer > 0.0f) {
            daemon_overlay_timer -= dt;
            if (!overlay_visible) {
                backupBackground();
                overlay_visible = true;
                daemon_needs_redraw = true;
            }
            
            if (daemon_needs_redraw) {
                restoreBackground();
                renderCard(mpv);
                daemon_needs_redraw = false;
            }
        } else {
            if (overlay_visible) {
                restoreBackground();
                overlay_visible = false;
            }
        }
        
        // Periodically refresh card progress (every 250ms)
        static float progress_timer = 0.0f;
        if (daemon_status == DaemonStatus::Playing && overlay_visible) {
            progress_timer += dt;
            if (progress_timer >= 0.25f) {
                progress_timer = 0.0f;
                daemon_needs_redraw = true;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    std::cerr << "[daemon] Stopping daemon...\n";
    if (overlay_visible) {
        restoreBackground();
    }
    
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
    open("/dev/null", O_WRONLY); // stdout
    open("/dev/null", O_WRONLY); // stderr
    
    char* args[] = { (char*)exec_path.c_str(), (char*)"--daemon", nullptr };
    execvp(args[0], args);
    exit(1);
#endif
}

#include "app.hpp"
#include "json.hpp"
#include "renderer_utils.hpp"
#include "stb_image.h"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
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

static void getSystemMemoryAndStorage(double& ram_used_mb, double& storage_free_gb, double& storage_total_gb) {
    ram_used_mb = 0.0;
    storage_free_gb = 0.0;
    storage_total_gb = 0.0;

    // RAM RSS
#ifdef _WIN32
    ram_used_mb = 0.0;
#else
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "VmRSS:", 6) == 0) {
                long rss_kb = 0;
                if (std::sscanf(line + 6, "%ld", &rss_kb) == 1) {
                    ram_used_mb = rss_kb / 1024.0;
                }
                break;
            }
        }
        std::fclose(f);
    }
#endif

    // Storage
#ifdef _WIN32
    ULARGE_INTEGER freeBytes, totalBytes, totalFreeBytes;
    if (GetDiskFreeSpaceExA(".", &freeBytes, &totalBytes, &totalFreeBytes)) {
        storage_free_gb = (double)freeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
        storage_total_gb = (double)totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    }
#else
    struct statvfs stat;
    if (statvfs(".", &stat) == 0) {
        storage_free_gb = (double)(stat.f_frsize * stat.f_bavail) / (1024.0 * 1024.0 * 1024.0);
        storage_total_gb = (double)(stat.f_frsize * stat.f_blocks) / (1024.0 * 1024.0 * 1024.0);
    }
#endif
}

// static void logInfo(const std::string& msg) { std::cout << "[INFO] " << msg << std::endl; }
static void logError(const std::string& msg) { std::cerr << "[ERROR] " << msg << std::endl; }

static void drawTextCentered(SDL_Renderer* renderer, int centerX, int y, const std::string& text, int scale, SDL_Color color, bool shadow = false) {
    int w = 0, h = 0;
    getTextSize(text, scale, &w, &h);
    int x = centerX - w / 2;
    if (shadow) {
        drawTextShadow(renderer, x, y, text, scale, color);
    } else {
        drawText(renderer, x, y, text, scale, color);
    }
}

App::~App() { shutdown(); }

bool App::initialize() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        logError(std::string("SDL_Init failed: ") + SDL_GetError());
        return false;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    if (!createWindow()) return false;
    

    
    if (!initFonts(renderer_)) {
        logError("Failed to initialize TTF fonts, falling back to pixel font");
    }
    getTextSize("TubeLite", 3, &headerTitleW_Home_, &headerTitleH_Home_);
    getTextSize("Search", 2, &headerTitleW_Search_, &headerTitleH_Search_);
    
    openController();
    image_manager_ = std::make_unique<ImageManager>(renderer_);
    thumb_atlas_ = std::make_unique<ThumbnailAtlas>(renderer_, 3);
    image_manager_->setAtlas(thumb_atlas_.get());
    
    home_grid_ = std::make_shared<ui::GridContainer>();
    home_grid_->title = "Trending Now";
    home_grid_->columns = 2;
    home_grid_->bounds = {0, 100, 640, 332};
    home_grid_->onScrolledToBottom = [this]() { loadMoreHomeFeeds(); };

    search_grid_ = std::make_shared<ui::GridContainer>();
    search_grid_->title = "";
    search_grid_->columns = 2;
    search_grid_->bounds = {0, 100, 640, 332};
    search_grid_->onScrolledToBottom = [this]() { loadMoreSearchResults(); };
    
    if (!mpv_player_.initialize(window_, renderer_)) {
        logError("MPV init failed");
        return false;
    }
    loadHistory();
    loadHomeFeeds();
    SDL_StartTextInput();

    int width = 0, height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    keyboard_.preload(renderer_, state_, width, height);

    return true;
}

void App::run() {
    using namespace std::chrono;
    const milliseconds frameTarget(16); // ~60fps
    last_fps_update_ = steady_clock::now();
    last_frame_time_ = steady_clock::now();
    
    while (state_.running) {
        auto start = steady_clock::now();
        
        auto now_dt = steady_clock::now();
        float dt = duration<float>(now_dt - last_frame_time_).count();
        last_frame_time_ = now_dt;
        if (dt > 0.1f) dt = 0.1f; // Clamp to prevent spikes after waking up
        
        processMainThreadQueue();
        
        if (state_.currentScreen != TubeState::Screen::Playback) {
            if (image_manager_ && image_manager_->update()) {
                uiDirty_ = true;
            }
        }
        
        auto focusedCard = focus_manager_.getFocusedCard();
        if (focusedCard && state_.inputMode != TubeState::InputMode::SearchText && focusedCard->focusedTime_ > 1.5f && focusedCard->focusedTime_ < 25.0f && focusedCard->titleW_ > focusedCard->maxPixelW_) {
            uiDirty_ = true;
        }
        
        if (play_flash_start_time_ > 0) {
            if (SDL_GetTicks() - play_flash_start_time_ < 400) {
                uiDirty_ = true;
            } else {
                play_flash_start_time_ = 0;
            }
        }
        
        bool active = uiDirty_ || mpv_player_.isPlaying() || is_playing_preview_ || is_loading_preview_ || state_.isScrubbing || (state_.inputMode == TubeState::InputMode::SearchText) || state_.isSearching || state_.isLoadingVideo;
        if (focusedCard && !is_playing_preview_ && !is_loading_preview_ && focusedCard->focusedTime_ < 0.85f) {
            active = true;
        }
        
        if (!active) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!main_thread_queue_.empty()) active = true;
        }
        
        SDL_Event event;
        if (!active) {
            if (SDL_WaitEventTimeout(&event, 100)) {
                handleEvent(event);
            }
        }
        while (SDL_PollEvent(&event)) { handleEvent(event); }
        
        updateSticks(dt);
        updateKeyboardCursorBlinkState();
        updateHoverPreviews();
        if (mpv_player_.update()) {
            uiDirty_ = true;
        }
        focus_manager_.update(dt);
        renderFrame();
        
        // Calculate FPS
        frame_count_++;
        auto now = steady_clock::now();
        auto fps_elapsed = duration_cast<milliseconds>(now - last_fps_update_);
        if (fps_elapsed >= seconds(1)) {
            current_fps_ = frame_count_ / (fps_elapsed.count() / 1000.0f);
            frame_count_ = 0;
            last_fps_update_ = now;
        }
        
        static float sleep_error_accum = 0.0f;
        auto loop_end = steady_clock::now();
        float work_time = duration<float>(loop_end - start).count();
        float target_frame_time = 1.0f / 60.0f;
        
        float sleep_sec = target_frame_time - work_time + sleep_error_accum;
        if (sleep_sec > 0.001f) {
            auto sleep_start = steady_clock::now();
            SDL_Delay(static_cast<Uint32>(sleep_sec * 1000.0f));
            float actual_sleep = duration<float>(steady_clock::now() - sleep_start).count();
            sleep_error_accum = sleep_sec - actual_sleep;
        } else {
            sleep_error_accum = sleep_sec;
        }
        
        // Clamp to prevent runaway accumulation during load stalls/crashes
        if (sleep_error_accum < -0.05f) sleep_error_accum = -0.05f;
        if (sleep_error_accum > 0.05f)  sleep_error_accum = 0.05f;
    }
}

void App::queueOnMainThread(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    main_thread_queue_.push_back(cb);
}

void App::processMainThreadQueue() {
    std::vector<std::function<void()>> local_queue;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        local_queue.swap(main_thread_queue_);
    }
    for (const auto& cb : local_queue) {
        cb();
    }
}

void App::shutdown() {
    SDL_StopTextInput();
    closeController();
    keyboard_.destroyTexture();
    status_.destroyTexture();
    storyboard_.stop();
    mpv_player_.shutdown();
    thumb_atlas_.reset();
    cleanupFonts();
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_ = nullptr;   }
    SDL_Quit();
}

bool App::createWindow() {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    // Force SDL to use its OpenGL ES 2.0 renderer (backed by EGL).
    // This is REQUIRED before SDL_CreateWindow so the KMSDRM backend
    // initialises EGL instead of its raw framebuffer path.
    // mpv's render context will then attach to the same EGL context.
    // Do NOT add SDL_WINDOW_OPENGL here — that flag fights with the
    // opengles2 renderer's internal EGL surface setup and causes a segfault.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");

    window_ = SDL_CreateWindow("tubelite", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               640, 480, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (window_ == nullptr) {
        logError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        // opengles2 unavailable — try without vsync
        renderer_ = SDL_CreateRenderer(window_, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    }
    if (renderer_ == nullptr) {
        logError(std::string("SDL_CreateRenderer (opengles2) failed: ") + SDL_GetError());
        logError("Falling back to software renderer (mpv video will be disabled)");
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer_ == nullptr) {
        logError(std::string("SDL_CreateRenderer failed entirely: ") + SDL_GetError());
        return false;
    }

    // Log which renderer SDL chose
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer_, &info) == 0)
        std::cerr << "[SDL] renderer: " << info.name << "\n";

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);
    return true;
}


void App::openController() {
    if (controller_ != nullptr) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            controller_ = SDL_GameControllerOpen(i);
            if (controller_ != nullptr) return;
        }
    }
    if (SDL_NumJoysticks() > 0) joystick_ = SDL_JoystickOpen(0);
}

void App::closeController() {
    if (controller_ != nullptr) { SDL_GameControllerClose(controller_); controller_ = nullptr; }
    if (joystick_ != nullptr)   { SDL_JoystickClose(joystick_);         joystick_ = nullptr;   }
}

std::shared_ptr<ui::GridContainer> App::activeGrid() const {
    if (state_.currentScreen == TubeState::Screen::Search) return search_grid_;
    if (state_.currentScreen == TubeState::Screen::Home) return home_grid_;
    return nullptr;
}

bool App::isInputLocked() const {
    if (state_.isLoadingVideo) return true;
    if (state_.isSearching) {
        auto grid = activeGrid();
        if (!grid || grid->cards.empty()) {
            return true;
        }
    }
    return false;
}

std::string App::streamCacheKey(const std::string& videoId, int maxHeight) const {
    return videoId + "#" + std::to_string(maxHeight);
}

std::optional<std::string> App::getCachedStreamUrl(const std::string& key) {
    auto it = stream_url_cache_.find(key);
    if (it == stream_url_cache_.end()) return std::nullopt;
    
    auto timeIt = stream_url_cache_times_.find(key);
    if (timeIt != stream_url_cache_times_.end()) {
        auto age = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - timeIt->second).count();
        if (age >= 120) {
            stream_url_cache_.erase(it);
            stream_url_cache_times_.erase(timeIt);
            return std::nullopt;
        }
    }
    return it->second;
}

void App::setCachedStreamUrl(const std::string& key, const std::string& url) {
    stream_url_cache_[key] = url;
    stream_url_cache_times_[key] = std::chrono::steady_clock::now();
}

void App::stopBrowsePreviewState() {
    if (is_playing_preview_) {
        mpv_player_.stop();
        mpv_player_.setMute(state_.muted);
    }
    if (preview_card_) {
        preview_card_->is_previewing = false;
        preview_card_ = nullptr;
    }
    is_playing_preview_ = false;
    is_loading_preview_ = false;
}

void App::leavePlayback() {
    mpv_player_.stop();
    storyboard_.stop();
    if (image_manager_) {
        image_manager_->clearCache();
    }
    state_.currentScreen = TubeState::Screen::Home;
    state_.showUi = true;
    last_playback_seconds_ = -1;
    uiDirty_ = true;
}

void App::showPlaybackToast(const std::string& text, bool withProgress) {
    mpv_player_.showText(text);
    if (withProgress) {
        mpv_player_.showProgress();
    }
}

void App::showPlaybackUi() {
    playback_ui_timeout_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!state_.showUi) {
        state_.showUi = true;
        uiDirty_ = true;
    }
}

void App::renderBrowseLoadingState(int width, int height, const std::string& text) {
    float time = SDL_GetTicks() / 1000.0f;
    drawSpinner(renderer_, width / 2, height / 2 - 15, 20, time);
    drawTextCentered(renderer_, width / 2, height / 2 + 20, text, 2, {150, 150, 150, 255});
    uiDirty_ = true;
}

void App::renderBrowseHeader(int width, int /*height*/, const std::string& title,
                              float scrollY, bool searchScreen) {
    const int expandedHeight = 84;
    const int collapsedHeight = 58;
    const int headerHeight = std::max(collapsedHeight,
        expandedHeight - static_cast<int>(scrollY * 0.12f));

    // ── Background ────────────────────────────────────────────────────────────
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 10, 10, 12, 255);
    SDL_Rect fill{0, 0, width, headerHeight};
    SDL_RenderFillRect(renderer_, &fill);
    // Subtle red accent line at bottom
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 255, 52, 52, 80);
    SDL_Rect accent{0, headerHeight - 2, width, 2};
    SDL_RenderFillRect(renderer_, &accent);

    float t = (headerHeight - collapsedHeight) / (float)(expandedHeight - collapsedHeight);
    t = std::max(0.0f, std::min(1.0f, t));

    // ── Title (left) ─────────────────────────────────────────────────────────
    int titleScale = searchScreen ? 2 : 3;
    int titleH = searchScreen ? headerTitleH_Search_ : headerTitleH_Home_;
    int titleY = static_cast<int>((headerHeight - titleH) / 2.0f * (1.0f - t) + 12.0f * t);
    SDL_Color titleColor = searchScreen ? SDL_Color{255, 80, 80, 255} : SDL_Color{255, 52, 52, 255};
    drawTextShadow(renderer_, 16, titleY, title, titleScale, titleColor);

    if (!searchScreen) {
        // ── "RECOMMENDED" sub-label (visible when expanded) ──────────────────
        if (t > 0.25f) {
            Uint8 alpha = static_cast<Uint8>(255.0f * std::min(1.0f, (t - 0.25f) / 0.5f));
            drawText(renderer_, 18, titleY + titleH + 5,
                     "RECOMMENDED", 1, {90, 90, 100, alpha});
        }

    } else {
        // ── Search: styled query bar (visible when expanded) ──────────────────
        if (t > 0.15f) {
            Uint8 alpha = static_cast<Uint8>(255.0f * std::min(1.0f, (t - 0.15f) / 0.5f));
            const int bx = 16;
            const int by = titleY + titleH + 6;
            const int bw = width - bx - 12;
            const int bh = 20;

            // Background trough (rounded pill with subtle border)
            SDL_Rect bar{bx, by, bw, bh};
            fillRoundedRect(renderer_, bar, 6, {26, 26, 30, alpha});
            drawRoundedRect(renderer_, bar, 6, {70, 70, 82, static_cast<Uint8>(alpha * 0.7f)});

            std::string q = current_search_query_.empty()
                            ? "Search..."
                            : utf8Truncate(current_search_query_, 50, true);
            SDL_Color qCol = current_search_query_.empty()
                             ? SDL_Color{70, 70, 80, alpha}
                             : SDL_Color{210, 210, 220, alpha};
            drawText(renderer_, bx + 8, by + 3, q, 1, qCol);
        }
    }
}

void App::renderPlaybackOverlay(int width, int height) {
    double pos    = mpv_player_.getPlaybackTime();
    double dur    = mpv_player_.getDuration();
    bool   playing = mpv_player_.isPlaying();

    auto fmtTime = [](double s) -> std::string {
        if (s < 0) s = 0;
        int tot = static_cast<int>(s);
        int h = tot / 3600, m = (tot % 3600) / 60, sec = tot % 60;
        char buf[16];
        if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
        else       snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
        return buf;
    };

    const double displayTime = state_.isScrubbing ? state_.scrubTargetTime : pos;
    const double frac        = (dur > 0.0) ? std::max(0.0, std::min(1.0, displayTime / dur)) : 0.0;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // ── Top Panel ─────────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer_, 16, 18, 22, 220);
    SDL_Rect topPanel{0, 0, width, 48};
    SDL_RenderFillRect(renderer_, &topPanel);
    
    SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 220);
    SDL_Rect topBorder{0, 48, width, 2};
    SDL_RenderFillRect(renderer_, &topBorder);
    
    {
        std::string titleTxt = utf8Truncate(current_video_.title, 48, true);
        drawTextShadow(renderer_, 14, 6, titleTxt, 2, {214, 220, 230, 255});

        int titleH = 0; getTextSize(titleTxt, 2, nullptr, &titleH);
        if (!current_video_.author.empty()) {
            std::string author = utf8Truncate(current_video_.author, 52, false);
            drawText(renderer_, 14, 6 + titleH + 2, author, 1, {214, 220, 230, 200});
        }

        // Speed badge (top right)
        if (state_.speed != 1.0) {
            char spd[10]; snprintf(spd, sizeof(spd), "%.2fx", state_.speed);
            int sw = 0, sh = 0; getTextSize(spd, 1, &sw, &sh);
            SDL_Rect badge{width - sw - 20, 12, sw + 12, sh + 6};
            fillRoundedRect(renderer_, badge, 4, {64, 148, 255, 200});
            drawText(renderer_, badge.x + 6, badge.y + 3, spd, 1, {255, 255, 255, 255});
        }
    }

    // ── Centre pause/play icon ─────────────────────────────────────────────────
    bool showPlayFlash = false;
    float flashProgress = 0.0f;
    Uint32 ticks = SDL_GetTicks();
    if (play_flash_start_time_ > 0) {
        Uint32 diff = ticks - play_flash_start_time_;
        if (diff < 400) {
            showPlayFlash = true;
            flashProgress = (float)diff / 400.0f;
        } else {
            play_flash_start_time_ = 0;
        }
    }

    if (!playing || state_.isScrubbing || showPlayFlash) {
        int baseIconSize = 40;
        int iconSize = baseIconSize;
        Uint8 bgAlpha = 120;
        Uint8 iconAlpha = 200;

        bool drawPlay = showPlayFlash;

        if (showPlayFlash) {
            iconSize = static_cast<int>(baseIconSize * (1.0f + flashProgress * 0.8f));
            bgAlpha = static_cast<Uint8>(120 * (1.0f - flashProgress));
            iconAlpha = static_cast<Uint8>(200 * (1.0f - flashProgress));
        }

        int iconX = (width - iconSize) / 2;
        int iconY = (height - iconSize) / 2;
        
        SDL_Rect iconBg{iconX - 8, iconY - 8, iconSize + 16, iconSize + 16};
        fillRoundedRect(renderer_, iconBg, iconBg.w / 2, {0, 0, 0, bgAlpha});

        int centerX = width / 2;
        int centerY = height / 2;

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        if (drawPlay) {
            SDL_Color color{255, 255, 255, iconAlpha};
            SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
            int halfSize = iconSize / 2;
            int startX = centerX - halfSize + 4;
            int endX = centerX + halfSize;
            int sizeX = endX - startX;
            for (int x = startX; x <= endX; ++x) {
                float t = (sizeX > 0) ? (float)(x - startX) / sizeX : 0.0f;
                int h = static_cast<int>(halfSize * (1.0f - t));
                SDL_RenderDrawLine(renderer_, x, centerY - h, x, centerY + h);
            }
        } else {
            SDL_Color color{255, 255, 255, iconAlpha};
            SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
            int barW = iconSize / 3;
            int barH = iconSize;
            int gap = iconSize / 3;
            SDL_Rect leftBar{centerX - barW - gap / 2, centerY - barH / 2, barW, barH};
            SDL_Rect rightBar{centerX + gap / 2, centerY - barH / 2, barW, barH};
            SDL_RenderFillRect(renderer_, &leftBar);
            SDL_RenderFillRect(renderer_, &rightBar);
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }

    // ── Bottom Panel ──────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer_, 16, 18, 22, 220);
    SDL_Rect botPanel{0, height - 72, width, 72};
    SDL_RenderFillRect(renderer_, &botPanel);
    
    SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 220);
    SDL_Rect botBorder{0, height - 72, width, 2};
    SDL_RenderFillRect(renderer_, &botBorder);

    // Progress bar
    const int mg  = 14;
    const int pbY = height - 64;
    const int pbH = 5;
    const int pbW = width - mg * 2;

    // Track (background)
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 80, 80, 88, 255);
    SDL_Rect pbBg{mg, pbY, pbW, pbH};
    SDL_RenderFillRect(renderer_, &pbBg);

    // Buffered indicator (subtle lighter region, simulated as 60% of duration)
    {
        int bufW = static_cast<int>(pbW * std::min(frac + 0.15, 1.0));
        SDL_SetRenderDrawColor(renderer_, 130, 130, 138, 255);
        SDL_Rect pbBuf{mg, pbY, bufW, pbH};
        SDL_RenderFillRect(renderer_, &pbBuf);
    }

    // Played (red fill)
    {
        int fillW = static_cast<int>(pbW * frac);
        SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
        SDL_Rect pbFill{mg, pbY, fillW, pbH};
        SDL_RenderFillRect(renderer_, &pbFill);
    }

    // Playhead circle
    {
        int dotX = mg + static_cast<int>(pbW * frac);
        int dotR = 7;
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        SDL_Rect dot{dotX - dotR, pbY - dotR + pbH / 2, dotR * 2, dotR * 2};
        SDL_RenderFillRect(renderer_, &dot); // Simple square dot (GPU-friendly)
        // Inner fill (red centre)
        SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
        SDL_Rect dotInner{dotX - 4, pbY - 4 + pbH / 2, 8, 8};
        SDL_RenderFillRect(renderer_, &dotInner);
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // Scrub preview thumbnail above playhead
    if (state_.isScrubbing) {
        int dotX = mg + static_cast<int>(pbW * frac);
        std::string timeStr = fmtTime(displayTime);
        int tw = 0, th = 0;
        getTextSize(timeStr, 1, &tw, &th);
        int previewW = tw + 8;
        int previewH = th + 4;
        int previewX = std::max(mg, std::min(width - mg - previewW, dotX - previewW / 2));
        int previewY = pbY - previewH - 12;

        SDL_Rect tsBg{previewX, previewY, previewW, previewH};
        fillRoundedRect(renderer_, tsBg, 3, {0, 0, 0, 200});
        drawText(renderer_, tsBg.x + 4, tsBg.y + 2, timeStr, 1, {255, 255, 255, 255});
    }

    // Timestamps
    {
        std::string posStr = fmtTime(displayTime);
        std::string durStr = (dur > 0.0) ? fmtTime(dur) : "--:--";
        int tsY = pbY + pbH + 4;
        drawText(renderer_, mg, tsY, posStr, 1, {220, 220, 230, 255});
        // "remaining" time in dim
        if (dur > 0.0) {
            std::string remStr = "-" + fmtTime(dur - displayTime);
            int rw = 0; getTextSize(remStr, 1, &rw, nullptr);
            drawText(renderer_, mg + pbW - rw, tsY, remStr, 1, {160, 160, 170, 255});
        }
    }

    // Bottom hint line
    struct HintItem {
        std::string button;
        SDL_Color btnColor;
        std::string action;
    };

    SDL_Color textColor{214, 220, 230, 255};
    const SDL_Color red{255, 48, 48, 255};
    const SDL_Color blue{64, 148, 255, 255};
    const SDL_Color yellow{255, 214, 64, 255};
    const SDL_Color green{64, 214, 96, 255};
    const SDL_Color panel{24, 28, 34, 200}; // semi-transparent dark panel

    std::vector<HintItem> activeHints = {
        {"A", red, playing ? "PAUSE" : "PLAY"},
        {"B", yellow, "EXIT"},
        {"SELECT", textColor, "MINIPLAYER"},
        {"Y", green, "SUBS"},
        {"X", blue, "STATS"},
        {"L1/R1", textColor, "SPEED"},
        {"L2/R2", textColor, "VOL"}
    };

    int boxH = 24;
    int boxY = height - 34;
    int fontHeight = 14;
    getTextSize("Ay", 1, nullptr, &fontHeight);

    int totalWidth = 0;
    std::vector<int> boxWidths;
    std::vector<int> btnWidths;
    std::vector<int> actWidths;
    
    for (const auto& item : activeHints) {
        int btnW = 0, actW = 0;
        getTextSize(item.button, 1, &btnW, nullptr);
        getTextSize(item.action, 1, &actW, nullptr);
        int boxW = btnW + actW + 16;
        boxWidths.push_back(boxW);
        btnWidths.push_back(btnW);
        actWidths.push_back(actW);
        totalWidth += boxW;
    }
    if (!activeHints.empty()) {
        totalWidth += (static_cast<int>(activeHints.size()) - 1) * 8;
    }

    int currentX = (width - totalWidth) / 2;
    for (size_t i = 0; i < activeHints.size(); ++i) {
        const auto& item = activeHints[i];
        int boxW = boxWidths[i];
        int btnW = btnWidths[i];
        int actW = actWidths[i];
        
        SDL_Rect box{currentX, boxY, boxW, boxH};
        fillRoundedRect(renderer_, box, 4, panel);
        drawRoundedRect(renderer_, box, 4, {42, 48, 56, 180});
        
        int contentW = btnW + 4 + actW;
        int contentX = currentX + (boxW - contentW) / 2;
        int textY = boxY + (boxH - fontHeight) / 2;
        
        drawTextShadow(renderer_, contentX, textY, item.button, 1, item.btnColor);
        drawTextShadow(renderer_, contentX + btnW + 4, textY, item.action, 1, textColor);
        
        currentX += boxW + 8;
    }
}


void App::openKeyboard() {
    state_.inputMode = TubeState::InputMode::SearchText;
    state_.textBuffer.clear();
    state_.textCursor = 0;
    state_.keyboardSelectedIndex = 0;
    keyboard_.resetRepeatState();
    uiDirty_ = true;
}

void App::closeKeyboard(bool commit) {
    state_.inputMode = TubeState::InputMode::None;
    state_.leftTrigger = 0.0f;
    state_.rightTrigger = 0.0f;
    keyboard_.resetRepeatState();
    if (commit && !state_.textBuffer.empty()) doSearch(state_.textBuffer);
    uiDirty_ = true;
}

void App::activateKeyboardGo() {
    if (state_.inputMode == TubeState::InputMode::SearchText) closeKeyboard(true);
}

void App::activateSelectedKey() {
    if (state_.inputMode != TubeState::InputMode::SearchText) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    const auto* key = keyboard_.getSelectedKey(state_, w, h);
    if (key == nullptr) return;
    
    std::string value = key->value;
    if (value == "__BACKSPACE__")     KeyboardOverlay::eraseActiveBufferChar(state_);
    else if (value == "__MODE__")     keyboard_.toggleMode(state_, uiDirty_);
    else if (value == "__LEFT__")     KeyboardOverlay::moveActiveCursor(state_, -1);
    else if (value == "__RIGHT__")    KeyboardOverlay::moveActiveCursor(state_, 1);
    else if (value == "__ENTER__")    { activateKeyboardGo(); return; }
    else if (value == "__CANCEL__")   { closeKeyboard(false); return; }
    else                              KeyboardOverlay::insertActiveText(state_, value);
    uiDirty_ = true;
}

void App::doSearch(const std::string& query) {
    stopBrowsePreviewState();
    state_.currentScreen = TubeState::Screen::Search;
    state_.isSearching = true;
    state_.isLoadingVideo = false;
    uiDirty_ = true;
    current_search_query_ = query;
    search_page_ = 1;
    
    search_grid_->cards.clear();
    focus_manager_.setGrid(search_grid_);
    
    if (image_manager_) image_manager_->clearCache();

    int reqPage = search_page_;
    std::string reqQuery = query;
    youtube_api_.search(query, reqPage, [this, reqQuery, reqPage](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqQuery, reqPage, results, finished]() {
            if (state_.currentScreen != TubeState::Screen::Search || current_search_query_ != reqQuery || search_page_ != reqPage) return;
            
            if (finished) {
                state_.isSearching = false;
                uiDirty_ = true;
                return;
            }
            
            if (!results.empty()) {
                bool isFirstCard = search_grid_->cards.empty();
                for (const auto& v : results) {
                    auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                    card->onClick = [this, v]() { playVideo(v); };
                    search_grid_->addCard(card);
                }
                if (isFirstCard && !search_grid_->cards.empty()) {
                    focus_manager_.setGrid(search_grid_);
                }
                uiDirty_ = true;
            }
        });
    });
}

void App::playVideo(const YouTubeVideo& video) {
    if (state_.currentScreen == TubeState::Screen::Playback) return;
    // If same video is already loading, do not restart it
    if (state_.isLoadingVideo && current_video_.id == video.id) return;
    // If a different video is loading, cancel it and start the new one
    state_.isLoadingVideo = false;
    
    if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
        previousBrowseScreen_ = state_.currentScreen;
    }
    state_.miniplayerActive = false;
    
    addToHistory(video);
    stopBrowsePreviewState();
    mpv_player_.stop();
    storyboard_.stop();
    if (image_manager_) {
        image_manager_->clearCache();
    }
    
    current_video_ = video;
    state_.isLoadingVideo = true;
    loading_status_text_ = "Resolving Stream...";
    last_playback_seconds_ = -1;
    uiDirty_ = true;

    const std::string cacheKey = streamCacheKey(video.id, state_.maxQualityHeight);
    auto cachedOpt = getCachedStreamUrl(cacheKey);
    if (cachedOpt.has_value() && !cachedOpt.value().empty()) {
        state_.isLoadingVideo = false;
        state_.currentScreen = TubeState::Screen::Playback;
        mpv_player_.setMute(state_.muted);
        mpv_player_.setVolume(state_.volume);
        mpv_player_.setSpeed(state_.speed);

        std::string cached_val = cachedOpt.value();
        std::string stream_url = cached_val;
        std::string subtitle_url = "";
        size_t pipe_pos = cached_val.find('|');
        if (pipe_pos != std::string::npos) {
            stream_url = cached_val.substr(0, pipe_pos);
            subtitle_url = cached_val.substr(pipe_pos + 1);
        }

        mpv_player_.play(stream_url, subtitle_url);
        mpv_player_.showText("Loading " + std::to_string(state_.maxQualityHeight) + "p");
        state_.showUi = false;
        uiDirty_ = true;

        // Start storyboard extraction
        const std::string lowResCacheKey = streamCacheKey(video.id, 144);
        auto lowResCachedOpt = getCachedStreamUrl(lowResCacheKey);
        if (lowResCachedOpt.has_value() && !lowResCachedOpt.value().empty()) {
            std::string low_res_cached = lowResCachedOpt.value();
            std::string low_res_url = low_res_cached;
            size_t p_pos = low_res_cached.find('|');
            if (p_pos != std::string::npos) {
                low_res_url = low_res_cached.substr(0, p_pos);
            }
            storyboard_.start(low_res_url, video.duration_seconds);
        } else {
            youtube_api_.getStreamUrl(video.id, 360, [this, video](bool success, const std::string& url, const std::string& subtitle_url) {
                queueOnMainThread([this, video, success, url, subtitle_url]() {
                    if (state_.currentScreen == TubeState::Screen::Playback && current_video_.id == video.id) {
                        if (success && !url.empty()) {
                            setCachedStreamUrl(streamCacheKey(video.id, 360), url + "|" + subtitle_url);
                            storyboard_.start(url, video.duration_seconds);
                        } else {
                            setCachedStreamUrl(streamCacheKey(video.id, 360), ""); // Cache failure
                        }
                    }
                });
            }, true /* isPreview */);
        }
        return;
    }

    youtube_api_.getStreamUrl(video.id, state_.maxQualityHeight, [this, video, cacheKey](bool success, const std::string& url, const std::string& subtitle_url) {
        queueOnMainThread([this, video, cacheKey, success, url, subtitle_url]() {
            if (!state_.isLoadingVideo || current_video_.id != video.id) return;
            state_.isLoadingVideo = false;
            if (success) {
                setCachedStreamUrl(cacheKey, url + "|" + subtitle_url);
                state_.currentScreen = TubeState::Screen::Playback;
                mpv_player_.setMute(state_.muted);
                mpv_player_.setVolume(state_.volume);
                mpv_player_.setSpeed(state_.speed);
                mpv_player_.play(url, subtitle_url);
                mpv_player_.showText("Loading " + std::to_string(state_.maxQualityHeight) + "p");
                state_.showUi = false;

                // Start storyboard extraction
                const std::string lowResCacheKey = streamCacheKey(video.id, 360);
                auto lowResCachedOpt = getCachedStreamUrl(lowResCacheKey);
                if (lowResCachedOpt.has_value() && !lowResCachedOpt.value().empty()) {
                    std::string low_res_cached = lowResCachedOpt.value();
                    std::string low_res_url = low_res_cached;
                    size_t p_pos = low_res_cached.find('|');
                    if (p_pos != std::string::npos) {
                        low_res_url = low_res_cached.substr(0, p_pos);
                    }
                    storyboard_.start(low_res_url, video.duration_seconds);
                } else {
                    youtube_api_.getStreamUrl(video.id, 360, [this, video](bool success2, const std::string& url2, const std::string& subtitle_url2) {
                        queueOnMainThread([this, video, success2, url2, subtitle_url2]() {
                            if (state_.currentScreen == TubeState::Screen::Playback && current_video_.id == video.id) {
                                if (success2 && !url2.empty()) {
                                    setCachedStreamUrl(streamCacheKey(video.id, 360), url2 + "|" + subtitle_url2);
                                    storyboard_.start(url2, video.duration_seconds);
                                } else {
                                    setCachedStreamUrl(streamCacheKey(video.id, 360), ""); // Cache failure
                                }
                            }
                        });
                    }, true /* isPreview */);
                }
            } else {
                setCachedStreamUrl(cacheKey, ""); // Cache failure
                loading_status_text_ = "Stream Resolve Failed — Press A to retry";
                state_.isLoadingVideo = false;
            }
            uiDirty_ = true;
        });
    });
}

void App::updateSticks(float dt) {
    if (state_.inputMode == TubeState::InputMode::SearchText) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        if (keyboard_.updateSelectionFromDpad(state_, w, h, uiDirty_)) return;
        if (keyboard_.updateSelectionFromStick(state_, w, h, uiDirty_)) return;
        keyboard_.updateCursorFromTriggers(state_, uiDirty_, [this](int delta) {
            KeyboardOverlay::moveActiveCursor(state_, delta);
        });
    } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
        int dirX = 0, dirY = 0;
        if (state_.dpadLeftPressed)       dirX = -1;
        else if (state_.dpadRightPressed) dirX = 1;
        else if (state_.dpadUpPressed)    dirY = -1;
        else if (state_.dpadDownPressed)  dirY = 1;
        
        if (dirX == 0 && dirY == 0) {
            const float threshold = 0.5f;
            const float absX = std::abs(state_.leftStickX);
            const float absY = std::abs(state_.leftStickY);
            if (absX >= threshold || absY >= threshold) {
                if (absX >= absY) dirX = (state_.leftStickX > 0.0f) ? 1 : -1;
                else              dirY = (state_.leftStickY > 0.0f) ? 1 : -1;
            }
        }
        
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (dirX != 0 || dirY != 0) {
            if (dirX != lastStickDirX_ || dirY != lastStickDirY_ || now >= nextStickNavAt_) {
                lastStickDirX_ = dirX;
                lastStickDirY_ = dirY;
                nextStickNavAt_ = now + milliseconds(140);
                focus_manager_.handleInput(dirX, dirY);
                uiDirty_ = true;
            }
        } else {
            lastStickDirX_ = 0;
            lastStickDirY_ = 0;
        }
    } else if (state_.currentScreen == TubeState::Screen::Playback) {
        // Trigger-based volume control during playback
        static auto lastVolumeAdjust = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVolumeAdjust).count() > 150) {
            if (state_.leftTrigger > 0.3f) {
                state_.volume = std::max(0, state_.volume - 5);
                mpv_player_.setVolume(state_.volume);
                showPlaybackToast("Volume " + std::to_string(state_.volume) + "%");
                lastVolumeAdjust = now;
            } else if (state_.rightTrigger > 0.3f) {
                state_.volume = std::min(100, state_.volume + 5);
                mpv_player_.setVolume(state_.volume);
                showPlaybackToast("Volume " + std::to_string(state_.volume) + "%");
                lastVolumeAdjust = now;
            }
        }

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        bool kbLeft = keys[SDL_SCANCODE_LEFT];
        bool kbRight = keys[SDL_SCANCODE_RIGHT];
        bool wantsScrub = state_.dpadLeftPressed || state_.dpadRightPressed || kbLeft || kbRight || std::abs(state_.leftStickX) > 0.2f;
        if (wantsScrub) {
            showPlaybackUi();
            if (!state_.isScrubbing) {
                state_.isScrubbing = true;
                state_.scrubTargetTime = mpv_player_.getPlaybackTime();
                mpv_player_.pause();
                scrub_hold_time_ = 0.0f;
            }
            
            double delta = 0.0;
            if (state_.dpadLeftPressed || kbLeft) {
                delta = -0.5; // 0.5 seconds base rate
            } else if (state_.dpadRightPressed || kbRight) {
                delta = 0.5;
            } else if (std::abs(state_.leftStickX) > 0.2f) {
                delta = state_.leftStickX * 1.0;
            }
            
            if (delta != 0.0) {
                scrub_hold_time_ += dt;
                double multiplier = 1.0 + static_cast<double>(scrub_hold_time_ * scrub_hold_time_ * 15.0);
                if (multiplier > 100.0) multiplier = 100.0;
                
                state_.scrubTargetTime += delta * multiplier;
                state_.scrubTargetTime = std::max(0.0, std::min(mpv_player_.getDuration(), state_.scrubTargetTime));
                uiDirty_ = true;
            }
        } else {
            if (state_.isScrubbing) {
                mpv_player_.seekAbsoluteKeyframes(state_.scrubTargetTime);
                mpv_player_.resume();
                play_flash_start_time_ = SDL_GetTicks();
                state_.isScrubbing = false;
                scrub_hold_time_ = 0.0f;
                uiDirty_ = true;
            }
        }
    }
}

void App::updateKeyboardCursorBlinkState() {
    if (state_.inputMode != TubeState::InputMode::SearchText) return;
    using namespace std::chrono;
    const auto phase = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 400;
    bool visible = (phase % 2) == 0;
    if (visible != lastKeyboardCursorVisible_) {
        lastKeyboardCursorVisible_ = visible;
        uiDirty_ = true;
    }
}

void App::renderFrame() {
    auto render_start = std::chrono::steady_clock::now();
    
    int width = 0, height = 0;
    SDL_GetWindowSize(window_, &width, &height);

    if (state_.currentScreen == TubeState::Screen::Playback) {
        auto now = std::chrono::steady_clock::now();
        bool shouldShow = (now < playback_ui_timeout_) || !mpv_player_.isPlaying() || state_.isScrubbing;
        if (state_.showUi != shouldShow) {
            state_.showUi = shouldShow;
            uiDirty_ = true;
        }
    }

    if (state_.currentScreen == TubeState::Screen::Playback && state_.showUi) {
        int cur_seconds = static_cast<int>(mpv_player_.getPlaybackTime());
        if (cur_seconds != last_playback_seconds_) {
            last_playback_seconds_ = cur_seconds;
            uiDirty_ = true;
        }
    }

    bool shouldPresent = uiDirty_ || state_.isLoadingVideo || state_.isScrubbing;
    if (!shouldPresent) return;

    if (state_.currentScreen == TubeState::Screen::Playback) {
        // ── Playback: mpv renders directly to the display framebuffer (FBO=0) ──
        // Do NOT use renderToTexture here – redirecting mpv into a side texture
        // requires re-entering the EGL context mid-frame which races with mpv's
        // internal decode thread and causes segfaults on KMSDRM/RK3326.
        //
        // Sequence:
        //   1. SDL draws the black background & UI overlay into its command buffer.
        //   2. SDL_RenderFlush pushes those SDL commands to GL (they go to FBO=0).
        //   3. mpv_player_.render() renders video into FBO=0 (behind everything).
        //   4. SDL_RenderPresent swaps the buffer.
        //
        // Because mpv uses keepaspect + its own viewport, we just clear black and
        // let mpv handle aspect-ratio letterboxing.

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);

        // Render the video frame first (directly to the display framebuffer FBO=0)
        mpv_player_.render(width, height);

        // Draw the UI overlay layers (HUD, loading spinner, overlays) on top of the video
        if (state_.showUi) {
            renderPlaybackOverlay(width, height);
        }

        if (state_.isLoadingVideo) {
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
            SDL_Rect bg{0, 0, width, height};
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_RenderFillRect(renderer_, &bg);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);

            float time = SDL_GetTicks() / 1000.0f;
            drawSpinner(renderer_, width / 2, height / 2 - 20, 30, time);
            drawTextCentered(renderer_, width / 2, height / 2 + 25, loading_status_text_, 2, {255, 255, 255, 255}, true);
            uiDirty_ = true;
        }

        // Volume / speed overlays
        {
            auto now = std::chrono::steady_clock::now();
            bool volumeActive = (now < volume_overlay_timeout_);
            bool speedActive  = (now < speed_overlay_timeout_);

            static bool lastVolumeActivePlayback = false;
            static bool lastSpeedActivePlayback  = false;
            if (volumeActive || speedActive || lastVolumeActivePlayback || lastSpeedActivePlayback) uiDirty_ = true;
            lastVolumeActivePlayback = volumeActive;
            lastSpeedActivePlayback  = speedActive;

            if (volumeActive) {
                int boxW = 200, boxH = 36;
                int boxX = (width - boxW) / 2, boxY = 64;
                SDL_Rect r{boxX, boxY, boxW, boxH};
                fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
                drawRoundedRect(renderer_, r, 6, {64, 148, 255, 255});
                char volBuf[32];
                snprintf(volBuf, sizeof(volBuf), "Volume: %d%%", state_.volume);
                drawTextCentered(renderer_, boxX + boxW / 2, boxY + 8, volBuf, 1, {255, 255, 255, 255}, true);
            }
            if (speedActive) {
                int boxW = 200, boxH = 36;
                int boxX = (width - boxW) / 2, boxY = 64;
                SDL_Rect r{boxX, boxY, boxW, boxH};
                fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
                drawRoundedRect(renderer_, r, 6, {64, 148, 255, 255});
                char speedBuf[32];
                snprintf(speedBuf, sizeof(speedBuf), "Speed: %.2fx", state_.speed);
                drawTextCentered(renderer_, boxX + boxW / 2, boxY + 8, speedBuf, 1, {255, 255, 255, 255}, true);
            }
        }

        keyboard_.render(renderer_, state_, width, height, uiDirty_);

        SDL_RenderPresent(renderer_);
        uiDirty_ = false;

        auto render_end = std::chrono::steady_clock::now();
        render_latency_ms_ = std::chrono::duration<float, std::milli>(render_end - render_start).count();
        return; // skip the rest of the generic render path
    }

    // ── Browse / Search screens ───────────────────────────────────────────────
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 15, 15, 15, 255);
    SDL_RenderClear(renderer_);

    auto currentGrid = activeGrid();
    float scrollY = currentGrid ? currentGrid->scrollY : 0.0f;

    if (state_.currentScreen == TubeState::Screen::Home) {
        if (home_grid_->cards.empty()) {
            if (homeLoadFailed_) {
                drawTextCentered(renderer_, width / 2, height / 2 - 10, "Failed to load feed.", 2, {255, 100, 100, 255});
                drawTextCentered(renderer_, width / 2, height / 2 + 20, "Press Y to search videos", 2, {150, 150, 150, 255});
            } else {
                renderBrowseLoadingState(width, height, "Loading Feed...");
            }
        } else {
            home_grid_->render(renderer_, 0.0f, 0.0f);
            if (is_playing_preview_ && preview_card_) {
                float screenY = preview_card_->bounds.y - scrollY;
                bool horizontal = (preview_card_->bounds.w > 400);
                int thumbW = horizontal ? 160 : static_cast<int>(preview_card_->bounds.w);
                int thumbH = horizontal ? 90 : static_cast<int>(preview_card_->bounds.w * (9.0f / 16.0f));
                SDL_Rect thumbDst{
                    static_cast<int>(preview_card_->bounds.x),
                    static_cast<int>(screenY),
                    thumbW,
                    thumbH
                };
                mpv_player_.renderViewport(width, height, thumbDst.x, thumbDst.y, thumbDst.w, thumbDst.h);
                maskRoundedCornersTop(renderer_, thumbDst, 8, {15, 15, 15, 255});
            }
            focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
        }
        auto focusedCard = focus_manager_.getFocusedCard();
        (void)focusedCard;
        renderBrowseHeader(width, height, "TubeLite", scrollY, false);
    } else if (state_.currentScreen == TubeState::Screen::Search) {
        if (state_.isSearching && search_grid_->cards.empty()) {
            renderBrowseLoadingState(width, height, "Searching...");
        } else if (search_grid_->cards.empty()) {
            if (current_search_query_.empty()) {
                drawTextCentered(renderer_, width / 2, height / 2, "Press Y to search videos", 2, {150, 150, 150, 255});
            } else {
                drawTextCentered(renderer_, width / 2, height / 2, "No results found.", 2, {150, 150, 150, 255});
            }
        } else {
            search_grid_->render(renderer_, 0.0f, 0.0f);
            if (is_playing_preview_ && preview_card_) {
                float screenY = preview_card_->bounds.y - scrollY;
                bool horizontal = (preview_card_->bounds.w > 400);
                int thumbW = horizontal ? 160 : static_cast<int>(preview_card_->bounds.w);
                int thumbH = horizontal ? 90 : static_cast<int>(preview_card_->bounds.w * (9.0f / 16.0f));
                SDL_Rect thumbDst{
                    static_cast<int>(preview_card_->bounds.x),
                    static_cast<int>(screenY),
                    thumbW,
                    thumbH
                };
                mpv_player_.renderViewport(width, height, thumbDst.x, thumbDst.y, thumbDst.w, thumbDst.h);
                maskRoundedCornersTop(renderer_, thumbDst, 8, {15, 15, 15, 255});
            }
            focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
        }
        renderBrowseHeader(width, height, "Search", scrollY, true);
    }

    if (state_.miniplayerActive) {
        int mX = width - 250;
        int mY = height - 193;
        int mW = 240;
        int mH = 135;
        
        mpv_player_.renderViewport(width, height, mX, mY, mW, mH);
        
        // Draw a 2px red accent border around the miniplayer
        SDL_Rect border1{mX - 1, mY - 1, mW + 2, mH + 2};
        SDL_Rect border2{mX - 2, mY - 2, mW + 4, mH + 4};
        SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
        SDL_RenderDrawRect(renderer_, &border1);
        SDL_RenderDrawRect(renderer_, &border2);
        
        if (!mpv_player_.isPlaying()) {
            int centerX = mX + mW / 2;
            int centerY = mY + mH / 2;
            SDL_Rect pauseBg{ centerX - 15, centerY - 15, 30, 30 };
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            fillRoundedRect(renderer_, pauseBg, 15, {0, 0, 0, 150});
            
            SDL_Rect pauseLeft{ centerX - 5, centerY - 8, 3, 16 };
            SDL_Rect pauseRight{ centerX + 2, centerY - 8, 3, 16 };
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer_, &pauseLeft);
            SDL_RenderFillRect(renderer_, &pauseRight);
        }
        
        // Hints: START: Play/Pause  B: Close
        std::string hint1 = "START: Play/Pause";
        std::string hint2 = "B: Close";
        int w1 = 0, h1 = 0;
        int w2 = 0, h2 = 0;
        getTextSize(hint1, 1, &w1, &h1);
        getTextSize(hint2, 1, &w2, &h2);
        int textW = std::max(w1, w2);
        int textH = h1 + h2 + 2;
        int plateW = textW + 8;
        int plateH = textH + 6;
        
        SDL_Rect plate{ mX + 4, mY + mH - plateH - 4, plateW, plateH };
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(renderer_, &plate);
        
        drawText(renderer_, mX + 8, mY + mH - plateH - 1, hint1, 1, {255, 255, 255, 255});
        drawText(renderer_, mX + 8, mY + mH - 14, hint2, 1, {255, 255, 255, 255});
    }

    // Render header-level spinner if searching and grid is not empty
    if (state_.isSearching && activeGrid() && !activeGrid()->cards.empty()) {
        const int expandedHeight = 84;
        const int collapsedHeight = 58;
        const int headerHeight = std::max(collapsedHeight, expandedHeight - static_cast<int>(scrollY * 0.12f));
        float time = SDL_GetTicks() / 1000.0f;
        drawSpinner(renderer_, width - 30, headerHeight / 2, 10, time);
        uiDirty_ = true;
    }

    // Browse status bar
    if (state_.showUi) {
        status_.render(renderer_, state_, width, height, uiDirty_);
    }
    
    // Loading overlay
    if (state_.isLoadingVideo) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_Rect bg{0, 0, width, height};
        SDL_RenderFillRect(renderer_, &bg);
        
        float time = SDL_GetTicks() / 1000.0f;
        drawSpinner(renderer_, width / 2, height / 2 - 20, 30, time);
        
        drawTextCentered(renderer_, width / 2, height / 2 + 25, loading_status_text_, 2, {255, 255, 255, 255}, true);
        uiDirty_ = true;
    }
    
    // Draw custom volume/speed overlays
    {
        auto now = std::chrono::steady_clock::now();
        bool volumeActive = (now < volume_overlay_timeout_);
        bool speedActive = (now < speed_overlay_timeout_);
        
        static bool lastVolumeActive = false;
        static bool lastSpeedActive = false;
        if (volumeActive || speedActive || lastVolumeActive || lastSpeedActive) {
            uiDirty_ = true;
        }
        lastVolumeActive = volumeActive;
        lastSpeedActive = speedActive;

        if (volumeActive) {
            int boxW = 200;
            int boxH = 36;
            int boxX = (width - boxW) / 2;
            int boxY = 64;
            
            SDL_Rect r{boxX, boxY, boxW, boxH};
            fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
            drawRoundedRect(renderer_, r, 6, {255, 48, 48, 255});
            
            std::string volText = "Volume: " + std::to_string(state_.volume) + "%";
            if (state_.muted) volText = "Mute: ON";
            
            int barW = 160;
            int barH = 6;
            int barX = boxX + 20;
            int barY = boxY + 24;
            SDL_Rect barBg{barX, barY, barW, barH};
            SDL_SetRenderDrawColor(renderer_, 60, 60, 60, 255);
            SDL_RenderFillRect(renderer_, &barBg);
            
            if (!state_.muted) {
                int fillW = static_cast<int>(barW * (state_.volume / 100.0f));
                SDL_Rect barFill{barX, barY, fillW, barH};
                SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
                SDL_RenderFillRect(renderer_, &barFill);
            }
            
            drawTextCentered(renderer_, boxX + boxW / 2, boxY + 4, volText, 1, {255, 255, 255, 255}, true);
        } else if (speedActive) {
            int boxW = 160;
            int boxH = 32;
            int boxX = (width - boxW) / 2;
            int boxY = 64;
            
            SDL_Rect r{boxX, boxY, boxW, boxH};
            fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
            drawRoundedRect(renderer_, r, 6, {64, 148, 255, 255});
            
            char speedBuf[32];
            snprintf(speedBuf, sizeof(speedBuf), "Speed: %.2fx", state_.speed);
            drawTextCentered(renderer_, boxX + boxW / 2, boxY + 8, speedBuf, 1, {255, 255, 255, 255}, true);
        }
    }

    keyboard_.render(renderer_, state_, width, height, uiDirty_);

    // Draw telemetry overlay if enabled
    if (state_.showDebugOverlay) {
        int panelW = 240;
        int panelH = 110;
        int panelX = width - panelW - 10;
        int panelY = 60; // below top bar/header

        SDL_Rect rect{panelX, panelY, panelW, panelH};
        fillRoundedRect(renderer_, rect, 6, {0, 0, 0, 200});
        drawRoundedRect(renderer_, rect, 6, {150, 150, 150, 255});

        char buf[256];
        int textY = panelY + 8;

        std::snprintf(buf, sizeof(buf), "FPS: %.1f", current_fps_);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        std::snprintf(buf, sizeof(buf), "Render Latency: %.2f ms", render_latency_ms_);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        int64_t vo_drops = mpv_player_.getPropertyInt("vo-drop-frame-count");
        int64_t dec_drops = mpv_player_.getPropertyInt("decoder-frame-drop-count");
        std::snprintf(buf, sizeof(buf), "Drops: VO %lld / Dec %lld", (long long)vo_drops, (long long)dec_drops);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        size_t q_size = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            q_size = main_thread_queue_.size();
        }
        std::snprintf(buf, sizeof(buf), "Queue Size: %zu", q_size);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        // Throttle to 1 Hz to avoid hammering /proc and statvfs at 60 fps
        static double cached_ram = 0.0, cached_storage_free = 0.0, cached_storage_total = 0.0;
        static uint32_t last_sys_poll = 0;
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks - last_sys_poll >= 1000) {
            getSystemMemoryAndStorage(cached_ram, cached_storage_free, cached_storage_total);
            last_sys_poll = now_ticks;
        }
        std::snprintf(buf, sizeof(buf), "RAM RSS: %.1f MB", cached_ram);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        std::snprintf(buf, sizeof(buf), "Storage: %.1f / %.1f GB free", cached_storage_free, cached_storage_total);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
    }

    SDL_RenderPresent(renderer_);
    uiDirty_ = false;

    auto render_end = std::chrono::steady_clock::now();
    render_latency_ms_ = std::chrono::duration<float, std::milli>(render_end - render_start).count();
}

void App::handleEvent(SDL_Event& event) {
    if (state_.currentScreen == TubeState::Screen::Playback) {
        if (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN ||
            event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYHATMOTION ||
            event.type == SDL_CONTROLLERAXISMOTION || event.type == SDL_JOYAXISMOTION) {
            showPlaybackUi();
        }
    }

    switch (event.type) {
    case SDL_QUIT: state_.running = false; break;
    case SDL_KEYDOWN: handleKey(event.key.keysym.sym); break;
    case SDL_CONTROLLERDEVICEADDED: openController(); break;
    case SDL_CONTROLLERDEVICEREMOVED: closeController(); openController(); break;
    case SDL_CONTROLLERBUTTONDOWN: handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), true); break;
    case SDL_CONTROLLERBUTTONUP: handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), false); break;
    case SDL_JOYHATMOTION: handleJoyHat(event.jhat.value); break;
    case SDL_JOYAXISMOTION: handleJoyAxis(event.jaxis); break;
    case SDL_JOYBUTTONDOWN: handleJoyButton(event.jbutton.button, event.jbutton.which, true); break;
    case SDL_JOYBUTTONUP: handleJoyButton(event.jbutton.button, event.jbutton.which, false); break;
    case SDL_CONTROLLERAXISMOTION: handleControllerAxis(event.caxis); break;
    case SDL_TEXTINPUT:
        if (state_.inputMode == TubeState::InputMode::SearchText) {
            KeyboardOverlay::insertActiveText(state_, KeyboardOverlay::transformTypedText(state_, event.text.text));
            uiDirty_ = true;
        }
        break;
    }
}

void App::handleKey(SDL_Keycode key) {
    if (state_.inputMode == TubeState::InputMode::SearchText) {
        if (key == SDLK_RETURN)    activateKeyboardGo();
        else if (key == SDLK_BACKSPACE) KeyboardOverlay::eraseActiveBufferChar(state_);
        else if (key == SDLK_LEFT)      KeyboardOverlay::moveActiveCursor(state_, -1);
        else if (key == SDLK_RIGHT)     KeyboardOverlay::moveActiveCursor(state_, 1);
        else if (key == SDLK_UP)        { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, -1, w, h, uiDirty_); }
        else if (key == SDLK_DOWN)      { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, 1, w, h, uiDirty_); }
        else if (key == SDLK_ESCAPE)    closeKeyboard(false);
        return;
    }
    switch (key) {
    case SDLK_q:
        state_.running = false;
        break;
    case SDLK_ESCAPE:
        if (state_.isLoadingVideo) {
            state_.isLoadingVideo = false;
            uiDirty_ = true;
        } else if (state_.miniplayerActive) {
            mpv_player_.stop();
            storyboard_.stop();
            if (image_manager_) {
                image_manager_->clearCache();
            }
            state_.miniplayerActive = false;
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            leavePlayback();
        } else {
            state_.running = false;
        }
        break;
    case SDLK_TAB:
    case SDLK_BACKSPACE:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.currentScreen = previousBrowseScreen_;
            state_.miniplayerActive = true;
            state_.showUi = true;
            uiDirty_ = true;
        } else if (state_.miniplayerActive) {
            state_.currentScreen = TubeState::Screen::Playback;
            state_.miniplayerActive = false;
            state_.showUi = false;
            uiDirty_ = true;
        }
        break;
    case SDLK_p:
    case SDLK_SPACE:
        if (state_.miniplayerActive || state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                showPlaybackToast("Playing");
            }
            uiDirty_ = true;
        }
        break;
    case SDLK_y:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.toggleSubtitles();
            showPlaybackToast("Subtitles Toggled");
        } else {
            openKeyboard();
        }
        break;
    case SDLK_s:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.cycleStatsOverlay();
            showPlaybackToast("Stats Overlay");
        }
        break;
    case SDLK_r:
        if (state_.currentScreen == TubeState::Screen::Home) {
            cached_trending_videos_.clear();
            loadHomeFeeds();
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            if (!current_search_query_.empty()) {
                doSearch(current_search_query_);
                uiDirty_ = true;
            }
        }
        break;
    case SDLK_x:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.cycleStatsOverlay();
            showPlaybackToast("Stats Overlay");
        } else if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            if (state_.maxQualityHeight == 240) state_.maxQualityHeight = 360;
            else if (state_.maxQualityHeight == 360) state_.maxQualityHeight = 480;
            else if (state_.maxQualityHeight == 480) state_.maxQualityHeight = 720;
            else state_.maxQualityHeight = 240;
            uiDirty_ = true;
        }
        break;
    case SDLK_UP:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.cycleSubtitleTrack();
            showPlaybackToast("Subtitles: " + mpv_player_.getSubtitleTrackName());
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(0, -1);
        }
        break;
    case SDLK_DOWN:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.cycleAudioTrack();
            showPlaybackToast("Audio: " + mpv_player_.getAudioTrackName());
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(0, 1);
        }
        break;
    case SDLK_LEFT:
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(-1, 0);
        }
        break;
    case SDLK_RIGHT:
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(1, 0);
        }
        break;
    case SDLK_RETURN:
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.clickFocused();
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                play_flash_start_time_ = SDL_GetTicks();
                showPlaybackToast("Playing");
            }
        }
        break;
    case SDLK_PAGEUP:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::min(2.0, state_.speed + 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        }
        break;
    case SDLK_PAGEDOWN:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::max(0.25, state_.speed - 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        }
        break;
    case SDLK_F12:
        state_.showDebugOverlay = !state_.showDebugOverlay;
        uiDirty_ = true;
        break;
    default: break;
    }
}

void App::handleControllerButton(SDL_GameControllerButton button, bool down) {
    if (button == SDL_CONTROLLER_BUTTON_START && controller_ != nullptr && SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_BACK)) {
        state_.running = false;
        return;
    }

    if (button == SDL_CONTROLLER_BUTTON_DPAD_UP)    state_.dpadUpPressed = down;
    if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  state_.dpadDownPressed = down;
    if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  state_.dpadLeftPressed = down;
    if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) state_.dpadRightPressed = down;

    if (!down) return;

    if (state_.inputMode == TubeState::InputMode::SearchText) {
        if (button == SDL_CONTROLLER_BUTTON_A)             activateSelectedKey();
        else if (button == SDL_CONTROLLER_BUTTON_X)        KeyboardOverlay::eraseActiveBufferChar(state_);
        else if (button == SDL_CONTROLLER_BUTTON_Y)        KeyboardOverlay::insertActiveText(state_, " ");
        else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) keyboard_.toggleMode(state_, uiDirty_);
        else if (button == SDL_CONTROLLER_BUTTON_START)    activateKeyboardGo();
        else if (button == SDL_CONTROLLER_BUTTON_B)        closeKeyboard(false);
        return;
    }

    if (button == SDL_CONTROLLER_BUTTON_Y) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.toggleSubtitles();
            showPlaybackToast("Subtitles Toggled");
        } else {
            openKeyboard();
        }
    } else if (button == SDL_CONTROLLER_BUTTON_START) {
        if (state_.miniplayerActive || state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                showPlaybackToast("Playing");
            }
            uiDirty_ = true;
        }
    } else if (button == SDL_CONTROLLER_BUTTON_A) {
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.clickFocused();
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                play_flash_start_time_ = SDL_GetTicks();
                showPlaybackToast("Playing");
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_B) {
        if (state_.miniplayerActive) {
            mpv_player_.stop();
            storyboard_.stop();
            if (image_manager_) {
                image_manager_->clearCache();
            }
            state_.miniplayerActive = false;
            uiDirty_ = true;
        } else if (state_.isLoadingVideo) {
            state_.isLoadingVideo = false;
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            leavePlayback();
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            state_.currentScreen = TubeState::Screen::Home;
            focus_manager_.setGrid(home_grid_);
        }
    } else if (button == SDL_CONTROLLER_BUTTON_X) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.cycleStatsOverlay();
            showPlaybackToast("Stats Overlay");
        } else if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            if (state_.maxQualityHeight == 240) state_.maxQualityHeight = 360;
            else if (state_.maxQualityHeight == 360) state_.maxQualityHeight = 480;
            else if (state_.maxQualityHeight == 480) state_.maxQualityHeight = 720;
            else state_.maxQualityHeight = 240;
            uiDirty_ = true;
        }
    } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
        if (state_.currentScreen == TubeState::Screen::Home) {
            cached_trending_videos_.clear();
            loadHomeFeeds();
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            if (!current_search_query_.empty()) {
                doSearch(current_search_query_);
                uiDirty_ = true;
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
        state_.showDebugOverlay = !state_.showDebugOverlay;
        uiDirty_ = true;
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.cycleSubtitleTrack();
            showPlaybackToast("Subtitles: " + mpv_player_.getSubtitleTrackName());
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.cycleAudioTrack();
            showPlaybackToast("Audio: " + mpv_player_.getAudioTrackName());
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
        // Handled via updateSticks() scrubbing
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        // Handled via updateSticks() scrubbing
    } else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::max(0.25, state_.speed - 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        }
    } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::min(2.0, state_.speed + 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        } else {
            state_.showUi = !state_.showUi;
            uiDirty_ = true;
        }
    } else if (button == SDL_CONTROLLER_BUTTON_BACK) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.currentScreen = previousBrowseScreen_;
            state_.miniplayerActive = true;
            state_.showUi = true;
            uiDirty_ = true;
        } else if (state_.miniplayerActive) {
            state_.currentScreen = TubeState::Screen::Playback;
            state_.miniplayerActive = false;
            state_.showUi = false;
            uiDirty_ = true;
        }
    }
}

void App::handleJoyHat(Uint8 value) {
    state_.dpadUpPressed    = (value & SDL_HAT_UP) != 0;
    state_.dpadDownPressed  = (value & SDL_HAT_DOWN) != 0;
    state_.dpadLeftPressed  = (value & SDL_HAT_LEFT) != 0;
    state_.dpadRightPressed = (value & SDL_HAT_RIGHT) != 0;

    if (state_.inputMode == TubeState::InputMode::SearchText) {
        if (value & SDL_HAT_UP)    { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, -1, w, h, uiDirty_); }
        if (value & SDL_HAT_DOWN)  { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, 1, w, h, uiDirty_); }
        if (value & SDL_HAT_LEFT)  { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, -1, 0, w, h, uiDirty_); }
        if (value & SDL_HAT_RIGHT) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 1, 0, w, h, uiDirty_); }
    }
}

void App::handleJoyAxis(const SDL_JoyAxisEvent& jaxis) {
    float normalized = (float)jaxis.value / 32767.0f;
    if (std::abs(jaxis.value) < 10000) normalized = 0.0f;
    if (jaxis.axis == 0) state_.leftStickX = normalized;
    else if (jaxis.axis == 1) state_.leftStickY = normalized;
    else if (jaxis.axis == 2) state_.rightStickX = normalized;
    else if (jaxis.axis == 3) state_.rightStickY = normalized;
}

void App::handleJoyButton(Uint8 button, SDL_JoystickID instanceId, bool down) {
    switch (button) {
    case 0: handleControllerButton(SDL_CONTROLLER_BUTTON_B, down); break;
    case 1: handleControllerButton(SDL_CONTROLLER_BUTTON_A, down); break;
    case 2: handleControllerButton(SDL_CONTROLLER_BUTTON_X, down); break;
    case 3: handleControllerButton(SDL_CONTROLLER_BUTTON_Y, down); break;
    case 4: handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, down); break;
    case 5: handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, down); break;
    case 8: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_UP, down); break;
    case 9: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, down); break;
    case 10: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, down); break;
    case 11: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, down); break;
    case 12: 
    case 13:
        if (down) {
            SDL_Joystick* joy = SDL_JoystickFromInstanceID(instanceId);
            if (joy && SDL_JoystickGetButton(joy, 12) && SDL_JoystickGetButton(joy, 13)) { state_.running = false; break; }
        }
        if (button == 12) handleControllerButton(SDL_CONTROLLER_BUTTON_BACK, down);
        else handleControllerButton(SDL_CONTROLLER_BUTTON_START, down);
        break;
    case 6:
    case 14:
        handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, down);
        break;
    case 7:
    case 15:
        handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, down);
        break;
    default: break;
    }
}

void App::handleControllerAxis(const SDL_ControllerAxisEvent& caxis) {
    float normalized = static_cast<float>(caxis.value) / 32767.0f;
    if (std::abs(caxis.value) < 8000) normalized = 0.0f;
    switch (caxis.axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:       state_.leftStickX = normalized; break;
    case SDL_CONTROLLER_AXIS_LEFTY:       state_.leftStickY = normalized; break;
    case SDL_CONTROLLER_AXIS_RIGHTX:      state_.rightStickX = normalized; break;
    case SDL_CONTROLLER_AXIS_RIGHTY:      state_.rightStickY = normalized; break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: state_.leftTrigger = normalized; break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: state_.rightTrigger = normalized; break;
    default: break;
    }
}

void App::loadHomeFeeds() {
    stopBrowsePreviewState();
    home_page_ = 1;
    homeLoadFailed_ = false;
    state_.isLoadingVideo = false;
    
    using namespace std::chrono;
    auto now = steady_clock::now();
    
    // Load from disk cache if memory cache is empty
    if (cached_trending_videos_.empty()) {
        loadHomeCache();
    }
    
    // If cache is fresh (< 30 minutes), use it and return
    if (!cached_trending_videos_.empty() && duration_cast<minutes>(now - trending_cache_time_).count() < 30) {
        state_.isSearching = false;
        if (home_grid_->cards.empty()) {
            for (const auto& v : cached_trending_videos_) {
                auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                card->onClick = [this, v]() { playVideo(v); };
                home_grid_->addCard(card);
            }
            focus_manager_.setGrid(home_grid_);
        }
        uiDirty_ = true;
        return;
    }
    
    // Stale-While-Revalidate: If we have no cache, we must clear and show loading
    if (cached_trending_videos_.empty()) {
        home_grid_->cards.clear();
        focus_manager_.setGrid(home_grid_);
    }
    
    state_.isSearching = true;
    uiDirty_ = true;
    home_grid_->title = "Trending";
    home_feed_query_ = "trending";
    
    int reqPage = home_page_;
    auto accumulated_results = std::make_shared<std::vector<YouTubeVideo>>();
    youtube_api_.search(home_feed_query_, reqPage, [this, reqPage, accumulated_results](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqPage, results, finished, accumulated_results]() {
            if (state_.currentScreen != TubeState::Screen::Home || home_page_ != reqPage) return;
            
            if (finished) {
                if (!accumulated_results->empty()) {
                    homeLoadFailed_ = false;
                    home_grid_->cards.clear();
                    cached_trending_videos_.clear();
                    for (const auto& v : *accumulated_results) {
                        auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                        card->onClick = [this, v]() { playVideo(v); };
                        home_grid_->addCard(card);
                        cached_trending_videos_.push_back(v);
                    }
                    focus_manager_.setGrid(home_grid_);
                    trending_cache_time_ = std::chrono::steady_clock::now();
                    saveHomeCache();
                } else if (home_grid_->cards.empty()) {
                    homeLoadFailed_ = true;
                }
                state_.isSearching = false;
                uiDirty_ = true;
                return;
            }
            
            if (!results.empty()) {
                accumulated_results->insert(accumulated_results->end(), results.begin(), results.end());
            }
        });
    });
}

void App::loadMoreHomeFeeds() {
    if (state_.isLoadingVideo || state_.isSearching) return;
    state_.isSearching = true;
    uiDirty_ = true;
    home_page_++;
    
    int reqPage = home_page_;
    youtube_api_.search(home_feed_query_, reqPage, [this, reqPage](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqPage, results, finished]() {
            if (state_.currentScreen != TubeState::Screen::Home || home_page_ != reqPage) return;
            
            if (finished) {
                state_.isSearching = false;
                focus_manager_.pruneGridIfNeeded(100);
                uiDirty_ = true;
                return;
            }
            
            if (!results.empty()) {
                for (const auto& v : results) {
                    bool inHistory = false;
                    for (const auto& hv : playback_history_) {
                        if (hv.id == v.id) { inHistory = true; break; }
                    }
                    if (inHistory) continue;
                    
                    auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                    card->onClick = [this, v]() { playVideo(v); };
                    home_grid_->addCard(card);
                    cached_trending_videos_.push_back(v);
                }
                uiDirty_ = true;
            }
        });
    });
}

void App::loadMoreSearchResults() {
    if (state_.isLoadingVideo || state_.isSearching || current_search_query_.empty()) return;
    state_.isSearching = true;
    uiDirty_ = true;
    search_page_++;
    
    int reqPage = search_page_;
    std::string reqQuery = current_search_query_;
    youtube_api_.search(current_search_query_, reqPage, [this, reqQuery, reqPage](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqQuery, reqPage, results, finished]() {
            if (state_.currentScreen != TubeState::Screen::Search || current_search_query_ != reqQuery || search_page_ != reqPage) return;
            
            if (finished) {
                state_.isSearching = false;
                focus_manager_.pruneGridIfNeeded(100);
                uiDirty_ = true;
                return;
            }
            
            if (!results.empty()) {
                for (const auto& v : results) {
                    auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                    card->onClick = [this, v]() { playVideo(v); };
                    search_grid_->addCard(card);
                }
                uiDirty_ = true;
            }
        });
    });
}

// Legacy presentation queue methods removed.

void App::updateHoverPreviews() {
    if ((state_.currentScreen != TubeState::Screen::Home && state_.currentScreen != TubeState::Screen::Search) || state_.isLoadingVideo || state_.inputMode == TubeState::InputMode::SearchText || state_.miniplayerActive) {
        stopBrowsePreviewState();
        return;
    }

    auto focusedCard = focus_manager_.getFocusedCard();
    if (!focusedCard) {
        stopBrowsePreviewState();
        return;
    }

    if (preview_card_ != focusedCard) {
        stopBrowsePreviewState();
        preview_card_ = focusedCard;
        return;
    }

    if (is_playing_preview_) {
        auto grid = activeGrid();
        if (grid) {
            const float screenY = focusedCard->bounds.y - grid->scrollY;
            const bool horizontal = (focusedCard->bounds.w > 400);
            const int thumbW = horizontal ? 160 : static_cast<int>(focusedCard->bounds.w);
            const int thumbH = horizontal ? 90 : static_cast<int>(focusedCard->bounds.w * (9.0f / 16.0f));
            
            const bool fullyVisible =
                screenY >= grid->bounds.y &&
                screenY + thumbH <= grid->bounds.y + grid->bounds.h &&
                focusedCard->bounds.x >= grid->bounds.x &&
                focusedCard->bounds.x + thumbW <= grid->bounds.x + grid->bounds.w;


            if (!fullyVisible || screenY < 96.0f) {
                stopBrowsePreviewState();
                return;
            }
        }
        return;
    }

    const std::string cacheKey = streamCacheKey(focusedCard->video.id, 360);
    
    // 1. Kick off prefetch if focused for >= 0.25s
    if (focusedCard->focusedTime_ >= 0.25f) {
        if (!getCachedStreamUrl(cacheKey).has_value() &&
            stream_prefetch_inflight_.find(cacheKey) == stream_prefetch_inflight_.end()) {
            stream_prefetch_inflight_.insert(cacheKey);
            is_loading_preview_ = true;
            youtube_api_.getStreamUrl(focusedCard->video.id, 360, [this, focusedCard, cacheKey](bool success, const std::string& url, const std::string& subtitle_url) {
                queueOnMainThread([this, focusedCard, cacheKey, success, url, subtitle_url]() {
                    stream_prefetch_inflight_.erase(cacheKey);
                    is_loading_preview_ = false;
                    if (success && !url.empty()) {
                        setCachedStreamUrl(cacheKey, url + "|" + subtitle_url);
                    } else {
                        setCachedStreamUrl(cacheKey, ""); // Cache failure
                    }
                    uiDirty_ = true;
                });
            }, true /* isPreview */);
        }
    }

    // 2. Play preview if focused for >= 0.85s and url is cached
    if (focusedCard->focusedTime_ < 0.85f || is_loading_preview_) {
        return;
    }

    auto cachedOpt = getCachedStreamUrl(cacheKey);
    if (cachedOpt.has_value() && !cachedOpt.value().empty()) {
        auto grid = activeGrid();
        if (!grid) return;

        const float screenY = focusedCard->bounds.y - grid->scrollY;
        const bool horizontal = (focusedCard->bounds.w > 400);
        const int thumbW = horizontal ? 160 : static_cast<int>(focusedCard->bounds.w);
        const int thumbH = horizontal ? 90 : static_cast<int>(focusedCard->bounds.w * (9.0f / 16.0f));
        const bool fullyVisible =
            screenY >= grid->bounds.y &&
            screenY + thumbH <= grid->bounds.y + grid->bounds.h &&
            focusedCard->bounds.x >= grid->bounds.x &&
            focusedCard->bounds.x + thumbW <= grid->bounds.x + grid->bounds.w;


        if (!fullyVisible || screenY < 96.0f) {
            return;
        }

        mpv_player_.setMute(true);
        std::string cached_val = cachedOpt.value();
        std::string stream_url = cached_val;
        size_t pipe_pos = cached_val.find('|');
        if (pipe_pos != std::string::npos) {
            stream_url = cached_val.substr(0, pipe_pos);
        }
        mpv_player_.play(stream_url);
        is_playing_preview_ = true;
        focusedCard->is_previewing = true;
        uiDirty_ = true;
    }
}

void App::saveHistory() {
    try {
        nlohmann::json j = nlohmann::json::array();
        int count = 0;
        for (auto it = playback_history_.rbegin(); it != playback_history_.rend(); ++it) {
            if (count++ >= 50) break;
            nlohmann::json item;
            item["id"] = it->id;
            item["title"] = it->title;
            item["author"] = it->author;
            item["duration_seconds"] = it->duration_seconds;
            item["duration_string"] = it->duration_string;
            item["view_count_string"] = it->view_count_string;
            item["uploaded_ago_string"] = it->uploaded_ago_string;
            j.push_back(item);
        }
        std::ofstream ofs(getAppDataPath("history.json"));
        if (ofs) {
            ofs << j.dump(4);
        }
    } catch (...) {}
}

void App::loadHistory() {
    playback_history_.clear();
    try {
        std::ifstream ifs(getAppDataPath("history.json"));
        if (ifs) {
            nlohmann::json j;
            ifs >> j;
            if (j.is_array()) {
                std::vector<YouTubeVideo> temp;
                for (const auto& item : j) {
                    YouTubeVideo v;
                    v.id = item.value("id", "");
                    v.title = item.value("title", "");
                    v.author = item.value("author", "");
                    v.duration_seconds = item.value("duration_seconds", 0);
                    v.duration_string = item.value("duration_string", "");
                    v.view_count_string = item.value("view_count_string", "");
                    v.uploaded_ago_string = item.value("uploaded_ago_string", "");
                    if (!v.id.empty()) {
                        temp.push_back(v);
                    }
                }
                std::reverse(temp.begin(), temp.end());
                playback_history_ = temp;
            }
        }
    } catch (...) {}
}

void App::addToHistory(const YouTubeVideo& video) {
    playback_history_.erase(
        std::remove_if(playback_history_.begin(), playback_history_.end(),
                       [&video](const YouTubeVideo& v) { return v.id == video.id; }),
        playback_history_.end()
    );
    playback_history_.push_back(video);
    saveHistory();
}

void App::saveHomeCache() {
    try {
        nlohmann::json j;
        j["timestamp"] = static_cast<long long>(std::time(nullptr));
        nlohmann::json videosArray = nlohmann::json::array();
        for (const auto& v : cached_trending_videos_) {
            nlohmann::json item;
            item["id"] = v.id;
            item["title"] = v.title;
            item["author"] = v.author;
            item["duration_seconds"] = v.duration_seconds;
            item["duration_string"] = v.duration_string;
            item["view_count_string"] = v.view_count_string;
            item["uploaded_ago_string"] = v.uploaded_ago_string;
            videosArray.push_back(item);
        }
        j["videos"] = videosArray;
        std::ofstream ofs(getAppDataPath("home_cache.json"));
        if (ofs) {
            ofs << j.dump(4);
        }
    } catch (...) {}
}

bool App::loadHomeCache() {
    try {
        std::ifstream ifs(getAppDataPath("home_cache.json"));
        if (!ifs) return false;
        nlohmann::json j;
        ifs >> j;
        if (!j.is_object() || !j.contains("videos") || !j["videos"].is_array()) return false;
        
        long long timestamp = j.value("timestamp", 0LL);
        
        std::vector<YouTubeVideo> temp;
        for (const auto& item : j["videos"]) {
            YouTubeVideo v;
            v.id = item.value("id", "");
            v.title = item.value("title", "");
            v.author = item.value("author", "");
            v.duration_seconds = item.value("duration_seconds", 0);
            v.duration_string = item.value("duration_string", "");
            v.view_count_string = item.value("view_count_string", "");
            v.uploaded_ago_string = item.value("uploaded_ago_string", "");
            if (!v.id.empty()) {
                temp.push_back(v);
            }
        }
        
        if (temp.empty()) return false;
        
        cached_trending_videos_ = temp;
        home_grid_->cards.clear();
        for (const auto& v : cached_trending_videos_) {
            auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
            card->onClick = [this, v]() { playVideo(v); };
            home_grid_->addCard(card);
        }
        
        focus_manager_.setGrid(home_grid_);
        
        long long now_epoch = static_cast<long long>(std::time(nullptr));
        long long diff_seconds = now_epoch - timestamp;
        if (diff_seconds < 0) diff_seconds = 0;
        
        trending_cache_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(diff_seconds);
        
        return true;
    } catch (...) {
        return false;
    }
}


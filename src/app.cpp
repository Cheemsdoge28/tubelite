#include "app.hpp"
#include "renderer_utils.hpp"
#include "stb_image.h"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

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
    loadHomeFeeds();
    SDL_StartTextInput();
    return true;
}

void App::run() {
    using namespace std::chrono;
    const milliseconds frameTarget(16); // ~60fps
    last_fps_update_ = steady_clock::now();
    while (state_.running) {
        auto start = steady_clock::now();
        processMainThreadQueue();
        
        bool active = uiDirty_ || mpv_player_.isPlaying() || is_playing_preview_ || is_loading_preview_ || state_.isScrubbing;
        if (!active) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!main_thread_queue_.empty()) active = true;
        }
        
        SDL_Event event;
        if (!active) {
            if (SDL_WaitEventTimeout(&event, 10)) {
                handleEvent(event);
            }
        }
        while (SDL_PollEvent(&event)) { handleEvent(event); }
        
        updateSticks();
        updateKeyboardCursorBlinkState();
        updateHoverPreviews();
        if (mpv_player_.update()) {
            uiDirty_ = true;
        }
        focus_manager_.update(16.0f / 1000.0f); // dt for 60fps
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
        
        auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
        if (elapsed < frameTarget) {
            SDL_Delay(static_cast<Uint32>((frameTarget - elapsed).count()));
        }
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
    int titleW = 0, titleH = 0;
    int titleScale = searchScreen ? 2 : 3;
    getTextSize(title, titleScale, &titleW, &titleH);
    int titleY = static_cast<int>((headerHeight - titleH) / 2.0f * (1.0f - t) + 12.0f * t);
    SDL_Color titleColor = searchScreen ? SDL_Color{255, 80, 80, 255} : SDL_Color{255, 52, 52, 255};
    drawTextShadow(renderer_, 16, titleY, title, titleScale, titleColor);

    if (!searchScreen) {
        // ── "TRENDING NOW" sub-label (visible when expanded) ─────────────────
        if (t > 0.25f) {
            Uint8 alpha = static_cast<Uint8>(255.0f * std::min(1.0f, (t - 0.25f) / 0.5f));
            drawText(renderer_, 18, titleY + titleH + 5,
                     "TRENDING NOW", 1, {90, 90, 100, alpha});
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

    // ── Top gradient bar: title + channel ─────────────────────────────────────
    // Draw a 4-step fading gradient from black to transparent.
    for (int i = 0; i < 5; ++i) {
        Uint8 a = static_cast<Uint8>(200 - i * 38);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, a);
        SDL_Rect band{0, i * 10, width, 10};
        SDL_RenderFillRect(renderer_, &band);
    }
    {
        std::string titleTxt = utf8Truncate(current_video_.title, 48, true);
        drawTextShadow(renderer_, 14, 8, titleTxt, 2, {240, 240, 240, 255});

        int titleH = 0; getTextSize(titleTxt, 2, nullptr, &titleH);
        if (!current_video_.author.empty()) {
            std::string author = utf8Truncate(current_video_.author, 52, false);
            drawText(renderer_, 14, 8 + titleH + 4, author, 1, {180, 180, 190, 255});
        }

        // Speed badge (top right)
        if (state_.speed != 1.0) {
            char spd[10]; snprintf(spd, sizeof(spd), "%.2fx", state_.speed);
            int sw = 0, sh = 0; getTextSize(spd, 1, &sw, &sh);
            SDL_Rect badge{width - sw - 20, 8, sw + 12, sh + 6};
            fillRoundedRect(renderer_, badge, 4, {64, 148, 255, 200});
            drawText(renderer_, badge.x + 6, badge.y + 3, spd, 1, {255, 255, 255, 255});
        }
    }

    // ── Centre pause/play icon ─────────────────────────────────────────────────
    if (!playing || state_.isScrubbing) {
        int iconSize = 40;
        int iconX = (width - iconSize) / 2;
        int iconY = (height - iconSize) / 2;
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 100);
        SDL_Rect iconBg{iconX - 8, iconY - 8, iconSize + 16, iconSize + 16};
        fillRoundedRect(renderer_, iconBg, 24, {0, 0, 0, 120});
        drawTextCentered(renderer_, width / 2, iconY + 8, state_.isScrubbing ? "||" : "||",
                         3, {255, 255, 255, 200});
    }

    // ── Bottom bar: progress + timestamps + hints ──────────────────────────────
    const int barAreaH = 72;
    // Bottom gradient: transparent → black
    for (int i = 0; i < 5; ++i) {
        Uint8 a = static_cast<Uint8>(i * 40 + 20);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, a);
        SDL_Rect band{0, height - barAreaH + i * (barAreaH / 5), width, barAreaH / 5};
        SDL_RenderFillRect(renderer_, &band);
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 200);
    SDL_Rect botSolid{0, height - 48, width, 48};
    SDL_RenderFillRect(renderer_, &botSolid);

    // Progress bar
    const int mg  = 14;
    const int pbY = height - 44;
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
        SDL_Texture* sbTex = storyboard_.getTexture(renderer_, displayTime);
        int previewW = 160, previewH = 90;
        int dotX = mg + static_cast<int>(pbW * frac);
        int previewX = std::max(mg, std::min(width - mg - previewW, dotX - previewW / 2));
        int previewY = pbY - previewH - 18;

        // Shadow frame
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 220);
        SDL_Rect shadow{previewX - 3, previewY - 3, previewW + 6, previewH + 6};
        SDL_RenderFillRect(renderer_, &shadow);
        // Red accent border
        SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
        SDL_RenderDrawRect(renderer_, &shadow);

        if (sbTex) {
            SDL_Rect dst{previewX, previewY, previewW, previewH};
            SDL_RenderCopy(renderer_, sbTex, nullptr, &dst);
        } else {
            SDL_SetRenderDrawColor(renderer_, 26, 26, 30, 255);
            SDL_Rect dst{previewX, previewY, previewW, previewH};
            SDL_RenderFillRect(renderer_, &dst);
            drawTextCentered(renderer_, previewX + previewW / 2, previewY + previewH / 2 - 8,
                             fmtTime(displayTime), 2, {200, 200, 210, 255});
            // trigger keyframe seek for preview
            static double last_kf_seek = -999.0;
            if (std::abs(displayTime - last_kf_seek) > 1.0) {
                mpv_player_.seekAbsoluteKeyframes(displayTime);
                last_kf_seek = displayTime;
            }
        }

        // Timestamp under preview
        std::string timeStr = fmtTime(displayTime);
        int tw = 0, th = 0; getTextSize(timeStr, 1, &tw, &th);
        SDL_Rect tsBg{previewX + (previewW - tw) / 2 - 4, previewY + previewH + 3, tw + 8, th + 4};
        fillRoundedRect(renderer_, tsBg, 3, {0, 0, 0, 190});
        drawText(renderer_, tsBg.x + 4, tsBg.y + 2, timeStr, 1, {255, 255, 255, 255});
    }

    // Timestamps
    {
        std::string posStr = fmtTime(displayTime);
        std::string durStr = (dur > 0.0) ? fmtTime(dur) : "--:--";
        int tsY = pbY + pbH + 7;
        drawText(renderer_, mg, tsY, posStr, 1, {220, 220, 230, 255});
        // "remaining" time in dim
        if (dur > 0.0) {
            std::string remStr = "-" + fmtTime(dur - displayTime);
            int rw = 0; getTextSize(remStr, 1, &rw, nullptr);
            drawText(renderer_, mg + pbW - rw, tsY, remStr, 1, {160, 160, 170, 255});
        }
    }

    // Bottom hint line
    const char* hints = "A:Pause  B:Exit  LB/RB:Speed  LT/RT:Vol  Y:Subs  X:Stats";
    drawTextCentered(renderer_, width / 2, height - 13, hints, 1, {110, 110, 120, 255});
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
    if (state_.isLoadingVideo || state_.currentScreen == TubeState::Screen::Playback) return;
    
    stopBrowsePreviewState();
    storyboard_.stop();
    
    current_video_ = video;
    state_.isLoadingVideo = true;
    loading_status_text_ = "Resolving Stream...";
    last_playback_seconds_ = -1;
    uiDirty_ = true;

    const std::string cacheKey = streamCacheKey(video.id, state_.maxQualityHeight);
    auto cached = stream_url_cache_.find(cacheKey);
    if (cached != stream_url_cache_.end()) {
        state_.isLoadingVideo = false;
        state_.currentScreen = TubeState::Screen::Playback;
        mpv_player_.setMute(state_.muted);
        mpv_player_.setVolume(state_.volume);
        mpv_player_.setSpeed(state_.speed);
        mpv_player_.play(cached->second);
        mpv_player_.showText("Loading " + std::to_string(state_.maxQualityHeight) + "p");
        state_.showUi = false;
        uiDirty_ = true;

        // Start storyboard extraction
        const std::string lowResCacheKey = streamCacheKey(video.id, 144);
        auto lowResCached = stream_url_cache_.find(lowResCacheKey);
        if (lowResCached != stream_url_cache_.end()) {
            storyboard_.start(lowResCached->second, video.duration_seconds);
        } else {
            youtube_api_.getStreamUrl(video.id, 144, [this, video](bool success, const std::string& url) {
                queueOnMainThread([this, video, success, url]() {
                    if (state_.currentScreen == TubeState::Screen::Playback && current_video_.id == video.id) {
                        if (success) {
                            stream_url_cache_[streamCacheKey(video.id, 144)] = url;
                            storyboard_.start(url, video.duration_seconds);
                        }
                    }
                });
            });
        }
        return;
    }

    youtube_api_.getStreamUrl(video.id, state_.maxQualityHeight, [this, video, cacheKey](bool success, const std::string& url) {
        queueOnMainThread([this, video, cacheKey, success, url]() {
            if (!state_.isLoadingVideo || current_video_.id != video.id) return;
            state_.isLoadingVideo = false;
            if (success) {
                stream_url_cache_[cacheKey] = url;
                state_.currentScreen = TubeState::Screen::Playback;
                mpv_player_.setMute(state_.muted);
                mpv_player_.setVolume(state_.volume);
                mpv_player_.setSpeed(state_.speed);
                mpv_player_.play(url);
                mpv_player_.showText("Loading " + std::to_string(state_.maxQualityHeight) + "p");
                state_.showUi = false;

                // Start storyboard extraction
                const std::string lowResCacheKey = streamCacheKey(video.id, 144);
                auto lowResCached = stream_url_cache_.find(lowResCacheKey);
                if (lowResCached != stream_url_cache_.end()) {
                    storyboard_.start(lowResCached->second, video.duration_seconds);
                } else {
                    youtube_api_.getStreamUrl(video.id, 144, [this, video](bool success2, const std::string& url2) {
                        queueOnMainThread([this, video, success2, url2]() {
                            if (state_.currentScreen == TubeState::Screen::Playback && current_video_.id == video.id) {
                                if (success2) {
                                    stream_url_cache_[streamCacheKey(video.id, 144)] = url2;
                                    storyboard_.start(url2, video.duration_seconds);
                                }
                            }
                        });
                    });
                }
            } else {
                loading_status_text_ = "Stream Resolve Failed";
            }
            uiDirty_ = true;
        });
    });
}

void App::updateSticks() {
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
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        bool kbLeft = keys[SDL_SCANCODE_LEFT];
        bool kbRight = keys[SDL_SCANCODE_RIGHT];
        bool wantsScrub = state_.dpadLeftPressed || state_.dpadRightPressed || kbLeft || kbRight || std::abs(state_.leftStickX) > 0.2f;
        if (wantsScrub) {
            if (!state_.isScrubbing) {
                state_.isScrubbing = true;
                state_.scrubTargetTime = mpv_player_.getPlaybackTime();
                mpv_player_.pause();
            }
            
            double delta = 0.0;
            if (state_.dpadLeftPressed || kbLeft) {
                delta = -2.0; // 2 seconds per frame
            } else if (state_.dpadRightPressed || kbRight) {
                delta = 2.0;
            } else if (std::abs(state_.leftStickX) > 0.2f) {
                delta = state_.leftStickX * 4.0;
            }
            
            if (delta != 0.0) {
                state_.scrubTargetTime += delta;
                state_.scrubTargetTime = std::max(0.0, std::min(mpv_player_.getDuration(), state_.scrubTargetTime));
                uiDirty_ = true;
            }
        } else {
            if (state_.isScrubbing) {
                mpv_player_.seekAbsoluteExact(state_.scrubTargetTime);
                mpv_player_.resume();
                state_.isScrubbing = false;
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
    if (image_manager_) image_manager_->update();
    
    int width = 0, height = 0;
    SDL_GetWindowSize(window_, &width, &height);

    if (state_.currentScreen == TubeState::Screen::Playback && state_.showUi) {
        int cur_seconds = static_cast<int>(mpv_player_.getPlaybackTime());
        if (cur_seconds != last_playback_seconds_) {
            last_playback_seconds_ = cur_seconds;
            uiDirty_ = true;
        }
    }

    bool shouldPresent = (state_.currentScreen != TubeState::Screen::Playback) || state_.isLoadingVideo || uiDirty_;
    if (!shouldPresent) return;

    if (state_.currentScreen == TubeState::Screen::Playback) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        mpv_player_.render(width, height);  // blits video texture fullscreen
    } else {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 15, 15, 15, 255); // #0f0f0f background
        SDL_RenderClear(renderer_);
    }

    auto currentGrid = activeGrid();
    float scrollY = currentGrid ? currentGrid->scrollY : 0.0f;

    if (state_.currentScreen == TubeState::Screen::Home) {
        if (home_grid_->cards.empty()) {
            if (homeLoadFailed_) {
                drawTextCentered(renderer_, width / 2, height / 2 - 10, "Failed to load trending.", 2, {255, 100, 100, 255});
                drawTextCentered(renderer_, width / 2, height / 2 + 20, "Press Y to search videos", 2, {150, 150, 150, 255});
            } else {
                renderBrowseLoadingState(width, height, "Loading Trending...");
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
                SDL_Texture* previewTex = mpv_player_.renderToTexture(renderer_, thumbW, thumbH);
                if (previewTex) {
                    SDL_RenderCopy(renderer_, previewTex, nullptr, &thumbDst);
                    maskRoundedCornersTop(renderer_, thumbDst, 8, {15, 15, 15, 255});
                }
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
                SDL_Texture* previewTex = mpv_player_.renderToTexture(renderer_, thumbW, thumbH);
                if (previewTex) {
                    SDL_RenderCopy(renderer_, previewTex, nullptr, &thumbDst);
                    maskRoundedCornersTop(renderer_, thumbDst, 8, {15, 15, 15, 255});
                }
            }
            focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
        }
        renderBrowseHeader(width, height, "Search", scrollY, true);
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

    // Playback HUD (progress, title, controls) or browse status bar
    if (state_.showUi) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            renderPlaybackOverlay(width, height);
        } else {
            status_.render(renderer_, state_, width, height, uiDirty_);
        }
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

        double ram_used = 0.0, storage_free = 0.0, storage_total = 0.0;
        getSystemMemoryAndStorage(ram_used, storage_free, storage_total);
        std::snprintf(buf, sizeof(buf), "RAM RSS: %.1f MB", ram_used);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        std::snprintf(buf, sizeof(buf), "Storage: %.1f / %.1f GB free", storage_free, storage_total);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
    }

    SDL_RenderPresent(renderer_);
    uiDirty_ = false;

    auto render_end = std::chrono::steady_clock::now();
    render_latency_ms_ = std::chrono::duration<float, std::milli>(render_end - render_start).count();
}

void App::handleEvent(SDL_Event& event) {
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
        } else {
            state_.running = false;
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
            state_.volume = std::min(100, state_.volume + 5);
            mpv_player_.setVolume(state_.volume);
            showPlaybackToast("Volume " + std::to_string(state_.volume) + "%");
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(0, -1);
        }
        break;
    case SDLK_DOWN:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.volume = std::max(0, state_.volume - 5);
            mpv_player_.setVolume(state_.volume);
            showPlaybackToast("Volume " + std::to_string(state_.volume) + "%");
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
    } else if (button == SDL_CONTROLLER_BUTTON_A) {
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.clickFocused();
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                showPlaybackToast("Playing");
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_B) {
        if (state_.isLoadingVideo) {
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
            state_.volume = std::min(100, state_.volume + 5);
            mpv_player_.setVolume(state_.volume);
            showPlaybackToast("Volume " + std::to_string(state_.volume) + "%");
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.volume = std::max(0, state_.volume - 5);
            mpv_player_.setVolume(state_.volume);
            showPlaybackToast("Volume " + std::to_string(state_.volume) + "%");
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
            mpv_player_.cycleStatsOverlay();
            showPlaybackToast("Stats Overlay");
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
    state_.isSearching = true;
    state_.isLoadingVideo = false;
    uiDirty_ = true;
    
    using namespace std::chrono;
    auto now = steady_clock::now();
    if (!cached_trending_videos_.empty() && duration_cast<minutes>(now - trending_cache_time_).count() < 15) {
        state_.isSearching = false;
        home_grid_->cards.clear();
        for (const auto& v : cached_trending_videos_) {
            auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
            card->onClick = [this, v]() { playVideo(v); };
            home_grid_->addCard(card);
        }
        focus_manager_.setGrid(home_grid_);
        uiDirty_ = true;
        return;
    }
    
    home_grid_->cards.clear();
    focus_manager_.setGrid(home_grid_);
    cached_trending_videos_.clear();
    
    int reqPage = home_page_;
    youtube_api_.search("trending", reqPage, [this, reqPage, now](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqPage, now, results, finished]() {
            if (state_.currentScreen != TubeState::Screen::Home || home_page_ != reqPage) return;
            
            if (finished) {
                state_.isSearching = false;
                uiDirty_ = true;
                return;
            }
            
            if (!results.empty()) {
                bool isFirstCard = home_grid_->cards.empty();
                for (const auto& v : results) {
                    auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                    card->onClick = [this, v]() { playVideo(v); };
                    home_grid_->addCard(card);
                    cached_trending_videos_.push_back(v);
                }
                if (isFirstCard && !home_grid_->cards.empty()) {
                    focus_manager_.setGrid(home_grid_);
                }
                trending_cache_time_ = std::chrono::steady_clock::now();
                uiDirty_ = true;
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
    youtube_api_.search("trending", reqPage, [this, reqPage](const std::vector<YouTubeVideo>& results, bool finished) {
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
    if ((state_.currentScreen != TubeState::Screen::Home && state_.currentScreen != TubeState::Screen::Search) || state_.isLoadingVideo) {
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

    const std::string cacheKey = streamCacheKey(focusedCard->video.id, 144);
    
    // 1. Kick off prefetch if focused for >= 0.25s
    if (focusedCard->focusedTime_ >= 0.25f) {
        if (stream_url_cache_.find(cacheKey) == stream_url_cache_.end() &&
            stream_prefetch_inflight_.find(cacheKey) == stream_prefetch_inflight_.end()) {
            stream_prefetch_inflight_.insert(cacheKey);
            youtube_api_.getStreamUrl(focusedCard->video.id, 144, [this, focusedCard, cacheKey](bool success, const std::string& url) {
                queueOnMainThread([this, focusedCard, cacheKey, success, url]() {
                    stream_prefetch_inflight_.erase(cacheKey);
                    if (success && !url.empty()) {
                        stream_url_cache_[cacheKey] = url;
                    }
                });
            });
        }
    }

    // 2. Play preview if focused for >= 0.85s and url is cached
    if (focusedCard->focusedTime_ < 0.85f || is_loading_preview_) {
        return;
    }

    auto cached = stream_url_cache_.find(cacheKey);
    if (cached != stream_url_cache_.end()) {
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
        mpv_player_.play(cached->second);
        is_playing_preview_ = true;
        focusedCard->is_previewing = true;
        uiDirty_ = true;
    }
}

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

void App::getSystemMemoryAndStorage(double& ram_used_mb, double& storage_free_gb, double& storage_total_gb) {
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
    compositor_ = std::make_unique<Compositor>(renderer_);

    state_manager_.setTransitionCallback([this](TubeState::Screen oldScreen, TubeState::Screen newScreen, bool oldMiniplayer, bool newMiniplayer) {
        bool stoppedPlayback = (oldScreen == TubeState::Screen::Playback && newScreen != TubeState::Screen::Playback && !newMiniplayer) ||
                               (oldMiniplayer && !newMiniplayer && newScreen != TubeState::Screen::Playback);
        if (stoppedPlayback) {
            mpv_player_.stop();
            storyboard_.stop();
            if (image_manager_) {
                image_manager_->clearCache();
            }
        }
        
        // Stop storyboard extraction during miniplayer to avoid RK3326 resource contention
        if (newMiniplayer && !oldMiniplayer) {
            storyboard_.stop();
        }
        
        // Resume/restart storyboard extraction if returning to fullscreen playback from miniplayer
        if (oldMiniplayer && !newMiniplayer && newScreen == TubeState::Screen::Playback) {
            const std::string cacheKey = streamCacheKey(current_video_.id, state_.maxQualityHeight);
            auto cachedOpt = getCachedStreamUrl(cacheKey);
            if (cachedOpt.has_value() && !cachedOpt.value().empty()) {
                std::string cached_val = cachedOpt.value();
                std::string stream_url = cached_val;
                size_t pipe_pos = cached_val.find('|');
                if (pipe_pos != std::string::npos) {
                    stream_url = cached_val.substr(0, pipe_pos);
                }
                storyboard_.start(stream_url, current_video_.duration_seconds);
            }
        }
        
        if (newScreen == TubeState::Screen::Search) {
            focus_manager_.setGrid(search_grid_);
        } else if (newScreen == TubeState::Screen::Home) {
            focus_manager_.setGrid(home_grid_);
        }
        
        uiDirty_ = true;
    });

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
        if (mpv_player_.checkAndClearEnded()) {
            handleVideoEnded();
        } else if (mpv_player_.isPlaying()) {
            double pos = mpv_player_.getPlaybackTime();
            double dur = mpv_player_.getDuration();
            if (dur > 0.0 && (dur - pos <= 45.0 || pos / dur >= 0.75)) {
                prefetchNextVideo();
            }
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
        mpv_player_.destroyPreviewTexture();
    }
    if (preview_card_) {
        preview_card_->is_previewing = false;
        preview_card_ = nullptr;
    }
    is_playing_preview_ = false;
    is_loading_preview_ = false;
}

void App::leavePlayback() {
    state_manager_.stopPlayback();
    state_.showUi = true;
    last_playback_seconds_ = -1;
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
    state_manager_.transitionTo(TubeState::Screen::Search);
    state_.isSearching = true;
    state_.isLoadingVideo = false;
    uiDirty_ = true;
    current_search_query_ = query;
    search_page_ = 1;
    
    search_grid_->cards.clear();
    focus_manager_.resetGridFocus(search_grid_);
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

void App::playVideo(const YouTubeVideo& video, bool forceFullscreen) {
    if (state_.currentScreen == TubeState::Screen::Playback && current_video_.id == video.id) return;
    // If same video is already loading, do not restart it
    if (state_.isLoadingVideo && current_video_.id == video.id) return;
    // If a different video is loading, cancel it and start the new one
    state_.isLoadingVideo = false;
    
    bool keepMiniplayer = state_.miniplayerActive && !forceFullscreen;
    if (!keepMiniplayer) {
        state_manager_.setMiniplayerActive(false);
    }
    
    addToHistory(video);
    stopBrowsePreviewState();
    mpv_player_.stop();
    storyboard_.stop();
    if (image_manager_) {
        image_manager_->clearCache();
    }
    
    current_video_ = video;
    state_.isLoadingVideo = true;
    state_.showDescriptionDrawer = false;
    description_scroll_row_ = 0;
    active_video_metadata_ = VideoPlaybackMetadata();
    loading_status_text_ = "Resolving Stream...";
    last_playback_seconds_ = -1;
    prefetched_next_video_id_.clear();
    uiDirty_ = true;

    const std::string cacheKey = streamCacheKey(video.id, state_.maxQualityHeight);
    auto cachedOpt = getCachedStreamUrl(cacheKey);
    if (cachedOpt.has_value() && !cachedOpt.value().empty()) {
        state_.isLoadingVideo = false;
        if (keepMiniplayer) {
            state_manager_.transitionTo(state_manager_.getPreviousBrowseScreen());
            state_manager_.setMiniplayerActive(true);
            state_.showUi = true;
        } else {
            state_manager_.transitionTo(TubeState::Screen::Playback);
            state_.showUi = false;
        }
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
        if (!keepMiniplayer) {
            state_.showUi = false;
        }
        uiDirty_ = true;

        // Start storyboard extraction
        storyboard_.start(stream_url, video.duration_seconds);
        return;
    }

    youtube_api_.getStreamUrl(video.id, state_.maxQualityHeight, [this, video, cacheKey, keepMiniplayer](bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& meta) {
        queueOnMainThread([this, video, cacheKey, success, url, subtitle_url, meta, keepMiniplayer]() {
            if (!state_.isLoadingVideo || current_video_.id != video.id) return;
            state_.isLoadingVideo = false;
            if (success) {
                active_video_metadata_ = meta;
                setCachedStreamUrl(cacheKey, url + "|" + subtitle_url);
                if (keepMiniplayer) {
                    state_manager_.transitionTo(state_manager_.getPreviousBrowseScreen());
                    state_manager_.setMiniplayerActive(true);
                    state_.showUi = true;
                } else {
                    state_manager_.transitionTo(TubeState::Screen::Playback);
                    state_.showUi = false;
                }
                mpv_player_.setMute(state_.muted);
                mpv_player_.setVolume(state_.volume);
                mpv_player_.setSpeed(state_.speed);
                mpv_player_.play(url, subtitle_url);
                mpv_player_.showText("Loading " + std::to_string(state_.maxQualityHeight) + "p");

                // Start storyboard extraction
                storyboard_.start(url, video.duration_seconds);
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

        // Repeating scroll for description drawer
        if (state_.showDescriptionDrawer) {
            static auto lastDescScroll = std::chrono::steady_clock::now();
            auto scrollNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(scrollNow - lastDescScroll).count() > 80) {
                const Uint8* keys = SDL_GetKeyboardState(nullptr);
                bool kbUp = keys[SDL_SCANCODE_UP];
                bool kbDown = keys[SDL_SCANCODE_DOWN];
                
                if (state_.dpadUpPressed || kbUp || state_.leftStickY < -0.5f) {
                    description_scroll_row_ = std::max(0, description_scroll_row_ - 1);
                    uiDirty_ = true;
                    lastDescScroll = scrollNow;
                } else if (state_.dpadDownPressed || kbDown || state_.leftStickY > 0.5f) {
                    description_scroll_row_++;
                    uiDirty_ = true;
                    lastDescScroll = scrollNow;
                }
            }
        }

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        bool kbLeft = keys[SDL_SCANCODE_LEFT];
        bool kbRight = keys[SDL_SCANCODE_RIGHT];
        bool wantsScrub = !state_.showDescriptionDrawer && (state_.dpadLeftPressed || state_.dpadRightPressed || kbLeft || kbRight || std::abs(state_.leftStickX) > 0.2f);
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
    bool shouldPresent = uiDirty_ || state_.isLoadingVideo || state_.isScrubbing;
    if (!shouldPresent) return;

    compositor_->render(this, width, height);

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
    case SDL_KEYUP: handleKeyUp(event.key.keysym.sym); break;
    case SDL_CONTROLLERDEVICEADDED: openController(); break;
    case SDL_CONTROLLERDEVICEREMOVED: closeController(); openController(); break;
    case SDL_CONTROLLERBUTTONDOWN: handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), true); break;
    case SDL_CONTROLLERBUTTONUP: handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), false); break;
    case SDL_JOYHATMOTION:
        if (controller_ == nullptr) handleJoyHat(event.jhat.value);
        break;
    case SDL_JOYAXISMOTION:
        if (controller_ == nullptr) handleJoyAxis(event.jaxis);
        break;
    case SDL_JOYBUTTONDOWN:
        if (event.jbutton.button == 16) {
            select_held_ = true;
            select_action_triggered_ = false;
        }
        if (controller_ == nullptr) handleJoyButton(event.jbutton.button, event.jbutton.which, true);
        break;
    case SDL_JOYBUTTONUP:
        if (event.jbutton.button == 16) {
            select_held_ = false;
        }
        if (controller_ == nullptr) handleJoyButton(event.jbutton.button, event.jbutton.which, false);
        break;
    case SDL_CONTROLLERAXISMOTION: handleControllerAxis(event.caxis); break;
    case SDL_TEXTINPUT:
        if (state_.inputMode == TubeState::InputMode::SearchText) {
            KeyboardOverlay::insertActiveText(state_, KeyboardOverlay::transformTypedText(state_, event.text.text));
            uiDirty_ = true;
        }
        break;
    }
}

void App::toggleMiniplayer() {
    static Uint32 lastToggleTime = 0;
    Uint32 now = SDL_GetTicks();
    if (now - lastToggleTime < 250) {
        return;
    }
    lastToggleTime = now;

    state_manager_.toggleMiniplayer();
    uiDirty_ = true;
}

void App::handleKeyUp(SDL_Keycode key) {
    if (key == SDLK_TAB || key == SDLK_F3 || key == SDLK_LSHIFT) {
        select_held_ = false;
        if (!select_action_triggered_) {
            toggleMiniplayer();
        }
    }
}

void App::handleKey(SDL_Keycode key) {
    if (key == SDLK_TAB || key == SDLK_F3 || key == SDLK_LSHIFT) {
        select_held_ = true;
        select_action_triggered_ = false;
        return;
    }

    if (select_held_) {
        if (key == SDLK_a || key == SDLK_RETURN) {
            if (state_.currentScreen == TubeState::Screen::Playback) {
                state_.showDescriptionDrawer = !state_.showDescriptionDrawer;
                description_scroll_row_ = 0;
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        }
    }

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
        } else if (state_.miniplayerActive || state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                state_.showDescriptionDrawer = false;
                uiDirty_ = true;
            } else {
                leavePlayback();
            }
        } else {
            state_.running = false;
        }
        break;
    case SDLK_TAB:
    case SDLK_BACKSPACE:
        toggleMiniplayer();
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
            if (state_.showDescriptionDrawer) {
                description_scroll_row_ = std::max(0, description_scroll_row_ - 1);
                uiDirty_ = true;
            } else {
                mpv_player_.cycleSubtitleTrack();
                showPlaybackToast("Subtitles: " + mpv_player_.getSubtitleTrackName());
            }
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(0, -1);
        }
        break;
    case SDLK_DOWN:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                description_scroll_row_++;
                uiDirty_ = true;
            } else {
                mpv_player_.cycleAudioTrack();
                showPlaybackToast("Audio: " + mpv_player_.getAudioTrackName());
            }
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
    if (button == SDL_CONTROLLER_BUTTON_BACK) {
        select_held_ = down;
        if (down) {
            select_action_triggered_ = false;
        } else {
            if (!select_action_triggered_) {
                toggleMiniplayer();
            }
        }
        return;
    }

    if (down && select_held_) {
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (state_.currentScreen == TubeState::Screen::Playback) {
                state_.showDescriptionDrawer = !state_.showDescriptionDrawer;
                description_scroll_row_ = 0;
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        }
    }

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
        if (state_.miniplayerActive || state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                state_.showDescriptionDrawer = false;
                uiDirty_ = true;
            } else {
                leavePlayback();
            }
        } else if (state_.isLoadingVideo) {
            state_.isLoadingVideo = false;
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            state_manager_.transitionTo(TubeState::Screen::Home);
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
            if (state_.showDescriptionDrawer) {
                description_scroll_row_ = std::max(0, description_scroll_row_ - 1);
                uiDirty_ = true;
            } else {
                mpv_player_.cycleSubtitleTrack();
                showPlaybackToast("Subtitles: " + mpv_player_.getSubtitleTrackName());
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                description_scroll_row_++;
                uiDirty_ = true;
            } else {
                mpv_player_.cycleAudioTrack();
                showPlaybackToast("Audio: " + mpv_player_.getAudioTrackName());
            }
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
    case 16:
        select_held_ = down;
        if (down) {
            select_action_triggered_ = false;
        }
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
        focus_manager_.resetGridFocus(home_grid_);
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

    const std::string cacheKey = streamCacheKey(focusedCard->video.id, state_.maxQualityHeight);
    auto grid = activeGrid();
    if (grid && focusedCard->focusedTime_ >= 0.10f) {
        int focusedIdx = -1;
        for (size_t i = 0; i < grid->cards.size(); ++i) {
            if (grid->cards[i] == focusedCard) {
                focusedIdx = static_cast<int>(i);
                break;
            }
        }

        if (focusedIdx != -1) {
            // A. Prefetch focused card
            if (!getCachedStreamUrl(cacheKey).has_value() &&
                stream_prefetch_inflight_.find(cacheKey) == stream_prefetch_inflight_.end()) {
                stream_prefetch_inflight_.insert(cacheKey);
                is_loading_preview_ = true;
                youtube_api_.getStreamUrl(focusedCard->video.id, state_.maxQualityHeight, [this, cacheKey](bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& /*meta*/) {
                    queueOnMainThread([this, cacheKey, success, url, subtitle_url]() {
                        stream_prefetch_inflight_.erase(cacheKey);
                        is_loading_preview_ = false;
                        if (success && !url.empty()) {
                            setCachedStreamUrl(cacheKey, url + "|" + subtitle_url);
                        } else {
                            setCachedStreamUrl(cacheKey, ""); // Cache failure
                        }
                        uiDirty_ = true;
                    });
                }, true /* isPreview */, focusedCard->video.id);
            }

            // B. Prefetch next two adjacent cards if focused for >= 0.15s
            if (focusedCard->focusedTime_ >= 0.15f) {
                for (int nextIdx = focusedIdx + 1; nextIdx <= focusedIdx + 2; ++nextIdx) {
                    if (nextIdx < static_cast<int>(grid->cards.size())) {
                        auto nextCard = grid->cards[nextIdx];
                        const std::string nextCacheKey = streamCacheKey(nextCard->video.id, state_.maxQualityHeight);
                        if (!getCachedStreamUrl(nextCacheKey).has_value() &&
                            stream_prefetch_inflight_.find(nextCacheKey) == stream_prefetch_inflight_.end()) {
                            stream_prefetch_inflight_.insert(nextCacheKey);
                            youtube_api_.getStreamUrl(nextCard->video.id, state_.maxQualityHeight, [this, nextCacheKey](bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& /*meta*/) {
                                queueOnMainThread([this, nextCacheKey, success, url, subtitle_url]() {
                                    stream_prefetch_inflight_.erase(nextCacheKey);
                                    if (success && !url.empty()) {
                                        setCachedStreamUrl(nextCacheKey, url + "|" + subtitle_url);
                                    } else {
                                        setCachedStreamUrl(nextCacheKey, "");
                                    }
                                });
                            }, true /* isPreview */, focusedCard->video.id);
                        }
                    }
                }
            }
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

void App::handleVideoEnded() {
    std::shared_ptr<ui::GridContainer> grid = (state_manager_.getPreviousBrowseScreen() == TubeState::Screen::Search) ? search_grid_ : home_grid_;
    bool playedNext = false;
    
    if (grid && !grid->cards.empty()) {
        for (size_t i = 0; i < grid->cards.size(); ++i) {
            if (grid->cards[i]->video.id == current_video_.id) {
                if (i + 1 < grid->cards.size()) {
                    auto nextVideo = grid->cards[i + 1]->video;
                    focus_manager_.setFocusedIndex(i + 1);
                    playVideo(nextVideo, !state_.miniplayerActive);
                    playedNext = true;
                }
                break;
            }
        }
    }
    
    if (!playedNext) {
        mpv_player_.seekAbsoluteKeyframes(0.0);
        mpv_player_.pause();
        uiDirty_ = true;
    }
}

void App::prefetchNextVideo() {
    if (prefetched_next_video_id_ == current_video_.id) return;
    
    // Find the next video in the active grid
    std::shared_ptr<ui::GridContainer> grid = (state_manager_.getPreviousBrowseScreen() == TubeState::Screen::Search) ? search_grid_ : home_grid_;
    if (!grid || grid->cards.empty()) return;
    
    YouTubeVideo nextVideo;
    bool foundCurrent = false;
    for (size_t i = 0; i < grid->cards.size(); ++i) {
        if (grid->cards[i]->video.id == current_video_.id) {
            if (i + 1 < grid->cards.size()) {
                nextVideo = grid->cards[i + 1]->video;
                foundCurrent = true;
            }
            break;
        }
    }
    
    if (!foundCurrent) return;
    
    // Mark as prefetched so we don't spam requests
    prefetched_next_video_id_ = current_video_.id;
    
    const std::string nextCacheKey = streamCacheKey(nextVideo.id, state_.maxQualityHeight);
    if (getCachedStreamUrl(nextCacheKey).has_value()) {
        // Already cached!
        return;
    }
    
    // Start background prefetch request (using isPreview=true so it doesn't cancel main playback requests)
    std::cerr << "[prefetch] Prefetching next video stream URL: " << nextVideo.title << "\n";
    youtube_api_.getStreamUrl(nextVideo.id, state_.maxQualityHeight, [this, nextCacheKey](bool success, const std::string& url, const std::string& subtitle_url, const VideoPlaybackMetadata& /*meta*/) {
        queueOnMainThread([this, nextCacheKey, success, url, subtitle_url]() {
            if (success && !url.empty()) {
                setCachedStreamUrl(nextCacheKey, url + "|" + subtitle_url);
                std::cerr << "[prefetch] Next video stream URL cached successfully.\n";
            }
        });
    }, true /* isPreview */, "autoplay_" + current_video_.id);
}


#include "app.hpp"
#include "renderer_utils.hpp"
#include "stb_image.h"
#include <iostream>
#include <algorithm>

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
    

    
    if (!initFonts()) {
        logError("Failed to initialize TTF fonts, falling back to pixel font");
    }
    
    openController();
    image_manager_ = std::make_unique<ImageManager>(renderer_);
    
    home_grid_ = std::make_shared<ui::GridContainer>();
    home_grid_->title = "Trending Now";
    home_grid_->columns = 2;
    home_grid_->bounds = {0, 92, 640, 340};
    home_grid_->onScrolledToBottom = [this]() { loadMoreHomeFeeds(); };

    search_grid_ = std::make_shared<ui::GridContainer>();
    search_grid_->title = "";
    search_grid_->columns = 2;
    search_grid_->bounds = {0, 92, 640, 340};
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
    while (state_.running) {
        processMainThreadQueue();
        SDL_Event event;
        while (SDL_PollEvent(&event)) { handleEvent(event); }
        updateSticks();
        updateKeyboardCursorBlinkState();
        updateHoverPreviews();
        if (mpv_player_.update()) {
            uiDirty_ = true;
        }
        focus_manager_.update(16.0f / 1000.0f); // dt for 60fps
        renderFrame();
        SDL_Delay(16);
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
    mpv_player_.shutdown();
    cleanupFonts();
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_ = nullptr;   }
    SDL_Quit();
}

bool App::createWindow() {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    window_ = SDL_CreateWindow("tubelite", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               640, 480, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (window_ == nullptr) {
        logError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (renderer_ == nullptr) {
        logError(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
        return false;
    }

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
        mpv_player_.resetGeometry();
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
    mpv_player_.resetGeometry();
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

void App::renderBrowseHeader(int width, int /*height*/, const std::string& title, const std::string& subtitle, float scrollY, bool searchScreen) {
    const int expandedHeight = 84;
    const int collapsedHeight = 58;
    const int headerHeight = std::max(collapsedHeight, expandedHeight - static_cast<int>(scrollY * 0.12f));

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 12, 12, 14, 255);
    SDL_Rect fill{0, 0, width, headerHeight};
    SDL_RenderFillRect(renderer_, &fill);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 28, 28, 32, 220);
    SDL_Rect accent{0, headerHeight - 1, width, 1};
    SDL_RenderFillRect(renderer_, &accent);

    float t = (headerHeight - collapsedHeight) / (float)(expandedHeight - collapsedHeight);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    int titleW = 0, titleH = 0;
    getTextSize(title, searchScreen ? 2 : 3, &titleW, &titleH);
    int titleY = static_cast<int>((headerHeight - titleH) / 2.0f * (1.0f - t) + 12.0f * t);

    SDL_Color titleColor = searchScreen ? SDL_Color{255, 96, 96, 255} : SDL_Color{255, 52, 52, 255};
    drawTextShadow(renderer_, 18, titleY, title, searchScreen ? 2 : 3, titleColor);

    if (!subtitle.empty() && t > 0.1f) {
        Uint8 alpha = static_cast<Uint8>(255.0f * t);
        int subtitleY = static_cast<int>(titleY + titleH + 8.0f * t - 4.0f * (1.0f - t));
        drawText(renderer_, 20, subtitleY, utf8Truncate(subtitle, 52, true), 1, {170, 170, 176, alpha});
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
    if (state_.isLoadingVideo || state_.currentScreen == TubeState::Screen::Playback) return;
    
    stopBrowsePreviewState();
    
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
                nextStickNavAt_ = now + milliseconds(250);
                focus_manager_.handleInput(dirX, dirY);
                uiDirty_ = true;
            }
        } else {
            lastStickDirX_ = 0;
            lastStickDirY_ = 0;
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
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        // mpv video is rendered natively by vo=drm underneath.
        // We render a transparent frame so the SDL overlay stays alive.
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
            focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
        }
        auto focusedCard = focus_manager_.getFocusedCard();
        std::string subtitle = focusedCard ? focusedCard->video.title : "";
        renderBrowseHeader(width, height, "TubeLite", subtitle, scrollY, false);
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
            focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
        }
        std::string subtitle = current_search_query_.empty() ? "Controller-first search and fast paging" : current_search_query_;
        renderBrowseHeader(width, height, "Search", subtitle, scrollY, true);
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

    // Bottom tooltips (Status Bar)
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
    SDL_RenderPresent(renderer_);
    uiDirty_ = false;
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
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(-10);
            showPlaybackToast("Seek -10s", true);
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(-1, 0);
        }
        break;
    case SDLK_RIGHT:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(10);
            showPlaybackToast("Seek +10s", true);
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
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
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(-10);
            showPlaybackToast("Seek -10s", true);
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(10);
            showPlaybackToast("Seek +10s", true);
        }
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
        if (is_playing_preview_) {
            mpv_player_.stop();
            mpv_player_.resetGeometry();
            mpv_player_.setMute(state_.muted);
            if (preview_card_) preview_card_->is_previewing = false;
            is_playing_preview_ = false;
        }
        preview_card_ = focusedCard;
        is_loading_preview_ = false;
        return;
    }

    const std::string cacheKey = streamCacheKey(focusedCard->video.id, state_.maxQualityHeight);
    
    // 1. Kick off prefetch if focused for >= 0.25s
    if (focusedCard->focusedTime_ >= 0.25f) {
        if (stream_url_cache_.find(cacheKey) == stream_url_cache_.end() &&
            stream_prefetch_inflight_.find(cacheKey) == stream_prefetch_inflight_.end()) {
            stream_prefetch_inflight_.insert(cacheKey);
            youtube_api_.getStreamUrl(focusedCard->video.id, state_.maxQualityHeight, [this, focusedCard, cacheKey](bool success, const std::string& url) {
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

    if (stream_url_cache_.find(cacheKey) != stream_url_cache_.end()) {
        if (is_playing_preview_) {
            return;
        }
        
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
        mpv_player_.setGeometry(static_cast<int>(focusedCard->bounds.x), static_cast<int>(screenY), thumbW, thumbH);
        mpv_player_.play(stream_url_cache_[cacheKey]);
        is_playing_preview_ = true;
        focusedCard->is_previewing = true;
        uiDirty_ = true;
    }
}

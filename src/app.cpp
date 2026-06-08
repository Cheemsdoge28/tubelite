#include "app.hpp"
#include "renderer_utils.hpp"
#include "stb_image.h"
#include <iostream>
#include <algorithm>

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
    openController();
    image_manager_ = std::make_unique<ImageManager>(renderer_);
    
    home_grid_ = std::make_shared<ui::GridContainer>();
    home_grid_->title = "Trending Now";
    home_grid_->columns = 2;
    home_grid_->bounds = {0, 60, 640, 420};
    home_grid_->onScrolledToBottom = [this]() { loadMoreHomeFeeds(); };

    search_grid_ = std::make_shared<ui::GridContainer>();
    search_grid_->title = "";
    search_grid_->columns = 1;
    search_grid_->bounds = {0, 60, 640, 420};
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
        SDL_Event event;
        while (SDL_PollEvent(&event)) { handleEvent(event); }
        updateSticks();
        updateKeyboardCursorBlinkState();
        mpv_player_.update();
        focus_manager_.update(16.0f / 1000.0f); // dt for 60fps
        renderFrame();
        SDL_Delay(16);
    }
}

void App::shutdown() {
    SDL_StopTextInput();
    closeController();
    keyboard_.destroyTexture();
    status_.destroyTexture();
    mpv_player_.shutdown();
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
    if (window_ == nullptr) return false;

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (renderer_ == nullptr) return false;

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
    state_.currentScreen = TubeState::Screen::Search;
    state_.isSearching = true;
    uiDirty_ = true;
    current_search_query_ = query;
    search_page_ = 1;
    
    if (image_manager_) image_manager_->clearCache();

    youtube_api_.search(query, search_page_, [this](bool success, const std::vector<YouTubeVideo>& results) {
        state_.isSearching = false;
        if (success) {
            search_grid_->cards.clear();
            for (const auto& v : results) {
                auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                card->onClick = [this, v]() { playVideo(v); };
                search_grid_->addCard(card);
            }
            focus_manager_.setGrid(search_grid_);
        }
        uiDirty_ = true;
    });
}

void App::playVideo(const YouTubeVideo& video) {
    if (state_.isLoadingVideo || state_.currentScreen == TubeState::Screen::Playback) return;
    
    current_video_ = video;
    state_.isLoadingVideo = true;
    uiDirty_ = true;
    
    youtube_api_.getStreamUrl(video.id, state_.maxQualityHeight, [this](bool success, const std::string& url) {
        state_.isLoadingVideo = false;
        if (success) {
            state_.currentScreen = TubeState::Screen::Playback;
            mpv_player_.play(url);
            state_.showUi = false;
        }
        uiDirty_ = true;
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
    } else if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
        const float threshold = 0.5f;
        const float absX = std::abs(state_.leftStickX);
        const float absY = std::abs(state_.leftStickY);
        int dirX = 0, dirY = 0;
        if (absX >= threshold || absY >= threshold) {
            if (absX >= absY) dirX = (state_.leftStickX > 0.0f) ? 1 : -1;
            else              dirY = (state_.leftStickY > 0.0f) ? 1 : -1;
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
        uiDirty_ = true; // Keep UI dirty to animate progress bar
    }

    bool shouldPresent = (state_.currentScreen != TubeState::Screen::Playback) || uiDirty_;
    if (!shouldPresent) return;

    if (state_.currentScreen == TubeState::Screen::Playback) {
        if (!state_.showUi && state_.inputMode == TubeState::InputMode::None) {
            return;
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
    } else {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 15, 15, 15, 255); // #0f0f0f background
        SDL_RenderClear(renderer_);
    }

    // Grids rendering (Home / Search)
    if (state_.currentScreen == TubeState::Screen::Home) {
        SDL_SetRenderDrawColor(renderer_, 31, 31, 31, 255); // #1f1f1f header
        SDL_Rect headerRect{0, 0, width, 50};
        SDL_RenderFillRect(renderer_, &headerRect);
        drawTextShadow(renderer_, 20, 15, "tubelite", 3, {255, 60, 60, 255});
        
        if (home_grid_->cards.empty()) {
            if (homeLoadFailed_) {
                drawText(renderer_, width / 2 - 144, height / 2 - 10, "Failed to load trending.", 2, {255, 100, 100, 255});
                drawText(renderer_, width / 2 - 144, height / 2 + 20, "Press Y to search videos", 2, {150, 150, 150, 255});
            } else {
                float time = SDL_GetTicks() / 1000.0f;
                drawSpinner(renderer_, width / 2, height / 2, 20, time);
                drawText(renderer_, width / 2 - 152, height / 2 + 30, "Loading Trending...", 2, {150, 150, 150, 255});
                uiDirty_ = true;
            }
        } else {
            home_grid_->render(renderer_, 0.0f, 0.0f);
            focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
            if (state_.isSearching) {
                float time = SDL_GetTicks() / 1000.0f;
                drawSpinner(renderer_, width - 30, 30, 15, time);
                uiDirty_ = true;
            }
        }
    } else if (state_.currentScreen == TubeState::Screen::Search) {
        SDL_SetRenderDrawColor(renderer_, 31, 31, 31, 255); // #1f1f1f header
        SDL_Rect headerRect{0, 0, width, 60};
        SDL_RenderFillRect(renderer_, &headerRect);
        
        std::string headerTitle = "Search: " + current_search_query_;
        drawTextShadow(renderer_, 20, 20, headerTitle, 2, {255, 80, 80, 255});
        
        if (state_.isSearching && search_grid_->cards.empty()) {
            float time = SDL_GetTicks() / 1000.0f;
            drawSpinner(renderer_, width / 2, height / 2, 20, time);
            drawText(renderer_, width / 2 - 96, height / 2 + 30, "Searching...", 2, {150, 150, 150, 255});
            uiDirty_ = true;
        } else if (search_grid_->cards.empty()) {
            if (current_search_query_.empty()) {
                drawText(renderer_, width / 2 - 144, height / 2, "Press Y to search videos", 2, {150, 150, 150, 255});
            } else {
                drawText(renderer_, width / 2 - 96, height / 2, "No results found.", 2, {150, 150, 150, 255});
            }
        } else {
            search_grid_->render(renderer_, 0.0f, 0.0f);
            focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
            if (state_.isSearching) {
                float time = SDL_GetTicks() / 1000.0f;
                drawSpinner(renderer_, width - 30, 30, 15, time);
                uiDirty_ = true;
            }
        }
    } else if (state_.currentScreen == TubeState::Screen::Playback) {
        if (state_.showUi) {
            // Header bar
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
            SDL_Rect headerRect{0, 0, width, 50};
            SDL_RenderFillRect(renderer_, &headerRect);
            drawTextShadow(renderer_, 20, 16, "Playing: " + current_video_.title, 2, {255, 255, 255, 255});
            
            // Progress Bar Overlay
            double curTime = mpv_player_.getPlaybackTime();
            double duration = mpv_player_.getDuration();
            
            int barY = height - 80;
            int barW = width - 80;
            int barX = 40;
            int barH = 8;
            
            SDL_SetRenderDrawColor(renderer_, 80, 80, 80, 255);
            SDL_Rect bgBar{barX, barY, barW, barH};
            SDL_RenderFillRect(renderer_, &bgBar);
            
            if (duration > 0.0) {
                double progress = std::clamp(curTime / duration, 0.0, 1.0);
                int fillW = static_cast<int>(progress * barW);
                SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255);
                SDL_Rect fillBar{barX, barY, fillW, barH};
                SDL_RenderFillRect(renderer_, &fillBar);
            }
            
            int total_cur = static_cast<int>(curTime);
            if (total_cur < 0) total_cur = 0;
            int cur_m = total_cur / 60;
            int cur_s = total_cur % 60;
            std::string cur_str = std::to_string(cur_m) + ":" + (cur_s < 10 ? "0" : "") + std::to_string(cur_s);
            
            int total_dur = static_cast<int>(duration);
            if (total_dur < 0) total_dur = 0;
            int dur_m = total_dur / 60;
            int dur_s = total_dur % 60;
            std::string dur_str = std::to_string(dur_m) + ":" + (dur_s < 10 ? "0" : "") + std::to_string(dur_s);
            
            drawTextShadow(renderer_, barX, barY + 15, cur_str, 2, {220, 220, 220, 255});
            drawTextShadow(renderer_, barX + barW - (static_cast<int>(dur_str.length()) * 12), barY + 15, dur_str, 2, {220, 220, 220, 255});
        }
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
        
        std::string text = "Extracting Stream URL...";
        drawTextShadow(renderer_, width / 2 - 192, height / 2 + 25, text, 2, {255, 255, 255, 255});
        uiDirty_ = true;
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
    case SDLK_ESCAPE: state_.running = false; break;
    case SDLK_y:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.toggleSubtitles();
            uiDirty_ = true;
        } else {
            openKeyboard();
        }
        break;
    case SDLK_r:
        if (state_.currentScreen == TubeState::Screen::Home) {
            cached_trending_videos_.clear();
            loadHomeFeeds();
            uiDirty_ = true;
        }
        break;
    case SDLK_x:
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            if (state_.maxQualityHeight == 240) state_.maxQualityHeight = 360;
            else if (state_.maxQualityHeight == 360) state_.maxQualityHeight = 480;
            else if (state_.maxQualityHeight == 480) state_.maxQualityHeight = 720;
            else state_.maxQualityHeight = 240;
            uiDirty_ = true;
        }
        break;
    case SDLK_UP:
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(0, -1);
        }
        break;
    case SDLK_DOWN:
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(0, 1);
        }
        break;
    case SDLK_LEFT:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(-10);
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(-1, 0);
        }
        break;
    case SDLK_RIGHT:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(10);
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(1, 0);
        }
        break;
    case SDLK_RETURN:
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.clickFocused();
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
            uiDirty_ = true;
        } else {
            openKeyboard();
        }
    } else if (button == SDL_CONTROLLER_BUTTON_A) {
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.clickFocused();
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) mpv_player_.pause();
            else mpv_player_.resume();
        }
    } else if (button == SDL_CONTROLLER_BUTTON_B) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.stop();
            state_.currentScreen = TubeState::Screen::Home;
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            state_.currentScreen = TubeState::Screen::Home;
            focus_manager_.setGrid(home_grid_);
        }
    } else if (button == SDL_CONTROLLER_BUTTON_X) {
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
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
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search)
            focus_manager_.handleInput(0, -1);
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search)
            focus_manager_.handleInput(0, 1);
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(-10);
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(-1, 0);
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.seek(10);
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(1, 0);
        }
    } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        state_.showUi = !state_.showUi;
        uiDirty_ = true;
    }
}

void App::handleJoyHat(Uint8 value) {
    if (value & SDL_HAT_UP) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, -1, w, h, uiDirty_); return; }
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(0, -1);
            uiDirty_ = true;
        }
    }
    if (value & SDL_HAT_DOWN) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, 1, w, h, uiDirty_); return; }
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(0, 1);
            uiDirty_ = true;
        }
    }
    if (value & SDL_HAT_LEFT) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, -1, 0, w, h, uiDirty_); return; }
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(-1, 0);
            uiDirty_ = true;
        }
    }
    if (value & SDL_HAT_RIGHT) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 1, 0, w, h, uiDirty_); return; }
        if (state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) {
            focus_manager_.handleInput(1, 0);
            uiDirty_ = true;
        }
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
    home_page_ = 1;
    homeLoadFailed_ = false;
    
    using namespace std::chrono;
    auto now = steady_clock::now();
    if (!cached_trending_videos_.empty() && duration_cast<minutes>(now - trending_cache_time_).count() < 15) {
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
    
    youtube_api_.search("trending", home_page_, [this, now](bool success, const std::vector<YouTubeVideo>& results) {
        if (success && !results.empty()) {
            cached_trending_videos_ = results;
            trending_cache_time_ = now;
            home_grid_->cards.clear();
            for (const auto& v : results) {
                auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                card->onClick = [this, v]() { playVideo(v); };
                home_grid_->addCard(card);
            }
            focus_manager_.setGrid(home_grid_);
        } else {
            homeLoadFailed_ = true;
        }
        uiDirty_ = true;
    });
}

void App::loadMoreHomeFeeds() {
    if (state_.isLoadingVideo || state_.isSearching) return;
    state_.isSearching = true;
    uiDirty_ = true;
    home_page_++;
    
    youtube_api_.search("trending", home_page_, [this](bool success, const std::vector<YouTubeVideo>& results) {
        state_.isSearching = false;
        if (success && !results.empty()) {
            for (const auto& v : results) {
                auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                card->onClick = [this, v]() { playVideo(v); };
                home_grid_->addCard(card);
            }
        }
        uiDirty_ = true;
    });
}

void App::loadMoreSearchResults() {
    if (state_.isLoadingVideo || state_.isSearching || current_search_query_.empty()) return;
    state_.isSearching = true;
    uiDirty_ = true;
    search_page_++;
    
    youtube_api_.search(current_search_query_, search_page_, [this](bool success, const std::vector<YouTubeVideo>& results) {
        state_.isSearching = false;
        if (success && !results.empty()) {
            for (const auto& v : results) {
                auto card = std::make_shared<ui::VideoCard>(image_manager_.get(), v);
                card->onClick = [this, v]() { playVideo(v); };
                search_grid_->addCard(card);
            }
        }
        uiDirty_ = true;
    });
}

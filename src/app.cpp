#include "app.hpp"
#include "renderer_utils.hpp"
#include "stb_image.h"
#include <iostream>
#include <algorithm>

static void logInfo(const std::string& msg) { std::cout << "[INFO] " << msg << std::endl; }
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
    if (!mpv_player_.initialize(window_, renderer_)) {
        logError("MPV init failed");
        return false;
    }
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
    search_results_.clear();
    selected_result_idx_ = 0;
    
    for (auto& pair : thumbnail_cache_) {
        if (pair.second) SDL_DestroyTexture(pair.second);
    }
    thumbnail_cache_.clear();

    youtube_api_.search(query, [this](bool success, const std::vector<YouTubeVideo>& results) {
        if (success) {
            search_results_ = results;
            uiDirty_ = true;
        }
    });
}

void App::playVideo(const YouTubeVideo& video) {
    current_video_ = video;
    youtube_api_.getStreamUrl(video.id, [this](bool success, const std::string& url) {
        if (success) {
            state_.currentScreen = TubeState::Screen::Playback;
            mpv_player_.play(url);
            uiDirty_ = true;
        }
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
    int width = 0, height = 0;
    SDL_GetWindowSize(window_, &width, &height);

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
        SDL_SetRenderDrawColor(renderer_, 20, 22, 26, 255);
        SDL_RenderClear(renderer_);
    }

    if (state_.showUi || state_.inputMode != TubeState::InputMode::None) {
        if (state_.currentScreen == TubeState::Screen::Home) {
            SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 255);
            SDL_Rect headerRect{0, 0, width, 60};
            SDL_RenderFillRect(renderer_, &headerRect);
            drawTextShadow(renderer_, 20, 20, "tubelite - YouTube Client", 3, {255, 80, 80, 255});
            
            drawText(renderer_, 40, 120, "Welcome to Tubelite!", 2, {220, 220, 220, 255});
            drawText(renderer_, 40, 160, "Press Y to search for a video.", 2, {150, 150, 150, 255});
            drawText(renderer_, 40, 200, "Press START + SELECT to exit.", 2, {150, 150, 150, 255});
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 255);
            SDL_Rect headerRect{0, 0, width, 60};
            SDL_RenderFillRect(renderer_, &headerRect);
            drawTextShadow(renderer_, 20, 20, "Search Results", 3, {255, 80, 80, 255});
            
            int y = 70;
            int idx = 0;
            for (const auto& video : search_results_) {
                if (y > height - 100) break;
                bool selected = (idx == selected_result_idx_);
                if (selected) {
                    SDL_SetRenderDrawColor(renderer_, 60, 68, 80, 255);
                    SDL_Rect bgRect{10, y - 5, width - 20, 56};
                    SDL_RenderFillRect(renderer_, &bgRect);
                    SDL_SetRenderDrawColor(renderer_, 100, 150, 255, 255);
                    SDL_RenderDrawRect(renderer_, &bgRect);
                }
                
                int thumbWidth = 60;
                int thumbHeight = 45;
                SDL_Texture* thumb = getThumbnail(video.id);
                if (thumb) {
                    SDL_Rect thumbRect{20, y, thumbWidth, thumbHeight};
                    SDL_RenderCopy(renderer_, thumb, nullptr, &thumbRect);
                } else {
                    SDL_SetRenderDrawColor(renderer_, 40, 44, 50, 255);
                    SDL_Rect thumbRect{20, y, thumbWidth, thumbHeight};
                    SDL_RenderFillRect(renderer_, &thumbRect);
                }
                
                int textX = 20 + thumbWidth + 10;
                SDL_Color titleColor = selected ? SDL_Color{255, 255, 255, 255} : SDL_Color{200, 200, 200, 255};
                std::string title = video.title;
                if (title.length() > 40) title = title.substr(0, 37) + "...";
                drawText(renderer_, textX, y + 2, title, 2, titleColor);
                drawText(renderer_, textX, y + 26, video.author + " | " + video.duration_string + " | " + video.view_count_string, 1, {120, 130, 140, 255});
                y += 56;
                idx++;
            }
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showUi) {
                SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
                SDL_Rect headerRect{0, 0, width, 50};
                SDL_RenderFillRect(renderer_, &headerRect);
                drawTextShadow(renderer_, 20, 16, "Playing: " + current_video_.title, 2, {255, 255, 255, 255});
            }
        }
        status_.render(renderer_, state_, width, height, uiDirty_);
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
    case SDLK_y: openKeyboard(); break;
    case SDLK_UP:
        if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) { selected_result_idx_--; uiDirty_ = true; }
        break;
    case SDLK_DOWN:
        if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < (int)search_results_.size() - 1) { selected_result_idx_++; uiDirty_ = true; }
        break;
    case SDLK_RETURN:
        if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) playVideo(search_results_[selected_result_idx_]);
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
        openKeyboard();
    } else if (button == SDL_CONTROLLER_BUTTON_A) {
        if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) {
            playVideo(search_results_[selected_result_idx_]);
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
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
        if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0)
            selected_result_idx_--;
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < (int)search_results_.size() - 1)
            selected_result_idx_++;
    } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        state_.showUi = !state_.showUi;
        uiDirty_ = true;
    }
}

void App::handleJoyHat(Uint8 value) {
    if (value & SDL_HAT_UP) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, -1, w, h, uiDirty_); return; }
        if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) { selected_result_idx_--; uiDirty_ = true; }
    }
    if (value & SDL_HAT_DOWN) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 0, 1, w, h, uiDirty_); return; }
        if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < (int)search_results_.size() - 1) { selected_result_idx_++; uiDirty_ = true; }
    }
    if (value & SDL_HAT_LEFT) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, -1, 0, w, h, uiDirty_); return; }
    }
    if (value & SDL_HAT_RIGHT) {
        if (state_.inputMode == TubeState::InputMode::SearchText) { int w=0,h=0; SDL_GetWindowSize(window_,&w,&h); keyboard_.moveSelection(state_, 1, 0, w, h, uiDirty_); return; }
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

SDL_Texture* App::getThumbnail(const std::string& video_id) {
    if (thumbnail_cache_.find(video_id) != thumbnail_cache_.end()) {
        return thumbnail_cache_[video_id];
    }
    std::string path = "/tmp/tubelite_thumbs/" + video_id + ".jpg";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    fclose(f);

    int w, h, channels;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        thumbnail_cache_[video_id] = nullptr; // prevent reloading bad image
        return nullptr;
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, data, w * 4);
        thumbnail_cache_[video_id] = tex;
    }
    stbi_image_free(data);
    return tex;
}

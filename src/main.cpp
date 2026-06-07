
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <SDL2/SDL.h>

#include "mpv_player.hpp"
#include "youtube_api.hpp"

struct TubeState {
    enum class Screen { Home, Search, Playback };
    enum class InputMode { None, SearchText };
    enum class KeyboardMode { Lowercase, Uppercase, Symbols };

    Screen currentScreen{Screen::Home};
    InputMode inputMode{InputMode::None};
    KeyboardMode keyboardMode{KeyboardMode::Lowercase};
    
    std::string textBuffer;
    int textCursor{0};
    bool running{true};
    int keyboardSelectedIndex{0};
    bool replaceBufferOnNextInput{false};
    bool showUi{true};
    
    float leftStickX{0.0f};
    float leftStickY{0.0f};
    float rightStickX{0.0f};
    float rightStickY{0.0f};
    float leftTrigger{0.0f};
    float rightTrigger{0.0f};
    bool dpadUpPressed{false};
    bool dpadDownPressed{false};
    bool dpadLeftPressed{false};
    bool dpadRightPressed{false};
};

// Logging utilities
inline void logInfo(const std::string& msg) {
    std::cout << "[INFO] " << msg << std::endl;
}
inline void logError(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

class App final {
public:
    App() {}
    ~App() { shutdown(); }

    bool initialize() {
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


    void updateSticks() {
        if (hasActiveKeyboard()) {
            if (updateKeyboardSelectionFromDpad()) return;
            if (updateKeyboardSelectionFromStick()) return;
            if (updateKeyboardCursorFromTriggers()) return;
        }
    }

    void run() {
        while (state_.running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handleEvent(event);
            }
            updateSticks();
            updateKeyboardCursorBlinkState();
            mpv_player_.update();
            renderFrame();
            SDL_Delay(16);
        }
    }


    void shutdown() {
        SDL_StopTextInput();
        closeController();
        destroyUiTextures();
        mpv_player_.shutdown();
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

';
            logError(err);
            return false;
        }

        Uint32 rendererFlags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE;
        if (useVsync) {
            rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
        }

        renderer_ = SDL_CreateRenderer(window_, -1, rendererFlags);
        if (renderer_ == nullptr) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }

        if (renderer_ == nullptr) {
            std::string err = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
            std::cerr << err << '\n';

        // Log and select preferred pixel format for framebuffer texture
        // Priority: ARGB8888 (matches Xvfb BGRX on little-endian) > XRGB8888 > first available
        preferredTextureFormat_ = SDL_PIXELFORMAT_UNKNOWN;
        std::ostringstream fmtss;
        fmtss << "Texture formats:";
        for (Uint32 i = 0; i < info.num_texture_formats; ++i) {
            Uint32 fmt = info.texture_formats[i];
            fmtss << ' ' << SDL_GetPixelFormatName(fmt);
            if (preferredTextureFormat_ == SDL_PIXELFORMAT_UNKNOWN) {
                preferredTextureFormat_ = fmt;
            }
            if (fmt == SDL_PIXELFORMAT_ARGB8888) {
                preferredTextureFormat_ = fmt;
            }
        }
        logInfo(fmtss.str());
        std::cout << fmtss.str() << '\n';

        std::ostringstream selss;
        selss << "Selected texture format: " << SDL_GetPixelFormatName(preferredTextureFormat_);
        logInfo(selss.str());
        std::cout << selss.str() << '\n

    std::string& activeBuffer() {
        return state_.textBuffer;
    }

    void eraseActiveBufferChar() {
        auto& buffer = activeBuffer();
        if (state_.replaceBufferOnNextInput) {
            buffer.clear();
            state_.textCursor = 0;
            state_.replaceBufferOnNextInput = false;
        } else if (!buffer.empty() && state_.textCursor > 0) {
            buffer.erase(static_cast<size_t>(state_.textCursor - 1), 1);
            state_.textCursor--;
        }
    }

    bool hasActiveKeyboard() const {
        return state_.inputMode == TubeState::InputMode::SearchText;
    }

    void openKeyboard() {
        state_.inputMode = TubeState::InputMode::SearchText;
        state_.textBuffer.clear();
        state_.textCursor = 0;
        state_.keyboardSelectedIndex = 0;
        uiDirty_ = true;
    }

    void closeKeyboard(bool commit) {
        state_.inputMode = TubeState::InputMode::None;
        if (commit && !state_.textBuffer.empty()) {
            doSearch(state_.textBuffer);
        }
        uiDirty_ = true;
    }

    void doSearch(const std::string& query) {
        state_.currentScreen = TubeState::Screen::Search;
        search_results_.clear();
        selected_result_idx_ = 0;
        youtube_api_.search(query, [this](bool success, const std::vector<YouTubeVideo>& results) {
            if (success) {
                search_results_ = results;
                uiDirty_ = true;
            }
        });
    }

    void playVideo(const YouTubeVideo& video) {
        current_video_ = video;
        youtube_api_.getStreamUrl(video.id, [this](bool success, const std::string& url) {
            if (success) {
                state_.currentScreen = TubeState::Screen::Playback;
                mpv_player_.play(url);
                uiDirty_ = true;
            }
        });
    }

    void activateKeyboardGo() {
        if (!hasActiveKeyboard()) return;
        closeKeyboard(true);
    }

    void activateSelectedKey() {
        if (!hasActiveKeyboard()) return;

        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        const KeyboardKeyGeometry* key = selectedKeyboardKey(layoutInfo);
        if (key == nullptr) return;
        
        const std::string value = key->value;

        if (value == "__BACKSPACE__") {
            eraseActiveBufferChar();
        } else if (value == "__MODE__") {
            toggleKeyboardMode();
        } else if (value == "__LEFT__") {
            moveActiveCursor(-1);
        } else if (value == "__RIGHT__") {
            moveActiveCursor(1);
        } else if (value == "__ENTER__") {
            activateKeyboardGo();
        } else if (value == "__CANCEL__") {
            closeKeyboard(false);
        } else {
            insertActiveText(value);
        }
        uiDirty_ = true;
    }

    void updateTitle() {
        // No-op for SDL title since R36S doesn't show window borders
    }

    void renderFrame() {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);

        if (state_.showUi || state_.inputMode != TubeState::InputMode::None) {
            if (state_.currentScreen == TubeState::Screen::Home) {
                drawTextShadow(20, 20, "tubelite - Home", 3, {255, 255, 255, 255});
                drawText(20, 60, "Press Y to search", 2, {200, 200, 200, 255});
            } else if (state_.currentScreen == TubeState::Screen::Search) {
                drawTextShadow(20, 20, "Search Results", 3, {255, 255, 255, 255});
                int y = 60;
                int idx = 0;
                for (const auto& video : search_results_) {
                    SDL_Color color = (idx == selected_result_idx_) ? SDL_Color{255, 200, 100, 255} : SDL_Color{200, 200, 200, 255};
                    drawText(20, y, video.title.substr(0, 40), 2, color);
                    drawText(20, y + 16, video.author + " - " + video.duration_string, 1, {150, 150, 150, 255});
                    y += 40;
                    idx++;
                }
            } else if (state_.currentScreen == TubeState::Screen::Playback) {
                if (state_.showUi) {
                    drawTextShadow(20, 20, "Playing: " + current_video_.title, 2, {255, 255, 255, 255});
                }
            }
        }
        
        renderKeyboardOverlay(width, height);
        SDL_RenderPresent(renderer_);
    }

    
    void handleEvent(SDL_Event& event) {
        if (event.type == SDL_QUIT) {
            state_.running = false;
        } else if (event.type == SDL_KEYDOWN) {
            handleKey(event.key.keysym.sym);
        } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), true);
        } else if (event.type == SDL_CONTROLLERBUTTONUP) {
            handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), false);
        } else if (event.type == SDL_TEXTINPUT) {
            if (hasActiveKeyboard()) {
                insertActiveText(transformTypedText(event.text.text));
                uiDirty_ = true;
            }
        }
    }

    void handleKey(SDL_Keycode key) {
        if (hasActiveKeyboard()) {
            // Virtual keyboard mapping
            if (key == SDLK_RETURN) activateKeyboardGo();
            else if (key == SDLK_BACKSPACE) eraseActiveBufferChar();
            else if (key == SDLK_LEFT) moveActiveCursor(-1);
            else if (key == SDLK_RIGHT) moveActiveCursor(1);
            else if (key == SDLK_ESCAPE) closeKeyboard(false);
            return;
        }

        switch (key) {
        case SDLK_q:
        case SDLK_ESCAPE:
            state_.running = false;
            break;
        case SDLK_y:
            openKeyboard();
            break;
        case SDLK_UP:
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) {
                selected_result_idx_--;
                uiDirty_ = true;
            }
            break;
        case SDLK_DOWN:
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < search_results_.size() - 1) {
                selected_result_idx_++;
                uiDirty_ = true;
            }
            break;
        case SDLK_RETURN:
            if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) {
                playVideo(search_results_[selected_result_idx_]);
            }
            break;
        }
    }

    void handleControllerButton(SDL_GameControllerButton button, bool down) {
        if (button == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_BACK)) {
            state_.running = false;
            return;
        }
        
        // Handle DPAD press state
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) state_.dpadUpPressed = down;
        if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) state_.dpadDownPressed = down;
        if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) state_.dpadLeftPressed = down;
        if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) state_.dpadRightPressed = down;

        if (!down) return;
        
        if (hasActiveKeyboard()) {
            if (button == SDL_CONTROLLER_BUTTON_A) { activateSelectedKey(); return; }
            if (button == SDL_CONTROLLER_BUTTON_X) { eraseActiveBufferChar(); return; }
            if (button == SDL_CONTROLLER_BUTTON_Y) { insertActiveText(" "); return; }
            if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) { toggleKeyboardMode(); return; }
            if (button == SDL_CONTROLLER_BUTTON_START) { activateKeyboardGo(); return; }
            if (button == SDL_CONTROLLER_BUTTON_B) { closeKeyboard(false); return; }
            return;
        }

        if (button == SDL_CONTROLLER_BUTTON_Y) {
            openKeyboard();
            return;
        }
        
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) {
                playVideo(search_results_[selected_result_idx_]);
            } else if (state_.currentScreen == TubeState::Screen::Playback) {
                if (mpv_player_.isPlaying()) mpv_player_.pause(); else mpv_player_.resume();
            }
            return;
        }
        
        if (button == SDL_CONTROLLER_BUTTON_B) {
            if (state_.currentScreen == TubeState::Screen::Playback) {
                mpv_player_.stop();
                state_.currentScreen = TubeState::Screen::Home;
            } else if (state_.currentScreen == TubeState::Screen::Search) {
                state_.currentScreen = TubeState::Screen::Home;
            }
            return;
        }

        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) selected_result_idx_--;
        } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < search_results_.size() - 1) selected_result_idx_++;
        }
    }

private:
    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    SDL_GameController* controller_{nullptr};
    SDL_Joystick* joystick_{nullptr};
    SDL_Texture* keyboardOverlayTexture_{nullptr};
    bool uiDirty_{true};

    TubeState state_;
    MpvPlayer mpv_player_;
    YouTubeAPI youtube_api_;
    std::vector<YouTubeVideo> search_results_;
    int selected_result_idx_{0};
    YouTubeVideo current_video_;

    int keyboardNavDirectionX_{0};
    int keyboardNavDirectionY_{0};
    int keyboardDpadDirectionX_{0};
    int keyboardDpadDirectionY_{0};
    int triggerCursorDirection_{0};
    std::chrono::steady_clock::time_point keyboardNavStartedAt_{};
    std::chrono::steady_clock::time_point keyboardNavNextAt_{};
    std::chrono::steady_clock::time_point keyboardDpadStartedAt_{};
    std::chrono::steady_clock::time_point keyboardDpadNextAt_{};
    std::chrono::steady_clock::time_point triggerCursorStartedAt_{};
    std::chrono::steady_clock::time_point triggerCursorNextAt_{};
    bool lastKeyboardCursorVisible_{true};
};

int main(int argc, char* argv[]) {
    App app;
    if (!app.initialize()) {
        return 1;
    }
    app.run();
    return 0;
}

// tubelite - Lightweight YouTube client for R36S
// Refactored from Fire4ArkOS: browser backend replaced with mpv + yt-dlp

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <array>
#include <limits>
#include <cctype>
#include <cstdlib>
#include <SDL2/SDL.h>

#include "mpv_player.hpp"
#include "youtube_api.hpp"

// ---------------------------------------------------------------------------
// TubeState - replaces the old BrowserState
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Simple logging helpers
// ---------------------------------------------------------------------------
static void logInfo(const std::string& msg) {
    std::cout << "[INFO] " << msg << std::endl;
}
static void logError(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

// ---------------------------------------------------------------------------
// App - main application class
// ---------------------------------------------------------------------------
class App final {
public:
    App() = default;
    ~App() { shutdown(); }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
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
        if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
        if (window_)   { SDL_DestroyWindow(window_);     window_ = nullptr;   }
        SDL_Quit();
    }

private:
    // -----------------------------------------------------------------------
    // Window creation
    // -----------------------------------------------------------------------
    bool createWindow() {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

        window_ = SDL_CreateWindow(
            "tubelite",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            640, 480,
            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (window_ == nullptr) {
            logError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
            return false;
        }

        Uint32 rendererFlags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC;
        renderer_ = SDL_CreateRenderer(window_, -1, rendererFlags);
        if (renderer_ == nullptr) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (renderer_ == nullptr) {
            logError(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
            return false;
        }

        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_ShowCursor(SDL_DISABLE);
        return true;
    }

    // -----------------------------------------------------------------------
    // Controller management
    // -----------------------------------------------------------------------
    void openController() {
        if (controller_ != nullptr) return;
        const int joystickCount = SDL_NumJoysticks();
        for (int index = 0; index < joystickCount; ++index) {
            if (SDL_IsGameController(index)) {
                controller_ = SDL_GameControllerOpen(index);
                if (controller_ != nullptr) {
                    logInfo(std::string("Opened controller: ") + SDL_GameControllerName(controller_));
                    return;
                }
            }
        }
        if (joystickCount > 0) {
            joystick_ = SDL_JoystickOpen(0);
            if (joystick_ != nullptr) {
                logInfo(std::string("Opened joystick fallback: ") + SDL_JoystickName(joystick_));
            }
        }
    }

    void closeController() {
        if (controller_ != nullptr) { SDL_GameControllerClose(controller_); controller_ = nullptr; }
        if (joystick_ != nullptr)   { SDL_JoystickClose(joystick_);         joystick_ = nullptr;   }
    }

    // -----------------------------------------------------------------------
    // UI texture lifecycle
    // -----------------------------------------------------------------------
    void destroyUiTextures() {
        if (statusOverlayTexture_) {
            SDL_DestroyTexture(statusOverlayTexture_);
            statusOverlayTexture_ = nullptr;
        }

        if (keyboardOverlayTexture_ != nullptr) {
            SDL_DestroyTexture(keyboardOverlayTexture_);
            keyboardOverlayTexture_ = nullptr;
        }
    }

    // -----------------------------------------------------------------------
    // Active buffer helpers (keyboard text entry)
    // -----------------------------------------------------------------------
    std::string& activeBuffer() { return state_.textBuffer; }
    const std::string& activeBuffer() const { return state_.textBuffer; }

    bool hasActiveKeyboard() const {
        return state_.inputMode == TubeState::InputMode::SearchText;
    }

    void eraseActiveBufferChar() {
        auto& buffer = activeBuffer();
        if (state_.replaceBufferOnNextInput) {
            buffer.clear();
            state_.textCursor = 0;
            state_.replaceBufferOnNextInput = false;
        } else if (state_.textCursor > 0 && !buffer.empty()) {
            buffer.erase(static_cast<size_t>(state_.textCursor - 1), 1);
            --state_.textCursor;
        }
    }

    void insertActiveText(const std::string& text) {
        auto& buffer = activeBuffer();
        if (state_.replaceBufferOnNextInput) {
            buffer.clear();
            state_.textCursor = 0;
            state_.replaceBufferOnNextInput = false;
        }
        state_.textCursor = std::clamp(state_.textCursor, 0, static_cast<int>(buffer.size()));
        buffer.insert(static_cast<size_t>(state_.textCursor), text);
        state_.textCursor += static_cast<int>(text.size());
    }

    void moveActiveCursor(int delta) {
        if (state_.replaceBufferOnNextInput) {
            state_.textCursor = (delta < 0) ? 0 : static_cast<int>(activeBuffer().size());
            state_.replaceBufferOnNextInput = false;
            return;
        }
        state_.textCursor = std::clamp(state_.textCursor + delta, 0, static_cast<int>(activeBuffer().size()));
    }

    std::string transformTypedText(const char* text) const {
        std::string transformed{text};
        if (state_.keyboardMode == TubeState::KeyboardMode::Uppercase) {
            for (char& ch : transformed) {
                if (std::isalpha(static_cast<unsigned char>(ch))) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
            }
        }
        return transformed;
    }

    // -----------------------------------------------------------------------
    // Keyboard open / close / search / play
    // -----------------------------------------------------------------------
    void openKeyboard() {
        state_.inputMode = TubeState::InputMode::SearchText;
        state_.textBuffer.clear();
        state_.textCursor = 0;
        state_.keyboardSelectedIndex = 0;
        resetKeyboardInputRepeat();
        uiDirty_ = true;
    }

    void closeKeyboard(bool commit) {
        state_.inputMode = TubeState::InputMode::None;
        state_.leftTrigger = 0.0f;
        state_.rightTrigger = 0.0f;
        resetKeyboardInputRepeat();
        if (commit && !state_.textBuffer.empty()) {
            doSearch(state_.textBuffer);
        }
        uiDirty_ = true;
    }

    void activateKeyboardGo() {
        if (!hasActiveKeyboard()) return;
        closeKeyboard(true);
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

    // -----------------------------------------------------------------------
    // Keyboard layout data
    // -----------------------------------------------------------------------
    struct KeyboardKey {
        const char* label;
        const char* value;
        int widthUnits;
    };

    struct KeyboardKeyGeometry {
        SDL_Rect bounds{};
        std::string label;
        std::string value;
        int row{0};
        int col{0};
        int index{0};
    };

    struct KeyboardOverlayLayout {
        SDL_Rect panel{};
        int statusBarHeight{48};
        std::vector<KeyboardKeyGeometry> keys;
    };

    static constexpr int kKeyboardGridColumns = 12;
    static constexpr bool kKeyboardWrapAround = true;

    const std::vector<std::vector<KeyboardKey>>& keyboardLayout() const {
        static const std::vector<std::vector<KeyboardKey>> lowerLayout = {
            {{"1","1",1},{"2","2",1},{"3","3",1},{"4","4",1},{"5","5",1},{"6","6",1},
             {"7","7",1},{"8","8",1},{"9","9",1},{"0","0",1},{"-","-",1},{".",".",1}},
            {{"q","q",1},{"w","w",1},{"e","e",1},{"r","r",1},{"t","t",1},{"y","y",1},
             {"u","u",1},{"i","i",1},{"o","o",1},{"p","p",1},{"/","/",1},{":",":",1}},
            {{"a","a",1},{"s","s",1},{"d","d",1},{"f","f",1},{"g","g",1},{"h","h",1},
             {"j","j",1},{"k","k",1},{"l","l",1},{"_","_",1},{"@","@",1},{"?","?",1}},
            {{"z","z",1},{"x","x",1},{"c","c",1},{"v","v",1},{"b","b",1},{"n","n",1},
             {"m","m",1},{"&","&",1},{"=","=",1},{"+","+",1},{"#","#",1},{"%","%",1}},
            {{"MODE","__MODE__",2},{"SPACE"," ",3},{"BKSP","__BACKSPACE__",2},{"<","__LEFT__",1},
             {">","__RIGHT__",1},{"GO","__ENTER__",1},{"ESC","__CANCEL__",2}}
        };
        static const std::vector<std::vector<KeyboardKey>> upperLayout = {
            {{"1","1",1},{"2","2",1},{"3","3",1},{"4","4",1},{"5","5",1},{"6","6",1},
             {"7","7",1},{"8","8",1},{"9","9",1},{"0","0",1},{"-","-",1},{".",".",1}},
            {{"Q","Q",1},{"W","W",1},{"E","E",1},{"R","R",1},{"T","T",1},{"Y","Y",1},
             {"U","U",1},{"I","I",1},{"O","O",1},{"P","P",1},{"/","/",1},{":",":",1}},
            {{"A","A",1},{"S","S",1},{"D","D",1},{"F","F",1},{"G","G",1},{"H","H",1},
             {"J","J",1},{"K","K",1},{"L","L",1},{"_","_",1},{"@","@",1},{"?","?",1}},
            {{"Z","Z",1},{"X","X",1},{"C","C",1},{"V","V",1},{"B","B",1},{"N","N",1},
             {"M","M",1},{"&","&",1},{"=","=",1},{"+","+",1},{"#","#",1},{"%","%",1}},
            {{"MODE","__MODE__",2},{"SPACE"," ",3},{"BKSP","__BACKSPACE__",2},{"<","__LEFT__",1},
             {">","__RIGHT__",1},{"GO","__ENTER__",1},{"ESC","__CANCEL__",2}}
        };
        static const std::vector<std::vector<KeyboardKey>> symbolsLayout = {
            {{"1","1",1},{"2","2",1},{"3","3",1},{"4","4",1},{"5","5",1},{"6","6",1},
             {"7","7",1},{"8","8",1},{"9","9",1},{"0","0",1},{"[","[",1},{"]","]",1}},
            {{"!","!",1},{"@","@",1},{"#","#",1},{"$","$",1},{"%","%",1},{"^","^",1},
             {"&","&",1},{"*","*",1},{"(","(",1},{")",")",1},{"{","{",1},{"}","}",1}},
            {{"<","<",1},{">",">",1},{"/","/",1},{"\\","\\",1},{"|","|",1},{"_","_",1},
             {"+","+",1},{"=","=",1},{"~","~",1},{";",";",1},{":",":",1},{"`","`",1}},
            {{"'","'",1},{"\"","\"",1},{",",",",1},{".",".",1},{"?","?",1},{"-","-",1},
             {"@","@",1},{"#","#",1},{"%","%",1},{"&","&",1},{"*","*",1},{"=","=",1}},
            {{"MODE","__MODE__",2},{"SPACE"," ",3},{"BKSP","__BACKSPACE__",2},{"<","__LEFT__",1},
             {">","__RIGHT__",1},{"GO","__ENTER__",1},{"ESC","__CANCEL__",2}}
        };

        switch (state_.keyboardMode) {
        case TubeState::KeyboardMode::Uppercase: return upperLayout;
        case TubeState::KeyboardMode::Symbols:   return symbolsLayout;
        case TubeState::KeyboardMode::Lowercase:
        default: return lowerLayout;
        }
    }

    KeyboardOverlayLayout buildKeyboardOverlayLayout(int width, int height) const {
        KeyboardOverlayLayout layoutInfo;
        const int outerMargin = 14;
        const int panelPadding = (width < 480) ? 8 : 10;
        const int rowHeight = (height < 360) ? 26 : 30;
        const int rowGap = (height < 360) ? 6 : 7;
        const int topContent = panelPadding + 18 + 4 + 16 + 8;
        const int gridHeight = static_cast<int>(keyboardLayout().size()) * rowHeight +
                               (static_cast<int>(keyboardLayout().size()) - 1) * rowGap;
        const int panelHeight = topContent + gridHeight + panelPadding * 2;
        const int panelY = std::max(outerMargin, height - layoutInfo.statusBarHeight - panelHeight - 12);
        layoutInfo.panel = {outerMargin, panelY, std::max(120, width - outerMargin * 2), panelHeight};

        const int cellWidth = std::max(20, (layoutInfo.panel.w - panelPadding * 2) / kKeyboardGridColumns);
        int keyIndex = 0;
        int y = layoutInfo.panel.y + topContent;
        const auto& rows = keyboardLayout();
        for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const auto& row = rows[rowIndex];
            int unitsUsed = 0;
            for (const auto& key : row) unitsUsed += std::max(1, key.widthUnits);
            int x = layoutInfo.panel.x + panelPadding +
                    std::max(0, ((layoutInfo.panel.w - panelPadding * 2) - unitsUsed * cellWidth) / 2);
            for (size_t colIndex = 0; colIndex < row.size(); ++colIndex) {
                const auto& key = row[colIndex];
                KeyboardKeyGeometry geometry;
                geometry.bounds = {x, y, std::max(18, std::max(1, key.widthUnits) * cellWidth - 6), rowHeight};
                geometry.label = key.label;
                geometry.value = key.value;
                geometry.row = static_cast<int>(rowIndex);
                geometry.col = static_cast<int>(colIndex);
                geometry.index = keyIndex++;
                layoutInfo.keys.push_back(std::move(geometry));
                x += std::max(1, key.widthUnits) * cellWidth;
            }
            y += rowHeight + rowGap;
        }
        return layoutInfo;
    }

    const KeyboardKeyGeometry* selectedKeyboardKey(const KeyboardOverlayLayout& layoutInfo) const {
        if (layoutInfo.keys.empty()) return nullptr;
        const int index = std::clamp(state_.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
        return &layoutInfo.keys[static_cast<size_t>(index)];
    }

    // -----------------------------------------------------------------------
    // Keyboard navigation helpers
    // -----------------------------------------------------------------------
    void ensureKeyboardSelectionValid() {
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        if (layoutInfo.keys.empty()) { state_.keyboardSelectedIndex = 0; return; }
        state_.keyboardSelectedIndex = std::clamp(state_.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
    }

    void toggleKeyboardMode() {
        switch (state_.keyboardMode) {
        case TubeState::KeyboardMode::Lowercase: state_.keyboardMode = TubeState::KeyboardMode::Uppercase; break;
        case TubeState::KeyboardMode::Uppercase: state_.keyboardMode = TubeState::KeyboardMode::Symbols;   break;
        default:                                 state_.keyboardMode = TubeState::KeyboardMode::Lowercase; break;
        }
        ensureKeyboardSelectionValid();
        uiDirty_ = true;
    }

    void resetKeyboardInputRepeat() {
        keyboardNavDirectionX_ = 0; keyboardNavDirectionY_ = 0;
        keyboardNavStartedAt_ = {}; keyboardNavNextAt_ = {};
        keyboardDpadDirectionX_ = 0; keyboardDpadDirectionY_ = 0;
        keyboardDpadStartedAt_ = {}; keyboardDpadNextAt_ = {};
        triggerCursorDirection_ = 0;
        triggerCursorStartedAt_ = {}; triggerCursorNextAt_ = {};
    }

    static int keyCenterX(const KeyboardKeyGeometry& key) { return key.bounds.x + key.bounds.w / 2; }
    static int keyCenterY(const KeyboardKeyGeometry& key) { return key.bounds.y + key.bounds.h / 2; }

    int repeatIntervalMs(std::chrono::steady_clock::time_point startedAt, int baseMs, int minMs) const {
        if (startedAt == std::chrono::steady_clock::time_point{}) return baseMs;
        const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        return std::max(minMs, baseMs - static_cast<int>(heldMs / 250) * 18);
    }

    int resolveWrappedKeyboardSelection(const KeyboardOverlayLayout& layoutInfo, int directionX, int directionY) const {
        if (!kKeyboardWrapAround || layoutInfo.keys.empty()) return -1;
        const KeyboardKeyGeometry* current = selectedKeyboardKey(layoutInfo);
        if (current == nullptr) return 0;
        int bestIndex = -1;
        int bestScore = std::numeric_limits<int>::max();
        for (const auto& candidate : layoutInfo.keys) {
            if (candidate.index == current->index) continue;
            int score = std::numeric_limits<int>::max();
            if (directionX > 0) score = candidate.bounds.x * 10 + std::abs(keyCenterY(candidate) - keyCenterY(*current));
            else if (directionX < 0) score = (layoutInfo.panel.x + layoutInfo.panel.w - candidate.bounds.x) * 10 + std::abs(keyCenterY(candidate) - keyCenterY(*current));
            else if (directionY > 0) score = candidate.bounds.y * 10 + std::abs(keyCenterX(candidate) - keyCenterX(*current));
            else if (directionY < 0) score = (layoutInfo.panel.y + layoutInfo.panel.h - candidate.bounds.y) * 10 + std::abs(keyCenterX(candidate) - keyCenterX(*current));
            if (score < bestScore) { bestScore = score; bestIndex = candidate.index; }
        }
        return bestIndex;
    }

    void moveKeyboardSelection(int directionX, int directionY) {
        if (!hasActiveKeyboard()) return;
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        if (layoutInfo.keys.empty()) return;
        state_.keyboardSelectedIndex = std::clamp(state_.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
        const KeyboardKeyGeometry& current = layoutInfo.keys[static_cast<size_t>(state_.keyboardSelectedIndex)];
        int bestIndex = -1;
        float bestScore = std::numeric_limits<float>::max();
        for (const auto& candidate : layoutInfo.keys) {
            if (candidate.index == current.index) continue;
            const float dx = static_cast<float>(keyCenterX(candidate) - keyCenterX(current));
            const float dy = static_cast<float>(keyCenterY(candidate) - keyCenterY(current));
            if (directionX > 0 && dx <= 0.0f) continue;
            if (directionX < 0 && dx >= 0.0f) continue;
            if (directionY > 0 && dy <= 0.0f) continue;
            if (directionY < 0 && dy >= 0.0f) continue;
            const float primary = (directionX != 0) ? std::abs(dx) : std::abs(dy);
            const float secondary = (directionX != 0) ? std::abs(dy) : std::abs(dx);
            const float score = primary + secondary * 2.6f;
            if (score < bestScore) { bestScore = score; bestIndex = candidate.index; }
        }
        if (bestIndex < 0) bestIndex = resolveWrappedKeyboardSelection(layoutInfo, directionX, directionY);
        if (bestIndex >= 0) state_.keyboardSelectedIndex = bestIndex;
        uiDirty_ = true;
    }

    bool updateKeyboardSelectionFromStick() {
        const float absX = std::abs(state_.leftStickX);
        const float absY = std::abs(state_.leftStickY);
        const float threshold = 0.45f;
        int dirX = 0, dirY = 0;
        if (absX >= threshold || absY >= threshold) {
            if (absX >= absY) dirX = (state_.leftStickX > 0.0f) ? 1 : -1;
            else              dirY = (state_.leftStickY > 0.0f) ? 1 : -1;
        }
        if (dirX == 0 && dirY == 0) {
            keyboardNavDirectionX_ = 0; keyboardNavDirectionY_ = 0;
            keyboardNavStartedAt_ = {}; keyboardNavNextAt_ = {};
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (dirX != keyboardNavDirectionX_ || dirY != keyboardNavDirectionY_) {
            keyboardNavDirectionX_ = dirX; keyboardNavDirectionY_ = dirY;
            keyboardNavStartedAt_ = now;
            keyboardNavNextAt_ = now + std::chrono::milliseconds(220);
            moveKeyboardSelection(dirX, dirY);
            return true;
        }
        if (now >= keyboardNavNextAt_) {
            moveKeyboardSelection(dirX, dirY);
            keyboardNavNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(keyboardNavStartedAt_, 135, 70));
            return true;
        }
        return false;
    }

    bool updateKeyboardSelectionFromDpad() {
        int dirX = 0, dirY = 0;
        if (state_.dpadLeftPressed != state_.dpadRightPressed) dirX = state_.dpadRightPressed ? 1 : -1;
        else if (state_.dpadUpPressed != state_.dpadDownPressed) dirY = state_.dpadDownPressed ? 1 : -1;
        if (dirX == 0 && dirY == 0) {
            keyboardDpadDirectionX_ = 0; keyboardDpadDirectionY_ = 0;
            keyboardDpadStartedAt_ = {}; keyboardDpadNextAt_ = {};
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (dirX != keyboardDpadDirectionX_ || dirY != keyboardDpadDirectionY_) {
            keyboardDpadDirectionX_ = dirX; keyboardDpadDirectionY_ = dirY;
            keyboardDpadStartedAt_ = now;
            keyboardDpadNextAt_ = now + std::chrono::milliseconds(220);
            moveKeyboardSelection(dirX, dirY);
            return true;
        }
        if (now >= keyboardDpadNextAt_) {
            moveKeyboardSelection(dirX, dirY);
            keyboardDpadNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(keyboardDpadStartedAt_, 135, 70));
            return true;
        }
        return false;
    }

    bool updateKeyboardCursorFromTriggers() {
        int direction = 0;
        if (state_.leftTrigger > 0.55f && state_.rightTrigger <= 0.55f) direction = -1;
        else if (state_.rightTrigger > 0.55f && state_.leftTrigger <= 0.55f) direction = 1;
        if (direction == 0) {
            triggerCursorDirection_ = 0; triggerCursorStartedAt_ = {}; triggerCursorNextAt_ = {};
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (direction != triggerCursorDirection_) {
            triggerCursorDirection_ = direction; triggerCursorStartedAt_ = now;
            triggerCursorNextAt_ = now + std::chrono::milliseconds(220);
            moveActiveCursor(direction);
            return true;
        }
        if (now >= triggerCursorNextAt_) {
            moveActiveCursor(direction);
            triggerCursorNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(triggerCursorStartedAt_, 140, 90));
            return true;
        }
        return false;
    }

    void updateSticks() {
        if (hasActiveKeyboard()) {
            if (updateKeyboardSelectionFromDpad()) return;
            if (updateKeyboardSelectionFromStick()) return;
            updateKeyboardCursorFromTriggers();
        }
    }

    // -----------------------------------------------------------------------
    // Keyboard overlay text helpers
    // -----------------------------------------------------------------------
    std::string keyboardModeLabel() const {
        switch (state_.keyboardMode) {
        case TubeState::KeyboardMode::Uppercase: return "UPPER";
        case TubeState::KeyboardMode::Symbols:   return "SYMBOLS";
        default: return "LOWER";
        }
    }

    std::string keyboardPreviewText() const {
        std::string preview = activeBuffer();
        const int cursor = state_.replaceBufferOnNextInput
            ? 0
            : std::clamp(state_.textCursor, 0, static_cast<int>(preview.size()));
        if (keyboardCursorVisible()) preview.insert(static_cast<size_t>(cursor), "|");
        else                         preview.insert(static_cast<size_t>(cursor), " ");
        constexpr int maxChars = 38;
        if (static_cast<int>(preview.size()) > maxChars) {
            int start = std::clamp(cursor - (maxChars / 2), 0, static_cast<int>(preview.size()) - maxChars);
            preview = preview.substr(static_cast<size_t>(start), maxChars);
        }
        return preview;
    }

    bool keyboardCursorVisible() const {
        using namespace std::chrono;
        const auto phase = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 400;
        return (phase % 2) == 0;
    }

    void updateKeyboardCursorBlinkState() {
        if (!hasActiveKeyboard()) {
            lastKeyboardCursorVisible_ = keyboardCursorVisible();
            return;
        }
        const bool visible = keyboardCursorVisible();
        if (visible != lastKeyboardCursorVisible_) {
            lastKeyboardCursorVisible_ = visible;
            uiDirty_ = true;
        }
    }

    // -----------------------------------------------------------------------
    // Activate selected keyboard key
    // -----------------------------------------------------------------------
    void activateSelectedKey() {
        if (!hasActiveKeyboard()) return;
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        const KeyboardKeyGeometry* key = selectedKeyboardKey(layoutInfo);
        if (key == nullptr) return;
        const std::string value = key->value;
        if (value == "__BACKSPACE__")     eraseActiveBufferChar();
        else if (value == "__MODE__")     { toggleKeyboardMode(); return; }
        else if (value == "__LEFT__")     moveActiveCursor(-1);
        else if (value == "__RIGHT__")    moveActiveCursor(1);
        else if (value == "__ENTER__")    { activateKeyboardGo(); return; }
        else if (value == "__CANCEL__")   { closeKeyboard(false); return; }
        else                              insertActiveText(value);
        uiDirty_ = true;
    }

    // -----------------------------------------------------------------------
    // 5x7 pixel font glyph data
    // -----------------------------------------------------------------------
    static std::array<uint8_t, 7> glyphFor(char ch) {
        switch (static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ch)))) {
        case 'A': return {14, 17, 17, 31, 17, 17, 17};
        case 'B': return {30, 17, 17, 30, 17, 17, 30};
        case 'C': return {14, 17, 16, 16, 16, 17, 14};
        case 'D': return {30, 17, 17, 17, 17, 17, 30};
        case 'E': return {31, 16, 16, 30, 16, 16, 31};
        case 'F': return {31, 16, 16, 30, 16, 16, 16};
        case 'G': return {14, 17, 16, 23, 17, 17, 15};
        case 'H': return {17, 17, 17, 31, 17, 17, 17};
        case 'I': return {31, 4, 4, 4, 4, 4, 31};
        case 'J': return {7, 2, 2, 2, 18, 18, 12};
        case 'K': return {17, 18, 20, 24, 20, 18, 17};
        case 'L': return {16, 16, 16, 16, 16, 16, 31};
        case 'M': return {17, 27, 21, 17, 17, 17, 17};
        case 'N': return {17, 25, 21, 19, 17, 17, 17};
        case 'O': return {14, 17, 17, 17, 17, 17, 14};
        case 'P': return {30, 17, 17, 30, 16, 16, 16};
        case 'Q': return {14, 17, 17, 17, 21, 18, 13};
        case 'R': return {30, 17, 17, 30, 20, 18, 17};
        case 'S': return {15, 16, 16, 14, 1, 1, 30};
        case 'T': return {31, 4, 4, 4, 4, 4, 4};
        case 'U': return {17, 17, 17, 17, 17, 17, 14};
        case 'V': return {17, 17, 17, 17, 17, 10, 4};
        case 'W': return {17, 17, 17, 17, 21, 21, 10};
        case 'X': return {17, 17, 10, 4, 10, 17, 17};
        case 'Y': return {17, 17, 10, 4, 4, 4, 4};
        case 'Z': return {31, 1, 2, 4, 8, 16, 31};
        case '0': return {14, 17, 19, 21, 25, 17, 14};
        case '1': return {4, 12, 4, 4, 4, 4, 14};
        case '2': return {14, 17, 1, 2, 4, 8, 31};
        case '3': return {30, 1, 1, 6, 1, 1, 30};
        case '4': return {2, 6, 10, 18, 31, 2, 2};
        case '5': return {31, 16, 16, 30, 1, 1, 30};
        case '6': return {14, 16, 16, 30, 17, 17, 14};
        case '7': return {31, 1, 2, 4, 8, 8, 8};
        case '8': return {14, 17, 17, 14, 17, 17, 14};
        case '9': return {14, 17, 17, 15, 1, 1, 14};
        case '-': return {0, 0, 0, 31, 0, 0, 0};
        case '.': return {0, 0, 0, 0, 0, 6, 6};
        case '/': return {1, 2, 2, 4, 8, 8, 16};
        case ':': return {0, 6, 6, 0, 6, 6, 0};
        case '_': return {0, 0, 0, 0, 0, 0, 31};
        case '?': return {14, 17, 1, 2, 4, 0, 4};
        case '@': return {14, 17, 1, 13, 21, 21, 14};
        case '&': return {12, 18, 20, 8, 21, 18, 13};
        case '=': return {0, 31, 0, 31, 0, 0, 0};
        case '+': return {0, 4, 4, 31, 4, 4, 0};
        case '#': return {10, 10, 31, 10, 31, 10, 10};
        case '%': return {24, 25, 2, 4, 8, 19, 3};
        case '!': return {4, 4, 4, 4, 4, 0, 4};
        case '$': return {4, 15, 20, 14, 5, 30, 4};
        case '^': return {4, 10, 17, 0, 0, 0, 0};
        case '*': return {0, 17, 10, 31, 10, 17, 0};
        case '(': return {2, 4, 8, 8, 8, 4, 2};
        case ')': return {8, 4, 2, 2, 2, 4, 8};
        case '[': return {14, 8, 8, 8, 8, 8, 14};
        case ']': return {14, 2, 2, 2, 2, 2, 14};
        case '{': return {2, 4, 4, 8, 4, 4, 2};
        case '}': return {8, 4, 4, 2, 4, 4, 8};
        case '<': return {2, 4, 8, 16, 8, 4, 2};
        case '>': return {8, 4, 2, 1, 2, 4, 8};
        case ';': return {0, 4, 4, 0, 4, 4, 8};
        case '\'': return {4, 4, 2, 0, 0, 0, 0};
        case '"': return {10, 10, 4, 0, 0, 0, 0};
        case '\\': return {16, 8, 8, 4, 2, 2, 1};
        case '|': return {4, 4, 4, 4, 4, 4, 4};
        case '~': return {0, 0, 13, 18, 0, 0, 0};
        case '`': return {8, 4, 2, 0, 0, 0, 0};
        case ',': return {0, 0, 0, 0, 0, 4, 8};
        case ' ': return {0, 0, 0, 0, 0, 0, 0};
        default:  return {0, 0, 0, 0, 0, 0, 0};
        }
    }

    void drawGlyph(int x, int y, char ch, int scale, SDL_Color color) {
        SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
        const auto glyph = glyphFor(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[static_cast<size_t>(row)] >> (4 - col)) & 1U) {
                    SDL_Rect pixel{x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer_, &pixel);
                }
            }
        }
    }

    void drawText(int x, int y, const std::string& text, int scale, SDL_Color color) {
        int cursor = x;
        for (char ch : text) { drawGlyph(cursor, y, ch, scale, color); cursor += scale * 6; }
    }

    void drawTextShadow(int x, int y, const std::string& text, int scale, SDL_Color color) {
        const int off = std::max(1, scale / 2);
        drawText(x + off, y + off, text, scale, {8, 10, 12, 200});
        drawText(x, y, text, scale, color);
    }

    SDL_Texture* createTargetTexture(int width, int height) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height);
        if (texture == nullptr)
            texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
        if (texture != nullptr) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        return texture;
    }

    // -----------------------------------------------------------------------
    // Keyboard overlay rendering
    // -----------------------------------------------------------------------
    
    SDL_Texture* statusOverlayTexture_{nullptr};
    int statusOverlayWidth_{0};
    int statusOverlayHeight_{0};

    void renderStatusOverlay(int width, int height) {
        int statusBarHeight = 48;
        if (statusOverlayTexture_ == nullptr || statusOverlayWidth_ != width || statusOverlayHeight_ != statusBarHeight || uiDirty_) {
            if (statusOverlayTexture_) SDL_DestroyTexture(statusOverlayTexture_);
            statusOverlayWidth_ = width;
            statusOverlayHeight_ = statusBarHeight;
            statusOverlayTexture_ = createTargetTexture(width, statusBarHeight);
            
            if (statusOverlayTexture_) {
                SDL_Texture* prev = SDL_GetRenderTarget(renderer_);
                SDL_SetRenderTarget(renderer_, statusOverlayTexture_);
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer_, 12, 14, 18, 255);
                SDL_RenderClear(renderer_);
                
                SDL_Color textColor{150, 160, 170, 255};
                SDL_Color accent{255, 100, 100, 255};
                
                std::string shortcuts = "A/ENTER: Select | B/ESC: Back/Close | Y: Search | START+SELECT: Quit";
                if (hasActiveKeyboard()) {
                    shortcuts = "A: Type | B: Close KB | Y: Space | X: Backspace | L1: Mode | START: Go";
                }
                
                drawTextShadow(20, 16, shortcuts, 1, textColor);
                
                SDL_SetRenderTarget(renderer_, prev);
            }
        }
        
        if (statusOverlayTexture_) {
            SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
            SDL_RenderCopy(renderer_, statusOverlayTexture_, nullptr, &dst);
        }
    }

    void renderKeyboardOverlay(int width, int height) {
        if (!hasActiveKeyboard()) {
            if (keyboardOverlayTexture_ != nullptr) {
                SDL_DestroyTexture(keyboardOverlayTexture_);
                keyboardOverlayTexture_ = nullptr;
                keyboardOverlayWidth_ = 0;
                keyboardOverlayHeight_ = 0;
            }
            return;
        }
        ensureKeyboardSelectionValid();
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        if (keyboardOverlayTexture_ == nullptr ||
            keyboardOverlayWidth_ != layoutInfo.panel.w ||
            keyboardOverlayHeight_ != layoutInfo.panel.h ||
            uiDirty_) {
            if (keyboardOverlayTexture_ != nullptr) {
                SDL_DestroyTexture(keyboardOverlayTexture_);
                keyboardOverlayTexture_ = nullptr;
            }
            keyboardOverlayWidth_ = layoutInfo.panel.w;
            keyboardOverlayHeight_ = layoutInfo.panel.h;
            keyboardOverlayTexture_ = createTargetTexture(layoutInfo.panel.w, layoutInfo.panel.h);
            if (keyboardOverlayTexture_ == nullptr) return;

            SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer_);
            SDL_SetRenderTarget(renderer_, keyboardOverlayTexture_);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, 18, 20, 24, 255);
            SDL_RenderClear(renderer_);

            SDL_Color textColor{226, 230, 236, 255};
            SDL_Color accent{110, 192, 255, 255};
            const std::string header = "SEARCH [" + keyboardModeLabel() + "]";
            drawTextShadow(12, 12, header, 2, accent);
            drawTextShadow(12, 34, keyboardPreviewText(), 2, textColor);

            SDL_SetRenderDrawColor(renderer_, 28, 32, 38, 255);
            SDL_RenderDrawLine(renderer_, 12, 60, layoutInfo.panel.w - 12, 60);

            for (const auto& key : layoutInfo.keys) {
                SDL_Rect keyRect{
                    key.bounds.x - layoutInfo.panel.x,
                    key.bounds.y - layoutInfo.panel.y,
                    key.bounds.w, key.bounds.h};
                const bool selected = key.index == state_.keyboardSelectedIndex;
                SDL_SetRenderDrawColor(renderer_,
                    selected ? 72 : 28, selected ? 138 : 32, selected ? 190 : 38, 255);
                SDL_RenderFillRect(renderer_, &keyRect);
                SDL_SetRenderDrawColor(renderer_,
                    selected ? 178 : 46, selected ? 216 : 52, selected ? 240 : 58, 255);
                SDL_RenderDrawRect(renderer_, &keyRect);
                drawTextShadow(keyRect.x + 8, keyRect.y + 8, key.label, 2,
                    selected ? SDL_Color{12, 16, 22, 255} : textColor);
            }
            SDL_SetRenderTarget(renderer_, previousTarget);
            uiDirty_ = false;
        }
        SDL_Rect overlay = layoutInfo.panel;
        SDL_RenderCopy(renderer_, keyboardOverlayTexture_, nullptr, &overlay);
    }

    // -----------------------------------------------------------------------
    // Main frame rendering
    // -----------------------------------------------------------------------
    
    void renderFrame() {
        SDL_SetRenderDrawColor(renderer_, 20, 22, 26, 255); // Dark background
        SDL_RenderClear(renderer_);
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);

        if (state_.showUi || state_.inputMode != TubeState::InputMode::None) {
            if (state_.currentScreen == TubeState::Screen::Home) {
                // Draw cool header
                SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 255);
                SDL_Rect headerRect{0, 0, width, 60};
                SDL_RenderFillRect(renderer_, &headerRect);
                drawTextShadow(20, 20, "tubelite - YouTube Client", 3, {255, 80, 80, 255});
                
                drawText(40, 120, "Welcome to Tubelite!", 2, {220, 220, 220, 255});
                drawText(40, 160, "Press Y to search for a video.", 2, {150, 150, 150, 255});
                drawText(40, 200, "Press START + SELECT to exit.", 2, {150, 150, 150, 255});
            } else if (state_.currentScreen == TubeState::Screen::Search) {
                SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 255);
                SDL_Rect headerRect{0, 0, width, 60};
                SDL_RenderFillRect(renderer_, &headerRect);
                drawTextShadow(20, 20, "Search Results", 3, {255, 80, 80, 255});
                
                int y = 70;
                int idx = 0;
                for (const auto& video : search_results_) {
                    if (y > height - 100) break; // Don't draw past screen
                    
                    bool selected = (idx == selected_result_idx_);
                    if (selected) {
                        SDL_SetRenderDrawColor(renderer_, 60, 68, 80, 255);
                        SDL_Rect bgRect{10, y - 5, width - 20, 48};
                        SDL_RenderFillRect(renderer_, &bgRect);
                        SDL_SetRenderDrawColor(renderer_, 100, 150, 255, 255);
                        SDL_RenderDrawRect(renderer_, &bgRect);
                    }
                    
                    SDL_Color titleColor = selected ? SDL_Color{255, 255, 255, 255} : SDL_Color{200, 200, 200, 255};
                    std::string title = video.title;
                    if (title.length() > 50) title = title.substr(0, 47) + "...";
                    drawText(20, y, title, 2, titleColor);
                    drawText(20, y + 20, video.author + " | " + video.duration_string, 1, {120, 130, 140, 255});
                    y += 50;
                    idx++;
                }
            } else if (state_.currentScreen == TubeState::Screen::Playback) {
                if (state_.showUi) {
                    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
                    SDL_Rect headerRect{0, 0, width, 50};
                    SDL_RenderFillRect(renderer_, &headerRect);
                    drawTextShadow(20, 16, "Playing: " + current_video_.title, 2, {255, 255, 255, 255});
                }
            }
        }
        
        if (state_.showUi || state_.inputMode != TubeState::InputMode::None) {
            renderStatusOverlay(width, height);
        }
        
        renderKeyboardOverlay(width, height);
        SDL_RenderPresent(renderer_);
    }

    void handleEvent(SDL_Event& event) {
        switch (event.type) {
        case SDL_QUIT:
            state_.running = false;
            break;
        case SDL_KEYDOWN:
            handleKey(event.key.keysym.sym);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            openController();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            closeController();
            openController();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), true);
            break;
        case SDL_CONTROLLERBUTTONUP:
            handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), false);
            break;
        
        case SDL_JOYHATMOTION:
            handleJoyHat(event.jhat.value); break;
        case SDL_JOYAXISMOTION:
            handleJoyAxis(event.jaxis); break;
        case SDL_JOYBUTTONDOWN:
            handleJoyButton(event.jbutton.button, event.jbutton.which, true); break;
        case SDL_JOYBUTTONUP:
            handleJoyButton(event.jbutton.button, event.jbutton.which, false); break;
    
        case SDL_CONTROLLERAXISMOTION:
            handleControllerAxis(event.caxis);
            break;
        case SDL_TEXTINPUT:
            if (hasActiveKeyboard()) {
                insertActiveText(transformTypedText(event.text.text));
                uiDirty_ = true;
            }
            break;
        default:
            break;
        }
    }

    void handleKey(SDL_Keycode key) {
        if (hasActiveKeyboard()) {
            if (key == SDLK_RETURN)    activateKeyboardGo();
            else if (key == SDLK_BACKSPACE) eraseActiveBufferChar();
            else if (key == SDLK_LEFT)      moveActiveCursor(-1);
            else if (key == SDLK_RIGHT)     moveActiveCursor(1);
            else if (key == SDLK_UP)        moveKeyboardSelection(0, -1);
            else if (key == SDLK_DOWN)      moveKeyboardSelection(0, 1);
            else if (key == SDLK_ESCAPE)    closeKeyboard(false);
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
            if (state_.currentScreen == TubeState::Screen::Search &&
                selected_result_idx_ < static_cast<int>(search_results_.size()) - 1) {
                selected_result_idx_++;
                uiDirty_ = true;
            }
            break;
        case SDLK_RETURN:
            if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) {
                playVideo(search_results_[static_cast<size_t>(selected_result_idx_)]);
            }
            break;
        default:
            break;
        }
    }

    void handleControllerButton(SDL_GameControllerButton button, bool down) {
        // Exit combo: Start + Select
        if (button == SDL_CONTROLLER_BUTTON_START &&
            controller_ != nullptr &&
            SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_BACK)) {
            state_.running = false;
            return;
        }

        // Track D-pad state
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP)    state_.dpadUpPressed = down;
        if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  state_.dpadDownPressed = down;
        if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  state_.dpadLeftPressed = down;
        if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) state_.dpadRightPressed = down;

        if (!down) return; // Only handle press, not release

        // Keyboard-specific controls
        if (hasActiveKeyboard()) {
            if (button == SDL_CONTROLLER_BUTTON_A)             activateSelectedKey();
            else if (button == SDL_CONTROLLER_BUTTON_X)        eraseActiveBufferChar();
            else if (button == SDL_CONTROLLER_BUTTON_Y)        insertActiveText(" ");
            else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) toggleKeyboardMode();
            else if (button == SDL_CONTROLLER_BUTTON_START)    activateKeyboardGo();
            else if (button == SDL_CONTROLLER_BUTTON_B)        closeKeyboard(false);
            return;
        }

        // Non-keyboard controls
        if (button == SDL_CONTROLLER_BUTTON_Y) {
            openKeyboard();
        } else if (button == SDL_CONTROLLER_BUTTON_A) {
            if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) {
                playVideo(search_results_[static_cast<size_t>(selected_result_idx_)]);
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
            if (state_.currentScreen == TubeState::Screen::Search &&
                selected_result_idx_ < static_cast<int>(search_results_.size()) - 1)
                selected_result_idx_++;
        } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
            state_.showUi = !state_.showUi;
            uiDirty_ = true;
        }
    }

    
    void handleJoyHat(Uint8 value) {
        if (value & SDL_HAT_UP) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(0, -1); return; }
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) { selected_result_idx_--; uiDirty_ = true; }
        }
        if (value & SDL_HAT_DOWN) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(0, 1); return; }
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < (int)search_results_.size() - 1) { selected_result_idx_++; uiDirty_ = true; }
        }
        if (value & SDL_HAT_LEFT) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(-1, 0); return; }
        }
        if (value & SDL_HAT_RIGHT) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(1, 0); return; }
        }
    }

    void handleJoyAxis(const SDL_JoyAxisEvent& jaxis) {
        float normalized = (float)jaxis.value / 32767.0f;
        if (std::abs(jaxis.value) < 10000) normalized = 0.0f;
        
        if (jaxis.axis == 0) state_.leftStickX = normalized;
        else if (jaxis.axis == 1) state_.leftStickY = normalized;
        else if (jaxis.axis == 2) state_.rightStickX = normalized;
        else if (jaxis.axis == 3) state_.rightStickY = normalized;
    }

    void handleJoyButton(Uint8 button, SDL_JoystickID instanceId, bool down) {
        switch (button) {
        case 0: // B (South)
            handleControllerButton(SDL_CONTROLLER_BUTTON_A, down); break;
        case 1: // A (East)
            if (hasActiveKeyboard()) handleControllerButton(SDL_CONTROLLER_BUTTON_B, down);
            break;
        case 2: // X
            handleControllerButton(SDL_CONTROLLER_BUTTON_X, down); break;
        case 3: // Y
            handleControllerButton(SDL_CONTROLLER_BUTTON_Y, down); break;
        case 4: // L1
            handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, down); break;
        case 5: // R1
            handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, down); break;
        case 8: // D-Pad Up
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_UP, down); break;
        case 9: // D-Pad Down
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, down); break;
        case 10: // D-Pad Left
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, down); break;
        case 11: // D-Pad Right
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, down); break;
        case 12: // Select
        case 13: // Start
            if (down) {
                SDL_Joystick* joy = SDL_JoystickFromInstanceID(instanceId);
                if (joy && SDL_JoystickGetButton(joy, 12) && SDL_JoystickGetButton(joy, 13)) {
                    state_.running = false;
                    break;
                }
            }
            if (button == 12) handleControllerButton(SDL_CONTROLLER_BUTTON_BACK, down);
            else handleControllerButton(SDL_CONTROLLER_BUTTON_START, down);
            break;
        default: break;
        }
    }

    void handleControllerAxis(const SDL_ControllerAxisEvent& caxis) {
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

    // -----------------------------------------------------------------------
    // Member data
    // -----------------------------------------------------------------------
    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    SDL_GameController* controller_{nullptr};
    SDL_Joystick* joystick_{nullptr};

    SDL_Texture* keyboardOverlayTexture_{nullptr};
    int keyboardOverlayWidth_{0};
    int keyboardOverlayHeight_{0};
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

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int /*argc*/, char* /*argv*/[]) {
    App app;
    if (!app.initialize()) return 1;
    app.run();
    return 0;
}

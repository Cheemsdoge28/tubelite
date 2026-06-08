#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <chrono>
#include "state.hpp"

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

struct KeyboardKey {
    const char* label;
    const char* value;
    int widthUnits;
};

class KeyboardOverlay {
public:
    KeyboardOverlay() = default;
    ~KeyboardOverlay() { destroyTexture(); }

    void destroyTexture();
    void render(SDL_Renderer* renderer, const TubeState& state, int width, int height, bool& uiDirty);
    
    void toggleMode(TubeState& state, bool& uiDirty);
    void ensureSelectionValid(TubeState& state, int width, int height);
    void moveSelection(TubeState& state, int directionX, int directionY, int width, int height, bool& uiDirty);
    
    const KeyboardKeyGeometry* getSelectedKey(const TubeState& state, int width, int height) const;

    bool updateSelectionFromStick(TubeState& state, int width, int height, bool& uiDirty);
    bool updateSelectionFromDpad(TubeState& state, int width, int height, bool& uiDirty);
    
    // For trigger cursor movement, we take a callback to move the active cursor
    template<typename F>
    bool updateCursorFromTriggers(TubeState& state, bool& uiDirty, F moveCursorCallback) {
        int direction = 0;
        if (state.leftTrigger > 0.55f && state.rightTrigger <= 0.55f) direction = -1;
        else if (state.rightTrigger > 0.55f && state.leftTrigger <= 0.55f) direction = 1;
        if (direction == 0) {
            triggerCursorDirection_ = 0; triggerCursorStartedAt_ = {}; triggerCursorNextAt_ = {};
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (direction != triggerCursorDirection_) {
            triggerCursorDirection_ = direction; triggerCursorStartedAt_ = now;
            triggerCursorNextAt_ = now + std::chrono::milliseconds(220);
            moveCursorCallback(direction);
            uiDirty = true;
            return true;
        }
        if (now >= triggerCursorNextAt_) {
            moveCursorCallback(direction);
            uiDirty = true;
            triggerCursorNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(triggerCursorStartedAt_, 140, 90));
            return true;
        }
        return false;
    }

    void resetRepeatState();
    
    static std::string keyboardModeLabel(const TubeState& state);
    static std::string keyboardPreviewText(const TubeState& state, bool cursorVisible);

private:
    SDL_Texture* texture_{nullptr};
    int width_{0};
    int height_{0};

    int navDirectionX_{0};
    int navDirectionY_{0};
    int dpadDirectionX_{0};
    int dpadDirectionY_{0};
    int triggerCursorDirection_{0};
    std::chrono::steady_clock::time_point navStartedAt_{};
    std::chrono::steady_clock::time_point navNextAt_{};
    std::chrono::steady_clock::time_point dpadStartedAt_{};
    std::chrono::steady_clock::time_point dpadNextAt_{};
    std::chrono::steady_clock::time_point triggerCursorStartedAt_{};
    std::chrono::steady_clock::time_point triggerCursorNextAt_{};

    int repeatIntervalMs(std::chrono::steady_clock::time_point startedAt, int baseMs, int minMs) const;
    const std::vector<std::vector<KeyboardKey>>& keyboardLayout(const TubeState& state) const;
    KeyboardOverlayLayout buildLayout(const TubeState& state, int width, int height) const;
    int resolveWrappedSelection(const KeyboardOverlayLayout& layoutInfo, const TubeState& state, int directionX, int directionY) const;
    
    static int keyCenterX(const KeyboardKeyGeometry& key);
    static int keyCenterY(const KeyboardKeyGeometry& key);
};

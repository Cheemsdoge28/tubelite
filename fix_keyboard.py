import sys

def patch_keyboard_logic():
    with open('src/main.cpp', 'r') as f:
        content = f.read()

    # The new logic to insert
    new_logic = """
    static int keyCenterX(const KeyboardKeyGeometry& key) {
        return key.bounds.x + key.bounds.w / 2;
    }

    static int keyCenterY(const KeyboardKeyGeometry& key) {
        return key.bounds.y + key.bounds.h / 2;
    }

    int resolveWrappedKeyboardSelection(const KeyboardOverlayLayout& layoutInfo, int directionX, int directionY) const {
        if (layoutInfo.keys.empty()) return -1;
        const KeyboardKeyGeometry* current = selectedKeyboardKey(layoutInfo);
        if (current == nullptr) return 0;
        
        int bestIndex = -1;
        int bestScore = std::numeric_limits<int>::max();
        for (const auto& candidate : layoutInfo.keys) {
            if (candidate.index == current->index) continue;
            
            int score = std::numeric_limits<int>::max();
            if (directionX > 0) {
                score = candidate.bounds.x * 10 + std::abs(keyCenterY(candidate) - keyCenterY(*current));
            } else if (directionX < 0) {
                score = (layoutInfo.panel.x + layoutInfo.panel.w - candidate.bounds.x) * 10 +
                        std::abs(keyCenterY(candidate) - keyCenterY(*current));
            } else if (directionY > 0) {
                score = candidate.bounds.y * 10 + std::abs(keyCenterX(candidate) - keyCenterX(*current));
            } else if (directionY < 0) {
                score = (layoutInfo.panel.y + layoutInfo.panel.h - candidate.bounds.y) * 10 +
                        std::abs(keyCenterX(candidate) - keyCenterX(*current));
            }
            
            if (score < bestScore) {
                bestScore = score;
                bestIndex = candidate.index;
            }
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
            
            if (score < bestScore) {
                bestScore = score;
                bestIndex = candidate.index;
            }
        }

        if (bestIndex < 0) {
            bestIndex = resolveWrappedKeyboardSelection(layoutInfo, directionX, directionY);
        }
        if (bestIndex >= 0) {
            state_.keyboardSelectedIndex = bestIndex;
        }
        uiDirty_ = true;
    }
"""

    # We need to find the old moveKeyboardSelection and replace it.
    start_pos = content.find("void moveKeyboardSelection(int dx, int dy) {")
    if start_pos == -1:
        start_pos = content.find("void moveKeyboardSelection(int directionX, int directionY) {")
        
    if start_pos != -1:
        # Find the end of moveKeyboardSelection
        # We'll just look for the next function which should be `bool updateKeyboardSelectionFromStick()`
        end_pos = content.find("bool updateKeyboardSelectionFromStick()", start_pos)
        if end_pos != -1:
            content = content[:start_pos] + new_logic + "\n    " + content[end_pos:]

    with open('src/main.cpp', 'w') as f:
        f.write(content)

if __name__ == '__main__':
    patch_keyboard_logic()

#include "keyboard_overlay.hpp"
#include "renderer_utils.hpp"
#include <algorithm>
#include <limits>
#include <cmath>

static constexpr int kKeyboardGridColumns = 12;

void KeyboardOverlay::destroyTexture() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
        width_ = 0;
        height_ = 0;
    }
}

void KeyboardOverlay::resetRepeatState() {
    navDirectionX_ = 0; navDirectionY_ = 0;
    navStartedAt_ = {}; navNextAt_ = {};
    dpadDirectionX_ = 0; dpadDirectionY_ = 0;
    dpadStartedAt_ = {}; dpadNextAt_ = {};
    triggerCursorDirection_ = 0;
    triggerCursorStartedAt_ = {}; triggerCursorNextAt_ = {};
}

int KeyboardOverlay::repeatIntervalMs(std::chrono::steady_clock::time_point startedAt, int baseMs, int minMs) const {
    if (startedAt == std::chrono::steady_clock::time_point{}) return baseMs;
    const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    return std::max(minMs, baseMs - static_cast<int>(heldMs / 250) * 18);
}

std::string KeyboardOverlay::keyboardModeLabel(const TubeState& state) {
    switch (state.keyboardMode) {
    case TubeState::KeyboardMode::Uppercase: return "UPPER";
    case TubeState::KeyboardMode::Symbols:   return "SYMBOLS";
    default: return "LOWER";
    }
}

std::string KeyboardOverlay::keyboardPreviewText(const TubeState& state, bool cursorVisible) {
    std::string preview = state.textBuffer;
    const int cursor = state.replaceBufferOnNextInput
        ? 0
        : std::clamp(state.textCursor, 0, static_cast<int>(preview.size()));
    if (cursorVisible) preview.insert(static_cast<size_t>(cursor), "|");
    else               preview.insert(static_cast<size_t>(cursor), " ");
    constexpr int maxChars = 38;
    if (static_cast<int>(preview.size()) > maxChars) {
        int start = std::clamp(cursor - (maxChars / 2), 0, static_cast<int>(preview.size()) - maxChars);
        preview = preview.substr(static_cast<size_t>(start), maxChars);
    }
    return preview;
}

const std::vector<std::vector<KeyboardKey>>& KeyboardOverlay::keyboardLayout(const TubeState& state) const {
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

    switch (state.keyboardMode) {
    case TubeState::KeyboardMode::Uppercase: return upperLayout;
    case TubeState::KeyboardMode::Symbols:   return symbolsLayout;
    case TubeState::KeyboardMode::Lowercase:
    default: return lowerLayout;
    }
}

KeyboardOverlayLayout KeyboardOverlay::buildLayout(const TubeState& state, int width, int height) const {
    if (state.keyboardMode == last_layout_mode_ && width == last_layout_w_ && height == last_layout_h_) {
        return cached_layout_;
    }

    KeyboardOverlayLayout layoutInfo;
    const int outerMargin = 14;
    const int panelPadding = (width < 480) ? 8 : 10;
    const int rowHeight = (height < 360) ? 28 : 32;
    const int rowGap = (height < 360) ? 6 : 7;
    const int topContent = panelPadding + 18 + 4 + 16 + 8;
    
    const auto& rows = keyboardLayout(state);
    const int gridHeight = static_cast<int>(rows.size()) * rowHeight +
                           (static_cast<int>(rows.size()) - 1) * rowGap;
    const int panelHeight = topContent + gridHeight + panelPadding * 2;
    const int panelY = std::max(outerMargin, height - layoutInfo.statusBarHeight - panelHeight - 12);
    layoutInfo.panel = {outerMargin, panelY, std::max(120, width - outerMargin * 2), panelHeight};

    const int cellWidth = std::max(20, (layoutInfo.panel.w - panelPadding * 2) / kKeyboardGridColumns);
    int keyIndex = 0;
    int y = layoutInfo.panel.y + topContent;
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

    last_layout_mode_ = state.keyboardMode;
    last_layout_w_ = width;
    last_layout_h_ = height;
    cached_layout_ = layoutInfo;
    return layoutInfo;
}

void KeyboardOverlay::ensureSelectionValid(TubeState& state, int width, int height) {
    const auto layoutInfo = buildLayout(state, width, height);
    if (layoutInfo.keys.empty()) { state.keyboardSelectedIndex = 0; return; }
    state.keyboardSelectedIndex = std::clamp(state.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
}

void KeyboardOverlay::toggleMode(TubeState& state, bool& uiDirty) {
    switch (state.keyboardMode) {
    case TubeState::KeyboardMode::Lowercase: state.keyboardMode = TubeState::KeyboardMode::Uppercase; break;
    case TubeState::KeyboardMode::Uppercase: state.keyboardMode = TubeState::KeyboardMode::Symbols;   break;
    default:                                 state.keyboardMode = TubeState::KeyboardMode::Lowercase; break;
    }
    uiDirty = true;
}

int KeyboardOverlay::keyCenterX(const KeyboardKeyGeometry& key) {
    return key.bounds.x + key.bounds.w / 2;
}

int KeyboardOverlay::keyCenterY(const KeyboardKeyGeometry& key) {
    return key.bounds.y + key.bounds.h / 2;
}

const KeyboardKeyGeometry* KeyboardOverlay::getSelectedKey(const TubeState& state, int width, int height) const {
    const auto layoutInfo = buildLayout(state, width, height);
    if (layoutInfo.keys.empty()) return nullptr;
    const int index = std::clamp(state.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
    return &layoutInfo.keys[static_cast<size_t>(index)];
}

int KeyboardOverlay::resolveWrappedSelection(const KeyboardOverlayLayout& layoutInfo, const TubeState& state, int directionX, int directionY) const {
    if (layoutInfo.keys.empty()) return -1;
    const int index = std::clamp(state.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
    const KeyboardKeyGeometry& current = layoutInfo.keys[static_cast<size_t>(index)];
    
    int bestIndex = -1;
    int bestScore = std::numeric_limits<int>::max();
    for (const auto& candidate : layoutInfo.keys) {
        if (candidate.index == current.index) continue;
        
        int score = std::numeric_limits<int>::max();
        if (directionX > 0) {
            score = candidate.bounds.x * 10 + std::abs(keyCenterY(candidate) - keyCenterY(current));
        } else if (directionX < 0) {
            score = (layoutInfo.panel.x + layoutInfo.panel.w - candidate.bounds.x) * 10 +
                    std::abs(keyCenterY(candidate) - keyCenterY(current));
        } else if (directionY > 0) {
            score = candidate.bounds.y * 10 + std::abs(keyCenterX(candidate) - keyCenterX(current));
        } else if (directionY < 0) {
            score = (layoutInfo.panel.y + layoutInfo.panel.h - candidate.bounds.y) * 10 +
                    std::abs(keyCenterX(candidate) - keyCenterX(current));
        }
        
        if (score < bestScore) {
            bestScore = score;
            bestIndex = candidate.index;
        }
    }
    return bestIndex;
}

void KeyboardOverlay::moveSelection(TubeState& state, int directionX, int directionY, int width, int height, bool& uiDirty) {
    const auto layoutInfo = buildLayout(state, width, height);
    if (layoutInfo.keys.empty()) return;
    
    state.keyboardSelectedIndex = std::clamp(state.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
    const KeyboardKeyGeometry& current = layoutInfo.keys[static_cast<size_t>(state.keyboardSelectedIndex)];

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
        const float score = primary + secondary * 8.0f;
        
        if (score < bestScore) {
            bestScore = score;
            bestIndex = candidate.index;
        }
    }

    if (bestIndex < 0) {
        bestIndex = resolveWrappedSelection(layoutInfo, state, directionX, directionY);
    }
    if (bestIndex >= 0) {
        state.keyboardSelectedIndex = bestIndex;
    }
    uiDirty = true;
}

bool KeyboardOverlay::updateSelectionFromStick(TubeState& state, int width, int height, bool& uiDirty) {
    const float absX = std::abs(state.leftStickX);
    const float absY = std::abs(state.leftStickY);
    const float threshold = 0.45f;
    int dirX = 0, dirY = 0;
    if (absX >= threshold || absY >= threshold) {
        if (absX >= absY) dirX = (state.leftStickX > 0.0f) ? 1 : -1;
        else              dirY = (state.leftStickY > 0.0f) ? 1 : -1;
    }
    if (dirX == 0 && dirY == 0) {
        navDirectionX_ = 0; navDirectionY_ = 0;
        navStartedAt_ = {}; navNextAt_ = {};
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (dirX != navDirectionX_ || dirY != navDirectionY_) {
        navDirectionX_ = dirX; navDirectionY_ = dirY;
        navStartedAt_ = now;
        navNextAt_ = now + std::chrono::milliseconds(180);
        moveSelection(state, dirX, dirY, width, height, uiDirty);
        return true;
    }
    if (now >= navNextAt_) {
        moveSelection(state, dirX, dirY, width, height, uiDirty);
        navNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(navStartedAt_, 90, 45));
        return true;
    }
    return false;
}

bool KeyboardOverlay::updateSelectionFromDpad(TubeState& state, int width, int height, bool& uiDirty) {
    int dirX = 0, dirY = 0;
    if (state.dpadLeftPressed != state.dpadRightPressed) dirX = state.dpadRightPressed ? 1 : -1;
    else if (state.dpadUpPressed != state.dpadDownPressed) dirY = state.dpadDownPressed ? 1 : -1;
    if (dirX == 0 && dirY == 0) {
        dpadDirectionX_ = 0; dpadDirectionY_ = 0;
        dpadStartedAt_ = {}; dpadNextAt_ = {};
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (dirX != dpadDirectionX_ || dirY != dpadDirectionY_) {
        dpadDirectionX_ = dirX; dpadDirectionY_ = dirY;
        dpadStartedAt_ = now;
        dpadNextAt_ = now + std::chrono::milliseconds(180);
        moveSelection(state, dirX, dirY, width, height, uiDirty);
        return true;
    }
    if (now >= dpadNextAt_) {
        moveSelection(state, dirX, dirY, width, height, uiDirty);
        dpadNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(dpadStartedAt_, 90, 45));
        return true;
    }
    return false;
}

void KeyboardOverlay::render(SDL_Renderer* renderer, const TubeState& state, int width, int height, bool& uiDirty) {
    if (state.inputMode != TubeState::InputMode::SearchText) {
        return;
    }

    const auto layoutInfo = buildLayout(state, width, height);

    // Cursor blink: compute visibility WITHOUT triggering a full redraw every frame.
    using namespace std::chrono;
    const auto phase = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 500;
    bool cursorVisible = (phase % 2) == 0;

    // Detect what actually changed in keyboard-relevant state.
    bool contentChanged = (
        kb_dirty_ ||
        last_selected_index_ != state.keyboardSelectedIndex ||
        last_kb_mode_         != state.keyboardMode         ||
        last_text_buffer_     != state.textBuffer           ||
        last_text_cursor_     != state.textCursor           ||
        last_cursor_visible_  != cursorVisible
    );

    bool needsRecreate = (texture_ == nullptr || width_ != layoutInfo.panel.w || height_ != layoutInfo.panel.h);
    if (needsRecreate) {
        destroyTexture();
        width_ = layoutInfo.panel.w;
        height_ = layoutInfo.panel.h;
        texture_ = createTargetTexture(renderer, width_, height_);
        contentChanged = true;
    }
    if (texture_ == nullptr) return;

    if (contentChanged) {
        // Cache current state for next-frame comparison.
        last_selected_index_ = state.keyboardSelectedIndex;
        last_kb_mode_        = state.keyboardMode;
        last_text_buffer_    = state.textBuffer;
        last_text_cursor_    = state.textCursor;
        last_cursor_visible_ = cursorVisible;
        kb_dirty_            = false;

        SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, texture_);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, theme::PANEL.r, theme::PANEL.g, theme::PANEL.b, 255);
        SDL_RenderClear(renderer);

        // Cheap 1px outline + 1px inner hairline so the panel reads as a card,
        // not a flat fill that bleeds into the screen edge.  Drawn directly
        // with SDL_RenderDrawRect — no rounded math, no extra textures.
        SDL_SetRenderDrawColor(renderer, theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b, 220);
        SDL_Rect outline{0, 0, layoutInfo.panel.w, layoutInfo.panel.h};
        SDL_RenderDrawRect(renderer, &outline);
        SDL_SetRenderDrawColor(renderer, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, 255);
        SDL_Rect outlineInner{1, 1, layoutInfo.panel.w - 2, layoutInfo.panel.h - 2};
        SDL_RenderDrawRect(renderer, &outlineInner);

        SDL_Color textColor = theme::TEXT;
        SDL_Color accent    = theme::ACCENT;   // unified red accent (was blue)

        const std::string header = "SEARCH [" + keyboardModeLabel(state) + "]";
        drawTextShadow(renderer, 12, 12, header, 2, accent);
        drawTextShadow(renderer, 12, 34, keyboardPreviewText(state, cursorVisible), 2, textColor);

        SDL_SetRenderDrawColor(renderer, theme::DIVIDER.r, theme::DIVIDER.g, theme::DIVIDER.b, 255);
        SDL_RenderDrawLine(renderer, 12, 60, layoutInfo.panel.w - 12, 60);

        for (const auto& key : layoutInfo.keys) {
            SDL_Rect keyRect{
                key.bounds.x - layoutInfo.panel.x,
                key.bounds.y - layoutInfo.panel.y,
                key.bounds.w,
                key.bounds.h
            };
            const bool selected = key.index == state.keyboardSelectedIndex;
            SDL_Color keyBg     = selected ? SDL_Color(theme::ACCENT)        : SDL_Color(theme::SURFACE);
            SDL_Color keyBorder = selected ? SDL_Color(theme::ACCENT_BRIGHT) : SDL_Color(theme::BORDER);

            fillRoundedRect(renderer, keyRect, theme::RADIUS_PILL, keyBg);
            drawRoundedRect(renderer, keyRect, theme::RADIUS_PILL, keyBorder);

            int scale = (key.label.length() > 1) ? 1 : 2;
            int labelW = 0, labelH = 0;
            getTextSize(key.label, scale, &labelW, &labelH);
            int textX = keyRect.x + (keyRect.w - labelW) / 2;
            int textY = keyRect.y + (keyRect.h - labelH) / 2;

            drawTextShadow(renderer, textX, textY, key.label, scale, selected ? SDL_Color(theme::BG) : textColor);
        }

        SDL_SetRenderTarget(renderer, previousTarget);
        uiDirty = true; // Keyboard content changed — push a frame.
    }

    SDL_Rect overlay = layoutInfo.panel;
    SDL_RenderCopy(renderer, texture_, nullptr, &overlay);
}

void KeyboardOverlay::eraseActiveBufferChar(TubeState& state) {
    if (state.replaceBufferOnNextInput) {
        state.textBuffer.clear();
        state.textCursor = 0;
        state.replaceBufferOnNextInput = false;
    } else if (state.textCursor > 0 && !state.textBuffer.empty()) {
        state.textBuffer.erase(static_cast<size_t>(state.textCursor - 1), 1);
        --state.textCursor;
    }
}

void KeyboardOverlay::insertActiveText(TubeState& state, const std::string& text) {
    if (state.replaceBufferOnNextInput) {
        state.textBuffer.clear();
        state.textCursor = 0;
        state.replaceBufferOnNextInput = false;
    }
    state.textCursor = std::clamp(state.textCursor, 0, static_cast<int>(state.textBuffer.size()));
    state.textBuffer.insert(static_cast<size_t>(state.textCursor), text);
    state.textCursor += static_cast<int>(text.size());
}

void KeyboardOverlay::moveActiveCursor(TubeState& state, int delta) {
    if (state.replaceBufferOnNextInput) {
        state.textCursor = (delta < 0) ? 0 : static_cast<int>(state.textBuffer.size());
        state.replaceBufferOnNextInput = false;
        return;
    }
    state.textCursor = std::clamp(state.textCursor + delta, 0, static_cast<int>(state.textBuffer.size()));
}

std::string KeyboardOverlay::transformTypedText(const TubeState& state, const char* text) {
    std::string transformed{text};
    if (state.keyboardMode == TubeState::KeyboardMode::Uppercase) {
        for (char& ch : transformed) {
            if (std::isalpha(static_cast<unsigned char>(ch))) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
        }
    }
    return transformed;
}

void KeyboardOverlay::preload(SDL_Renderer* renderer, const TubeState& state, int width, int height) {
    TubeState tempState = state;
    tempState.inputMode = TubeState::InputMode::SearchText;
    bool tempUiDirty = false;
    render(renderer, tempState, width, height, tempUiDirty);
}

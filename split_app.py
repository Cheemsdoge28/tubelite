import os

def write_file(path, content):
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content.strip() + "\n")

state_hpp = """
#pragma once
#include <string>

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
"""

renderer_utils_hpp = """
#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <array>

void drawGlyph(SDL_Renderer* renderer, int x, int y, char ch, int scale, SDL_Color color);
void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);
void drawTextShadow(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);
SDL_Texture* createTargetTexture(SDL_Renderer* renderer, int width, int height);
"""

# We'll extract renderer_utils.cpp by reading from main.cpp
renderer_utils_cpp_template = """
#include "renderer_utils.hpp"
#include <cctype>

{glyphFor}

void drawGlyph(SDL_Renderer* renderer, int x, int y, char ch, int scale, SDL_Color color) {{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const auto glyph = glyphFor(ch);
    for (int row = 0; row < 7; ++row) {{
        for (int col = 0; col < 5; ++col) {{
            if ((glyph[static_cast<size_t>(row)] >> (4 - col)) & 1U) {{
                SDL_Rect pixel{{x + col * scale, y + row * scale, scale, scale}};
                SDL_RenderFillRect(renderer, &pixel);
            }}
        }}
    }}
}}

void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {{
    int cursor = x;
    for (char ch : text) {{ drawGlyph(renderer, cursor, y, ch, scale, color); cursor += scale * 6; }}
}}

void drawTextShadow(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {{
    const int off = std::max(1, scale / 2);
    drawText(renderer, x + off, y + off, text, scale, {{8, 10, 12, 200}});
    drawText(renderer, x, y, text, scale, color);
}}

SDL_Texture* createTargetTexture(SDL_Renderer* renderer, int width, int height) {{
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (texture == nullptr)
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (texture != nullptr) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}}
"""

status_overlay_hpp = """
#pragma once
#include <SDL2/SDL.h>
#include "state.hpp"

class StatusOverlay {
public:
    StatusOverlay() = default;
    ~StatusOverlay() { destroyTexture(); }
    void destroyTexture();
    void render(SDL_Renderer* renderer, const TubeState& state, int width, int height, bool& uiDirty);
private:
    SDL_Texture* texture_{nullptr};
    int width_{0};
    int height_{0};
};
"""

status_overlay_cpp = """
#include "status_overlay.hpp"
#include "renderer_utils.hpp"

void StatusOverlay::destroyTexture() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
}

void StatusOverlay::render(SDL_Renderer* renderer, const TubeState& state, int width, int height, bool& uiDirty) {
    int statusBarHeight = 48;
    if (texture_ == nullptr || width_ != width || height_ != statusBarHeight || uiDirty) {
        destroyTexture();
        width_ = width;
        height_ = statusBarHeight;
        texture_ = createTargetTexture(renderer, width, statusBarHeight);
        
        if (texture_) {
            SDL_Texture* prev = SDL_GetRenderTarget(renderer);
            SDL_SetRenderTarget(renderer, texture_);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
            SDL_RenderClear(renderer);
            
            SDL_Color textColor{150, 160, 170, 255};
            
            std::string shortcuts = "A: Select | B: Back/Close | Y: Search | START+SELECT: Quit";
            if (state.inputMode == TubeState::InputMode::SearchText) {
                shortcuts = "A: Type | B: Close KB | L1: Mode | START: Go";
            } else if (state.currentScreen == TubeState::Screen::Playback) {
                shortcuts = "A: Pause/Play | B: Stop | R1: Toggle UI | START+SELECT: Quit";
            }
            
            drawTextShadow(renderer, 20, 16, shortcuts, 1, textColor);
            
            SDL_SetRenderTarget(renderer, prev);
        }
    }
    
    if (texture_) {
        SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
        SDL_RenderCopy(renderer, texture_, nullptr, &dst);
    }
}
"""

main_cpp_template = """
#include "app.hpp"
#include <iostream>

int main(int, char*[]) {
    App app;
    if (!app.initialize()) return 1;
    app.run();
    return 0;
}
"""

def extract():
    with open('src/main.cpp', 'r') as f:
        original = f.read()
        
    glyph_start = original.find("static std::array<uint8_t, 7> glyphFor(char ch) {")
    glyph_end = original.find("    void drawGlyph", glyph_start)
    glyphFor = original[glyph_start:glyph_end].replace("static std::array", "std::array")
    
    renderer_utils_cpp = renderer_utils_cpp_template.format(glyphFor=glyphFor)
    
    write_file('src/state.hpp', state_hpp)
    write_file('src/renderer_utils.hpp', renderer_utils_hpp)
    write_file('src/renderer_utils.cpp', renderer_utils_cpp)
    write_file('src/status_overlay.hpp', status_overlay_hpp)
    write_file('src/status_overlay.cpp', status_overlay_cpp)
    write_file('src/main.cpp', main_cpp_template)

if __name__ == '__main__':
    extract()

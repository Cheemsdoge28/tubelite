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

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
            SDL_SetRenderDrawColor(renderer, 11, 11, 11, 255); // Modern YouTube dark mode black (#0b0b0b)
            SDL_RenderClear(renderer);
            
            SDL_Color textColor{140, 140, 140, 255}; // Modern slate grey text
            
            std::string shortcuts = "A:Play B:Back Y:Search X:" + std::to_string(state.maxQualityHeight) + "p R3:Reload R1:UI";
            if (state.inputMode == TubeState::InputMode::SearchText) {
                shortcuts = "A:Type B:Close L1:Caps/Sym START:Go";
            } else if (state.currentScreen == TubeState::Screen::Playback) {
                shortcuts = "A:Pause B:Stop Y:Subs L/R:Seek R1:UI";
            }
            
            // Centered vertically (statusBarHeight 48 - scale 2 font height 14) / 2 = 17
            drawTextShadow(renderer, 20, 17, shortcuts, 2, textColor);
            
            SDL_SetRenderTarget(renderer, prev);
        }
    }
    
    if (texture_) {
        SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
        SDL_RenderCopy(renderer, texture_, nullptr, &dst);
    }
}

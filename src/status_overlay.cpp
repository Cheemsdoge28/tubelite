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
            
            // Match fire4arkos status bar background: 16, 18, 22
            SDL_SetRenderDrawColor(renderer, 16, 18, 22, 255);
            SDL_RenderClear(renderer);
            
            // Match fire4arkos top border line: 30, 34, 40 at y = 0
            SDL_SetRenderDrawColor(renderer, 30, 34, 40, 255);
            SDL_RenderDrawLine(renderer, 0, 0, width, 0);
            
            // Match fire4arkos text color: 214, 220, 230
            SDL_Color textColor{214, 220, 230, 255};
            
            std::string line1, line2;
            if (state.inputMode == TubeState::InputMode::SearchText) {
                line1 = "A TYPE  B CLOSE  L1 CAPS/SYM";
                line2 = "START GO  L2/R2 CUR  LT/RT SCROLL";
            } else if (state.currentScreen == TubeState::Screen::Playback) {
                line1 = "A PAUSE  B STOP  Y SUBS  BACK STATS";
                line2 = "D-PAD L/R SEEK  U/D VOL  L1/R1 SPEED";
            } else {
                line1 = "A PLAY  B BACK  Y SEARCH";
                line2 = "X " + std::to_string(state.maxQualityHeight) + "P  R3 RELOAD  R1 UI";
            }
            
            // Match fire4arkos two-line layout: x=12, line 1 at y=8, line 2 at y=32
            drawTextShadow(renderer, 12, 8, line1, 2, textColor);
            drawTextShadow(renderer, 12, 32, line2, 2, textColor);
            
            SDL_SetRenderTarget(renderer, prev);
        }
    }
    
    if (texture_) {
        SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
        SDL_RenderCopy(renderer, texture_, nullptr, &dst);
    }
}

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
            
            SDL_Color textColor{214, 220, 230, 255};

            const SDL_Color red{255, 48, 48, 255};
            const SDL_Color blue{64, 148, 255, 255};
            const SDL_Color yellow{255, 214, 64, 255};
            const SDL_Color green{64, 214, 96, 255};
            const SDL_Color panel{24, 28, 34, 255};
            const int boxY = 8;
            const int boxH = 30;
            const int gap = 10;

            auto drawHint = [&](int x, int w, const std::string& button, SDL_Color buttonColor, const std::string& action) {
                SDL_Rect box{x, boxY, w, boxH};
                SDL_SetRenderDrawColor(renderer, panel.r, panel.g, panel.b, panel.a);
                SDL_RenderFillRect(renderer, &box);
                SDL_SetRenderDrawColor(renderer, 42, 48, 56, 255);
                SDL_RenderDrawRect(renderer, &box);
                drawTextShadow(renderer, x + 8, boxY + 7, button, 2, buttonColor);
                drawTextShadow(renderer, x + 30, boxY + 7, action, 2, textColor);
            };

            if (state.inputMode == TubeState::InputMode::SearchText) {
                drawHint(10, 128, "A", red, "TYPE");
                drawHint(148, 132, "B", yellow, "CLOSE");
                drawHint(290, 152, "Y", green, "SPACE");
                drawHint(452, 176, "START", textColor, "GO");
            } else if (state.currentScreen == TubeState::Screen::Playback) {
                drawHint(10, 124, "A", red, "PAUSE");
                drawHint(144, 116, "B", yellow, "STOP");
                drawHint(270, 116, "Y", green, "SUBS");
                drawHint(396, 108, "X", blue, "INFO");
                drawHint(514, 114, "L1/R1", textColor, "SPD");
            } else {
                drawHint(10, 110, "A", red, "PLAY");
                drawHint(130, 112, "B", yellow, "BACK");
                drawHint(252, 132, "Y", green, "SEARCH");
                drawHint(394, 108, "X", blue, std::to_string(state.maxQualityHeight) + "P");
                drawHint(512, 116, "R3", textColor, "RELOAD");
            }
            
            SDL_SetRenderTarget(renderer, prev);
        }
    }
    
    if (texture_) {
        SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
        SDL_RenderCopy(renderer, texture_, nullptr, &dst);
    }
}

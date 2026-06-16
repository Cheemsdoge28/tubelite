#include "status_overlay.hpp"
#include "renderer_utils.hpp"
#include <vector>
#include <algorithm>

void StatusOverlay::destroyTexture() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
}

void StatusOverlay::render(SDL_Renderer* renderer, const TubeState& state, int width, int height, bool& uiDirty) {
    int statusBarHeight = 48;
    bool needsRecreate = (texture_ == nullptr || width_ != width || height_ != statusBarHeight);
    bool stateChanged = (
        state.inputMode != last_input_mode_ ||
        state.currentScreen != last_screen_ ||
        state.maxQualityHeight != last_max_quality_
    );

    if (needsRecreate || stateChanged) {
        if (needsRecreate) {
            destroyTexture();
            width_ = width;
            height_ = statusBarHeight;
            texture_ = createTargetTexture(renderer, width, statusBarHeight);
        }
        
        if (texture_) {
            last_input_mode_ = state.inputMode;
            last_screen_ = state.currentScreen;
            last_max_quality_ = state.maxQualityHeight;

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

            std::vector<HintItem> activeHints;
            if (state.inputMode == TubeState::InputMode::SearchText) {
                activeHints = {
                    {"A", red, "TYPE"},
                    {"B", yellow, "CLOSE"},
                    {"Y", green, "SPACE"},
                    {"START", textColor, "GO"}
                };
            } else if (state.currentScreen == TubeState::Screen::Playback) {
                activeHints = {
                    {"A", red, "PAUSE"},
                    {"B", yellow, "STOP"},
                    {"Y", green, "SUBS"},
                    {"X", blue, "INFO"},
                    {"L1/R1", textColor, "SPD"}
                };
            } else {
                activeHints = {
                    {"A", red, "PLAY"},
                    {"B", yellow, "BACK"},
                    {"Y", green, "SEARCH"},
                    {"X", blue, std::to_string(state.maxQualityHeight) + "P"},
                    {"L1", textColor, state.backgroundDaemonEnabled ? "BG:ON" : "BG:OFF"},
                    {"R3", textColor, "RELOAD"},
                    {"R1", textColor, "UI"}
                };
            }

            drawHintButtons(renderer, activeHints, boxY, boxH, 2, width, panel, {42, 48, 56, 255}, textColor);

            
            SDL_SetRenderTarget(renderer, prev);
        }
    }
    
    if (texture_) {
        SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
        SDL_RenderCopy(renderer, texture_, nullptr, &dst);
    }
}

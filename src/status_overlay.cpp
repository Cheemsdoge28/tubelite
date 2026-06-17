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
        state.maxQualityHeight != last_max_quality_ ||
        state.backgroundDaemonEnabled != last_background_daemon_enabled_
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
            last_background_daemon_enabled_ = state.backgroundDaemonEnabled;

            SDL_Texture* prev = SDL_GetRenderTarget(renderer);
            SDL_SetRenderTarget(renderer, texture_);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            
            // Unified chrome bar background (shared with the playback HUD bars)
            SDL_SetRenderDrawColor(renderer, theme::BAR.r, theme::BAR.g, theme::BAR.b, 255);
            SDL_RenderClear(renderer);

            // Top border line
            SDL_SetRenderDrawColor(renderer, theme::DIVIDER.r, theme::DIVIDER.g, theme::DIVIDER.b, 255);
            SDL_RenderDrawLine(renderer, 0, 0, width, 0);

            SDL_Color textColor = theme::TEXT_ON;

            const SDL_Color red    = theme::ACCENT;
            const SDL_Color blue   = theme::BLUE;
            const SDL_Color yellow = theme::YELLOW;
            const SDL_Color green  = theme::GREEN;
            const SDL_Color panel  = theme::PANEL;
            const int boxY = 13;
            const int boxH = 22;

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

            drawHintButtons(renderer, activeHints, boxY, boxH, 1, width, panel, theme::CHIP, textColor);

            
            SDL_SetRenderTarget(renderer, prev);
        }
    }
    
    if (texture_) {
        SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
        SDL_RenderCopy(renderer, texture_, nullptr, &dst);
    }
}

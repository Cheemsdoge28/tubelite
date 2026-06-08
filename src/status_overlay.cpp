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

            struct HintItem {
                std::string button;
                SDL_Color btnColor;
                std::string action;
            };

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
                    {"R3", textColor, "RELOAD"},
                    {"R1", textColor, "UI"}
                };
            }

            int totalWidth = 0;
            std::vector<int> boxWidths;
            std::vector<int> btnWidths;
            std::vector<int> actWidths;
            
            for (const auto& item : activeHints) {
                int btnW = 0, actW = 0;
                getTextSize(item.button, 2, &btnW, nullptr);
                getTextSize(item.action, 2, &actW, nullptr);
                int boxW = btnW + actW + 24;
                boxWidths.push_back(boxW);
                btnWidths.push_back(btnW);
                actWidths.push_back(actW);
                totalWidth += boxW;
            }
            if (!activeHints.empty()) {
                totalWidth += (static_cast<int>(activeHints.size()) - 1) * 12;
            }

            int fontHeight = 18;
            getTextSize("Ay", 2, nullptr, &fontHeight);

            int currentX = (width - totalWidth) / 2;
            for (size_t i = 0; i < activeHints.size(); ++i) {
                const auto& item = activeHints[i];
                int boxW = boxWidths[i];
                int btnW = btnWidths[i];
                int actW = actWidths[i];
                
                SDL_Rect box{currentX, boxY, boxW, boxH};
                fillRoundedRect(renderer, box, 4, panel);
                drawRoundedRect(renderer, box, 4, {42, 48, 56, 255});
                
                int contentW = btnW + 8 + actW;
                int contentX = currentX + (boxW - contentW) / 2;
                
                int textY = boxY + (boxH - fontHeight) / 2;
                
                drawTextShadow(renderer, contentX, textY, item.button, 2, item.btnColor);
                drawTextShadow(renderer, contentX + btnW + 8, textY, item.action, 2, textColor);
                
                currentX += boxW + 12;
            }
            
            SDL_SetRenderTarget(renderer, prev);
        }
    }
    
    if (texture_) {
        SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
        SDL_RenderCopy(renderer, texture_, nullptr, &dst);
    }
}

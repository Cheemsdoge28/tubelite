#include "compositor.hpp"
#include "app.hpp"
#include "renderer_utils.hpp"
#include "ui_framework.hpp"
#include <algorithm>
#include <chrono>

void Compositor::render(App* app, int width, int height) {
    if (app->state_.currentScreen == TubeState::Screen::Playback) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);

        // Render the video frame first (directly to the display framebuffer FBO=0)
        app->mpv_player_.render(width, height);

        // Draw the UI overlay layers (HUD, loading spinner, overlays) on top of the video
        if (app->state_.showUi) {
            app->renderPlaybackOverlay(width, height);
        }

        if (app->state_.isLoadingVideo) {
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
            SDL_Rect bg{0, 0, width, height};
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_RenderFillRect(renderer_, &bg);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);

            float time = SDL_GetTicks() / 1000.0f;
            drawSpinner(renderer_, width / 2, height / 2 - 20, 30, time);
            drawTextCentered(renderer_, width / 2, height / 2 + 25, app->loading_status_text_, 2, {255, 255, 255, 255}, true);
            app->uiDirty_ = true;
        }

        // Volume / speed overlays
        {
            auto now = std::chrono::steady_clock::now();
            bool volumeActive = (now < app->volume_overlay_timeout_);
            bool speedActive  = (now < app->speed_overlay_timeout_);

            static bool lastVolumeActivePlayback = false;
            static bool lastSpeedActivePlayback  = false;
            if (volumeActive || speedActive || lastVolumeActivePlayback || lastSpeedActivePlayback) app->uiDirty_ = true;
            lastVolumeActivePlayback = volumeActive;
            lastSpeedActivePlayback  = speedActive;

            if (volumeActive) {
                int boxW = 200, boxH = 36;
                int boxX = (width - boxW) / 2, boxY = 64;
                SDL_Rect r{boxX, boxY, boxW, boxH};
                fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
                drawRoundedRect(renderer_, r, 6, {64, 148, 255, 255});
                char volBuf[32];
                snprintf(volBuf, sizeof(volBuf), "Volume: %d%%", app->state_.volume);
                drawTextCentered(renderer_, boxX + boxW / 2, boxY + 8, volBuf, 1, {255, 255, 255, 255}, true);
            }
            if (speedActive) {
                int boxW = 200, boxH = 36;
                int boxX = (width - boxW) / 2, boxY = 64;
                SDL_Rect r{boxX, boxY, boxW, boxH};
                fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
                drawRoundedRect(renderer_, r, 6, {64, 148, 255, 255});
                char speedBuf[32];
                snprintf(speedBuf, sizeof(speedBuf), "Speed: %.2fx", app->state_.speed);
                drawTextCentered(renderer_, boxX + boxW / 2, boxY + 8, speedBuf, 1, {255, 255, 255, 255}, true);
            }
        }

        app->keyboard_.render(renderer_, app->state_, width, height, app->uiDirty_);
        SDL_RenderPresent(renderer_);
        return;
    }

    // ── Browse / Search screens ───────────────────────────────────────────────
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 15, 15, 15, 255);
    SDL_RenderClear(renderer_);

    auto currentGrid = app->activeGrid();
    float scrollY = currentGrid ? currentGrid->scrollY : 0.0f;

    if (app->state_.currentScreen == TubeState::Screen::Home) {
        if (app->home_grid_->cards.empty()) {
            if (app->homeLoadFailed_) {
                drawTextCentered(renderer_, width / 2, height / 2 - 10, "Failed to load feed.", 2, {255, 100, 100, 255});
                drawTextCentered(renderer_, width / 2, height / 2 + 20, "Press Y to search videos", 2, {150, 150, 150, 255});
            } else {
                app->renderBrowseLoadingState(width, height, "Loading Feed...");
            }
        } else {
            app->home_grid_->render(renderer_, 0.0f, 0.0f);
            if (app->is_playing_preview_ && app->preview_card_) {
                float screenY = app->preview_card_->bounds.y - scrollY;
                bool horizontal = (app->preview_card_->bounds.w > 400);
                int thumbW = horizontal ? 160 : static_cast<int>(app->preview_card_->bounds.w);
                int thumbH = horizontal ? 90 : static_cast<int>(app->preview_card_->bounds.w * (9.0f / 16.0f));
                SDL_Rect thumbDst{
                    static_cast<int>(app->preview_card_->bounds.x),
                    static_cast<int>(screenY),
                    thumbW,
                    thumbH
                };
                SDL_Texture* previewTex = app->mpv_player_.renderToTexture(renderer_, thumbW, thumbH);
                if (previewTex) {
                    SDL_RenderCopy(renderer_, previewTex, nullptr, &thumbDst);
                    maskRoundedCornersTop(renderer_, thumbDst, 8, {15, 15, 15, 255});
                }
            }
            app->focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
        }
        auto focusedCard = app->focus_manager_.getFocusedCard();
        (void)focusedCard;
        app->renderBrowseHeader(width, height, "TubeLite", scrollY, false);
    } else if (app->state_.currentScreen == TubeState::Screen::Search) {
        if (app->state_.isSearching && app->search_grid_->cards.empty()) {
            app->renderBrowseLoadingState(width, height, "Searching...");
        } else if (app->search_grid_->cards.empty()) {
            if (app->current_search_query_.empty()) {
                drawTextCentered(renderer_, width / 2, height / 2, "Press Y to search videos", 2, {150, 150, 150, 255});
            } else {
                drawTextCentered(renderer_, width / 2, height / 2, "No results found.", 2, {150, 150, 150, 255});
            }
        } else {
            app->search_grid_->render(renderer_, 0.0f, 0.0f);
            if (app->is_playing_preview_ && app->preview_card_) {
                float screenY = app->preview_card_->bounds.y - scrollY;
                bool horizontal = (app->preview_card_->bounds.w > 400);
                int thumbW = horizontal ? 160 : static_cast<int>(app->preview_card_->bounds.w);
                int thumbH = horizontal ? 90 : static_cast<int>(app->preview_card_->bounds.w * (9.0f / 16.0f));
                SDL_Rect thumbDst{
                    static_cast<int>(app->preview_card_->bounds.x),
                    static_cast<int>(screenY),
                    thumbW,
                    thumbH
                };
                SDL_Texture* previewTex = app->mpv_player_.renderToTexture(renderer_, thumbW, thumbH);
                if (previewTex) {
                    SDL_RenderCopy(renderer_, previewTex, nullptr, &thumbDst);
                    maskRoundedCornersTop(renderer_, thumbDst, 8, {15, 15, 15, 255});
                }
            }
            app->focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f);
        }
        app->renderBrowseHeader(width, height, "Search", scrollY, true);
    }

    if (app->state_.miniplayerActive) {
        int mX = width - 250;
        int mY = height - 193;
        int mW = 240;
        int mH = 135;
        
        SDL_Rect miniplayerBounds{mX, mY, mW, mH};
        SDL_Texture* previewTex = app->mpv_player_.renderToTexture(renderer_, mW, mH);
        if (previewTex) {
            SDL_RenderCopy(renderer_, previewTex, nullptr, &miniplayerBounds);
        }
        
        // Draw a 2px red accent border around the miniplayer
        SDL_Rect border1{mX - 1, mY - 1, mW + 2, mH + 2};
        SDL_Rect border2{mX - 2, mY - 2, mW + 4, mH + 4};
        SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
        SDL_RenderDrawRect(renderer_, &border1);
        SDL_RenderDrawRect(renderer_, &border2);
        
        if (!app->mpv_player_.isPlaying()) {
            int centerX = mX + mW / 2;
            int centerY = mY + mH / 2;
            SDL_Rect pauseBg{ centerX - 15, centerY - 15, 30, 30 };
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            fillRoundedRect(renderer_, pauseBg, 15, {0, 0, 0, 150});
            
            SDL_Rect pauseLeft{ centerX - 5, centerY - 8, 3, 16 };
            SDL_Rect pauseRight{ centerX + 2, centerY - 8, 3, 16 };
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer_, &pauseLeft);
            SDL_RenderFillRect(renderer_, &pauseRight);
        }
        
        // Hints: START: Play/Pause  B: Close
        std::string hint1 = "START: Play/Pause";
        std::string hint2 = "B: Close";
        int w1 = 0, h1 = 0;
        int w2 = 0, h2 = 0;
        getTextSize(hint1, 1, &w1, &h1);
        getTextSize(hint2, 1, &w2, &h2);
        int textW = std::max(w1, w2);
        int textH = h1 + h2 + 2;
        int plateW = textW + 8;
        int plateH = textH + 6;
        
        SDL_Rect plate{ mX + 4, mY + mH - plateH - 4, plateW, plateH };
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(renderer_, &plate);
        
        drawText(renderer_, mX + 8, mY + mH - plateH - 1, hint1, 1, {255, 255, 255, 255});
        drawText(renderer_, mX + 8, mY + mH - 14, hint2, 1, {255, 255, 255, 255});
    }

    // Render header-level spinner if searching and grid is not empty
    if (app->state_.isSearching && app->activeGrid() && !app->activeGrid()->cards.empty()) {
        const int expandedHeight = 84;
        const int collapsedHeight = 58;
        const int headerHeight = std::max(collapsedHeight, expandedHeight - static_cast<int>(scrollY * 0.12f));
        float time = SDL_GetTicks() / 1000.0f;
        drawSpinner(renderer_, width - 30, headerHeight / 2, 10, time);
        app->uiDirty_ = true;
    }

    // Browse status bar
    if (app->state_.showUi) {
        app->status_.render(renderer_, app->state_, width, height, app->uiDirty_);
    }
    
    // Loading overlay
    if (app->state_.isLoadingVideo) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_Rect bg{0, 0, width, height};
        SDL_RenderFillRect(renderer_, &bg);
        
        float time = SDL_GetTicks() / 1000.0f;
        drawSpinner(renderer_, width / 2, height / 2 - 20, 30, time);
        
        drawTextCentered(renderer_, width / 2, height / 2 + 25, app->loading_status_text_, 2, {255, 255, 255, 255}, true);
        app->uiDirty_ = true;
    }
    
    // Draw custom volume/speed overlays
    {
        auto now = std::chrono::steady_clock::now();
        bool volumeActive = (now < app->volume_overlay_timeout_);
        bool speedActive = (now < app->speed_overlay_timeout_);
        
        static bool lastVolumeActive = false;
        static bool lastSpeedActive = false;
        if (volumeActive || speedActive || lastVolumeActive || lastSpeedActive) {
            app->uiDirty_ = true;
        }
        lastVolumeActive = volumeActive;
        lastSpeedActive = speedActive;

        if (volumeActive) {
            int boxW = 200;
            int boxH = 36;
            int boxX = (width - boxW) / 2;
            int boxY = 64;
            
            SDL_Rect r{boxX, boxY, boxW, boxH};
            fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
            drawRoundedRect(renderer_, r, 6, {255, 48, 48, 255});
            
            std::string volText = "Volume: " + std::to_string(app->state_.volume) + "%";
            if (app->state_.muted) volText = "Mute: ON";
            
            int barW = 160;
            int barH = 6;
            int barX = boxX + 20;
            int barY = boxY + 24;
            SDL_Rect barBg{barX, barY, barW, barH};
            SDL_SetRenderDrawColor(renderer_, 60, 60, 60, 255);
            SDL_RenderFillRect(renderer_, &barBg);
            
            if (!app->state_.muted) {
                int fillW = static_cast<int>(barW * (app->state_.volume / 100.0f));
                SDL_Rect barFill{barX, barY, fillW, barH};
                SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
                SDL_RenderFillRect(renderer_, &barFill);
            }
            
            drawTextCentered(renderer_, boxX + boxW / 2, boxY + 4, volText, 1, {255, 255, 255, 255}, true);
        } else if (speedActive) {
            int boxW = 160;
            int boxH = 32;
            int boxX = (width - boxW) / 2;
            int boxY = 64;
            
            SDL_Rect r{boxX, boxY, boxW, boxH};
            fillRoundedRect(renderer_, r, 6, {0, 0, 0, 200});
            drawRoundedRect(renderer_, r, 6, {64, 148, 255, 255});
            
            char speedBuf[32];
            snprintf(speedBuf, sizeof(speedBuf), "Speed: %.2fx", app->state_.speed);
            drawTextCentered(renderer_, boxX + boxW / 2, boxY + 8, speedBuf, 1, {255, 255, 255, 255}, true);
        }
    }

    app->keyboard_.render(renderer_, app->state_, width, height, app->uiDirty_);

    // Draw telemetry overlay if enabled
    if (app->state_.showDebugOverlay) {
        int panelW = 240;
        int panelH = 110;
        int panelX = width - panelW - 10;
        int panelY = 60; // below top bar/header

        SDL_Rect rect{panelX, panelY, panelW, panelH};
        fillRoundedRect(renderer_, rect, 6, {0, 0, 0, 200});
        drawRoundedRect(renderer_, rect, 6, {150, 150, 150, 255});

        char buf[256];
        int textY = panelY + 8;

        std::snprintf(buf, sizeof(buf), "FPS: %.1f", app->current_fps_);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        std::snprintf(buf, sizeof(buf), "Render Latency: %.2f ms", app->render_latency_ms_);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        int64_t vo_drops = app->mpv_player_.getPropertyInt("vo-drop-frame-count");
        int64_t dec_drops = app->mpv_player_.getPropertyInt("decoder-frame-drop-count");
        std::snprintf(buf, sizeof(buf), "Drops: VO %lld / Dec %lld", (long long)vo_drops, (long long)dec_drops);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        size_t q_size = 0;
        {
            std::lock_guard<std::mutex> lock(app->queue_mutex_);
            q_size = app->main_thread_queue_.size();
        }
        std::snprintf(buf, sizeof(buf), "Queue Size: %zu", q_size);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        // Throttle to 1 Hz to avoid hammering /proc and statvfs at 60 fps
        static double cached_ram = 0.0, cached_storage_free = 0.0, cached_storage_total = 0.0;
        static uint32_t last_sys_poll = 0;
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks - last_sys_poll >= 1000) {
            getSystemMemoryAndStorage(cached_ram, cached_storage_free, cached_storage_total);
            last_sys_poll = now_ticks;
        }
        std::snprintf(buf, sizeof(buf), "RAM RSS: %.1f MB", cached_ram);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        std::snprintf(buf, sizeof(buf), "Storage: %.1f / %.1f GB free", cached_storage_free, cached_storage_total);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
    }

    SDL_RenderPresent(renderer_);
}

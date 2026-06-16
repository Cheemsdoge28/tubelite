#include "compositor.hpp"
#include "app.hpp"
#include "renderer_utils.hpp"
#include "ui_framework.hpp"
#include <algorithm>
#include <chrono>
static std::string truncateTextToWidth(const std::string& text, int scale, int maxWidth) {
    int w = 0;
    getTextSize(text, scale, &w, nullptr);
    if (w <= maxWidth) return text;
    
    std::string ell = "...";
    int ellW = 0;
    getTextSize(ell, scale, &ellW, nullptr);
    int targetW = maxWidth - ellW;
    size_t len = utf8Length(text);
    
    size_t low = 0;
    size_t high = len;
    size_t best_len = 0;
    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        std::string temp = utf8Slice(text, 0, mid);
        int tempW = 0;
        getTextSize(temp, scale, &tempW, nullptr);
        if (tempW <= targetW) {
            best_len = mid;
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }
    return utf8Slice(text, 0, best_len) + ell;
}

void Compositor::render(App* app, int width, int height) {
    if (app->state_.currentScreen == TubeState::Screen::Playback) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);

        // Render the video frame (internally delegates to offscreen Layer in MpvPlayer)
        app->mpv_player_.render(width, height);

        // Draw the HUD overlay offscreen via Layer and composite it on top of the video
        if (app->state_.showUi) {
            renderPlaybackOverlay(app, width, height);
        }

        // Draw volume/speed adjust overlays
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
                drawVolumeOverlay(renderer_, width / 2, 64, app->state_.volume, app->state_.muted, {64, 148, 255, 255});
            } else if (speedActive) {
                drawSpeedOverlay(renderer_, width / 2, 64, app->state_.speed, {64, 148, 255, 255});
            }
        }

        // Draw loading overlay
        if (app->state_.isLoadingVideo) {
            drawLoadingOverlay(renderer_, width, height, app->loading_status_text_, SDL_GetTicks() / 1000.0f, {255, 255, 255, 255}, true);
            app->uiDirty_ = true;
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
                drawLoadingOverlay(renderer_, width, height, "Loading Feed...", SDL_GetTicks() / 1000.0f, {150, 150, 150, 255}, false);
                app->uiDirty_ = true;
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
        renderBrowseHeader(app, width, height, "TubeLite", scrollY, false);
    } else if (app->state_.currentScreen == TubeState::Screen::Search) {
        if (app->state_.isSearching && app->search_grid_->cards.empty()) {
            drawLoadingOverlay(renderer_, width, height, "Searching...", SDL_GetTicks() / 1000.0f, {150, 150, 150, 255}, false);
            app->uiDirty_ = true;
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
        renderBrowseHeader(app, width, height, "Search", scrollY, true);
    }

    // Draw Miniplayer
    // Architecture: two-pass dirty cache (same pattern as the header layer).
    //   Pass 1 — chrome layer (borders, title strip, hint text, pause icon):
    //     Only redrawn when video ID or play-state changes. Result is cached
    //     in miniplayer_layer_ and composited cheaply every frame.
    //   Pass 2 — live video frame:
    //     mpv's texture is fetched and blitted *directly* to the screen (not
    //     through the layer) so we avoid the FBO / render-target conflict that
    //     caused the full-screen flicker in the first place.
    if (app->state_.miniplayerActive) {
        const int mW  = 240;
        const int mVH = 135;   // video area height
        const int mSH = 48;    // details/title strip height
        const int mH  = mVH + mSH;
        const int mX  = width  - mW - 16;
        const int mY  = height - 60 - mH - 12; // float above 60px bottom panel

        const SDL_Rect miniplayerBounds{mX - 2, mY - 2, mW + 4, mH + 4};

        // Clear the screen region to prevent card thumbnail bleed-through
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 15, 15, 15, 255); // match background color
        SDL_RenderFillRect(renderer_, &miniplayerBounds);

        // ── Pass 1: chrome layer (dirty-cached) ───────────────────────────────
        bool playing   = app->mpv_player_.isPlaying();
        bool stripDirty = miniplayer_strip_dirty_
            || !miniplayer_layer_.getTexture()
            || miniplayer_layer_.getWidth()  != mW + 4
            || miniplayer_layer_.getHeight() != mH + 4
            || app->current_video_.id != last_miniplayer_video_id_
            || playing != last_miniplayer_playing_;

        if (stripDirty) {
            miniplayer_layer_.init(renderer_, mW + 4, mH + 4, miniplayerBounds);
            miniplayer_layer_.begin(renderer_, {0, 0, 0, 0}); // fully transparent — video shows through

            // Unified sleek border around the entire miniplayer card
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_Rect borderOuter{0, 0, mW + 4, mH + 4};
            drawRoundedRect(renderer_, borderOuter, 8, {255, 48, 48, 200}); // Red outline
            SDL_Rect borderInner{1, 1, mW + 2, mH + 2};
            drawRoundedRect(renderer_, borderInner, 7, {26, 28, 32, 100});

            // Pause icon (only shown when paused)
            if (!playing) {
                int cx = 2 + mW / 2;
                int cy = 2 + mVH / 2;
                SDL_Rect pauseBg{cx - 16, cy - 16, 32, 32};
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
                fillRoundedRect(renderer_, pauseBg, 16, {0, 0, 0, 170});
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
                SDL_Rect pL{cx - 6, cy - 8, 4, 16};
                SDL_Rect pR{cx + 2, cy - 8, 4, 16};
                SDL_RenderFillRect(renderer_, &pL);
                SDL_RenderFillRect(renderer_, &pR);
            }

            // ── Details strip background ─────────────────────────────────────
            SDL_Rect stripBg{2, 2 + mVH, mW, mSH};
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, 26, 28, 32, 255); // sleek card color
            fillRoundedRect(renderer_, stripBg, 6, {26, 28, 32, 255});
            // Keep top edges flat
            SDL_Rect stripTopFlat{2, 2 + mVH, mW, 12};
            SDL_RenderFillRect(renderer_, &stripTopFlat);

            // Red divider between video and details
            SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 160);
            SDL_Rect divider{2, 2 + mVH, mW, 1};
            SDL_RenderFillRect(renderer_, &divider);

            // Truncate title to fit the strip (only computed on dirty)
            std::string titleTxt = truncateTextToWidth(app->current_video_.title, 1, mW - 16);
            drawTextShadow(renderer_, 8, 2 + mVH + 6, titleTxt, 1, {240, 242, 245, 255});

            // Channel name - left aligned in row 2
            std::string authorTxt = truncateTextToWidth(app->current_video_.author, 1, 100);
            drawText(renderer_, 8, 2 + mVH + 24, authorTxt, 1, {154, 165, 184, 255});

            // Interactive button hints - right aligned in row 2
            int rightX = mW + 4 - 8;
            int row2Y = 2 + mVH + 24;

            // Hint B: Close
            std::string closeLabel = "Close";
            int closeLabelW = 0;
            getTextSize(closeLabel, 1, &closeLabelW, nullptr);
            int badgeBW = 14;
            int badgeBH = 12;
            int badgeBX = rightX - closeLabelW - 4 - badgeBW;

            SDL_Rect rectB{badgeBX, row2Y + 1, badgeBW, badgeBH};
            fillRoundedRect(renderer_, rectB, 3, {255, 214, 64, 40});
            drawRoundedRect(renderer_, rectB, 3, {255, 214, 64, 140});
            int charBW = 0, charBH = 0;
            getTextSize("B", 1, &charBW, &charBH);
            drawText(renderer_, badgeBX + (badgeBW - charBW) / 2, row2Y + 1 + (badgeBH - charBH) / 2 - 1, "B", 1, {255, 214, 64, 255});
            drawText(renderer_, badgeBX + badgeBW + 4, row2Y, closeLabel, 1, {160, 165, 175, 255});

            // Hint A: Play/Pause
            std::string playLabel = playing ? "Pause" : "Play";
            int playLabelW = 0;
            getTextSize(playLabel, 1, &playLabelW, nullptr);
            int badgeAW = 14;
            int badgeAH = 12;
            int badgeAX = badgeBX - 8 - playLabelW - 4 - badgeAW;

            SDL_Rect rectA{badgeAX, row2Y + 1, badgeAW, badgeAH};
            fillRoundedRect(renderer_, rectA, 3, {255, 48, 48, 40});
            drawRoundedRect(renderer_, rectA, 3, {255, 48, 48, 140});
            int charAW = 0, charAH = 0;
            getTextSize("A", 1, &charAW, &charAH);
            drawText(renderer_, badgeAX + (badgeAW - charAW) / 2, row2Y + 1 + (badgeAH - charAH) / 2 - 1, "A", 1, {255, 48, 48, 255});
            drawText(renderer_, badgeAX + badgeAW + 4, row2Y, playLabel, 1, {160, 165, 175, 255});

            miniplayer_layer_.end(renderer_);

            last_miniplayer_video_id_ = app->current_video_.id;
            last_miniplayer_playing_  = playing;
            miniplayer_strip_dirty_   = false;
        }

        // ── Pass 2: live video frame (every frame, directly to screen) ────────
        // Fetch BEFORE any SDL render-target change to avoid the FBO conflict.
        SDL_Texture* previewTex = app->mpv_player_.renderToTexture(renderer_, mW, mVH);
        if (previewTex) {
            SDL_Rect videoDst{mX + 2, mY + 2, mW, mVH};
            SDL_RenderCopy(renderer_, previewTex, nullptr, &videoDst);
            // Mask top rounded corners of the video frame to align with card corners
            maskRoundedCornersTop(renderer_, videoDst, 6, {15, 15, 15, 255});
        }

        // Composite chrome layer on top of the live video
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        miniplayer_layer_.present(renderer_);
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
            drawVolumeOverlay(renderer_, width / 2, 64, app->state_.volume, app->state_.muted, {255, 48, 48, 255});
        } else if (speedActive) {
            drawSpeedOverlay(renderer_, width / 2, 64, app->state_.speed, {64, 148, 255, 255});
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

        static double cached_ram = 0.0, cached_storage_free = 0.0, cached_storage_total = 0.0;
        static uint32_t last_sys_poll = 0;
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks - last_sys_poll >= 1000) {
            App::getSystemMemoryAndStorage(cached_ram, cached_storage_free, cached_storage_total);
            last_sys_poll = now_ticks;
        }
        std::snprintf(buf, sizeof(buf), "RAM RSS: %.1f MB", cached_ram);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
        textY += 16;

        std::snprintf(buf, sizeof(buf), "Storage: %.1f / %.1f GB free", cached_storage_free, cached_storage_total);
        drawText(renderer_, panelX + 10, textY, buf, 1, {255, 255, 255, 255});
    }

    // Browse status bar
    if (app->state_.showUi) {
        app->status_.render(renderer_, app->state_, width, height, app->uiDirty_);
    }
    
    // Loading overlay
    if (app->state_.isLoadingVideo) {
        drawLoadingOverlay(renderer_, width, height, app->loading_status_text_, SDL_GetTicks() / 1000.0f, {255, 255, 255, 255}, true);
        app->uiDirty_ = true;
    }

    SDL_RenderPresent(renderer_);
}

void Compositor::renderBrowseHeader(App* app, int width, int /*height*/, const std::string& title, float scrollY, bool searchScreen) {
    const int expandedHeight = 84;
    const int collapsedHeight = 58;
    const int headerHeight = std::max(collapsedHeight, expandedHeight - static_cast<int>(scrollY * 0.12f));

    bool isSearching = app->state_.isSearching && app->activeGrid() && !app->activeGrid()->cards.empty();

    // Check cache validity.
    bool needsRedraw = (
        !header_layer_.getTexture() ||
        header_layer_.getWidth() != width ||
        header_layer_.getHeight() != headerHeight ||
        last_header_width_ != width ||
        last_header_height_ != headerHeight ||
        last_header_query_ != app->current_search_query_ ||
        last_header_search_screen_ != searchScreen
    );

    if (needsRedraw) {
        header_layer_.init(renderer_, width, headerHeight, {0, 0, width, headerHeight});
        header_layer_.begin(renderer_, {10, 10, 13, 255});

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        // Left red accent bar (3px)
        SDL_SetRenderDrawColor(renderer_, 255, 52, 52, 220);
        SDL_Rect leftBar{0, 0, 3, headerHeight};
        SDL_RenderFillRect(renderer_, &leftBar);

        // Bottom separator line
        SDL_SetRenderDrawColor(renderer_, 255, 52, 52, 60);
        SDL_Rect accent{0, headerHeight - 1, width, 1};
        SDL_RenderFillRect(renderer_, &accent);
        SDL_SetRenderDrawColor(renderer_, 50, 50, 60, 180);
        SDL_Rect accent2{0, headerHeight - 2, width, 1};
        SDL_RenderFillRect(renderer_, &accent2);

        float t = (headerHeight - collapsedHeight) / (float)(expandedHeight - collapsedHeight);
        t = std::max(0.0f, std::min(1.0f, t));

        // ── Title (left, offset past accent bar) ─────────────────────────────
        int titleScale = searchScreen ? 2 : 3;
        int titleH = 0;
        getTextSize(title, titleScale, nullptr, &titleH);
        int titleY = static_cast<int>((headerHeight - titleH) / 2.0f * (1.0f - t) + 12.0f * t);
        SDL_Color titleColor = searchScreen ? SDL_Color{255, 85, 85, 255} : SDL_Color{255, 52, 52, 255};
        drawTextShadow(renderer_, 20, titleY, title, titleScale, titleColor);

        if (!searchScreen) {
            if (t > 0.25f) {
                Uint8 alpha = static_cast<Uint8>(255.0f * std::min(1.0f, (t - 0.25f) / 0.5f));
                drawText(renderer_, 22, titleY + titleH + 4, "RECOMMENDED", 1, {135, 135, 150, alpha});
            }
        } else {
            if (t > 0.15f) {
                Uint8 alpha = static_cast<Uint8>(255.0f * std::min(1.0f, (t - 0.15f) / 0.5f));
                const int bx = 20;
                const int by = titleY + titleH + 6;
                const int bw = width - bx - 12;
                const int bh = 20;

                SDL_Rect bar{bx, by, bw, bh};
                fillRoundedRect(renderer_, bar, 6, {22, 22, 28, alpha});
                drawRoundedRect(renderer_, bar, 6, {82, 82, 100, static_cast<Uint8>(alpha * 0.8f)});

                std::string q = app->current_search_query_.empty()
                                ? "Search..."
                                : utf8Truncate(app->current_search_query_, 50, true);
                SDL_Color qCol = app->current_search_query_.empty()
                                 ? SDL_Color{75, 75, 88, alpha}
                                 : SDL_Color{215, 215, 225, alpha};
                drawText(renderer_, bx + 8, by + 3, q, 1, qCol);
            }
        }

        header_layer_.end(renderer_);

        last_header_width_ = width;
        last_header_height_ = headerHeight;
        last_header_query_ = app->current_search_query_;
        last_header_search_screen_ = searchScreen;
    }

    // Present header layer
    header_layer_.present(renderer_);

    // Loading spinner drawn outside the cached layer so it animates freely
    if (isSearching) {
        float time = SDL_GetTicks() / 1000.0f;
        drawSpinner(renderer_, width - 30, headerHeight / 2, 10, time);
        app->uiDirty_ = true;
    }
}

void Compositor::renderPlaybackOverlay(App* app, int width, int height) {
    double pos    = app->mpv_player_.getPlaybackTime();
    double dur    = app->mpv_player_.getDuration();
    bool   playing = app->mpv_player_.isPlaying();

    auto fmtTime = [](double s) -> std::string {
        if (s < 0) s = 0;
        int tot = static_cast<int>(s);
        int h = tot / 3600, m = (tot % 3600) / 60, sec = tot % 60;
        char buf[16];
        if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
        else       snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
        return buf;
    };

    const double displayTime = app->state_.isScrubbing ? app->state_.scrubTargetTime : pos;
    const double frac        = (dur > 0.0) ? std::max(0.0, std::min(1.0, displayTime / dur)) : 0.0;

    // Initialize or resize HUD layer if needed
    if (!hud_layer_.getTexture() || hud_layer_.getWidth() != width || hud_layer_.getHeight() != height) {
        hud_layer_.init(renderer_, width, height, {0, 0, width, height});
    }

    hud_layer_.begin(renderer_, {0, 0, 0, 0});

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // ── Top Panel ─────────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(renderer_, 16, 18, 22, 220);
    SDL_Rect topPanel{0, 0, width, 56};
    SDL_RenderFillRect(renderer_, &topPanel);
    
    SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 220);
    SDL_Rect topBorder{0, 56, width, 2};
    SDL_RenderFillRect(renderer_, &topBorder);
    
    {
        std::string titleTxt = app->current_video_.title;
        int maxTitleW = width - 28;
        if (app->state_.speed != 1.0) {
            maxTitleW -= 60;
        }
        int titleW = 0, titleH = 0;
        getTextSize(titleTxt, 2, &titleW, &titleH);
        if (titleW > maxTitleW) {
            while (!titleTxt.empty() && titleW > maxTitleW - 16) {
                titleTxt = utf8Slice(titleTxt, 0, utf8Length(titleTxt) - 1);
                getTextSize(titleTxt + "...", 2, &titleW, &titleH);
            }
            titleTxt += "...";
        }
        drawTextShadow(renderer_, 14, 6, titleTxt, 2, {255, 255, 255, 255});

        // Speed badge (top right)
        if (app->state_.speed != 1.0) {
            char spd[10]; snprintf(spd, sizeof(spd), "%.2fx", app->state_.speed);
            int sw = 0, sh = 0; getTextSize(spd, 1, &sw, &sh);
            SDL_Rect badge{width - sw - 20, 10, sw + 12, sh + 6};
            fillRoundedRect(renderer_, badge, 4, {64, 148, 255, 200});
            drawText(renderer_, badge.x + 6, badge.y + 3, spd, 1, {255, 255, 255, 255});
        }

        // Stats string formatted on the right of Line 2
        std::string statsStr = "";
        if (app->active_video_metadata_.view_count > 0 || app->active_video_metadata_.like_count > 0) {
            statsStr = formatStatsNumber(app->active_video_metadata_.view_count) + " VIEWS   •   " +
                       formatStatsNumber(app->active_video_metadata_.like_count) + " LIKES";
            if (app->active_video_metadata_.subscriber_count > 0) {
                statsStr += "   •   " + formatStatsNumber(app->active_video_metadata_.subscriber_count) + " SUBS";
            }
            if (app->active_video_metadata_.comment_count > 0) {
                statsStr += "   •   " + formatStatsNumber(app->active_video_metadata_.comment_count) + " COMMENTS";
            }
        } else {
            statsStr = "LOADING STATS...";
        }

        int statsW = 0, statsH = 0;
        getTextSize(statsStr, 1, &statsW, &statsH);
        drawText(renderer_, width - 14 - statsW, 32, statsStr, 1, {214, 220, 230, 200});

        // Channel Author on the left of Line 2
        if (!app->current_video_.author.empty()) {
            std::string author = app->current_video_.author;
            int maxAuthorW = width - 28 - statsW - 20;
            int authW = 0, authH = 0;
            getTextSize(author, 1, &authW, &authH);
            if (authW > maxAuthorW) {
                while (!author.empty() && authW > maxAuthorW - 12) {
                    author = utf8Slice(author, 0, utf8Length(author) - 1);
                    getTextSize(author + "...", 1, &authW, &authH);
                }
                author += "...";
            }
            drawText(renderer_, 14, 32, author, 1, {255, 48, 48, 255});
        }
    }

    // ── Centre pause/play icon ─────────────────────────────────────────────────
    bool showPlayFlash = false;
    float flashProgress = 0.0f;
    Uint32 ticks = SDL_GetTicks();
    if (app->play_flash_start_time_ > 0) {
        Uint32 diff = ticks - app->play_flash_start_time_;
        if (diff < 400) {
            showPlayFlash = true;
            flashProgress = (float)diff / 400.0f;
        } else {
            app->play_flash_start_time_ = 0;
        }
    }

    if (!playing || app->state_.isScrubbing || showPlayFlash) {
        int baseIconSize = 40;
        int iconSize = baseIconSize;
        Uint8 bgAlpha = 120;
        Uint8 iconAlpha = 200;

        bool drawPlay = showPlayFlash;

        if (showPlayFlash) {
            iconSize = static_cast<int>(baseIconSize * (1.0f + flashProgress * 0.8f));
            bgAlpha = static_cast<Uint8>(120 * (1.0f - flashProgress));
            iconAlpha = static_cast<Uint8>(200 * (1.0f - flashProgress));
        }

        int iconX = (width - iconSize) / 2;
        int iconY = (height - iconSize) / 2;
        
        SDL_Rect iconBg{iconX - 8, iconY - 8, iconSize + 16, iconSize + 16};
        fillRoundedRect(renderer_, iconBg, iconBg.w / 2, {0, 0, 0, bgAlpha});

        int centerX = width / 2;
        int centerY = height / 2;

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        if (drawPlay) {
            SDL_Color color{255, 255, 255, iconAlpha};
            SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
            int halfSize = iconSize / 2;
            int startX = centerX - halfSize + 4;
            int endX = centerX + halfSize;
            int sizeX = endX - startX;
            for (int x = startX; x <= endX; ++x) {
                float t = (sizeX > 0) ? (float)(x - startX) / sizeX : 0.0f;
                int h = static_cast<int>(halfSize * (1.0f - t));
                SDL_RenderDrawLine(renderer_, x, centerY - h, x, centerY + h);
            }
        } else {
            SDL_Color color{255, 255, 255, iconAlpha};
            SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
            int barW = iconSize / 3;
            int barH = iconSize;
            int gap = iconSize / 3;
            SDL_Rect leftBar{centerX - barW - gap / 2, centerY - barH / 2, barW, barH};
            SDL_Rect rightBar{centerX + gap / 2, centerY - barH / 2, barW, barH};
            SDL_RenderFillRect(renderer_, &leftBar);
            SDL_RenderFillRect(renderer_, &rightBar);
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }

    // ── Bottom Panel ──────────────────────────────────────────────────────────
    const int botH = 60; // was 72; saved 12px
    SDL_SetRenderDrawColor(renderer_, 14, 16, 20, 225);
    SDL_Rect botPanel{0, height - botH, width, botH};
    SDL_RenderFillRect(renderer_, &botPanel);
    // Top separator
    SDL_SetRenderDrawColor(renderer_, 32, 36, 44, 255);
    SDL_Rect botBorder{0, height - botH, width, 1};
    SDL_RenderFillRect(renderer_, &botBorder);

    // Progress bar
    const int mg  = 14;
    const int pbY = height - botH + 10;
    const int pbH = 6;
    const int pbW = width - mg * 2;

    // Track (background)
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 68, 68, 78, 255);
    SDL_Rect pbBg{mg, pbY, pbW, pbH};
    SDL_RenderFillRect(renderer_, &pbBg);

    // Buffered indicator
    {
        int bufW = static_cast<int>(pbW * std::min(frac + 0.15, 1.0));
        SDL_SetRenderDrawColor(renderer_, 100, 100, 112, 255);
        SDL_Rect pbBuf{mg, pbY, bufW, pbH};
        SDL_RenderFillRect(renderer_, &pbBuf);
    }

    // Played
    {
        int fillW = static_cast<int>(pbW * frac);
        SDL_SetRenderDrawColor(renderer_, 255, 48, 48, 255);
        SDL_Rect pbFill{mg, pbY, fillW, pbH};
        SDL_RenderFillRect(renderer_, &pbFill);
    }

    // Playhead circle (using fillRoundedRect for a proper disc)
    {
        int dotX = mg + static_cast<int>(pbW * frac);
        int dotR = 6;
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        // White outer disc
        SDL_Rect dotOuter{dotX - dotR, pbY - dotR + pbH / 2, dotR * 2, dotR * 2};
        fillRoundedRect(renderer_, dotOuter, dotR, {255, 255, 255, 255});
        // Red inner disc
        int iR = 4;
        SDL_Rect dotInner{dotX - iR, pbY - iR + pbH / 2, iR * 2, iR * 2};
        fillRoundedRect(renderer_, dotInner, iR, {255, 48, 48, 255});
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // Scrub preview thumbnail above playhead
    if (app->state_.isScrubbing) {
        int dotX = mg + static_cast<int>(pbW * frac);
        std::string timeStr = fmtTime(displayTime);

        SDL_Texture* sbTex = app->storyboard_.getTexture(renderer_, displayTime);
        if (sbTex) {
            int thumbW = 160;
            int thumbH = 90;
            int previewX = std::max(mg, std::min(width - mg - thumbW, dotX - thumbW / 2));
            int previewY = pbY - thumbH - 24;

            SDL_Rect thumbRect{previewX, previewY, thumbW, thumbH};
            SDL_RenderCopy(renderer_, sbTex, nullptr, &thumbRect);
            
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 180);
            SDL_RenderDrawRect(renderer_, &thumbRect);

            int tw = 0, th = 0;
            getTextSize(timeStr, 1, &tw, &th);
            int pillW = tw + 8;
            int pillH = th + 4;
            int pillX = thumbRect.x + (thumbW - pillW) / 2;
            int pillY = thumbRect.y + thumbH - pillH - 4;

            SDL_Rect tsBg{pillX, pillY, pillW, pillH};
            fillRoundedRect(renderer_, tsBg, 3, {0, 0, 0, 200});
            drawText(renderer_, tsBg.x + 4, tsBg.y + 2, timeStr, 1, {255, 255, 255, 255});
        } else {
            int tw = 0, th = 0;
            getTextSize(timeStr, 1, &tw, &th);
            int previewW = tw + 8;
            int previewH = th + 4;
            int previewX = std::max(mg, std::min(width - mg - previewW, dotX - previewW / 2));
            int previewY = pbY - previewH - 12;

            SDL_Rect tsBg{previewX, previewY, previewW, previewH};
            fillRoundedRect(renderer_, tsBg, 3, {0, 0, 0, 200});
            drawText(renderer_, tsBg.x + 4, tsBg.y + 2, timeStr, 1, {255, 255, 255, 255});
        }
    }

    // Timestamps
    {
        std::string posStr = fmtTime(displayTime);
        int tsY = pbY + pbH + 5;
        drawText(renderer_, mg, tsY, posStr, 1, {220, 220, 232, 255});
        if (dur > 0.0) {
            std::string remStr = "-" + fmtTime(dur - displayTime);
            int rw = 0; getTextSize(remStr, 1, &rw, nullptr);
            drawText(renderer_, mg + pbW - rw, tsY, remStr, 1, {160, 160, 172, 255});
        }
    }

    // ── Description Drawer ─────────────────────────────────────────────────────
    if (app->state_.showDescriptionDrawer) {
        SDL_Rect drawerRect{width - 300, 58, 300, height - 120}; // 60px bottom + 58px header + 2px margin
        fillRoundedRect(renderer_, drawerRect, 0, {12, 14, 18, 240});
        
        SDL_SetRenderDrawColor(renderer_, 42, 48, 56, 255);
        SDL_RenderDrawLine(renderer_, drawerRect.x, drawerRect.y, drawerRect.x, drawerRect.y + drawerRect.h);

        if (app->wrapped_description_lines_.empty() && !app->active_video_metadata_.description.empty()) {
            app->wrapped_description_lines_ = wrapText(app->active_video_metadata_.description, 280, 1);
        }
        const auto& descLines = app->wrapped_description_lines_;
        int lineH = 14;
        int visibleLines = drawerRect.h / (lineH + 4);
        int maxScroll = std::max(0, static_cast<int>(descLines.size()) - visibleLines);
        
        app->description_scroll_row_ = std::max(0, std::min(app->description_scroll_row_, maxScroll));

        if (descLines.empty()) {
            std::string noDesc = app->active_video_metadata_.description.empty() ? "No description available." : "Loading...";
            drawTextCentered(renderer_, drawerRect.x + drawerRect.w / 2, drawerRect.y + drawerRect.h / 2, noDesc, 1, {150, 150, 160, 255});
        } else {
            int startIdx = app->description_scroll_row_;
            int endIdx = std::min(static_cast<int>(descLines.size()), startIdx + visibleLines);
            int drawY = drawerRect.y + 10;
            for (int i = startIdx; i < endIdx; ++i) {
                drawText(renderer_, drawerRect.x + 10, drawY, descLines[i], 1, {230, 230, 240, 255});
                drawY += lineH + 4;
            }

            if (descLines.size() > static_cast<size_t>(visibleLines)) {
                int barHeight = drawerRect.h - 20;
                int scrollbarH = static_cast<int>(barHeight * ((double)visibleLines / descLines.size()));
                scrollbarH = std::max(10, scrollbarH);
                int scrollbarY = drawerRect.y + 10 + static_cast<int>((barHeight - scrollbarH) * ((double)app->description_scroll_row_ / maxScroll));
                SDL_Rect scrollbar{drawerRect.x + drawerRect.w - 6, scrollbarY, 4, scrollbarH};
                fillRoundedRect(renderer_, scrollbar, 2, {255, 48, 48, 180});
            }
        }
    }

    // Bottom hint line
    SDL_Color textColor{214, 220, 230, 255};
    const SDL_Color red{255, 48, 48, 255};
    const SDL_Color blue{64, 148, 255, 255};
    const SDL_Color yellow{255, 214, 64, 255};
    const SDL_Color green{64, 214, 96, 255};
    const SDL_Color panel{24, 28, 34, 200};

    std::vector<HintItem> activeHints;
    if (app->state_.showDescriptionDrawer) {
        activeHints = {
            {"A", red, playing ? "PAUSE" : "PLAY"},
            {"B", yellow, "CLOSE"},
            {"UP/DOWN", textColor, "SCROLL"},
            {"FN+A", red, "TOGGLE DESC"},
            {"L1/R1", textColor, "SPEED"},
            {"L2/R2", textColor, "VOL"}
        };
    } else {
        activeHints = {
            {"A", red, playing ? "PAUSE" : "PLAY"},
            {"B", yellow, "EXIT"},
            {"FN+A", red, "DESC"},
            {"SELECT", textColor, "MINI"},
            {"Y", green, "SUBS"},
            {"X", blue, "STATS"},
            {"L1/R1", textColor, "SPEED"},
            {"L2/R2", textColor, "VOL"}
        };
    }

    // Hint bar sits inside the 60px bottom panel, below the timestamp row
    drawHintButtons(renderer_, activeHints, height - 28, 22, 1, width, panel, {42, 48, 56, 180}, textColor);

    hud_layer_.end(renderer_);

    // Copy HUD onto screen
    hud_layer_.present(renderer_);
}

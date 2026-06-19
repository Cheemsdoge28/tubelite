#include "compositor.hpp"
#include "app.hpp"
#include "profiler.hpp"
#include "renderer_utils.hpp"
#include "ui_framework.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
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
    PROFILE_SCOPE("Compositor::render");
    if (app->state_.currentScreen == TubeState::Screen::Playback) {
        PROFILE_SCOPE("playback_screen");
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);

        // Render the video frame (internally delegates to offscreen Layer in MpvPlayer)
        { PROFILE_SCOPE("mpv_video_blit"); app->mpv_player_.render(width, height); }

        // Fade-from-black transition when a new video surface appears.
        { SDL_Rect full{0, 0, width, height}; drawVideoFade(app, full, 0); }

        // Draw the HUD overlay offscreen via Layer and composite it on top of the video
        if (app->state_.showUi) {
            PROFILE_SCOPE("playback_HUD");
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
                drawVolumeOverlay(renderer_, width / 2, 64, app->state_.volume, app->state_.muted, theme::BLUE);
            } else if (speedActive) {
                drawSpeedOverlay(renderer_, width / 2, 64, app->state_.speed, theme::BLUE);
            }
        }

        // Draw loading overlay
        if (app->state_.isLoadingVideo) {
            drawLoadingOverlay(renderer_, width, height, app->loading_status_text_, SDL_GetTicks() / 1000.0f, theme::WHITE, true);
            app->uiDirty_ = true;
        }

        app->keyboard_.render(renderer_, app->state_, width, height, app->uiDirty_);

        // Debug stats overlay (F12 / L3 toggle) now renders during playback too,
        // composited over the player HUD so the user can watch FPS/profiler
        // numbers without leaving the player.  The user-facing "stats" hint
        // already lives in the playback HUD's hint pill row.
        drawDebugOverlay(app, width, height);

        { PROFILE_SCOPE("SDL_RenderPresent"); SDL_RenderPresent(renderer_); }
        return;
    }

    // ── Browse / Search screens ───────────────────────────────────────────────
    PROFILE_SCOPE("browse_screen");
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, theme::BG.r, theme::BG.g, theme::BG.b, 255);
    SDL_RenderClear(renderer_);

    auto currentGrid = app->activeGrid();
    float scrollY = currentGrid ? currentGrid->scrollY : 0.0f;

    if (app->state_.currentScreen == TubeState::Screen::Home) {
        if (app->home_grid_->cards.empty()) {
            if (app->homeLoadFailed_) {
                drawTextCentered(renderer_, width / 2, height / 2 - 10, "Failed to load feed.", 2, theme::ACCENT_BRIGHT);
                drawTextCentered(renderer_, width / 2, height / 2 + 20, "Press Y to search videos", 2, theme::TEXT_3);
            } else {
                drawLoadingOverlay(renderer_, width, height, "Loading Feed...", SDL_GetTicks() / 1000.0f, theme::TEXT_3, false);
                app->uiDirty_ = true;
            }
        } else {
            { PROFILE_SCOPE("home_grid"); app->home_grid_->render(renderer_, 0.0f, 0.0f); }
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
                    maskRoundedCornersTop(renderer_, thumbDst, theme::RADIUS_CARD, theme::BG);
                    drawVideoFade(app, thumbDst, theme::RADIUS_CARD);
                }
            }
            { PROFILE_SCOPE("focus_ring"); app->focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f); }
        }
        { PROFILE_SCOPE("browse_header"); renderBrowseHeader(app, width, height, "TubeLite", scrollY, false); }
    } else if (app->state_.currentScreen == TubeState::Screen::Search) {
        if (app->state_.isSearching && app->search_grid_->cards.empty()) {
            drawLoadingOverlay(renderer_, width, height, "Searching...", SDL_GetTicks() / 1000.0f, theme::TEXT_3, false);
            app->uiDirty_ = true;
        } else if (app->search_grid_->cards.empty()) {
            if (app->current_search_query_.empty()) {
                drawTextCentered(renderer_, width / 2, height / 2, "Press Y to search videos", 2, theme::TEXT_3);
            } else {
                drawTextCentered(renderer_, width / 2, height / 2, "No results found.", 2, theme::TEXT_3);
            }
        } else {
            { PROFILE_SCOPE("search_grid"); app->search_grid_->render(renderer_, 0.0f, 0.0f); }
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
                    maskRoundedCornersTop(renderer_, thumbDst, theme::RADIUS_CARD, theme::BG);
                    drawVideoFade(app, thumbDst, theme::RADIUS_CARD);
                }
            }
            { PROFILE_SCOPE("focus_ring"); app->focus_manager_.renderFocusRing(renderer_, 0.0f, 0.0f); }
        }
        { PROFILE_SCOPE("browse_header"); renderBrowseHeader(app, width, height, "Search", scrollY, true); }
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
        PROFILE_SCOPE("miniplayer");
        const int mW  = 240;
        const int mVH = 135;   // video area height (16:9 of 240)
        const int mSH = 60;    // details/title strip height
        const int mH  = mVH + mSH;
        const int kStatusBarH = 48;  // matches StatusOverlay's bar height
        const int mX  = width  - mW - 12;
        const int mY  = height - kStatusBarH - mH - 10; // float just above the status bar

        const SDL_Rect miniplayerBounds{mX - 2, mY - 2, mW + 4, mH + 4};

        // Per-frame backplate: opaque BG fill behind the video so cards
        // scrolling underneath can't bleed through during the brief window
        // before the live video texture catches up.  This is what prevents
        // flicker on a fullscreen↔miniplayer toggle where the video FBO
        // gets re-created at a new size (640×480 → 240×135) and is empty
        // for a frame.  Cheap: one rounded blit.
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        fillRoundedRect(renderer_, miniplayerBounds, theme::RADIUS_CARD, theme::BG);

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
            drawRoundedRect(renderer_, borderOuter, theme::RADIUS_CARD, theme::ACCENT.a8(200)); // Red outline
            SDL_Rect borderInner{1, 1, mW + 2, mH + 2};
            drawRoundedRect(renderer_, borderInner, 7, theme::SURFACE.a8(100));

            // Pause icon (only shown when paused)
            if (!playing) {
                int cx = 2 + mW / 2;
                int cy = 2 + mVH / 2;
                SDL_Rect pauseBg{cx - 16, cy - 16, 32, 32};
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
                fillRoundedRect(renderer_, pauseBg, 16, theme::BLACK.a8(170));
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer_, theme::WHITE.r, theme::WHITE.g, theme::WHITE.b, 255);
                SDL_Rect pL{cx - 6, cy - 8, 4, 16};
                SDL_Rect pR{cx + 2, cy - 8, 4, 16};
                SDL_RenderFillRect(renderer_, &pL);
                SDL_RenderFillRect(renderer_, &pR);
            }

            // ── Details strip background ─────────────────────────────────────
            SDL_Rect stripBg{2, 2 + mVH, mW, mSH};
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, theme::SURFACE.r, theme::SURFACE.g, theme::SURFACE.b, 255); // sleek card color
            fillRoundedRect(renderer_, stripBg, theme::RADIUS_PANEL, theme::SURFACE);
            // Keep top edges flat
            SDL_Rect stripTopFlat{2, 2 + mVH, mW, 12};
            SDL_RenderFillRect(renderer_, &stripTopFlat);

            // Red divider between video and details
            SDL_SetRenderDrawColor(renderer_, theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b, 160);
            SDL_Rect divider{2, 2 + mVH, mW, 1};
            SDL_RenderFillRect(renderer_, &divider);

            // Row 1: Title (Truncated to fit, centered)
            std::string titleTxt = truncateTextToWidth(app->current_video_.title, 1, mW - 16);
            drawTextCentered(renderer_, (mW + 4) / 2, 2 + mVH + 6, titleTxt, 1, theme::TEXT, true);

            // Row 2: Channel name (Truncated, centered)
            std::string authorTxt = truncateTextToWidth(app->current_video_.author, 1, mW - 16);
            drawTextCentered(renderer_, (mW + 4) / 2, 2 + mVH + 22, authorTxt, 1, theme::TEXT_2);

            // Row 3: Centered compact hint buttons
            std::string btnB = "SEL+B";
            std::string actB = "CLOSE";
            std::string btnA = "SEL+A";
            std::string actA = playing ? "PAUSE" : "PLAY";

            int btnBW = 0, actBW = 0, btnAW = 0, actAW = 0;
            int fh = 0;
            getTextSize(btnB, 1, &btnBW, &fh);
            getTextSize(actB, 1, &actBW, nullptr);
            getTextSize(btnA, 1, &btnAW, nullptr);
            getTextSize(actA, 1, &actAW, nullptr);

            int pillPad = 6;
            int labelGap = 4;
            int buttonGap = 12;

            int pillBW = btnBW + pillPad;
            int pillAW = btnAW + pillPad;

            int itemBW = pillBW + labelGap + actBW;
            int itemAW = pillAW + labelGap + actAW;

            int totalW = itemAW + buttonGap + itemBW;
            int startX = (mW + 4 - totalW) / 2;
            int row3Y = 2 + mVH + 38;
            int pillH = 14;

            // Draw SEL+A
            SDL_Rect pillARect{startX, row3Y, pillAW, pillH};
            fillRoundedRect(renderer_, pillARect, theme::RADIUS_SM, theme::CHIP);
            drawRoundedRect(renderer_, pillARect, theme::RADIUS_SM, theme::HAIRLINE);
            drawText(renderer_, startX + pillPad / 2, row3Y + (pillH - fh) / 2 - 1, btnA, 1, theme::ACCENT); // Red
            drawText(renderer_, startX + pillAW + labelGap, row3Y + (pillH - fh) / 2 - 1, actA, 1, theme::TEXT_ON);

            // Draw SEL+B
            int startBX = startX + itemAW + buttonGap;
            SDL_Rect pillBRect{startBX, row3Y, pillBW, pillH};
            fillRoundedRect(renderer_, pillBRect, theme::RADIUS_SM, theme::CHIP);
            drawRoundedRect(renderer_, pillBRect, theme::RADIUS_SM, theme::HAIRLINE);
            drawText(renderer_, startBX + pillPad / 2, row3Y + (pillH - fh) / 2 - 1, btnB, 1, theme::YELLOW); // Yellow
            drawText(renderer_, startBX + pillBW + labelGap, row3Y + (pillH - fh) / 2 - 1, actB, 1, theme::TEXT_ON);

            miniplayer_layer_.end(renderer_);

            last_miniplayer_video_id_ = app->current_video_.id;
            last_miniplayer_playing_  = playing;
            miniplayer_strip_dirty_   = false;
        }

        // ── Pass 2: live video frame (every frame, directly to screen) ────────
        // Fetch BEFORE any SDL render-target change to avoid the FBO conflict.
        // mpv now renders directly at our 16:9 target (240x135) so no source
        // crop / aspect adjustment is needed — the texture matches the dst.
        // If preview already rendered this frame at a different size, the
        // shared texture is whatever preview produced; both are 16:9 so the
        // SDL_RenderCopy scale is a clean uniform stretch with no distortion.
        SDL_Texture* previewTex = app->mpv_player_.renderToTexture(renderer_, mW, mVH);
        if (previewTex) {
            SDL_Rect videoDst{mX + 2, mY + 2, mW, mVH};
            SDL_RenderCopy(renderer_, previewTex, nullptr, &videoDst);
            maskRoundedCornersTop(renderer_, videoDst, theme::RADIUS_PANEL, theme::BG);
            drawVideoFade(app, videoDst, theme::RADIUS_PANEL);
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
            drawVolumeOverlay(renderer_, width / 2, 64, app->state_.volume, app->state_.muted, theme::ACCENT);
        } else if (speedActive) {
            drawSpeedOverlay(renderer_, width / 2, 64, app->state_.speed, theme::BLUE);
        }
    }

    { PROFILE_SCOPE("keyboard"); app->keyboard_.render(renderer_, app->state_, width, height, app->uiDirty_); }

    // Draw telemetry overlay if enabled
    drawDebugOverlay(app, width, height);

    // Browse status bar
    if (app->state_.showUi) {
        PROFILE_SCOPE("status_overlay");
        app->status_.render(renderer_, app->state_, width, height, app->uiDirty_);
    }

    // Loading overlay
    if (app->state_.isLoadingVideo) {
        drawLoadingOverlay(renderer_, width, height, app->loading_status_text_, SDL_GetTicks() / 1000.0f, theme::WHITE, true);
        app->uiDirty_ = true;
    }

    { PROFILE_SCOPE("SDL_RenderPresent"); SDL_RenderPresent(renderer_); }
}

void Compositor::drawDebugOverlay(App* app, int width, int /*height*/) {
    if (!app->state_.showDebugOverlay) return;
    PROFILE_SCOPE("debug_overlay");
    {
        // ── Snapshot profiler sections, sort by avg time, drop empties ────────
        struct Row { const char* name; float avg_ms; float max_ms; float avg_calls; };
        Row rows[Profiler::MAX_SECTIONS];
        int  nRows = 0;
        const auto& prof = Profiler::instance();
        for (int i = 0; i < prof.sectionCount(); ++i) {
            const auto& s = prof.section(i);
            const float ms = s.avg_ns / 1.0e6f;
            // Keep entries that ran in any recent frame even if cur frame was 0.
            if (s.avg_calls < 0.01f && ms < 0.001f) continue;
            rows[nRows++] = {s.name, ms, s.max_ns / 1.0e6f, s.avg_calls};
            if (nRows >= Profiler::MAX_SECTIONS) break;
        }
        std::sort(rows, rows + nRows, [](const Row& a, const Row& b){
            return a.avg_ms > b.avg_ms;
        });
        const int maxShown = std::min(nRows, 14);

        // Panel sized to fit header rows + sidecar block + profiler rows + sparkline
        const int headerRows  = 7;     // FPS, profiler, latency, drops, queue, RAM, storage
        const int sidecarRows = 5;     // tubed status + 3 latency lines + image stats
        const int sparkH      = 28;
        const int rowH        = 12;
        const int panelW      = 320;
        const int panelH      = 14 + headerRows * 16 + 6 + sidecarRows * 12 + 6
                              + 6 + (maxShown + 1) * rowH + 8 + sparkH + 10;
        const int panelX     = width - panelW - 10;
        const int panelY     = 60;

        SDL_Rect rect{panelX, panelY, panelW, panelH};
        fillRoundedRect(renderer_, rect, theme::RADIUS_PANEL, theme::BLACK.a8(210));
        drawRoundedRect(renderer_, rect, theme::RADIUS_PANEL, theme::TEXT_3);

        char buf[256];
        int textY = panelY + 8;

        std::snprintf(buf, sizeof(buf), "FPS: %.1f   Frame: %.2f ms",
                      app->current_fps_, prof.lastFrameMs());
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::WHITE); textY += 16;

        // Profiler self-overhead: per-scope cost × calls this frame.  Subtract
        // from frame total to see "useful" work vs. measurement noise.
        std::snprintf(buf, sizeof(buf), "Profiler: %.2f ms (%d scopes @ %.0f ns)",
                      prof.avgOverheadMs(), prof.lastTotalScopeCalls(),
                      prof.perScopeOverheadNs());
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::TEXT_3); textY += 16;

        std::snprintf(buf, sizeof(buf), "Render Latency: %.2f ms", app->render_latency_ms_);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::WHITE); textY += 16;

        int64_t vo_drops  = app->mpv_player_.getPropertyInt("vo-drop-frame-count");
        int64_t dec_drops = app->mpv_player_.getPropertyInt("decoder-frame-drop-count");
        std::snprintf(buf, sizeof(buf), "Drops: VO %lld / Dec %lld",
                      (long long)vo_drops, (long long)dec_drops);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::WHITE); textY += 16;

        size_t q_size = 0;
        {
            std::lock_guard<std::mutex> lock(app->queue_mutex_);
            q_size = app->main_thread_queue_.size();
        }
        std::snprintf(buf, sizeof(buf), "Queue Size: %zu", q_size);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::WHITE); textY += 16;

        static double cached_ram = 0.0, cached_storage_free = 0.0, cached_storage_total = 0.0;
        static uint32_t last_sys_poll = 0;
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks - last_sys_poll >= 1000) {
            App::getSystemMemoryAndStorage(cached_ram, cached_storage_free, cached_storage_total);
            last_sys_poll = now_ticks;
        }
        std::snprintf(buf, sizeof(buf), "RAM RSS: %.1f MB", cached_ram);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::WHITE); textY += 16;

        std::snprintf(buf, sizeof(buf), "Storage: %.1f / %.1f GB free",
                      cached_storage_free, cached_storage_total);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::WHITE); textY += 16;

        // ── Sidecar (tubed) + ImageManager telemetry ─────────────────────────
        SDL_SetRenderDrawColor(renderer_, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, 180);
        SDL_Rect div1{panelX + 8, textY + 2, panelW - 16, 1};
        SDL_RenderFillRect(renderer_, &div1);
        textY += 6;

        const auto& yt = app->youtube_api_.telemetry();
        const auto& im = app->image_manager_->telemetry();

        // tubed-side request inflight + total counts
        std::snprintf(buf, sizeof(buf), "tubed: S%d P%d /S%llu P%llu F%llu C%llu",
            yt.searches_inflight.load() + yt.streams_inflight.load(),
            yt.previews_inflight.load(),
            (unsigned long long)yt.streams_total.load(),
            (unsigned long long)yt.previews_total.load(),
            (unsigned long long)yt.streams_failed.load(),
            (unsigned long long)yt.previews_cancelled.load());
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::TEXT); textY += 12;

        // Latencies — last + EMA
        std::snprintf(buf, sizeof(buf), "  stream last %u ms / ema %.0f ms",
            yt.last_stream_ms.load(),
            yt.ema_stream_ms_x10.load() / 10.0);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::TEXT_2); textY += 12;

        std::snprintf(buf, sizeof(buf), "  preview last %u ms / ema %.0f ms",
            yt.last_preview_ms.load(),
            yt.ema_preview_ms_x10.load() / 10.0);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::TEXT_2); textY += 12;

        std::snprintf(buf, sizeof(buf), "  search last %u ms / total wait %.1fs",
            yt.last_search_ms.load(),
            yt.tubed_wait_ms_total.load() / 1000.0);
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::TEXT_2); textY += 12;

        // Image manager
        std::snprintf(buf, sizeof(buf), "imgs: dl %d  q %d  tex %d  cache %d  ok %llu  fail %llu",
            im.downloads_inflight.load(),
            im.queue_depth.load(),
            im.texture_queue_depth.load(),
            im.cache_size.load(),
            (unsigned long long)im.thumbnails_loaded_total.load(),
            (unsigned long long)im.thumbnails_failed_total.load());
        drawText(renderer_, panelX + 10, textY, buf, 1, theme::TEXT); textY += 14;

        // ── Section list (top N, sorted by avg ms) ───────────────────────────
        SDL_SetRenderDrawColor(renderer_, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, 180);
        SDL_Rect divider{panelX + 8, textY + 2, panelW - 16, 1};
        SDL_RenderFillRect(renderer_, &divider);
        textY += 6;

        // Header
        drawText(renderer_, panelX + 10,           textY, "section",      1, theme::TEXT_3);
        drawText(renderer_, panelX + panelW - 132, textY, "avg",          1, theme::TEXT_3);
        drawText(renderer_, panelX + panelW - 88,  textY, "max",          1, theme::TEXT_3);
        drawText(renderer_, panelX + panelW - 40,  textY, "n",            1, theme::TEXT_3);
        textY += rowH;

        // Find peak for the inline bar visualization
        float peak = 0.0f;
        for (int i = 0; i < maxShown; ++i) if (rows[i].avg_ms > peak) peak = rows[i].avg_ms;
        if (peak < 0.001f) peak = 0.001f;

        for (int i = 0; i < maxShown; ++i) {
            const Row& r = rows[i];
            // Heat color: green < 1ms < yellow < 3ms < red
            SDL_Color col = theme::TEXT;
            if      (r.avg_ms < 1.0f) col = theme::GREEN;
            else if (r.avg_ms < 3.0f) col = theme::YELLOW;
            else                      col = theme::ACCENT;

            // Inline bar under the row
            int barW = (int)((panelW - 24) * (r.avg_ms / peak));
            SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, 60);
            SDL_Rect bar{panelX + 10, textY + rowH - 2, barW, 1};
            SDL_RenderFillRect(renderer_, &bar);

            std::snprintf(buf, sizeof(buf), "%.20s", r.name ? r.name : "?");
            drawText(renderer_, panelX + 10, textY, buf, 1, col);

            std::snprintf(buf, sizeof(buf), "%5.2f", r.avg_ms);
            drawText(renderer_, panelX + panelW - 132, textY, buf, 1, col);

            std::snprintf(buf, sizeof(buf), "%5.2f", r.max_ms);
            drawText(renderer_, panelX + panelW - 88,  textY, buf, 1, col);

            std::snprintf(buf, sizeof(buf), "%4.1f", r.avg_calls);
            drawText(renderer_, panelX + panelW - 40,  textY, buf, 1, col);

            textY += rowH;
        }

        // ── Frame-time sparkline ──────────────────────────────────────────────
        textY += 4;
        float hist[Profiler::HIST_FRAMES];
        int   histN = 0;
        prof.getFrameHistory(hist, histN);

        SDL_Rect sparkRect{panelX + 10, textY, panelW - 20, sparkH};
        SDL_SetRenderDrawColor(renderer_, theme::TRACK.r, theme::TRACK.g, theme::TRACK.b, 80);
        SDL_RenderFillRect(renderer_, &sparkRect);

        // 16.6 ms (60fps) reference line
        int refY = sparkRect.y + sparkRect.h
                   - (int)((16.6f / 33.3f) * sparkRect.h);
        SDL_SetRenderDrawColor(renderer_, theme::GREEN.r, theme::GREEN.g, theme::GREEN.b, 140);
        SDL_Rect refLine{sparkRect.x, refY, sparkRect.w, 1};
        SDL_RenderFillRect(renderer_, &refLine);

        if (histN > 0) {
            // Scale to a 0..33.3 ms range (anything over 33ms clips to the top).
            for (int i = 0; i < histN; ++i) {
                float ms = hist[i];
                if (ms < 0.0f) ms = 0.0f;
                float t  = ms / 33.3f;
                if (t > 1.0f) t = 1.0f;
                int barH = (int)(t * sparkRect.h);
                int x = sparkRect.x + (int)((float)i / histN * sparkRect.w);
                SDL_Color col = (ms <= 16.6f) ? theme::GREEN
                              : (ms <= 25.0f) ? theme::YELLOW
                              :                 theme::ACCENT;
                SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, 220);
                SDL_Rect bar{x, sparkRect.y + sparkRect.h - barH, 2, barH};
                SDL_RenderFillRect(renderer_, &bar);
            }
        }
    }
}

void Compositor::drawVideoFade(App* app, const SDL_Rect& region, int radius) {
    if (app->video_fade_start_time_ == 0) return;
    const Uint32 dur = 280;  // ms
    Uint32 elapsed = SDL_GetTicks() - app->video_fade_start_time_;
    if (elapsed >= dur) { app->video_fade_start_time_ = 0; return; }
    Uint8 a = static_cast<Uint8>(255.0f * (1.0f - static_cast<float>(elapsed) / dur));
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    fillRoundedRect(renderer_, region, radius, theme::BLACK.a8(a));
    app->uiDirty_ = true;  // keep the frames coming while it animates
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
        header_layer_.begin(renderer_, theme::BG);

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        // Left red accent bar (3px)
        SDL_SetRenderDrawColor(renderer_, theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b, 220);
        SDL_Rect leftBar{0, 0, 3, headerHeight};
        SDL_RenderFillRect(renderer_, &leftBar);

        // Bottom separator line
        SDL_SetRenderDrawColor(renderer_, theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b, 60);
        SDL_Rect accent{0, headerHeight - 1, width, 1};
        SDL_RenderFillRect(renderer_, &accent);
        SDL_SetRenderDrawColor(renderer_, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, 180);
        SDL_Rect accent2{0, headerHeight - 2, width, 1};
        SDL_RenderFillRect(renderer_, &accent2);

        float t = (headerHeight - collapsedHeight) / (float)(expandedHeight - collapsedHeight);
        t = std::max(0.0f, std::min(1.0f, t));

        // ── Title (left, offset past accent bar) ─────────────────────────────
        int titleScale = searchScreen ? 2 : 3;
        int titleH = 0;
        getTextSize(title, titleScale, nullptr, &titleH);
        int titleY = static_cast<int>((headerHeight - titleH) / 2.0f * (1.0f - t) + 12.0f * t);
        SDL_Color titleColor = searchScreen ? SDL_Color(theme::ACCENT_BRIGHT) : SDL_Color(theme::ACCENT);
        drawTextShadow(renderer_, 20, titleY, title, titleScale, titleColor);

        if (!searchScreen) {
            if (t > 0.25f) {
                Uint8 alpha = static_cast<Uint8>(255.0f * std::min(1.0f, (t - 0.25f) / 0.5f));
                drawText(renderer_, 22, titleY + titleH + 4, "RECOMMENDED", 1, theme::TEXT_MUTED.a8(alpha));
            }
        } else {
            if (t > 0.15f) {
                Uint8 alpha = static_cast<Uint8>(255.0f * std::min(1.0f, (t - 0.15f) / 0.5f));
                const int bx = 20;
                const int by = titleY + titleH + 6;
                const int bw = width - bx - 12;
                const int bh = 20;

                SDL_Rect bar{bx, by, bw, bh};
                fillRoundedRect(renderer_, bar, theme::RADIUS_PANEL, theme::PANEL.a8(alpha));
                drawRoundedRect(renderer_, bar, theme::RADIUS_PANEL, theme::HAIRLINE.a8(static_cast<Uint8>(alpha * 0.8f)));

                std::string q = app->current_search_query_.empty()
                                ? "Search..."
                                : utf8Truncate(app->current_search_query_, 50, true);
                SDL_Color qCol = app->current_search_query_.empty()
                                 ? SDL_Color(theme::TEXT_MUTED.a8(alpha))
                                 : SDL_Color(theme::TEXT.a8(alpha));
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
    using namespace std::chrono;
    auto now = steady_clock::now();
    double remaining = duration<double>(app->playback_ui_timeout_ - now).count();
    int opacity = 255;
    if (app->state_.showDescriptionDrawer) {
        opacity = 255;
    } else if (remaining <= 0.0) {
        opacity = 0;
    } else if (remaining < 0.5) {
        opacity = static_cast<int>(255.0 * (remaining / 0.5));
        if (opacity < 0) opacity = 0;
        if (opacity > 255) opacity = 255;
    }

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

    double displayTime = pos;
    if (app->state_.isScrubbing) {
        displayTime = app->state_.scrubTargetTime;
    } else if (app->last_seek_time_.has_value()) {
        auto elapsed_ms = duration_cast<milliseconds>(now - app->last_seek_time_point_).count();
        if (elapsed_ms < 800 && std::abs(pos - app->last_seek_time_.value()) >= 1.0) {
            displayTime = app->last_seek_time_.value();
        } else {
            app->last_seek_time_ = std::nullopt;
        }
    }
    const double frac        = (dur > 0.0) ? std::max(0.0, std::min(1.0, displayTime / dur)) : 0.0;

    // ── Text-cache update (cheap, no SDL state changes) ───────────────────────
    // Recompute truncated title/author/stats only when the inputs change.
    // truncateTextToWidth uses binary search (O(log n) getTextSize calls).
    const bool hasBadge = (app->state_.speed != 1.0);
    const long long curViews = app->active_video_metadata_.view_count;
    if (hud_cache_id_    != app->current_video_.id ||
        hud_cache_width_ != width                   ||
        hud_cache_speed_ != hasBadge                ||
        hud_cache_views_ != curViews) {

        hud_cache_id_    = app->current_video_.id;
        hud_cache_width_ = width;
        hud_cache_speed_ = hasBadge;
        hud_cache_views_ = curViews;

        int maxTitleW = width - 28 - (hasBadge ? 60 : 0);
        hud_title_ = truncateTextToWidth(app->current_video_.title, 2, maxTitleW);

        const auto& meta = app->active_video_metadata_;
        if (meta.view_count > 0 || meta.like_count > 0) {
            hud_stats_ = formatStatsNumber(meta.view_count) + " VIEWS   •   " +
                         formatStatsNumber(meta.like_count) + " LIKES";
            if (meta.subscriber_count > 0)
                hud_stats_ += "   •   " + formatStatsNumber(meta.subscriber_count) + " SUBS";
            if (meta.comment_count > 0)
                hud_stats_ += "   •   " + formatStatsNumber(meta.comment_count) + " COMMENTS";
        } else {
            hud_stats_ = "LOADING STATS...";
        }
        getTextSize(hud_stats_, 1, &hud_stats_w_, nullptr);

        int maxAuthorW = width - 28 - hud_stats_w_ - 20;
        hud_author_ = truncateTextToWidth(app->current_video_.author, 1, maxAuthorW);

        hud_static_dirty_ = true;
    }

    // ── Static-decoration layer cache ─────────────────────────────────────────
    // Bar backgrounds, title/author/stats text, speed badge, hint buttons —
    // anything that doesn't move from one frame to the next — lives in a
    // cached SDL texture.  We pay ONE SDL_SetRenderTarget (Mali tile flush) at
    // rebuild time, then composite the whole HUD chrome with a single
    // SDL_RenderCopy every frame thereafter.
    const bool drawerOpen   = app->state_.showDescriptionDrawer;
    const bool playingState = playing;
    if (!hud_static_layer_.getTexture() ||
        hud_static_layer_.getWidth()  != width  ||
        hud_static_layer_.getHeight() != height ||
        hud_static_w_                 != width  ||
        hud_static_h_                 != height ||
        hud_static_video_id_          != app->current_video_.id ||
        hud_static_speed_badge_       != hasBadge ||
        hud_static_views_             != curViews ||
        hud_static_drawer_open_       != drawerOpen ||
        hud_static_playing_           != playingState) {
        hud_static_dirty_ = true;
    }

    if (hud_static_dirty_) {
        PROFILE_SCOPE("hud_static_rebuild");
        PROFILE_COUNT("hud_static_rebuild_n");
        if (!hud_static_layer_.getTexture() ||
            hud_static_layer_.getWidth()  != width ||
            hud_static_layer_.getHeight() != height) {
            hud_static_layer_.init(renderer_, width, height, {0, 0, width, height});
        }
        hud_static_layer_.begin(renderer_, {0, 0, 0, 0});

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        // ── Top Panel ─────────────────────────────────────────────────────────
        // 52px two-row layout: title row + meta row.  Layered scrim fades the
        // background toward transparent at the bottom so the video shows
        // through.  Left red accent bar mirrors the browse header.
        const int topH = 52;
        SDL_SetRenderDrawColor(renderer_, theme::BAR.r, theme::BAR.g, theme::BAR.b, 235);
        SDL_Rect topPanelHi{0, 0, width, topH - 16};
        SDL_RenderFillRect(renderer_, &topPanelHi);
        SDL_SetRenderDrawColor(renderer_, theme::BAR.r, theme::BAR.g, theme::BAR.b, 170);
        SDL_Rect topPanelLo{0, topH - 16, width, 16};
        SDL_RenderFillRect(renderer_, &topPanelLo);

        // Left red accent bar (matches browse header)
        SDL_SetRenderDrawColor(renderer_, theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b, 220);
        SDL_Rect leftBar{0, 0, 3, topH};
        SDL_RenderFillRect(renderer_, &leftBar);

        // Bottom hairline (single 1px — feels cleaner than the old 2px slab)
        SDL_SetRenderDrawColor(renderer_, theme::DIVIDER.r, theme::DIVIDER.g, theme::DIVIDER.b, 200);
        SDL_Rect topBorder{0, topH, width, 1};
        SDL_RenderFillRect(renderer_, &topBorder);

        // Title row
        drawTextShadow(renderer_, 12, 4, hud_title_, 2, theme::WHITE);

        // Speed badge (top-right corner)
        int titleRightLimit = width - 12;
        if (hasBadge) {
            char spd[10]; snprintf(spd, sizeof(spd), "%.2fx", app->state_.speed);
            int sw = 0, sh = 0; getTextSize(spd, 1, &sw, &sh);
            SDL_Rect badge{width - sw - 18, 7, sw + 12, sh + 6};
            fillRoundedRect(renderer_, badge, theme::RADIUS_PILL, theme::BLUE.a8(220));
            drawRoundedRect(renderer_, badge, theme::RADIUS_PILL, theme::WHITE.a8(60));
            drawText(renderer_, badge.x + 6, badge.y + 3, spd, 1, theme::WHITE);
            titleRightLimit = badge.x - 8;
        }

        // Meta row (channel · stats) — second line, smaller, muted
        const int metaY = 32;
        if (!hud_author_.empty()) {
            drawText(renderer_, 12, metaY, hud_author_, 1, theme::ACCENT);
        }
        // Stats right-aligned; uses dimmer color so the title still owns the
        // visual emphasis.
        if (titleRightLimit - hud_stats_w_ - 12 >= 0) {
            drawText(renderer_, width - 12 - hud_stats_w_, metaY,
                     hud_stats_, 1, theme::TEXT_3);
        }

        // ── Bottom Panel ──────────────────────────────────────────────────────
        // Tighter 52px bar.  Layered scrim like the top panel.
        const int botH = 52;
        SDL_SetRenderDrawColor(renderer_, theme::BAR.r, theme::BAR.g, theme::BAR.b, 175);
        SDL_Rect botPanelHi{0, height - botH, width, 12};
        SDL_RenderFillRect(renderer_, &botPanelHi);
        SDL_SetRenderDrawColor(renderer_, theme::BAR.r, theme::BAR.g, theme::BAR.b, 235);
        SDL_Rect botPanelLo{0, height - botH + 12, width, botH - 12};
        SDL_RenderFillRect(renderer_, &botPanelLo);

        // Top hairline
        SDL_SetRenderDrawColor(renderer_, theme::DIVIDER.r, theme::DIVIDER.g, theme::DIVIDER.b, 200);
        SDL_Rect botBorder{0, height - botH, width, 1};
        SDL_RenderFillRect(renderer_, &botBorder);

        // Hint buttons (the row of pills along the bottom — same content every
        // frame for a given drawer/playing state, so safe to cache).
        SDL_Color textColor    = theme::TEXT_ON;
        const SDL_Color red    = theme::ACCENT;
        const SDL_Color blue   = theme::BLUE;
        const SDL_Color yellow = theme::YELLOW;
        const SDL_Color green  = theme::GREEN;
        const SDL_Color purple = theme::PURPLE;
        const SDL_Color panel  = theme::PANEL.a8(180);

        std::vector<HintItem> activeHints;
        if (drawerOpen) {
            activeHints = {
                {"A", red, playingState ? "PAUSE" : "PLAY"},
                {"B", yellow, "CLOSE"},
                {"UP/DOWN", textColor, "SCROLL"},
                {"FN+A", purple, "TOGGLE DESC"},
                {"L1/R1", textColor, "SPEED"},
                {"L2/R2", textColor, "VOL"}
            };
        } else {
            activeHints = {
                {"A", red, playingState ? "PAUSE" : "PLAY"},
                {"B", yellow, "EXIT"},
                {"FN+A", purple, "DESC"},
                {"SEL", textColor, "MINI"},
                {"Y", green, "SUBS"},
                {"L3", blue, "STATS"},
                {"L1/R1", textColor, "SPEED"},
                {"L2/R2", textColor, "VOL"}
            };
        }
        drawHintButtons(renderer_, activeHints, height - 24, 20, 1, width, panel, theme::CHIP.a8(160), textColor);

        hud_static_layer_.end(renderer_);

        hud_static_dirty_       = false;
        hud_static_w_           = width;
        hud_static_h_           = height;
        hud_static_video_id_    = app->current_video_.id;
        hud_static_speed_badge_ = hasBadge;
        hud_static_views_       = curViews;
        hud_static_drawer_open_ = drawerOpen;
        hud_static_playing_     = playingState;
    }

    // ── Pick the per-frame draw target ────────────────────────────────────────
    // Common case (opacity == 255): draw dynamic parts straight to the screen
    // and composite the cached static layer.  Fade-out case: route everything
    // through hud_layer_ so the whole HUD fades as a unit.
    const bool use_fbo = (opacity < 255);
    if (use_fbo) {
        if (!hud_layer_.getTexture() || hud_layer_.getWidth() != width || hud_layer_.getHeight() != height) {
            hud_layer_.init(renderer_, width, height, {0, 0, width, height});
        }
        hud_layer_.begin(renderer_, {0, 0, 0, 0});
    }

    // Blit the cached static decoration first (covers both panels + their text).
    if (hud_static_layer_.getTexture()) {
        SDL_SetTextureBlendMode(hud_static_layer_.getTexture(), SDL_BLENDMODE_BLEND);
        SDL_Rect full{0, 0, width, height};
        SDL_RenderCopy(renderer_, hud_static_layer_.getTexture(), nullptr, &full);
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

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
        fillRoundedRect(renderer_, iconBg, iconBg.w / 2, theme::BLACK.a8(bgAlpha));

        int centerX = width / 2;
        int centerY = height / 2;

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        if (drawPlay) {
            SDL_Color color = theme::WHITE.a8(iconAlpha);
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
            SDL_Color color = theme::WHITE.a8(iconAlpha);
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

    // ── Dynamic bottom panel content ──────────────────────────────────────────
    // The bottom-panel BACKGROUND lives in hud_static_layer_; here we only draw
    // the live progress bar, timestamps, and scrub thumbnail.
    const int botH = 52;

    // Progress bar — flatter (4px), pulled right against the top of the panel
    const int mg  = 12;
    const int pbY = height - botH + 4;
    const int pbH = 4;
    const int pbW = width - mg * 2;

    // Track (background)
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, theme::TRACK.r, theme::TRACK.g, theme::TRACK.b, 255);
    SDL_Rect pbBg{mg, pbY, pbW, pbH};
    SDL_RenderFillRect(renderer_, &pbBg);

    // Buffered indicator
    {
        int bufW = static_cast<int>(pbW * std::min(frac + 0.15, 1.0));
        SDL_SetRenderDrawColor(renderer_, theme::BUFFERED.r, theme::BUFFERED.g, theme::BUFFERED.b, 255);
        SDL_Rect pbBuf{mg, pbY, bufW, pbH};
        SDL_RenderFillRect(renderer_, &pbBuf);
    }

    // Played
    {
        int fillW = static_cast<int>(pbW * frac);
        SDL_SetRenderDrawColor(renderer_, theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b, 255);
        SDL_Rect pbFill{mg, pbY, fillW, pbH};
        SDL_RenderFillRect(renderer_, &pbFill);
    }

    // Playhead circle (using fillRoundedRect for a proper disc)
    {
        int dotX = mg + static_cast<int>(pbW * frac);
        int dotR = 5;
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        // White outer disc
        SDL_Rect dotOuter{dotX - dotR, pbY - dotR + pbH / 2, dotR * 2, dotR * 2};
        fillRoundedRect(renderer_, dotOuter, dotR, theme::WHITE);
        // Red inner disc
        int iR = 3;
        SDL_Rect dotInner{dotX - iR, pbY - iR + pbH / 2, iR * 2, iR * 2};
        fillRoundedRect(renderer_, dotInner, iR, theme::ACCENT);
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

            SDL_SetRenderDrawColor(renderer_, theme::WHITE.r, theme::WHITE.g, theme::WHITE.b, 180);
            SDL_RenderDrawRect(renderer_, &thumbRect);

            int tw = 0, th = 0;
            getTextSize(timeStr, 1, &tw, &th);
            int pillW = tw + 8;
            int pillH = th + 4;
            int pillX = thumbRect.x + (thumbW - pillW) / 2;
            int pillY = thumbRect.y + thumbH - pillH - 4;

            SDL_Rect tsBg{pillX, pillY, pillW, pillH};
            fillRoundedRect(renderer_, tsBg, theme::RADIUS_SM, theme::BLACK.a8(200));
            drawText(renderer_, tsBg.x + 4, tsBg.y + 2, timeStr, 1, theme::WHITE);
        } else {
            int tw = 0, th = 0;
            getTextSize(timeStr, 1, &tw, &th);
            int previewW = tw + 8;
            int previewH = th + 4;
            int previewX = std::max(mg, std::min(width - mg - previewW, dotX - previewW / 2));
            int previewY = pbY - previewH - 12;

            SDL_Rect tsBg{previewX, previewY, previewW, previewH};
            fillRoundedRect(renderer_, tsBg, theme::RADIUS_SM, theme::BLACK.a8(200));
            drawText(renderer_, tsBg.x + 4, tsBg.y + 2, timeStr, 1, theme::WHITE);
        }
    }

    // Timestamps — pulled in tight under the bar
    {
        std::string posStr = fmtTime(displayTime);
        int tsY = pbY + pbH + 3;
        drawText(renderer_, mg, tsY, posStr, 1, theme::TEXT_ON);
        if (dur > 0.0) {
            std::string remStr = "-" + fmtTime(dur - displayTime);
            int rw = 0; getTextSize(remStr, 1, &rw, nullptr);
            drawText(renderer_, mg + pbW - rw, tsY, remStr, 1, theme::TEXT_3);
        }
    }

    // ── Description Drawer ─────────────────────────────────────────────────────
    if (app->state_.showDescriptionDrawer) {
        // 52px top bar (+1 hairline) + 52px bottom bar + 6px breathing room.
        SDL_Rect drawerRect{width - 300, 56, 300, height - 114};
        fillRoundedRect(renderer_, drawerRect, 0, theme::BG.a8(240));

        SDL_SetRenderDrawColor(renderer_, theme::CHIP.r, theme::CHIP.g, theme::CHIP.b, 255);
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
            drawTextCentered(renderer_, drawerRect.x + drawerRect.w / 2, drawerRect.y + drawerRect.h / 2, noDesc, 1, theme::TEXT_3);
        } else {
            int startIdx = app->description_scroll_row_;
            int endIdx = std::min(static_cast<int>(descLines.size()), startIdx + visibleLines);
            int drawY = drawerRect.y + 10;
            for (int i = startIdx; i < endIdx; ++i) {
                drawText(renderer_, drawerRect.x + 10, drawY, descLines[i], 1, theme::TEXT);
                drawY += lineH + 4;
            }

            if (descLines.size() > static_cast<size_t>(visibleLines)) {
                int barHeight = drawerRect.h - 20;
                int scrollbarH = static_cast<int>(barHeight * ((double)visibleLines / descLines.size()));
                scrollbarH = std::max(10, scrollbarH);
                int scrollbarY = drawerRect.y + 10 + static_cast<int>((barHeight - scrollbarH) * ((double)app->description_scroll_row_ / maxScroll));
                SDL_Rect scrollbar{drawerRect.x + drawerRect.w - 6, scrollbarY, 4, scrollbarH};
                fillRoundedRect(renderer_, scrollbar, 2, theme::ACCENT.a8(180));
            }
        }
    }

    // Hint pills are baked into hud_static_layer_ (cached), so no per-frame
    // draw here. The cache invalidates on play/pause + drawer-state change.

    if (use_fbo) {
        hud_layer_.end(renderer_);
        hud_layer_.present(renderer_, opacity);
    }
}

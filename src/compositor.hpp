#pragma once
#include <SDL.h>
#include "layer.hpp"
#include <string>

class App;

class Compositor {
public:
    Compositor(SDL_Renderer* renderer) : renderer_(renderer) {}
    void render(App* app, int width, int height);

private:
    void renderBrowseHeader(App* app, int width, int height, const std::string& title, float scrollY, bool searchScreen);
    void renderPlaybackOverlay(App* app, int width, int height);
    void drawDebugOverlay(App* app, int width, int height);
    void drawSignInHelp(App* app, int width, int height);
    // Topmost screen-off confirmation prompt (drawn above the player HUD).
    void drawScreenOffPrompt(App* app, int width, int height);
    // Short fade-from-black drawn over a freshly-appeared video surface.
    void drawVideoFade(App* app, const SDL_Rect& region, int radius);

    SDL_Renderer* renderer_{nullptr};
    
    Layer header_layer_;
    Layer hud_layer_;
    Layer hud_static_layer_;   // cached static HUD decoration (bars + text + hint buttons)
    Layer miniplayer_layer_;

    // Cache invalidation state for the browse header
    int last_header_width_{-1};
    int last_header_height_{-1};
    std::string last_header_query_;
    bool last_header_search_screen_{false};

    // Cache invalidation state for the miniplayer strip
    // The video frame itself is always live (from mpv), but the title strip
    // and hint text only need to redraw when the video or play-state changes.
    std::string last_miniplayer_video_id_;
    bool last_miniplayer_playing_{false};
    bool miniplayer_strip_dirty_{true};

    // Playback HUD string cache.  The truncated title, author, and stats string
    // are all static during playback (they change only when the video or its
    // loaded metadata changes).  Caching them eliminates O(title_len +
    // author_len) getTextSize calls that the per-character shrink loops
    // previously issued every frame.
    std::string  hud_cache_id_;
    int          hud_cache_width_{-1};
    bool         hud_cache_speed_{false};    // speed-badge visible last frame
    long long    hud_cache_views_{-2};       // sentinel: -2 = never populated
    std::string  hud_title_;                 // pre-truncated title
    std::string  hud_author_;                // pre-truncated author
    std::string  hud_stats_;                 // formatted stats line
    int          hud_stats_w_{0};            // pre-measured stats width

    // Static-decoration cache for the playback HUD.  All non-animated parts
    // of the HUD (top/bottom bar backgrounds, title, author, stats, hint
    // buttons, speed badge) are rasterised ONCE into hud_static_layer_ and
    // composited every frame with a single SDL_RenderCopy.  Only the
    // truly dynamic parts (progress bar, timestamps, centre play/pause
    // icon, scrub thumbnail, description drawer) are redrawn per-frame.
    //
    // This is the equivalent of a DRM hardware-overlay HUD plane done at
    // the SDL layer — we get the same "no per-frame GLES decoration cost"
    // win without needing DRM_MASTER arbitration with SDL2's KMSDRM backend.
    bool         hud_static_dirty_{true};
    int          hud_static_w_{-1};
    int          hud_static_h_{-1};
    std::string  hud_static_video_id_;
    bool         hud_static_drawer_open_{false};
    bool         hud_static_playing_{false};
    bool         hud_static_speed_badge_{false};
    long long    hud_static_views_{-2};
};

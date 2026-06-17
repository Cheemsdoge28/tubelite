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
    // Short fade-from-black drawn over a freshly-appeared video surface.
    void drawVideoFade(App* app, const SDL_Rect& region, int radius);

    SDL_Renderer* renderer_{nullptr};
    
    Layer header_layer_;
    Layer hud_layer_;
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
};

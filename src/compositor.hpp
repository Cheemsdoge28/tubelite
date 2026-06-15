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
};

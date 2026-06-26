#pragma once
#include <SDL2/SDL.h>
#include "state.hpp"

class StatusOverlay {
public:
    StatusOverlay() = default;
    ~StatusOverlay() { destroyTexture(); }
    void destroyTexture();
    void render(SDL_Renderer* renderer, const TubeState& state, int width, int height, bool& uiDirty);
private:
    SDL_Texture* texture_{nullptr};
    int width_{0};
    int height_{0};
    TubeState::InputMode last_input_mode_{TubeState::InputMode::None};
    TubeState::Screen last_screen_{TubeState::Screen::Home};
    int last_max_quality_{0};
    bool last_background_daemon_enabled_{false};
    bool last_authed_{false};
    bool last_auth_checked_{false};
};

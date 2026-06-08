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
};

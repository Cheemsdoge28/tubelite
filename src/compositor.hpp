#pragma once
#include <SDL.h>

class App;

class Compositor {
public:
    Compositor(SDL_Renderer* renderer) : renderer_(renderer) {}
    void render(App* app, int width, int height);

private:
    SDL_Renderer* renderer_{nullptr};
};

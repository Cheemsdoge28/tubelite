#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <array>

void drawGlyph(SDL_Renderer* renderer, int x, int y, char ch, int scale, SDL_Color color);
void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);
void drawTextShadow(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);

// Draws an animated spinner. 'time' should be a continuously increasing value (e.g. SDL_GetTicks() / 1000.0f).
void drawSpinner(SDL_Renderer* renderer, int x, int y, int radius, float time);
SDL_Texture* createTargetTexture(SDL_Renderer* renderer, int width, int height);

bool initFonts();
void cleanupFonts();

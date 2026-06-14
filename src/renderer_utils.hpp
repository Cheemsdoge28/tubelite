#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <array>

void drawGlyph(SDL_Renderer* renderer, int x, int y, char ch, int scale, SDL_Color color);
void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);
void drawTextShadow(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);
void drawTextCentered(SDL_Renderer* renderer, int centerX, int y, const std::string& text, int scale, SDL_Color color, bool shadow = false);
std::string utf8Truncate(const std::string& text, size_t maxCodepoints, bool ellipsis = false);
std::string utf8Slice(const std::string& text, size_t startCodepoint, size_t maxCodepoints);
size_t utf8Length(const std::string& text);

void getTextSize(const std::string& text, int scale, int* w, int* h);
void fillRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color);
void drawRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color);

// Draws an animated spinner. 'time' should be a continuously increasing value (e.g. SDL_GetTicks() / 1000.0f).
void drawSpinner(SDL_Renderer* renderer, int x, int y, int radius, float time);
SDL_Texture* createTargetTexture(SDL_Renderer* renderer, int width, int height);

struct GlyphInfo {
    SDL_Rect src_rect;
    int minx, maxx, miny, maxy, advance;
};

struct FontAtlas {
    SDL_Texture* texture = nullptr;
    int tex_width = 0;
    int tex_height = 0;
    std::array<GlyphInfo, 128> glyphs;
    int line_skip  = 0;
    int ascent     = 0;
    int cap_height = 0; // ascent + |descent| — visual line height for centering
};

extern FontAtlas g_atlas_small;
extern FontAtlas g_atlas_medium;
extern FontAtlas g_atlas_large;

bool initFonts(SDL_Renderer* renderer);
void cleanupFonts();

void maskRoundedCorners(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color);
void maskRoundedCornersTop(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color);

#pragma once
#include <SDL2/SDL.h>
#include "theme.hpp"   // unified design system; included after SDL so theme::Rgba gains its SDL_Color cast
#include <string>
#include <array>
#include <vector>

void drawGlyph(SDL_Renderer* renderer, int x, int y, char ch, int scale, SDL_Color color);
void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);
void drawTextShadow(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color);
void drawTextCentered(SDL_Renderer* renderer, int centerX, int y, const std::string& text, int scale, SDL_Color color, bool shadow = false);
std::string utf8Truncate(const std::string& text, size_t maxCodepoints, bool ellipsis = false);
std::string utf8Slice(const std::string& text, size_t startCodepoint, size_t maxCodepoints);
size_t utf8Length(const std::string& text);

void getTextSize(const std::string& text, int scale, int* w, int* h);
std::vector<std::string> wrapText(const std::string& text, int maxWidth, int scale);
std::string formatStatsNumber(long long count);
void fillRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color);
void drawRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color);

// 1 px horizontal rule.  Pass an already-alpha'd colour (e.g.
// theme::HAIRLINE.a8(110)).  Lives here so every panel/modal/overlay can
// share the exact same daemon-style section divider.
void drawHairline(SDL_Renderer* renderer, int x, int y, int w, SDL_Color color);

// Daemon-style status badge: low-alpha tinted fill + matching border +
// bright text in the tint colour.  Used for SIGNED IN / LIVE / etc.
// Returns the drawn rect so callers can position adjacent elements.
SDL_Rect drawStatusPill(SDL_Renderer* renderer, int x, int y,
                        const std::string& label, SDL_Color tint);

// Shared chrome for every modal/panel in the app: diffuse two-pass shadow,
// surface body, top-half lift gradient, 3 px left accent bar + horizontal
// glow, border.  One call replaces ~30 lines of inlined boilerplate at
// each call site.  Pass radius=0 to skip rounded corners.
void drawDaemonCard(SDL_Renderer* renderer, const SDL_Rect& rect,
                    int radius = 8, bool drawShadow = true,
                    bool drawAccentBar = true);

void drawSpinner(SDL_Renderer* renderer, int x, int y, int radius, float time);
SDL_Texture* createTargetTexture(SDL_Renderer* renderer, int width, int height);

struct HintItem {
    std::string button;
    SDL_Color btnColor;
    std::string action;
};

void drawHintButtons(SDL_Renderer* renderer, const std::vector<HintItem>& hints, int y, int height, int scale, int width, SDL_Color panelColor, SDL_Color borderColor, SDL_Color textColor);
void drawVolumeOverlay(SDL_Renderer* renderer, int centerX, int y, int volume, bool muted, SDL_Color themeColor);
void drawSpeedOverlay(SDL_Renderer* renderer, int centerX, int y, double speed, SDL_Color themeColor);
void drawLoadingOverlay(SDL_Renderer* renderer, int width, int height, const std::string& text, float time, SDL_Color textColor, bool drawBg);

struct GlyphInfo {
    SDL_Rect src_rect;     // full atlas cell (for external callers)
    SDL_Rect glyph_src;    // exact pixel rect of this glyph within the atlas texture
    int pen_x_offset;      // draw_x = cursor_x + sg.x_offset + pen_x_offset
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

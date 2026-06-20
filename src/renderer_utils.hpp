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

// Daemon-card chrome.  Renders the shared visual language used by the DRM
// now-playing overlay AND every in-app modal/panel: diffuse two-pass
// shadow, surface fill, top-half lift gradient, left accent bar + glow,
// optional border.  Call once per panel; the caller then draws its own
// content inside the card rect.  Pass radius=0 to skip rounded corners.
void drawDaemonCard(SDL_Renderer* renderer, const SDL_Rect& rect,
                    int radius = 8, bool drawShadow = true,
                    bool drawAccentBar = true);

// 1px horizontal rule helper used everywhere in the daemon-styled UI.
// Pass an already-alpha'd color (e.g. theme::HAIRLINE.a8(110)).
void drawHairline(SDL_Renderer* renderer, int x, int y, int w, SDL_Color color);

// Daemon-style status pill: low-alpha tinted fill + matching border +
// bright text.  Used everywhere a small "LIVE" / "SIGNED IN" / "LOADING"
// affordance appears.  Returns the rect actually drawn so callers can
// position adjacent elements relative to it.
SDL_Rect drawStatusPill(SDL_Renderer* renderer, int x, int y,
                        const std::string& label, SDL_Color tint);

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

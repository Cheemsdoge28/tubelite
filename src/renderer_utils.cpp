#include "renderer_utils.hpp"
#include <cctype>
#include <iostream>
#include <vector>
#include <cmath>
#include <map>

#ifdef _WIN32
#include <SDL_ttf.h>
#else
#include <SDL2/SDL_ttf.h>
#endif

std::array<uint8_t, 7> glyphFor(char ch) {
        switch (static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ch)))) {
        case 'A': return {14, 17, 17, 31, 17, 17, 17};
        case 'B': return {30, 17, 17, 30, 17, 17, 30};
        case 'C': return {14, 17, 16, 16, 16, 17, 14};
        case 'D': return {30, 17, 17, 17, 17, 17, 30};
        case 'E': return {31, 16, 16, 30, 16, 16, 31};
        case 'F': return {31, 16, 16, 30, 16, 16, 16};
        case 'G': return {14, 17, 16, 23, 17, 17, 15};
        case 'H': return {17, 17, 17, 31, 17, 17, 17};
        case 'I': return {31, 4, 4, 4, 4, 4, 31};
        case 'J': return {7, 2, 2, 2, 18, 18, 12};
        case 'K': return {17, 18, 20, 24, 20, 18, 17};
        case 'L': return {16, 16, 16, 16, 16, 16, 31};
        case 'M': return {17, 27, 21, 17, 17, 17, 17};
        case 'N': return {17, 25, 21, 19, 17, 17, 17};
        case 'O': return {14, 17, 17, 17, 17, 17, 14};
        case 'P': return {30, 17, 17, 30, 16, 16, 16};
        case 'Q': return {14, 17, 17, 17, 21, 18, 13};
        case 'R': return {30, 17, 17, 30, 20, 18, 17};
        case 'S': return {15, 16, 16, 14, 1, 1, 30};
        case 'T': return {31, 4, 4, 4, 4, 4, 4};
        case 'U': return {17, 17, 17, 17, 17, 17, 14};
        case 'V': return {17, 17, 17, 17, 17, 10, 4};
        case 'W': return {17, 17, 17, 17, 21, 21, 10};
        case 'X': return {17, 17, 10, 4, 10, 17, 17};
        case 'Y': return {17, 17, 10, 4, 4, 4, 4};
        case 'Z': return {31, 1, 2, 4, 8, 16, 31};
        case '0': return {14, 17, 19, 21, 25, 17, 14};
        case '1': return {4, 12, 4, 4, 4, 4, 14};
        case '2': return {14, 17, 1, 2, 4, 8, 31};
        case '3': return {30, 1, 1, 6, 1, 1, 30};
        case '4': return {2, 6, 10, 18, 31, 2, 2};
        case '5': return {31, 16, 16, 30, 1, 1, 30};
        case '6': return {14, 16, 16, 30, 17, 17, 14};
        case '7': return {31, 1, 2, 4, 8, 8, 8};
        case '8': return {14, 17, 17, 14, 17, 17, 14};
        case '9': return {14, 17, 17, 15, 1, 1, 14};
        case '-': return {0, 0, 0, 31, 0, 0, 0};
        case '.': return {0, 0, 0, 0, 0, 6, 6};
        case '/': return {1, 2, 2, 4, 8, 8, 16};
        case ':': return {0, 6, 6, 0, 6, 6, 0};
        case '_': return {0, 0, 0, 0, 0, 0, 31};
        case '?': return {14, 17, 1, 2, 4, 0, 4};
        case '@': return {14, 17, 1, 13, 21, 21, 14};
        case '&': return {12, 18, 20, 8, 21, 18, 13};
        case '=': return {0, 31, 0, 31, 0, 0, 0};
        case '+': return {0, 4, 4, 31, 4, 4, 0};
        case '#': return {10, 10, 31, 10, 31, 10, 10};
        case '%': return {24, 25, 2, 4, 8, 19, 3};
        case '!': return {4, 4, 4, 4, 4, 0, 4};
        case '$': return {4, 15, 20, 14, 5, 30, 4};
        case '^': return {4, 10, 17, 0, 0, 0, 0};
        case '*': return {0, 17, 10, 31, 10, 17, 0};
        case '(': return {2, 4, 8, 8, 8, 4, 2};
        case ')': return {8, 4, 2, 2, 2, 4, 8};
        case '[': return {14, 8, 8, 8, 8, 8, 14};
        case ']': return {14, 2, 2, 2, 2, 2, 14};
        case '{': return {2, 4, 4, 8, 4, 4, 2};
        case '}': return {8, 4, 4, 2, 4, 4, 8};
        case '<': return {2, 4, 8, 16, 8, 4, 2};
        case '>': return {8, 4, 2, 1, 2, 4, 8};
        case ';': return {0, 4, 4, 0, 4, 4, 8};
        case '\'': return {4, 4, 2, 0, 0, 0, 0};
        case '"': return {10, 10, 4, 0, 0, 0, 0};
        case '\\': return {16, 8, 8, 4, 2, 2, 1};
        case '|': return {4, 4, 4, 4, 4, 4, 4};
        case '~': return {0, 0, 13, 18, 0, 0, 0};
        case '`': return {8, 4, 2, 0, 0, 0, 0};
        case ',': return {0, 0, 0, 0, 0, 4, 8};
        case ' ': return {0, 0, 0, 0, 0, 0, 0};
        default:  return {0, 0, 0, 0, 0, 0, 0};
        }
    }



void drawGlyph(SDL_Renderer* renderer, int x, int y, char ch, int scale, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const auto glyph = glyphFor(ch);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((glyph[static_cast<size_t>(row)] >> (4 - col)) & 1U) {
                SDL_Rect pixel{x + col * scale, y + row * scale, scale, scale};
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
    }
}

static TTF_Font* g_font_small = nullptr;
static TTF_Font* g_font_medium = nullptr;
static TTF_Font* g_font_large = nullptr;

static bool isUtf8ContinuationByte(unsigned char ch) {
    return (ch & 0xC0U) == 0x80U;
}

size_t utf8Length(const std::string& text) {
    size_t count = 0;
    for (unsigned char ch : text) {
        if (!isUtf8ContinuationByte(ch)) {
            ++count;
        }
    }
    return count;
}

std::string utf8Slice(const std::string& text, size_t startCodepoint, size_t maxCodepoints) {
    if (text.empty() || maxCodepoints == 0) return "";

    size_t byteStart = std::string::npos;
    size_t byteEnd = text.size();
    size_t codepoint = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (isUtf8ContinuationByte(ch)) continue;

        if (codepoint == startCodepoint) {
            byteStart = i;
        }
        if (codepoint == startCodepoint + maxCodepoints) {
            byteEnd = i;
            break;
        }
        ++codepoint;
    }

    if (byteStart == std::string::npos) {
        return "";
    }
    return text.substr(byteStart, byteEnd - byteStart);
}

std::string utf8Truncate(const std::string& text, size_t maxCodepoints, bool ellipsis) {
    if (utf8Length(text) <= maxCodepoints) {
        return text;
    }
    if (maxCodepoints == 0) {
        return "";
    }
    const size_t sliceCount = ellipsis && maxCodepoints > 3 ? maxCodepoints - 3 : maxCodepoints;
    std::string clipped = utf8Slice(text, 0, sliceCount);
    if (ellipsis && maxCodepoints > 3) {
        clipped += "...";
    }
    return clipped;
}

FontAtlas g_atlas_small;
FontAtlas g_atlas_medium;
FontAtlas g_atlas_large;

SDL_Texture* g_corner_mask_texture = nullptr;
SDL_Texture* g_solid_corner_texture = nullptr;

static FontAtlas createFontAtlas(SDL_Renderer* renderer, TTF_Font* font) {
    FontAtlas atlas;

    // cell_h: use line_skip so every glyph (including deep descenders) fits without clipping.
    int cell_h  = TTF_FontLineSkip(font);

    // Measure the widest advance in the printable ASCII range.
    int max_adv = 0;
    for (int ch = 32; ch < 127; ++ch) {
        int adv = 0;
        TTF_GlyphMetrics(font, ch, nullptr, nullptr, nullptr, nullptr, &adv);
        if (adv > max_adv) max_adv = adv;
    }
    // Also consider the rendered glyph pixel width (e.g. italic overhang).
    int cell_w = max_adv + 4; // +4px safety margin

    // Pack 96 printable ASCII chars (32..126) into a square-ish atlas.
    int cols = 16;
    int rows = 6;  // ceil(95 / 16) = 6

    SDL_Surface* atlas_surface = SDL_CreateRGBSurfaceWithFormat(
        0, cols * cell_w, rows * cell_h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!atlas_surface) return atlas;

    SDL_FillRect(atlas_surface, nullptr, 0x00000000);

    atlas.line_skip = TTF_FontLineSkip(font);
    atlas.ascent    = TTF_FontAscent(font);
    // cap_height = ascent + |descent|, used for centering single lines of text.
    atlas.cap_height = atlas.ascent + (-TTF_FontDescent(font));

    for (int ch = 32; ch < 127; ++ch) {
        int idx = ch - 32;
        int col = idx % cols;
        int row = idx / cols;

        int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
        TTF_GlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &advance);

        SDL_Surface* glyph_surf = TTF_RenderGlyph_Blended(font, ch, {255, 255, 255, 255});
        if (glyph_surf) {
            // Clamp blit to stay within cell bounds.
            int blit_w = std::min(glyph_surf->w, cell_w);
            int blit_h = std::min(glyph_surf->h, cell_h);
            SDL_Rect src_clip{0, 0, blit_w, blit_h};
            SDL_Rect dst{col * cell_w, row * cell_h, blit_w, blit_h};

            SDL_BlendMode prev_mode;
            SDL_GetSurfaceBlendMode(glyph_surf, &prev_mode);
            SDL_SetSurfaceBlendMode(glyph_surf, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(glyph_surf, &src_clip, atlas_surface, &dst);
            SDL_SetSurfaceBlendMode(glyph_surf, prev_mode);

            GlyphInfo& info = atlas.glyphs[ch];
            // src_rect uses clamped size so we never read past cell boundary.
            info.src_rect = {col * cell_w, row * cell_h, blit_w, blit_h};
            info.minx  = minx;
            info.maxx  = maxx;
            info.miny  = miny;
            info.maxy  = maxy;
            info.advance = advance;

            SDL_FreeSurface(glyph_surf);
        } else {
            GlyphInfo& info = atlas.glyphs[ch];
            info.src_rect = {col * cell_w, row * cell_h, 0, 0};
            info.minx = info.maxx = info.miny = info.maxy = 0;
            info.advance = advance > 0 ? advance : cell_w / 4;
        }
    }

    atlas.texture    = SDL_CreateTextureFromSurface(renderer, atlas_surface);
    atlas.tex_width  = atlas_surface->w;
    atlas.tex_height = atlas_surface->h;

    SDL_FreeSurface(atlas_surface);
    return atlas;
}

static void initCornerMask(SDL_Renderer* renderer) {
    int r = 8;
    int size = r * 2;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return;
    
    SDL_FillRect(surf, nullptr, 0x00000000);
    
    uint32_t* pixels = static_cast<uint32_t*>(surf->pixels);
    uint32_t mask_color = SDL_MapRGBA(surf->format, 15, 15, 15, 255);
    
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double dx = x - (r - 0.5);
            double dy = y - (r - 0.5);
            if (dx * dx + dy * dy > r * r) {
                pixels[y * size + x] = mask_color;
            }
        }
    }
    
    g_corner_mask_texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
}

static void initSolidCorner(SDL_Renderer* renderer) {
    int r = 8;
    int size = r * 2;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return;
    
    SDL_FillRect(surf, nullptr, 0x00000000);
    
    uint32_t* pixels = static_cast<uint32_t*>(surf->pixels);
    uint32_t white = SDL_MapRGBA(surf->format, 255, 255, 255, 255);
    
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double dx = x - (r - 0.5);
            double dy = y - (r - 0.5);
            if (dx * dx + dy * dy <= r * r) {
                pixels[y * size + x] = white;
            }
        }
    }
    
    g_solid_corner_texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
}

bool initFonts(SDL_Renderer* renderer) {
    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
        return false;
    }
    
    std::vector<std::string> regPaths = {
        "res/fonts/AtkinsonHyperlegible-Regular.ttf",
        "../res/fonts/AtkinsonHyperlegible-Regular.ttf",
        "/roms/tools/tubelite/res/fonts/AtkinsonHyperlegible-Regular.ttf",
        "AtkinsonHyperlegible-Regular.ttf"
    };
    std::vector<std::string> boldPaths = {
        "res/fonts/AtkinsonHyperlegible-Bold.ttf",
        "../res/fonts/AtkinsonHyperlegible-Bold.ttf",
        "/roms/tools/tubelite/res/fonts/AtkinsonHyperlegible-Bold.ttf",
        "AtkinsonHyperlegible-Bold.ttf"
    };
    
    std::string regPath = "";
    for (const auto& p : regPaths) {
        if (FILE* f = fopen(p.c_str(), "rb")) {
            fclose(f);
            regPath = p;
            break;
        }
    }
    
    std::string boldPath = "";
    for (const auto& p : boldPaths) {
        if (FILE* f = fopen(p.c_str(), "rb")) {
            fclose(f);
            boldPath = p;
            break;
        }
    }
    
    if (regPath.empty() || boldPath.empty()) {
        std::cerr << "Could not find Atkinson Hyperlegible font files, falling back to pixel font." << std::endl;
        return false;
    }
    
    g_font_small = TTF_OpenFont(regPath.c_str(), 14);
    g_font_medium = TTF_OpenFont(regPath.c_str(), 18);
    g_font_large = TTF_OpenFont(boldPath.c_str(), 24);
    
    if (!g_font_small || !g_font_medium || !g_font_large) {
        std::cerr << "TTF_OpenFont failed: " << TTF_GetError() << std::endl;
        return false;
    }
    
    g_atlas_small = createFontAtlas(renderer, g_font_small);
    g_atlas_medium = createFontAtlas(renderer, g_font_medium);
    g_atlas_large = createFontAtlas(renderer, g_font_large);
    
    initCornerMask(renderer);
    initSolidCorner(renderer);
    
    return true;
}

struct TextCacheKey {
    std::string text;
    int scale;
    uint32_t color_rgba;

    bool operator<(const TextCacheKey& o) const {
        if (scale != o.scale) return scale < o.scale;
        if (color_rgba != o.color_rgba) return color_rgba < o.color_rgba;
        return text < o.text;
    }
};

struct CachedText {
    SDL_Texture* texture;
    int w;
    int h;
    uint32_t last_used;
};

static std::map<TextCacheKey, CachedText> g_text_cache;
static uint32_t g_last_prune_time = 0;

void pruneTextCache(uint32_t now) {
    g_last_prune_time = now;
    for (auto it = g_text_cache.begin(); it != g_text_cache.end(); ) {
        if (now - it->second.last_used > 5000) {
            if (it->second.texture) {
                SDL_DestroyTexture(it->second.texture);
            }
            it = g_text_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void clearTextCache() {
    for (auto& pair : g_text_cache) {
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
        }
    }
    g_text_cache.clear();
}

void cleanupFonts() {
    clearTextCache();
    if (g_atlas_small.texture)  { SDL_DestroyTexture(g_atlas_small.texture);  g_atlas_small.texture = nullptr; }
    if (g_atlas_medium.texture) { SDL_DestroyTexture(g_atlas_medium.texture); g_atlas_medium.texture = nullptr; }
    if (g_atlas_large.texture)  { SDL_DestroyTexture(g_atlas_large.texture);  g_atlas_large.texture = nullptr; }
    if (g_corner_mask_texture)  { SDL_DestroyTexture(g_corner_mask_texture);  g_corner_mask_texture = nullptr; }
    if (g_solid_corner_texture) { SDL_DestroyTexture(g_solid_corner_texture); g_solid_corner_texture = nullptr; }
    if (g_font_small)  { TTF_CloseFont(g_font_small);  g_font_small = nullptr; }
    if (g_font_medium) { TTF_CloseFont(g_font_medium); g_font_medium = nullptr; }
    if (g_font_large)  { TTF_CloseFont(g_font_large);  g_font_large = nullptr; }
    TTF_Quit();
}

void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {
    if (text.empty()) return;
    
    const FontAtlas* atlas = nullptr;
    if (scale <= 1)      atlas = &g_atlas_small;
    else if (scale == 2) atlas = &g_atlas_medium;
    else                 atlas = &g_atlas_large;
    
    if (atlas && atlas->texture) {
        SDL_SetTextureColorMod(atlas->texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(atlas->texture, color.a);
        SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND);
        
        int cursor_x = x;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char ch = text[i];
            if (ch >= 32 && ch < 127) {
                const GlyphInfo& info = atlas->glyphs[ch];
                if (info.src_rect.w > 0) {
                    // Align the glyph's top to the shared font baseline
                    int draw_y = y + (atlas->ascent - info.maxy);
                    int draw_x = cursor_x + info.minx;
                    SDL_Rect dst{draw_x, draw_y, info.src_rect.w, info.src_rect.h};
                    SDL_RenderCopy(renderer, atlas->texture, &info.src_rect, &dst);
                }
                cursor_x += info.advance;
                i += 1;
            } else {
                if ((ch & 0x80) == 0) i += 1;
                else if ((ch & 0xE0) == 0xC0) i += 2;
                else if ((ch & 0xF0) == 0xE0) i += 3;
                else if ((ch & 0xF8) == 0xF0) i += 4;
                else i += 1;
            }
        }
        return;
    }
    
    // Fallback to custom pixel font if TTF is not available
    int cursor = x;
    for (char ch : text) {
        drawGlyph(renderer, cursor, y, ch, scale, color);
        cursor += scale * 6;
    }
}

void drawTextShadow(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {
    const int offset = std::max(1, scale / 2);
    drawText(renderer, x - offset, y + offset, text, scale, {0, 0, 0, color.a});
    drawText(renderer, x, y, text, scale, color);
}

void drawSpinner(SDL_Renderer* renderer, int x, int y, int radius, float time) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    struct Point2D { float x; float y; };
    static const Point2D directions[8] = {
        {1.0f, 0.0f}, {0.7071f, 0.7071f}, {0.0f, 1.0f}, {-0.7071f, 0.7071f},
        {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f}
    };
    int tick = static_cast<int>(time * 8.0f);
    for (int i = 0; i < 8; ++i) {
        float dotX = x + directions[i].x * radius;
        float dotY = y + directions[i].y * radius;
        
        int index = (8 - tick + i) % 8;
        if (index < 0) index += 8;
        int alpha = 255 - (index * 28);
        if (alpha < 30) alpha = 30;
        
        SDL_SetRenderDrawColor(renderer, 255, 60, 60, alpha);
        SDL_Rect r{static_cast<int>(dotX) - 3, static_cast<int>(dotY) - 3, 6, 6};
        SDL_RenderFillRect(renderer, &r);
    }
}

SDL_Texture* createTargetTexture(SDL_Renderer* renderer, int width, int height) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (texture == nullptr)
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (texture != nullptr) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

void getTextSize(const std::string& text, int scale, int* w, int* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (text.empty()) return;
    
    const FontAtlas* atlas = nullptr;
    if (scale <= 1)      atlas = &g_atlas_small;
    else if (scale == 2) atlas = &g_atlas_medium;
    else                 atlas = &g_atlas_large;
    
    if (atlas && atlas->texture) {
        int width = 0;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char ch = text[i];
            if (ch >= 32 && ch < 127) {
                width += atlas->glyphs[ch].advance;
                i += 1;
            } else {
                if ((ch & 0x80) == 0) i += 1;
                else if ((ch & 0xE0) == 0xC0) i += 2;
                else if ((ch & 0xF0) == 0xE0) i += 3;
                else if ((ch & 0xF8) == 0xF0) i += 4;
                else i += 1;
            }
        }
        if (w) *w = width;
        // Return cap_height (ascent + |descent|) instead of line_skip so callers
        // can vertically-centre text without the extra external leading.
        if (h) *h = (atlas->cap_height > 0) ? atlas->cap_height : atlas->line_skip;
        return;
    }
    
    if (w) *w = static_cast<int>(text.length()) * 6 * scale;
    if (h) *h = 7 * scale;
}

void fillRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    if (radius <= 0 || !g_solid_corner_texture) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }
    
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_SetTextureColorMod(g_solid_corner_texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(g_solid_corner_texture, color.a);
    
    int r = radius;
    SDL_Rect middleRect{rect.x + r, rect.y, rect.w - 2 * r, rect.h};
    SDL_Rect sideRect{rect.x, rect.y + r, rect.w, rect.h - 2 * r};
    SDL_RenderFillRect(renderer, &middleRect);
    SDL_RenderFillRect(renderer, &sideRect);
    
    SDL_Rect srcTL{0, 0, r, r};
    SDL_Rect dstTL{rect.x, rect.y, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcTL, &dstTL);
    
    SDL_Rect srcTR{r, 0, r, r};
    SDL_Rect dstTR{rect.x + rect.w - r, rect.y, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcTR, &dstTR);
    
    SDL_Rect srcBL{0, r, r, r};
    SDL_Rect dstBL{rect.x, rect.y + rect.h - r, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcBL, &dstBL);
    
    SDL_Rect srcBR{r, r, r, r};
    SDL_Rect dstBR{rect.x + rect.w - r, rect.y + rect.h - r, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcBR, &dstBR);
}

void drawRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    if (radius <= 0) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderDrawRect(renderer, &rect);
        return;
    }
    
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    
    int right = rect.x + rect.w - 1;
    int bottom = rect.y + rect.h - 1;
    
    SDL_RenderDrawLine(renderer, rect.x + radius, rect.y, right - radius, rect.y);
    SDL_RenderDrawLine(renderer, rect.x + radius, bottom, right - radius, bottom);
    SDL_RenderDrawLine(renderer, rect.x, rect.y + radius, rect.x, bottom - radius);
    SDL_RenderDrawLine(renderer, right, rect.y + radius, right, bottom - radius);
    
    int x = 0;
    int y = radius;
    int d = 3 - 2 * radius;
    
    auto drawPoints = [&](int cx, int cy, int corner) {
        if (corner == 0) {
            SDL_RenderDrawPoint(renderer, cx - x, cy - y);
            SDL_RenderDrawPoint(renderer, cx - y, cy - x);
        } else if (corner == 1) {
            SDL_RenderDrawPoint(renderer, cx + x, cy - y);
            SDL_RenderDrawPoint(renderer, cx + y, cy - x);
        } else if (corner == 2) {
            SDL_RenderDrawPoint(renderer, cx - x, cy + y);
            SDL_RenderDrawPoint(renderer, cx - y, cy + x);
        } else if (corner == 3) {
            SDL_RenderDrawPoint(renderer, cx + x, cy + y);
            SDL_RenderDrawPoint(renderer, cx + y, cy + x);
        }
    };
    
    while (y >= x) {
        drawPoints(rect.x + radius, rect.y + radius, 0);
        drawPoints(right - radius, rect.y + radius, 1);
        drawPoints(rect.x + radius, bottom - radius, 2);
        drawPoints(right - radius, bottom - radius, 3);
        
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

void maskRoundedCorners(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    if (radius <= 0 || !g_corner_mask_texture) return;
    
    SDL_SetTextureColorMod(g_corner_mask_texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(g_corner_mask_texture, color.a);
    SDL_SetTextureBlendMode(g_corner_mask_texture, SDL_BLENDMODE_BLEND);
    
    int r_base = 8;
    int r = radius;
    
    SDL_Rect srcTL{0, 0, r_base, r_base};
    SDL_Rect dstTL{rect.x, rect.y, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcTL, &dstTL);
    
    SDL_Rect srcTR{r_base, 0, r_base, r_base};
    SDL_Rect dstTR{rect.x + rect.w - r, rect.y, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcTR, &dstTR);
    
    SDL_Rect srcBL{0, r_base, r_base, r_base};
    SDL_Rect dstBL{rect.x, rect.y + rect.h - r, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcBL, &dstBL);
    
    SDL_Rect srcBR{r_base, r_base, r_base, r_base};
    SDL_Rect dstBR{rect.x + rect.w - r, rect.y + rect.h - r, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcBR, &dstBR);
}

void maskRoundedCornersTop(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    if (radius <= 0 || !g_corner_mask_texture) return;
    
    SDL_SetTextureColorMod(g_corner_mask_texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(g_corner_mask_texture, color.a);
    SDL_SetTextureBlendMode(g_corner_mask_texture, SDL_BLENDMODE_BLEND);
    
    int r_base = 8;
    int r = radius;
    
    SDL_Rect srcTL{0, 0, r_base, r_base};
    SDL_Rect dstTL{rect.x, rect.y, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcTL, &dstTL);
    
    SDL_Rect srcTR{r_base, 0, r_base, r_base};
    SDL_Rect dstTR{rect.x + rect.w - r, rect.y, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcTR, &dstTR);
}

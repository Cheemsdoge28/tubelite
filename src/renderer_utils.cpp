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

bool initFonts() {
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
    
    // Loaded sizes optimized for 640x480 screen
    g_font_small = TTF_OpenFont(regPath.c_str(), 14);
    g_font_medium = TTF_OpenFont(regPath.c_str(), 18);
    g_font_large = TTF_OpenFont(boldPath.c_str(), 24);
    
    if (!g_font_small || !g_font_medium || !g_font_large) {
        std::cerr << "TTF_OpenFont failed: " << TTF_GetError() << std::endl;
        return false;
    }
    
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
    if (g_font_small)  { TTF_CloseFont(g_font_small);  g_font_small = nullptr; }
    if (g_font_medium) { TTF_CloseFont(g_font_medium); g_font_medium = nullptr; }
    if (g_font_large)  { TTF_CloseFont(g_font_large);  g_font_large = nullptr; }
    TTF_Quit();
}

void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {
    if (text.empty()) return;
    
    TTF_Font* font = nullptr;
    if (scale <= 1)      font = g_font_small;
    else if (scale == 2) font = g_font_medium;
    else                 font = g_font_large;
    
    if (font) {
        uint32_t color_rgba = (static_cast<uint32_t>(color.r) << 24) |
                              (static_cast<uint32_t>(color.g) << 16) |
                              (static_cast<uint32_t>(color.b) << 8) |
                              color.a;
        TextCacheKey key{text, scale, color_rgba};
        uint32_t now = SDL_GetTicks();
        
        auto it = g_text_cache.find(key);
        if (it != g_text_cache.end()) {
            it->second.last_used = now;
            SDL_Rect dst{x, y, it->second.w, it->second.h};
            SDL_RenderCopy(renderer, it->second.texture, nullptr, &dst);
            
            if (now - g_last_prune_time > 5000) {
                pruneTextCache(now);
            }
            return;
        }
        
        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (surface) {
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
            if (texture) {
                int w = surface->w;
                int h = surface->h;
                SDL_Rect dst{x, y, w, h};
                SDL_RenderCopy(renderer, texture, nullptr, &dst);
                
                CachedText cached{texture, w, h, now};
                g_text_cache[key] = cached;
            }
            SDL_FreeSurface(surface);
            
            if (now - g_last_prune_time > 5000) {
                pruneTextCache(now);
            }
            return;
        }
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
    
    TTF_Font* font = nullptr;
    if (scale <= 1)      font = g_font_small;
    else if (scale == 2) font = g_font_medium;
    else                 font = g_font_large;
    
    if (font) {
        TTF_SizeUTF8(font, text.c_str(), w, h);
        return;
    }
    
    if (w) *w = static_cast<int>(text.length()) * 6 * scale;
    if (h) *h = 7 * scale;
}

void fillRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    if (radius <= 0) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }
    
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    
    int right = rect.x + rect.w - 1;
    int bottom = rect.y + rect.h - 1;
    
    SDL_Rect middleRect{rect.x + radius, rect.y, rect.w - 2 * radius, rect.h};
    SDL_Rect leftRect{rect.x, rect.y + radius, radius, rect.h - 2 * radius};
    SDL_Rect rightRect{right - radius + 1, rect.y + radius, radius, rect.h - 2 * radius};
    
    SDL_RenderFillRect(renderer, &middleRect);
    SDL_RenderFillRect(renderer, &leftRect);
    SDL_RenderFillRect(renderer, &rightRect);
    
    auto drawCornerHelper = [&](int cx, int cy, int corner) {
        for (int w = 0; w < radius; w++) {
            int h = static_cast<int>(std::sqrt(radius * radius - w * w));
            if (corner == 0) {
                SDL_RenderDrawLine(renderer, cx - w, cy - h, cx - w, cy);
            } else if (corner == 1) {
                SDL_RenderDrawLine(renderer, cx + w, cy - h, cx + w, cy);
            } else if (corner == 2) {
                SDL_RenderDrawLine(renderer, cx - w, cy, cx - w, cy + h);
            } else if (corner == 3) {
                SDL_RenderDrawLine(renderer, cx + w, cy, cx + w, cy + h);
            }
        }
    };
    
    drawCornerHelper(rect.x + radius, rect.y + radius, 0);
    drawCornerHelper(right - radius, rect.y + radius, 1);
    drawCornerHelper(rect.x + radius, bottom - radius, 2);
    drawCornerHelper(right - radius, bottom - radius, 3);
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
    if (radius <= 0) return;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    
    int right = rect.x + rect.w - 1;
    int bottom = rect.y + rect.h - 1;
    
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            int dx = radius - x;
            int dy = radius - y;
            if (dx * dx + dy * dy > radius * radius) {
                // Top-left
                SDL_RenderDrawPoint(renderer, rect.x + x, rect.y + y);
                // Top-right
                SDL_RenderDrawPoint(renderer, right - x, rect.y + y);
                // Bottom-left
                SDL_RenderDrawPoint(renderer, rect.x + x, bottom - y);
                // Bottom-right
                SDL_RenderDrawPoint(renderer, right - x, bottom - y);
            }
        }
    }
}

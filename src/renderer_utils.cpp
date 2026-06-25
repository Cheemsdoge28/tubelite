#include "renderer_utils.hpp"
#include <cctype>
#include <iostream>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <SDL_ttf.h>
#else
#include <SDL2/SDL_ttf.h>
#endif

#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

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

SDL_Texture* g_corner_mask_texture  = nullptr;
SDL_Texture* g_solid_corner_texture = nullptr;
SDL_Texture* g_ring_corner_texture  = nullptr;   // outline-only ring; used by drawRoundedRect

static FontAtlas createFontAtlas(SDL_Renderer* renderer, TTF_Font* font) {
    FontAtlas atlas;

    // cell_h: use line height + 8 padding so every glyph fits without clipping.
    int cell_h  = TTF_FontHeight(font) + 8;

    // Measure the widest advance in the printable ASCII range.
    int max_adv = 0;
    for (int ch = 32; ch < 127; ++ch) {
        int adv = 0;
        TTF_GlyphMetrics(font, ch, nullptr, nullptr, nullptr, nullptr, &adv);
        if (adv > max_adv) max_adv = adv;
    }
    // Also consider the rendered glyph pixel width (e.g. italic overhang).
    int cell_w = max_adv + 8; // +8px safety margin

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
            // The surface returned by TTF_RenderGlyph_Blended already has the glyph
            // vertically aligned/shifted relative to the baseline inside a surface
            // of height TTF_FontHeight. We blit it directly to the cell top.
            int dst_x = col * cell_w + 4 + minx;
            int dst_y = row * cell_h;

            int src_x = 0;
            int src_y = 0;
            int blit_w = glyph_surf->w;
            int blit_h = glyph_surf->h;

            // Clip boundaries to stay strictly within the current cell
            if (dst_x < col * cell_w) {
                int diff = (col * cell_w) - dst_x;
                src_x += diff;
                dst_x += diff;
                blit_w -= diff;
            }
            if (dst_x + blit_w > (col + 1) * cell_w) {
                blit_w = ((col + 1) * cell_w) - dst_x;
            }
            if (dst_y < row * cell_h) {
                int diff = (row * cell_h) - dst_y;
                src_y += diff;
                dst_y += diff;
                blit_h -= diff;
            }
            if (dst_y + blit_h > (row + 1) * cell_h) {
                blit_h = ((row + 1) * cell_h) - dst_y;
            }

            SDL_Rect src_clip{src_x, src_y, std::max(0, blit_w), std::max(0, blit_h)};
            SDL_Rect dst{dst_x, dst_y, std::max(0, blit_w), std::max(0, blit_h)};

            SDL_BlendMode prev_mode;
            SDL_GetSurfaceBlendMode(glyph_surf, &prev_mode);
            SDL_SetSurfaceBlendMode(glyph_surf, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(glyph_surf, &src_clip, atlas_surface, &dst);
            SDL_SetSurfaceBlendMode(glyph_surf, prev_mode);

            GlyphInfo& info = atlas.glyphs[ch];
            info.src_rect = {col * cell_w, row * cell_h, cell_w, cell_h};
            info.minx  = minx;
            info.maxx  = maxx;
            info.miny  = miny;
            info.maxy  = maxy;
            info.advance = advance;
            // Exact glyph pixel rect in the atlas.  pen_x_offset encodes the
            // correct screen-x adjustment even when left-edge clipping occurred.
            info.glyph_src    = {dst_x, dst_y, std::max(0, blit_w), std::max(0, blit_h)};
            info.pen_x_offset = minx + src_x; // src_x > 0 only when clipped

            SDL_FreeSurface(glyph_surf);
        } else {
            GlyphInfo& info = atlas.glyphs[ch];
            info.src_rect = {col * cell_w, row * cell_h, cell_w, cell_h};
            info.minx = info.maxx = info.miny = info.maxy = 0;
            info.advance = advance > 0 ? advance : cell_w / 4;
            info.glyph_src    = {0, 0, 0, 0};
            info.pen_x_offset = 0;
        }
    }

    atlas.texture    = SDL_CreateTextureFromSurface(renderer, atlas_surface);
    atlas.tex_width  = atlas_surface->w;
    atlas.tex_height = atlas_surface->h;

    SDL_FreeSurface(atlas_surface);
    return atlas;
}

static void initCornerMask(SDL_Renderer* renderer) {
    int r = 64;
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
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist > r + 0.5) {
                pixels[y * size + x] = white;
            } else if (dist >= r - 0.5) {
                double alpha = (dist - (r - 0.5)); // 0.0 to 1.0 transition
                uint8_t a = static_cast<uint8_t>(alpha * 255.0);
                pixels[y * size + x] = SDL_MapRGBA(surf->format, 255, 255, 255, a);
            }
        }
    }
    
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_corner_mask_texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_FreeSurface(surf);
}

static void initSolidCorner(SDL_Renderer* renderer) {
    int r = 64;
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
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= r - 0.5) {
                pixels[y * size + x] = white;
            } else if (dist < r + 0.5) {
                double alpha = (r + 0.5 - dist); // 0.0 to 1.0 transition
                uint8_t a = static_cast<uint8_t>(alpha * 255.0);
                pixels[y * size + x] = SDL_MapRGBA(surf->format, 255, 255, 255, a);
            }
        }
    }
    
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_solid_corner_texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_FreeSurface(surf);
}

// Outline-only quarter-circle ring used by drawRoundedRect.  Source is 128x128
// containing one full antialiased ring centred at (63.5, 63.5); drawRoundedRect
// samples each 64x64 quadrant for the four corners.
//
// Native ring thickness ~3px so that a typical card-border radius (8-12) scales
// it to ~0.4-0.6 px — bilinear filtering keeps the line visually crisp at 1 px
// without blooming.  At larger radii (24-32) the line grows to ~1-1.5 px,
// which still reads as a clean outline.
//
// Previously drawRoundedRect drew corners with a midpoint-circle loop calling
// SDL_RenderDrawPoint per pixel — ~96 calls per corner pass, and renderFocusRing
// stacks 3 of those, giving ~280 SDL ops per frame just for the focus ring.
// The profiler measured 16.7 ms on the RG351MP.  Routing through this single
// texture collapses it to 4 SDL_RenderCopy per drawRoundedRect call.
static void initRingCorner(SDL_Renderer* renderer) {
    int r = 64;
    int size = r * 2;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return;

    SDL_FillRect(surf, nullptr, 0x00000000);

    uint32_t* pixels = static_cast<uint32_t*>(surf->pixels);

    const double thickness = 3.0;
    const double inner = (double)r - thickness * 0.5;
    const double outer = (double)r + thickness * 0.5;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double dx = x - (r - 0.5);
            double dy = y - (r - 0.5);
            double dist = std::sqrt(dx * dx + dy * dy);

            // Coverage = clamp(min(dist - (inner - 0.5), (outer + 0.5) - dist), 0, 1)
            // → smooth 1px-AA edges on both sides of the ring band.
            double cov = std::min(dist - (inner - 0.5), (outer + 0.5) - dist);
            if (cov <= 0.0) continue;
            if (cov > 1.0) cov = 1.0;

            uint8_t a = static_cast<uint8_t>(cov * 255.0);
            pixels[y * size + x] = SDL_MapRGBA(surf->format, 255, 255, 255, a);
        }
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_ring_corner_texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_FreeSurface(surf);
}

static FT_Library g_ft_library = nullptr;
static FT_Face g_ft_faces[8] = {nullptr}; // [0..5] primary+fallback per size, [6..7] emoji small/medium
static hb_font_t* g_hb_fonts[8] = {nullptr};

struct CachedGlyph {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    int bearing_x = 0;
    int bearing_y = 0;
    int advance = 0;
};

static std::unordered_map<uint64_t, CachedGlyph> g_glyph_cache;

// Maps FreeType glyph_id → ASCII char code for each primary face slot (0, 2, 4).
// Built at font-init time so drawTextDirect can look up atlas glyphs by HarfBuzz
// glyph_id without touching FreeType per-frame.
static std::unordered_map<uint32_t, unsigned int> g_atlas_glyph_map[6];

static std::vector<uint32_t> utf8ToUtf32(const std::string& utf8) {
    std::vector<uint32_t> utf32;
    for (size_t i = 0; i < utf8.size(); ) {
        unsigned char c = utf8[i];
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            i++;
            continue;
        }
        if (i + len > utf8.size()) break;
        for (size_t j = 1; j < len; j++) {
            cp = (cp << 6) | (utf8[i + j] & 0x3F);
        }
        utf32.push_back(cp);
        i += len;
    }
    return utf32;
}

struct TextRun {
    int font_idx;
    std::vector<uint32_t> codepoints;
};

// Returns true for codepoints that are best rendered by an emoji font.
static bool isEmojiCodepoint(uint32_t cp) {
    // Miscellaneous Symbols, Dingbats, Emoticons, Transport, Misc Symbols & Pictographs,
    // Supplemental Symbols, Regional Indicators, Enclosed Alphanumeric Supplement,
    // Mahjong/Domino, Playing Cards, and high-plane emoji blocks.
    if (cp >= 0x2600 && cp <= 0x27FF) return true;  // Misc Symbols, Dingbats
    if (cp >= 0x2900 && cp <= 0x297F) return true;  // Supplemental Arrows-B
    if (cp >= 0x1F000 && cp <= 0x1FFFF) return true; // All emoji planes
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;  // Variation selectors
    if (cp >= 0x1F1E0 && cp <= 0x1F1FF) return true; // Regional indicators
    if (cp >= 0x200D && cp <= 0x200D) return true;  // ZWJ
    if (cp == 0x20E3) return true; // Combining enclosing keycap
    return false;
}

static std::vector<TextRun> segmentText(const std::vector<uint32_t>& codepoints, int size_idx) {
    int prim_idx = size_idx * 2;
    int fall_idx = size_idx * 2 + 1;
    // Emoji font uses slots 6 (small) or 7 (medium/large), mapped from size_idx 0→6, 1+→7
    int emoji_idx = (size_idx == 0) ? 6 : 7;

    FT_Face prim_face  = g_ft_faces[prim_idx];
    FT_Face fall_face  = g_ft_faces[fall_idx];
    FT_Face emoji_face = g_ft_faces[emoji_idx];

    std::vector<TextRun> runs;
    if (codepoints.empty()) return runs;

    int current_font = -1;
    for (uint32_t cp : codepoints) {
        int font;
        if (isEmojiCodepoint(cp) && emoji_face && FT_Get_Char_Index(emoji_face, cp) != 0) {
            font = emoji_idx;
        } else if (prim_face && FT_Get_Char_Index(prim_face, cp) != 0) {
            font = prim_idx;
        } else if (fall_face && FT_Get_Char_Index(fall_face, cp) != 0) {
            font = fall_idx;
        } else if (emoji_face && FT_Get_Char_Index(emoji_face, cp) != 0) {
            font = emoji_idx; // last-resort emoji
        } else {
            font = fall_face ? fall_idx : prim_idx;
        }

        if (runs.empty() || font != current_font) {
            runs.push_back({font, {cp}});
            current_font = font;
        } else {
            runs.back().codepoints.push_back(cp);
        }
    }
    return runs;
}

struct ShapedGlyph {
    uint32_t glyph_id;
    int x_offset;
    int y_offset;
    int x_advance;
    int y_advance;
};

static std::vector<ShapedGlyph> shapeRun(const TextRun& run) {
    std::vector<ShapedGlyph> shaped;
    hb_font_t* hb_font = g_hb_fonts[run.font_idx];
    if (!hb_font) return shaped;

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf32(buf, reinterpret_cast<const uint32_t*>(run.codepoints.data()), run.codepoints.size(), 0, run.codepoints.size());
    hb_buffer_guess_segment_properties(buf);

    hb_shape(hb_font, buf, nullptr, 0);

    unsigned int glyph_count;
    hb_glyph_info_t* glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
    hb_glyph_position_t* glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

    shaped.reserve(glyph_count);
    for (unsigned int i = 0; i < glyph_count; i++) {
        ShapedGlyph sg;
        sg.glyph_id = glyph_info[i].codepoint;
        sg.x_offset = glyph_pos[i].x_offset >> 6;
        sg.y_offset = glyph_pos[i].y_offset >> 6;
        sg.x_advance = glyph_pos[i].x_advance >> 6;
        sg.y_advance = glyph_pos[i].y_advance >> 6;
        shaped.push_back(sg);
    }

    hb_buffer_destroy(buf);
    return shaped;
}

struct TextCacheKey {
    std::string text;
    int scale;
    bool operator==(const TextCacheKey& o) const {
        return scale == o.scale && text == o.text;
    }
};

struct TextCacheKeyHash {
    std::size_t operator()(const TextCacheKey& k) const {
        return std::hash<std::string>{}(k.text) ^ (std::hash<int>{}(k.scale) << 1);
    }
};

struct CachedShapedRun {
    int font_idx;
    std::vector<ShapedGlyph> shaped;
};

struct CachedTextShaping {
    std::vector<CachedShapedRun> runs;
    int width = 0;
    int height = 0;
};

static std::unordered_map<TextCacheKey, CachedTextShaping, TextCacheKeyHash> g_text_shaping_cache;

static CachedGlyph getOrCacheGlyph(SDL_Renderer* renderer, int font_idx, uint32_t glyph_id) {
    uint64_t key = (static_cast<uint64_t>(font_idx) << 32) | glyph_id;
    auto it = g_glyph_cache.find(key);
    if (it != g_glyph_cache.end()) {
        return it->second;
    }

    FT_Face face = g_ft_faces[font_idx];
    CachedGlyph glyph;
    if (!face) return glyph;

    if (FT_Load_Glyph(face, glyph_id, FT_LOAD_RENDER) != 0) {
        return glyph;
    }

    FT_GlyphSlot slot = face->glyph;
    glyph.width = slot->bitmap.width;
    glyph.height = slot->bitmap.rows;
    glyph.bearing_x = slot->bitmap_left;
    glyph.bearing_y = slot->bitmap_top;
    glyph.advance = slot->advance.x >> 6;

    if (glyph.width > 0 && glyph.height > 0) {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, glyph.width, glyph.height, 32, SDL_PIXELFORMAT_RGBA32);
        if (surf) {
            SDL_LockSurface(surf);
            uint32_t* pixels = static_cast<uint32_t*>(surf->pixels);
            for (int y = 0; y < glyph.height; y++) {
                for (int x = 0; x < glyph.width; x++) {
                    uint8_t alpha = slot->bitmap.buffer[y * slot->bitmap.pitch + x];
                    pixels[y * surf->pitch / 4 + x] = SDL_MapRGBA(surf->format, 255, 255, 255, alpha);
                }
            }
            SDL_UnlockSurface(surf);

            glyph.texture = SDL_CreateTextureFromSurface(renderer, surf);
            SDL_FreeSurface(surf);
        }
    }

    g_glyph_cache[key] = glyph;
    return glyph;
}

// ── Shaping cache helper ──────────────────────────────────────────────────────
// Shapes `text` once (FreeType + HarfBuzz, with font fallback) and caches the
// result. Shared by getTextSize() and drawText() so both hit the same cache.
static const CachedTextShaping& getOrShape(const std::string& text, int scale, int size_idx) {
    TextCacheKey key{text, scale};
    auto it = g_text_shaping_cache.find(key);
    if (it != g_text_shaping_cache.end()) return it->second;

    if (g_text_shaping_cache.size() > 1000) {
        g_text_shaping_cache.clear();
    }

    CachedTextShaping shaping;
    std::vector<uint32_t> utf32 = utf8ToUtf32(text);
    std::vector<TextRun> runs = segmentText(utf32, size_idx);

    int total_width = 0;
    int max_height = 0;
    shaping.runs.reserve(runs.size());
    for (const auto& run : runs) {
        CachedShapedRun csr;
        csr.font_idx = run.font_idx;
        csr.shaped = shapeRun(run);

        FT_Face face = g_ft_faces[run.font_idx];
        if (face) {
            int cap_height = (face->size->metrics.ascender - face->size->metrics.descender) >> 6;
            if (cap_height > max_height) max_height = cap_height;
            for (const auto& sg : csr.shaped) total_width += sg.x_advance;
        }
        shaping.runs.push_back(std::move(csr));
    }
    shaping.width  = total_width;
    shaping.height = max_height > 0 ? max_height : (14 * scale);

    return g_text_shaping_cache.emplace(std::move(key), std::move(shaping)).first->second;
}

// Draws an already-shaped string to the current render target.
//
// Fast path (primary Latin faces, ASCII glyphs): blits from the shared atlas texture
// so the SDL2/GLES2 renderer can batch an entire text run into ONE draw call instead
// of one per glyph.  On the Mali-G31 (tile-based), collapsing N texture-bind+draw
// pairs into 1 is the biggest text-rendering win available.
//
// Slow path (fallback/emoji faces, non-ASCII, any atlas miss): falls back to the
// per-glyph CachedGlyph texture path (correct but causes one draw call per glyph).
//
// NOTE: the earlier "whole-string render-to-texture cache" was reverted because
// SDL_SetRenderTarget forces a Mali-G31 tile flush on every call, which cost more
// than it saved.  The atlas approach avoids SDL_SetRenderTarget entirely.
static void drawTextDirect(SDL_Renderer* renderer, int x, int y, const CachedTextShaping& shaping, SDL_Color color) {
    int cursor_x = x;
    for (const auto& run : shaping.runs) {
        FT_Face face = g_ft_faces[run.font_idx];
        if (!face) continue;
        int ascender  = face->size->metrics.ascender >> 6;
        int baseline_y = y + ascender;

        // Atlas selection: only primary faces (slots 0/2/4 = small/medium/large
        // Atkinson Hyperlegible) have a matching atlas and glyph-id map.
        // Fallback faces (1/3/5) and emoji (6/7) always use per-glyph textures.
        FontAtlas* atlas = nullptr;
        if      (run.font_idx == 0) atlas = &g_atlas_small;
        else if (run.font_idx == 2) atlas = &g_atlas_medium;
        else if (run.font_idx == 4) atlas = &g_atlas_large;

        const auto* gmap = (atlas && run.font_idx < 6)
                           ? &g_atlas_glyph_map[run.font_idx] : nullptr;
        const bool can_atlas = atlas && atlas->texture && gmap && !gmap->empty();

        if (can_atlas) {
            // Set color/blend once for the whole run so SDL2's GLES2 renderer
            // can accumulate every SDL_RenderCopy into a single draw call.
            SDL_SetTextureColorMod(atlas->texture,  color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(atlas->texture,  color.a);
            SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND);
        }

        for (const auto& sg : run.shaped) {
            bool drew = false;
            if (can_atlas) {
                auto it = gmap->find(sg.glyph_id);
                if (it != gmap->end()) {
                    const GlyphInfo& gi = atlas->glyphs[it->second];
                    if (gi.glyph_src.w > 0 && gi.glyph_src.h > 0) {
                        int draw_x = cursor_x + sg.x_offset + gi.pen_x_offset;
                        int draw_y = baseline_y - sg.y_offset - atlas->ascent;
                        SDL_Rect src = gi.glyph_src;
                        SDL_Rect dst{draw_x, draw_y, src.w, src.h};
                        SDL_RenderCopy(renderer, atlas->texture, &src, &dst);
                        drew = true;
                    }
                }
            }
            if (!drew) {
                CachedGlyph cg = getOrCacheGlyph(renderer, run.font_idx, sg.glyph_id);
                if (cg.texture) {
                    SDL_SetTextureColorMod(cg.texture,  color.r, color.g, color.b);
                    SDL_SetTextureAlphaMod(cg.texture,  color.a);
                    SDL_SetTextureBlendMode(cg.texture, SDL_BLENDMODE_BLEND);
                    int draw_x = cursor_x + sg.x_offset + cg.bearing_x;
                    int draw_y = baseline_y - sg.y_offset - cg.bearing_y;
                    SDL_Rect dst{draw_x, draw_y, cg.width, cg.height};
                    SDL_RenderCopy(renderer, cg.texture, nullptr, &dst);
                }
            }
            cursor_x += sg.x_advance;
        }
    }
}

bool initFonts(SDL_Renderer* renderer) {
    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
        return false;
    }

    if (FT_Init_FreeType(&g_ft_library) != 0) {
        std::cerr << "FT_Init_FreeType failed" << std::endl;
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

    std::vector<std::string> fallbackPaths = {
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
        "C:/Windows/Fonts/malgun.ttf",
        "C:/Windows/Fonts/msjh.ttc",
        "C:/Windows/Fonts/arial.ttf"
    };

    std::string fallbackPath = "";
    for (const auto& p : fallbackPaths) {
        if (FILE* f = fopen(p.c_str(), "rb")) {
            fclose(f);
            fallbackPath = p;
            break;
        }
    }

    int sizes[3] = {14, 18, 24};
    for (int i = 0; i < 3; i++) {
        std::string primaryFile = (i == 2) ? boldPath : regPath;
        int size = sizes[i];

        if (FT_New_Face(g_ft_library, primaryFile.c_str(), 0, &g_ft_faces[i * 2]) == 0) {
            FT_Set_Pixel_Sizes(g_ft_faces[i * 2], 0, size);
            g_hb_fonts[i * 2] = hb_ft_font_create(g_ft_faces[i * 2], nullptr);
        }

        if (!fallbackPath.empty()) {
            if (FT_New_Face(g_ft_library, fallbackPath.c_str(), 0, &g_ft_faces[i * 2 + 1]) == 0) {
                FT_Set_Pixel_Sizes(g_ft_faces[i * 2 + 1], 0, size);
                g_hb_fonts[i * 2 + 1] = hb_ft_font_create(g_ft_faces[i * 2 + 1], nullptr);
            }
        }
    }

    // ── Emoji font (slots 6 = 14px, 7 = 18px) ───────────────────────────────
    std::vector<std::string> emojiPaths = {
        "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/noto-color-emoji/NotoColorEmoji.ttf",
        "res/fonts/NotoEmoji-Regular.ttf",
        "../res/fonts/NotoEmoji-Regular.ttf",
        "/roms/tools/tubelite/res/fonts/NotoEmoji-Regular.ttf",
        "C:/Windows/Fonts/seguiemj.ttf",  // Windows: Segoe UI Emoji
    };
    std::string emojiPath = "";
    for (const auto& p : emojiPaths) {
        if (FILE* f = fopen(p.c_str(), "rb")) { fclose(f); emojiPath = p; break; }
    }
    if (!emojiPath.empty()) {
        // Slot 6: small (14px), Slot 7: medium (18px) — large scale uses medium emoji
        for (int ei = 0; ei < 2; ei++) {
            int esize = (ei == 0) ? 14 : 18;
            if (FT_New_Face(g_ft_library, emojiPath.c_str(), 0, &g_ft_faces[6 + ei]) == 0) {
                FT_Set_Pixel_Sizes(g_ft_faces[6 + ei], 0, esize);
                g_hb_fonts[6 + ei] = hb_ft_font_create(g_ft_faces[6 + ei], nullptr);
            }
        }
        std::cerr << "[fonts] Emoji font loaded: " << emojiPath << "\n";
    } else {
        std::cerr << "[fonts] No emoji font found — emoji will render as tofu\n";
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

    // Build glyph_id → ASCII char maps for the three primary faces (slots 0, 2, 4).
    // This lets drawTextDirect hit the shared atlas texture for all ASCII glyphs,
    // collapsing N per-glyph texture binds into one per text run on the Mali-G31.
    for (int si = 0; si < 3; si++) {
        int fi = si * 2;  // primary face: 0 (14px), 2 (18px), 4 (24px)
        FT_Face face = g_ft_faces[fi];
        if (!face) continue;
        for (unsigned int ch = 32; ch < 127; ++ch) {
            FT_UInt gid = FT_Get_Char_Index(face, ch);
            if (gid != 0) g_atlas_glyph_map[fi][gid] = ch;
        }
    }

    initCornerMask(renderer);
    initSolidCorner(renderer);
    initRingCorner(renderer);

    return true;
}


void cleanupFonts() {
    g_text_shaping_cache.clear();
    for (int i = 0; i < 6; i++) g_atlas_glyph_map[i].clear();
    for (auto& pair : g_glyph_cache) {
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
        }
    }
    g_glyph_cache.clear();

    for (int i = 0; i < 8; i++) {
        if (g_hb_fonts[i]) {
            hb_font_destroy(g_hb_fonts[i]);
            g_hb_fonts[i] = nullptr;
        }
        if (g_ft_faces[i]) {
            FT_Done_Face(g_ft_faces[i]);
            g_ft_faces[i] = nullptr;
        }
    }

    if (g_ft_library) {
        FT_Done_FreeType(g_ft_library);
        g_ft_library = nullptr;
    }

    if (g_atlas_small.texture)  { SDL_DestroyTexture(g_atlas_small.texture);  g_atlas_small.texture = nullptr; }
    if (g_atlas_medium.texture) { SDL_DestroyTexture(g_atlas_medium.texture); g_atlas_medium.texture = nullptr; }
    if (g_atlas_large.texture)  { SDL_DestroyTexture(g_atlas_large.texture);  g_atlas_large.texture = nullptr; }
    if (g_corner_mask_texture)  { SDL_DestroyTexture(g_corner_mask_texture);  g_corner_mask_texture = nullptr; }
    if (g_solid_corner_texture) { SDL_DestroyTexture(g_solid_corner_texture); g_solid_corner_texture = nullptr; }
    if (g_ring_corner_texture)  { SDL_DestroyTexture(g_ring_corner_texture);  g_ring_corner_texture = nullptr; }
    if (g_font_small)  { TTF_CloseFont(g_font_small);  g_font_small = nullptr; }
    if (g_font_medium) { TTF_CloseFont(g_font_medium); g_font_medium = nullptr; }
    if (g_font_large)  { TTF_CloseFont(g_font_large);  g_font_large = nullptr; }
    TTF_Quit();
}

void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {
    if (text.empty()) return;

    int size_idx = (scale <= 1) ? 0 : (scale == 2 ? 1 : 2);
    drawTextDirect(renderer, x, y, getOrShape(text, scale, size_idx), color);
}

void drawTextShadow(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {
    const int offset = std::max(1, scale / 2);
    drawText(renderer, x - offset, y + offset, text, scale, {0, 0, 0, color.a});
    drawText(renderer, x, y, text, scale, color);
}

void drawTextCentered(SDL_Renderer* renderer, int centerX, int y, const std::string& text, int scale, SDL_Color color, bool shadow) {
    int w = 0, h = 0;
    getTextSize(text, scale, &w, &h);
    int x = centerX - w / 2;
    if (shadow) {
        drawTextShadow(renderer, x, y, text, scale, color);
    } else {
        drawText(renderer, x, y, text, scale, color);
    }
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
        
        SDL_SetRenderDrawColor(renderer, theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b, alpha);
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
    
    int size_idx = (scale <= 1) ? 0 : (scale == 2 ? 1 : 2);
    const CachedTextShaping& shaping = getOrShape(text, scale, size_idx);
    if (w) *w = shaping.width;
    if (h) *h = shaping.height;
}

std::vector<std::string> wrapText(const std::string& text, int maxWidth, int scale) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    std::stringstream ss(text);
    std::string paragraph;
    while (std::getline(ss, paragraph, '\n')) {
        if (paragraph.empty()) {
            lines.push_back("");
            continue;
        }

        std::vector<uint32_t> cp_vec = utf8ToUtf32(paragraph);
        std::string current_line = "";
        
        for (size_t i = 0; i < cp_vec.size(); ) {
            uint32_t cp = cp_vec[i];
            
            std::string cp_utf8;
            if (cp < 0x80) {
                cp_utf8 += static_cast<char>(cp);
            } else if (cp < 0x800) {
                cp_utf8 += static_cast<char>(0xC0 | (cp >> 6));
                cp_utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                cp_utf8 += static_cast<char>(0xE0 | (cp >> 12));
                cp_utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                cp_utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                cp_utf8 += static_cast<char>(0xF0 | (cp >> 18));
                cp_utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                cp_utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                cp_utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            }
            
            std::string test_line = current_line + cp_utf8;
            int w = 0, h = 0;
            getTextSize(test_line, scale, &w, &h);
            
            if (w > maxWidth && !current_line.empty()) {
                size_t last_space = current_line.find_last_of(" \t\r\n");
                if (last_space != std::string::npos && last_space > 0) {
                    lines.push_back(current_line.substr(0, last_space));
                    std::string remainder = current_line.substr(last_space + 1);
                    current_line = remainder + cp_utf8;
                } else {
                    lines.push_back(current_line);
                    current_line = cp_utf8;
                }
            } else {
                current_line = test_line;
            }
            i++;
        }
        if (!current_line.empty()) {
            lines.push_back(current_line);
        }
    }
    return lines;
}

std::string formatStatsNumber(long long count) {
    if (count < 0) return "0";
    if (count < 1000) {
        return std::to_string(count);
    }
    if (count < 1000000) {
        double k = count / 1000.0;
        char buf[32];
        if (k < 10.0) {
            snprintf(buf, sizeof(buf), "%.1fK", k);
        } else {
            snprintf(buf, sizeof(buf), "%.0fK", k);
        }
        return buf;
    }
    if (count < 1000000000) {
        double m = count / 1000000.0;
        char buf[32];
        if (m < 10.0) {
            snprintf(buf, sizeof(buf), "%.1fM", m);
        } else {
            snprintf(buf, sizeof(buf), "%.0fM", m);
        }
        return buf;
    }
    double b = count / 1000000000.0;
    char buf[32];
    if (b < 10.0) {
        snprintf(buf, sizeof(buf), "%.1fB", b);
    } else {
        snprintf(buf, sizeof(buf), "%.0fB", b);
    }
    return buf;
}

void fillRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    // Clamp the radius so a rect smaller than 2*radius (short pills, thin
    // chips) can't produce negative-width middle/side rects or corner blits
    // that overlap and overdraw each other — both read as "broken" corners.
    int r = radius;
    if (r > 0) {
        int maxR = std::min(rect.w, rect.h) / 2;
        if (r > maxR) r = maxR;
    }
    if (r <= 0 || !g_solid_corner_texture) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    // Corners carry the antialiased edge, so they MUST blend.  fillRoundedRect
    // used to inherit whatever blend state the previous draw left on the
    // renderer/texture; when that was BLENDMODE_NONE the AA alpha was written
    // as hard pixels and the corners looked chipped.  Pin both explicitly so
    // the result is identical regardless of caller order.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_SetTextureBlendMode(g_solid_corner_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(g_solid_corner_texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(g_solid_corner_texture, color.a);

    int r_base = 64;
    SDL_Rect middleRect{rect.x + r, rect.y, rect.w - 2 * r, rect.h};
    SDL_Rect sideRect{rect.x, rect.y + r, rect.w, rect.h - 2 * r};
    SDL_RenderFillRect(renderer, &middleRect);
    SDL_RenderFillRect(renderer, &sideRect);
    
    SDL_Rect srcTL{0, 0, r_base, r_base};
    SDL_Rect dstTL{rect.x, rect.y, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcTL, &dstTL);
    
    SDL_Rect srcTR{r_base, 0, r_base, r_base};
    SDL_Rect dstTR{rect.x + rect.w - r, rect.y, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcTR, &dstTR);
    
    SDL_Rect srcBL{0, r_base, r_base, r_base};
    SDL_Rect dstBL{rect.x, rect.y + rect.h - r, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcBL, &dstBL);
    
    SDL_Rect srcBR{r_base, r_base, r_base, r_base};
    SDL_Rect dstBR{rect.x + rect.w - r, rect.y + rect.h - r, r, r};
    SDL_RenderCopy(renderer, g_solid_corner_texture, &srcBR, &dstBR);
}

void drawRoundedRect(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    // Clamp (don't drop to a sharp rect) when the rect is smaller than
    // 2*radius — fillRoundedRect now clamps the same way, so the border keeps
    // matching the fill instead of squaring off only the outline.
    int r = radius;
    if (r > 0) {
        int maxR = std::min(rect.w, rect.h) / 2;
        if (r > maxR) r = maxR;
    }
    if (r <= 0) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderDrawRect(renderer, &rect);
        return;
    }

    const int x = rect.x, y = rect.y, w = rect.w, h = rect.h;

    // ── Straight edges (4 line draws) ───────────────────────────────────────
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderDrawLine(renderer, x + r,     y,         x + w - r - 1, y);            // top
    SDL_RenderDrawLine(renderer, x + r,     y + h - 1, x + w - r - 1, y + h - 1);    // bottom
    SDL_RenderDrawLine(renderer, x,         y + r,     x,             y + h - r - 1); // left
    SDL_RenderDrawLine(renderer, x + w - 1, y + r,     x + w - 1,     y + h - r - 1); // right

    // ── Corners ─────────────────────────────────────────────────────────────
    // Fast path: blit each corner from the pre-built ring atlas.  This drops
    // a typical drawRoundedRect from ~140 SDL ops (midpoint circle was 8
    // SDL_RenderDrawPoint per loop iteration) to 8 (4 lines + 4 RenderCopy).
    // On the RG351MP the previous focus-ring path was 16.7 ms per frame.
    if (g_ring_corner_texture) {
        SDL_SetTextureColorMod(g_ring_corner_texture,  color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(g_ring_corner_texture,  color.a);
        SDL_SetTextureBlendMode(g_ring_corner_texture, SDL_BLENDMODE_BLEND);

        const int r_base = 64;  // ring texture is 128x128; each 64x64 quadrant = one corner
        SDL_Rect srcTL{0,      0,      r_base, r_base};
        SDL_Rect srcTR{r_base, 0,      r_base, r_base};
        SDL_Rect srcBL{0,      r_base, r_base, r_base};
        SDL_Rect srcBR{r_base, r_base, r_base, r_base};

        SDL_Rect dstTL{x,             y,             r, r};
        SDL_Rect dstTR{x + w - r,     y,             r, r};
        SDL_Rect dstBL{x,             y + h - r,     r, r};
        SDL_Rect dstBR{x + w - r,     y + h - r,     r, r};

        SDL_RenderCopy(renderer, g_ring_corner_texture, &srcTL, &dstTL);
        SDL_RenderCopy(renderer, g_ring_corner_texture, &srcTR, &dstTR);
        SDL_RenderCopy(renderer, g_ring_corner_texture, &srcBL, &dstBL);
        SDL_RenderCopy(renderer, g_ring_corner_texture, &srcBR, &dstBR);
        return;
    }

    // Fallback path (only hit if g_ring_corner_texture failed to initialise):
    // midpoint-circle outline.  Pixel-exact at small radii but slow because
    // every point is its own SDL call.
    const int cxL = x + r,         cxR = x + w - 1 - r;
    const int cyT = y + r,         cyB = y + h - 1 - r;
    int px = r, py = 0, err = 1 - r;
    while (px >= py) {
        SDL_RenderDrawPoint(renderer, cxL - px, cyT - py);
        SDL_RenderDrawPoint(renderer, cxL - py, cyT - px);
        SDL_RenderDrawPoint(renderer, cxR + px, cyT - py);
        SDL_RenderDrawPoint(renderer, cxR + py, cyT - px);
        SDL_RenderDrawPoint(renderer, cxL - px, cyB + py);
        SDL_RenderDrawPoint(renderer, cxL - py, cyB + px);
        SDL_RenderDrawPoint(renderer, cxR + px, cyB + py);
        SDL_RenderDrawPoint(renderer, cxR + py, cyB + px);
        ++py;
        if (err < 0) {
            err += 2 * py + 1;
        } else {
            --px;
            err += 2 * (py - px) + 1;
        }
    }
}

void maskRoundedCorners(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color color) {
    if (radius <= 0 || !g_corner_mask_texture) return;
    
    SDL_SetTextureColorMod(g_corner_mask_texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(g_corner_mask_texture, color.a);
    SDL_SetTextureBlendMode(g_corner_mask_texture, SDL_BLENDMODE_BLEND);
    
    int r_base = 64;
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
    
    int r_base = 64;
    int r = radius;
    
    SDL_Rect srcTL{0, 0, r_base, r_base};
    SDL_Rect dstTL{rect.x, rect.y, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcTL, &dstTL);
    
    SDL_Rect srcTR{r_base, 0, r_base, r_base};
    SDL_Rect dstTR{rect.x + rect.w - r, rect.y, r, r};
    SDL_RenderCopy(renderer, g_corner_mask_texture, &srcTR, &dstTR);
}

void drawHairline(SDL_Renderer* renderer, int x, int y, int w, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_Rect line{x, y, w, 1};
    SDL_RenderFillRect(renderer, &line);
}

SDL_Rect drawStatusPill(SDL_Renderer* renderer, int x, int y,
                        const std::string& label, SDL_Color tint) {
    int tw = 0, th = 0;
    getTextSize(label, 1, &tw, &th);
    constexpr int padX = 7, padY = 2;
    SDL_Rect pill{x, y, tw + padX * 2, th + padY * 2};
    fillRoundedRect(renderer, pill, theme::RADIUS_PILL,
                    SDL_Color{tint.r, tint.g, tint.b, 38});
    drawRoundedRect(renderer, pill, theme::RADIUS_PILL,
                    SDL_Color{tint.r, tint.g, tint.b, 180});
    drawText(renderer, pill.x + padX, pill.y + padY, label, 1, tint);
    return pill;
}

void drawDaemonCard(SDL_Renderer* renderer, const SDL_Rect& rect,
                    int radius, bool drawShadow, bool drawAccentBar) {
    // Blend mode is set ONCE up-front instead of inside each call below —
    // every primitive we draw here is alpha-blended, so the state stays
    // valid across the whole function and we save ~30 redundant
    // SDL_SetRenderDrawBlendMode calls per frame.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const int cx = rect.x, cy = rect.y, cw = rect.w, ch = rect.h;

    // Diffuse two-pass shadow.
    if (drawShadow) {
        SDL_Rect s1{cx + 2, cy + 4, cw, ch};
        SDL_Rect s2{cx + 1, cy + 2, cw, ch + 1};
        fillRoundedRect(renderer, s1, radius, theme::BLACK.a8(70));
        fillRoundedRect(renderer, s2, radius, theme::BLACK.a8(40));
    }

    // Card body.
    SDL_Rect body{cx, cy, cw, ch};
    fillRoundedRect(renderer, body, radius, theme::SURFACE);

    // Top-half lift gradient — 12 stacked 2 px-tall bands with decreasing
    // alpha.  These are all radius=0, so we bypass fillRoundedRect's
    // shortcut path entirely and call SDL directly — saves the per-call
    // texture branch + two extra function frames.  Pre-batched as one
    // FRect array would be slightly faster still but requires SDL2
    // ≥2.0.18; the explicit loop is portable and still cheap.
    {
        const int bandX = cx + 1;
        const int bandW = cw - 2;
        for (int i = 0; i < 12; ++i) {
            const int alpha = 14 - i;
            if (alpha <= 0) break;
            SDL_SetRenderDrawColor(renderer,
                                   theme::RAISED.r, theme::RAISED.g, theme::RAISED.b,
                                   static_cast<Uint8>(alpha));
            SDL_Rect band{bandX, cy + 1 + i * 2, bandW, 2};
            SDL_RenderFillRect(renderer, &band);
        }
    }

    // Left accent bar + horizontal glow.  Signature daemon element —
    // 3 px solid accent on the left, then a 16 px gradient fading right.
    if (drawAccentBar) {
        constexpr int kAccentW = 3;
        SDL_Rect accent{cx, cy, kAccentW, ch};
        fillRoundedRect(renderer, accent, 2, theme::ACCENT);
        const int glowX0 = cx + kAccentW;
        const int glowY0 = cy + 1;
        const int glowH  = ch - 2;
        for (int i = 0; i < 16; ++i) {
            const int alpha = 30 - i * 2;
            if (alpha <= 0) break;
            SDL_SetRenderDrawColor(renderer,
                                   theme::ACCENT.r, theme::ACCENT.g, theme::ACCENT.b,
                                   static_cast<Uint8>(alpha));
            SDL_Rect g{glowX0 + i, glowY0, 1, glowH};
            SDL_RenderFillRect(renderer, &g);
        }
    }

    // Border line.
    drawRoundedRect(renderer, body, radius, theme::BORDER);
}

void drawHintButtons(SDL_Renderer* renderer, const std::vector<HintItem>& hints, int y, int height, int scale, int width, SDL_Color panelColor, SDL_Color borderColor, SDL_Color textColor) {
    if (hints.empty()) return;
    int fontHeight = 14;
    getTextSize("Ay", scale, nullptr, &fontHeight);

    int totalWidth = 0;
    std::vector<int> boxWidths;
    std::vector<int> btnWidths;
    std::vector<int> actWidths;
    
    int gap = (scale == 2) ? 12 : 8;
    int innerPad = (scale == 2) ? 24 : 16;
    int labelGap = (scale == 2) ? 8 : 4;

    for (const auto& item : hints) {
        int btnW = 0, actW = 0;
        getTextSize(item.button, scale, &btnW, nullptr);
        getTextSize(item.action, scale, &actW, nullptr);
        int boxW = btnW + actW + innerPad;
        boxWidths.push_back(boxW);
        btnWidths.push_back(btnW);
        actWidths.push_back(actW);
        totalWidth += boxW;
    }
    totalWidth += (static_cast<int>(hints.size()) - 1) * gap;

    int currentX = (width - totalWidth) / 2;
    for (size_t i = 0; i < hints.size(); ++i) {
        const auto& item = hints[i];
        int boxW = boxWidths[i];
        int btnW = btnWidths[i];
        int actW = actWidths[i];
        
        SDL_Rect box{currentX, y, boxW, height};
        fillRoundedRect(renderer, box, theme::RADIUS_PILL, panelColor);
        drawRoundedRect(renderer, box, theme::RADIUS_PILL, borderColor);
        
        int contentW = btnW + labelGap + actW;
        int contentX = currentX + (boxW - contentW) / 2;
        int textY = y + (height - fontHeight) / 2;
        
        drawTextShadow(renderer, contentX, textY, item.button, scale, item.btnColor);
        drawTextShadow(renderer, contentX + btnW + labelGap, textY, item.action, scale, textColor);
        
        currentX += boxW + gap;
    }
}

void drawVolumeOverlay(SDL_Renderer* renderer, int centerX, int y, int volume, bool muted, SDL_Color themeColor) {
    int boxW = 200, boxH = 36;
    int boxX = centerX - boxW / 2;
    SDL_Rect r{boxX, y, boxW, boxH};
    fillRoundedRect(renderer, r, theme::RADIUS_PANEL, theme::BLACK.a8(200));
    drawRoundedRect(renderer, r, theme::RADIUS_PANEL, themeColor);

    std::string volText = "Volume: " + std::to_string(volume) + "%";
    if (muted) volText = "Mute: ON";

    int barW = 160, barH = 6;
    int barX = boxX + 20;
    int barY = y + 24;
    SDL_Rect barBg{barX, barY, barW, barH};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, theme::TRACK.r, theme::TRACK.g, theme::TRACK.b, 255);
    SDL_RenderFillRect(renderer, &barBg);

    if (!muted) {
        int fillW = static_cast<int>(barW * (volume / 100.0f));
        SDL_Rect barFill{barX, barY, fillW, barH};
        SDL_SetRenderDrawColor(renderer, themeColor.r, themeColor.g, themeColor.b, themeColor.a);
        SDL_RenderFillRect(renderer, &barFill);
    }

    drawTextCentered(renderer, centerX, y + 4, volText, 1, theme::WHITE, true);
}

void drawSpeedOverlay(SDL_Renderer* renderer, int centerX, int y, double speed, SDL_Color themeColor) {
    int boxW = 200, boxH = 36;
    int boxX = centerX - boxW / 2;
    SDL_Rect r{boxX, y, boxW, boxH};
    fillRoundedRect(renderer, r, theme::RADIUS_PANEL, theme::BLACK.a8(200));
    drawRoundedRect(renderer, r, theme::RADIUS_PANEL, themeColor);

    char speedBuf[32];
    snprintf(speedBuf, sizeof(speedBuf), "Speed: %.2fx", speed);
    drawTextCentered(renderer, centerX, y + 8, speedBuf, 1, theme::WHITE, true);
}

void drawLoadingOverlay(SDL_Renderer* renderer, int width, int height, const std::string& text, float time, SDL_Color textColor, bool drawBg) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (drawBg) {
        SDL_SetRenderDrawColor(renderer, theme::BLACK.r, theme::BLACK.g, theme::BLACK.b, 180);
        SDL_Rect bg{0, 0, width, height};
        SDL_RenderFillRect(renderer, &bg);
    }
    
    drawSpinner(renderer, width / 2, height / 2 - 20, 30, time);
    drawTextCentered(renderer, width / 2, height / 2 + 25, text, 2, textColor, true);
}


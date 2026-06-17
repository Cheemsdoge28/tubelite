#include "renderer_utils.hpp"
#include <cctype>
#include <climits>
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

SDL_Texture* g_corner_mask_texture = nullptr;
SDL_Texture* g_solid_corner_texture = nullptr;

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
            // Store full cell coordinates
            info.src_rect = {col * cell_w, row * cell_h, cell_w, cell_h};
            info.minx  = minx;
            info.maxx  = maxx;
            info.miny  = miny;
            info.maxy  = maxy;
            info.advance = advance;

            SDL_FreeSurface(glyph_surf);
        } else {
            GlyphInfo& info = atlas.glyphs[ch];
            info.src_rect = {col * cell_w, row * cell_h, cell_w, cell_h};
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

// ── Whole-string texture cache ────────────────────────────────────────────────
// Browsing redraws the whole visible grid every frame while scrolling or while a
// focused title marquees. Previously each drawText issued one SDL_RenderCopy
// (plus three texture-state calls) per glyph — hundreds of draw calls per frame.
// We composite each repeated string once into a premultiplied-alpha texture of
// white coverage, so a warmed-up string costs exactly one colour-modulated blit.
struct CachedStringTexture {
    SDL_Texture* texture = nullptr; // premultiplied white coverage; null = nothing visible
    int width = 0;
    int height = 0;
    int blit_dx = 0;        // on-screen offset of the texture's top-left from (x, y)
    int blit_dy = 0;
    bool fallback = false;  // build failed — caller should draw per-glyph instead
};

static std::unordered_map<TextCacheKey, CachedStringTexture, TextCacheKeyHash> g_string_texture_cache;
static SDL_BlendMode g_premult_blend = SDL_BLENDMODE_BLEND;
static bool g_string_cache_ok = true;   // disabled if the renderer rejects premultiplied blending

// Original per-glyph path: draws an already-shaped string directly to the current
// target in `color`. Used as a fallback and for translucent text, where alpha-mod
// on a premultiplied texture would not rescale RGB.
static void drawTextDirect(SDL_Renderer* renderer, int x, int y, const CachedTextShaping& shaping, SDL_Color color) {
    int cursor_x = x;
    for (const auto& run : shaping.runs) {
        FT_Face face = g_ft_faces[run.font_idx];
        if (!face) continue;
        int ascender = face->size->metrics.ascender >> 6;
        int baseline_y = y + ascender;
        for (const auto& sg : run.shaped) {
            CachedGlyph cg = getOrCacheGlyph(renderer, run.font_idx, sg.glyph_id);
            if (cg.texture) {
                SDL_SetTextureColorMod(cg.texture, color.r, color.g, color.b);
                SDL_SetTextureAlphaMod(cg.texture, color.a);
                SDL_SetTextureBlendMode(cg.texture, SDL_BLENDMODE_BLEND);
                int draw_x = cursor_x + sg.x_offset + cg.bearing_x;
                int draw_y = baseline_y - sg.y_offset - cg.bearing_y;
                SDL_Rect dst{draw_x, draw_y, cg.width, cg.height};
                SDL_RenderCopy(renderer, cg.texture, nullptr, &dst);
            }
            cursor_x += sg.x_advance;
        }
    }
}

// Composite a shaped string once into a premultiplied-alpha texture. Drawing the
// straight-alpha white glyphs src-over into a cleared (transparent) target yields
// premultiplied coverage; tint/translucency is applied later via colour-mod at
// blit time with g_premult_blend. For opaque, non-overlapping glyphs this is
// pixel-identical to drawTextDirect.
static CachedStringTexture buildStringTexture(SDL_Renderer* renderer, const CachedTextShaping& shaping) {
    CachedStringTexture cst;

    // Pass 1: bounding box of all visible glyphs (origin x=0, baseline=ascender).
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    int cursor_x = 0;
    bool any = false;
    for (const auto& run : shaping.runs) {
        FT_Face face = g_ft_faces[run.font_idx];
        int baseline = face ? (face->size->metrics.ascender >> 6) : 0;
        for (const auto& sg : run.shaped) {
            CachedGlyph cg = getOrCacheGlyph(renderer, run.font_idx, sg.glyph_id);
            if (cg.texture && cg.width > 0 && cg.height > 0) {
                int gx = cursor_x + sg.x_offset + cg.bearing_x;
                int gy = baseline - sg.y_offset - cg.bearing_y;
                if (gx < min_x) min_x = gx;
                if (gy < min_y) min_y = gy;
                if (gx + cg.width  > max_x) max_x = gx + cg.width;
                if (gy + cg.height > max_y) max_y = gy + cg.height;
                any = true;
            }
            cursor_x += sg.x_advance;
        }
    }
    if (!any) return cst;                         // e.g. all-whitespace — nothing to draw
    int tex_w = max_x - min_x;
    int tex_h = max_y - min_y;
    if (tex_w <= 0 || tex_h <= 0) return cst;

    SDL_Texture* tex = createTargetTexture(renderer, tex_w, tex_h);
    if (!tex) { cst.fallback = true; return cst; }
    if (SDL_SetTextureBlendMode(tex, g_premult_blend) != 0) {
        // Renderer can't do premultiplied blending — disable the fast path entirely.
        SDL_DestroyTexture(tex);
        g_string_cache_ok = false;
        cst.fallback = true;
        return cst;
    }

    SDL_Texture* prev_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, tex);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    // Pass 2: blit white glyphs at the same relative positions.
    cursor_x = 0;
    for (const auto& run : shaping.runs) {
        FT_Face face = g_ft_faces[run.font_idx];
        int baseline = face ? (face->size->metrics.ascender >> 6) : 0;
        for (const auto& sg : run.shaped) {
            CachedGlyph cg = getOrCacheGlyph(renderer, run.font_idx, sg.glyph_id);
            if (cg.texture && cg.width > 0 && cg.height > 0) {
                SDL_SetTextureColorMod(cg.texture, 255, 255, 255);
                SDL_SetTextureAlphaMod(cg.texture, 255);
                SDL_SetTextureBlendMode(cg.texture, SDL_BLENDMODE_BLEND);
                SDL_Rect dst{cursor_x + sg.x_offset + cg.bearing_x - min_x,
                             baseline - sg.y_offset - cg.bearing_y - min_y,
                             cg.width, cg.height};
                SDL_RenderCopy(renderer, cg.texture, nullptr, &dst);
            }
            cursor_x += sg.x_advance;
        }
    }

    SDL_SetRenderTarget(renderer, prev_target);

    cst.texture = tex;
    cst.width   = tex_w;
    cst.height  = tex_h;
    cst.blit_dx = min_x;
    cst.blit_dy = min_y;
    return cst;
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
    
    initCornerMask(renderer);
    initSolidCorner(renderer);

    // Blend mode for compositing the premultiplied-alpha string textures. Uses
    // only standard FUNC_ADD blending (GLES2 core). If the renderer can't compose
    // it, the first buildStringTexture() falls back to per-glyph drawing.
    g_premult_blend = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

    return true;
}


void cleanupFonts() {
    g_text_shaping_cache.clear();
    for (auto& pair : g_string_texture_cache) {
        if (pair.second.texture) SDL_DestroyTexture(pair.second.texture);
    }
    g_string_texture_cache.clear();
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
    if (g_font_small)  { TTF_CloseFont(g_font_small);  g_font_small = nullptr; }
    if (g_font_medium) { TTF_CloseFont(g_font_medium); g_font_medium = nullptr; }
    if (g_font_large)  { TTF_CloseFont(g_font_large);  g_font_large = nullptr; }
    TTF_Quit();
}

void drawText(SDL_Renderer* renderer, int x, int y, const std::string& text, int scale, SDL_Color color) {
    if (text.empty()) return;

    int size_idx = (scale <= 1) ? 0 : (scale == 2 ? 1 : 2);
    const CachedTextShaping& shaping = getOrShape(text, scale, size_idx);

    // Fast path: opaque text via a cached, premultiplied whole-string texture.
    // Translucent text falls back to per-glyph drawing so the colour's alpha stays
    // correct (alpha-mod does not rescale a premultiplied texture's RGB).
    if (g_string_cache_ok && color.a == 255) {
        TextCacheKey key{text, scale};
        auto sit = g_string_texture_cache.find(key);
        if (sit == g_string_texture_cache.end()) {
            if (g_string_texture_cache.size() > 512) {
                for (auto& p : g_string_texture_cache) {
                    if (p.second.texture) SDL_DestroyTexture(p.second.texture);
                }
                g_string_texture_cache.clear();
            }
            sit = g_string_texture_cache.emplace(key, buildStringTexture(renderer, shaping)).first;
        }
        const CachedStringTexture& cst = sit->second;
        if (cst.texture) {
            SDL_SetTextureColorMod(cst.texture, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(cst.texture, 255);
            SDL_Rect dst{x + cst.blit_dx, y + cst.blit_dy, cst.width, cst.height};
            SDL_RenderCopy(renderer, cst.texture, nullptr, &dst);
            return;
        }
        if (!cst.fallback) return;   // nothing visible (e.g. whitespace)
        // build failed: fall through to direct per-glyph drawing
    }

    drawTextDirect(renderer, x, y, shaping, color);
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
    if (radius <= 0 || !g_solid_corner_texture) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }
    
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_SetTextureColorMod(g_solid_corner_texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(g_solid_corner_texture, color.a);
    
    int r_base = 64;
    int r = radius;
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
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    int r = radius;
    if (r <= 0 || rect.w < 2 * r || rect.h < 2 * r) {
        SDL_RenderDrawRect(renderer, &rect);  // too small to round — plain box
        return;
    }

    const int x = rect.x, y = rect.y, w = rect.w, h = rect.h;

    // Straight edges, inset by the corner radius.
    SDL_RenderDrawLine(renderer, x + r,         y,           x + w - r - 1, y);            // top
    SDL_RenderDrawLine(renderer, x + r,         y + h - 1,   x + w - r - 1, y + h - 1);    // bottom
    SDL_RenderDrawLine(renderer, x,             y + r,       x,             y + h - r - 1); // left
    SDL_RenderDrawLine(renderer, x + w - 1,     y + r,       x + w - 1,     y + h - r - 1); // right

    // Four quarter-circle corners via the midpoint-circle algorithm.
    const int cxL = x + r,         cxR = x + w - 1 - r;
    const int cyT = y + r,         cyB = y + h - 1 - r;
    int px = r, py = 0, err = 1 - r;
    while (px >= py) {
        SDL_RenderDrawPoint(renderer, cxL - px, cyT - py);   // top-left
        SDL_RenderDrawPoint(renderer, cxL - py, cyT - px);
        SDL_RenderDrawPoint(renderer, cxR + px, cyT - py);   // top-right
        SDL_RenderDrawPoint(renderer, cxR + py, cyT - px);
        SDL_RenderDrawPoint(renderer, cxL - px, cyB + py);   // bottom-left
        SDL_RenderDrawPoint(renderer, cxL - py, cyB + px);
        SDL_RenderDrawPoint(renderer, cxR + px, cyB + py);   // bottom-right
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


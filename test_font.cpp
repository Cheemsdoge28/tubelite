#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <iostream>
#include <string>
#include <vector>

int main() {
    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
        return 1;
    }

    std::string fontPath = "res/fonts/AtkinsonHyperlegible-Regular.ttf";
    TTF_Font* font = TTF_OpenFont(fontPath.c_str(), 14);
    if (!font) {
        std::cerr << "Failed to open font: " << TTF_GetError() << std::endl;
        return 1;
    }

    int height = TTF_FontHeight(font);
    int line_skip = TTF_FontLineSkip(font);
    int ascent = TTF_FontAscent(font);
    int descent = TTF_FontDescent(font);

    std::cout << "=== Font Metrics (Size 14) ===" << std::endl;
    std::cout << "Height: " << height << std::endl;
    std::cout << "LineSkip: " << line_skip << std::endl;
    std::cout << "Ascent: " << ascent << std::endl;
    std::cout << "Descent: " << descent << std::endl;

    std::string test_chars = "gypqtfT";
    for (char ch : test_chars) {
        int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
        TTF_GlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &advance);
        
        SDL_Surface* surf = TTF_RenderGlyph_Blended(font, ch, {255, 255, 255, 255});
        if (surf) {
            std::cout << "Glyph '" << ch << "': "
                      << "metrics[miny=" << miny << ", maxy=" << maxy << ", minx=" << minx << ", maxx=" << maxx << "] "
                      << "surf[w=" << surf->w << ", h=" << surf->h << "] "
                      << "ascent-maxy=" << (ascent - maxy) << " "
                      << "ascent-miny=" << (ascent - miny)
                      << std::endl;
            SDL_FreeSurface(surf);
        } else {
            std::cout << "Glyph '" << ch << "': failed to render" << std::endl;
        }
    }

    TTF_CloseFont(font);
    TTF_Quit();
    return 0;
}

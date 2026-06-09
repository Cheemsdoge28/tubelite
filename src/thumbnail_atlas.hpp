#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <list>
#include <unordered_map>

// Atlas region for a single thumbnail slot.
struct ThumbAtlasEntry {
    int  page   = -1;   // atlas page index (-1 = invalid)
    int  slot_x =  0;   // pixel X origin within page
    int  slot_y =  0;   // pixel Y origin within page
    bool valid  = false;
};

/**
 * Packs video thumbnails into persistent atlas pages.
 *
 * Each page is PAGE_W x PAGE_H, holding PER_PAGE slots of SLOT_W x SLOT_H.
 * Thumbnails are uploaded once via SDL_UpdateTexture (-> glTexSubImage2D)
 * and rendered afterward via SDL_RenderCopy with a src rect (= UV quad).
 *
 * LRU eviction recycles the oldest slot when the atlas is full.
 * Atlas page textures are NEVER destroyed until clear() is called.
 *
 * Thread safety: all public methods must be called from the main (SDL) thread.
 */
class ThumbnailAtlas {
public:
    static constexpr int SLOT_W    = 160;
    static constexpr int SLOT_H    = 90;
    static constexpr int PAGE_COLS = 4;
    static constexpr int PAGE_ROWS = 4;
    static constexpr int PER_PAGE  = PAGE_COLS * PAGE_ROWS;  // 16 slots per page
    static constexpr int PAGE_W    = PAGE_COLS * SLOT_W;     // 640 px
    static constexpr int PAGE_H    = PAGE_ROWS * SLOT_H;     // 360 px

    explicit ThumbnailAtlas(SDL_Renderer* renderer, int max_pages = 3);
    ~ThumbnailAtlas();

    /**
     * Upload ARGB8888 pixels for videoId into an atlas slot.
     * Pixels are nearest-neighbour scaled to SLOT_W x SLOT_H if needed.
     * If videoId is already cached, its LRU position is refreshed.
     */
    void upload(const std::string& videoId,
                const uint8_t* argb8888, int src_w, int src_h);

    /** Returns the atlas entry for videoId, or nullptr if not loaded. */
    const ThumbAtlasEntry* get(const std::string& videoId) const;

    /** Returns true if videoId has a valid atlas slot. */
    bool isLoaded(const std::string& videoId) const;

    /**
     * Blit the thumbnail into dst via SDL_RenderCopy (src rect = UV coords).
     * Returns false if not yet loaded (caller should draw a placeholder).
     */
    bool render(SDL_Renderer* renderer,
                const std::string& videoId,
                const SDL_Rect& dst) const;

    /** Destroy all page textures and clear all entries. */
    void clear();

private:
    struct Page { SDL_Texture* tex = nullptr; };

    SDL_Renderer*                                                      renderer_;
    int                                                                max_pages_;
    std::vector<Page>                                                  pages_;
    int                                                                total_slots_;
    int                                                                next_free_ = 0; // grows until full

    // LRU: front = oldest, back = most recently used
    std::list<std::string>                                             lru_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_iters_;
    std::unordered_map<std::string, ThumbAtlasEntry>                  entries_;

    bool ensurePage(int page_idx);
    // Returns flat slot index to use for a new entry (may evict oldest).
    int  allocSlot();
    void slotToPixelOrigin(int slot, int& px, int& py, int& page_idx) const;
    void touchLRU(const std::string& id);
};

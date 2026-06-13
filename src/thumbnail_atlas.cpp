#include "thumbnail_atlas.hpp"
#include <algorithm>
#include <iostream>

ThumbnailAtlas::ThumbnailAtlas(SDL_Renderer* renderer, int max_pages)
    : renderer_(renderer), max_pages_(max_pages) {
    total_slots_ = max_pages_ * PER_PAGE;
    pages_.resize(max_pages_);
}

ThumbnailAtlas::~ThumbnailAtlas() {
    clear();
}

bool ThumbnailAtlas::ensurePage(int page_idx) {
    if (page_idx < 0 || page_idx >= max_pages_) return false;
    if (pages_[page_idx].tex != nullptr) return true;

    SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, PAGE_W, PAGE_H);
    if (!tex) {
        std::cerr << "[Atlas] Failed to create atlas page texture: " << SDL_GetError() << "\n";
        return false;
    }

    // Pre-fill with black pixels
    std::vector<uint32_t> black(PAGE_W * PAGE_H, 0xFF000000);
    SDL_UpdateTexture(tex, nullptr, black.data(), PAGE_W * 4);

    pages_[page_idx].tex = tex;
    return true;
}

int ThumbnailAtlas::allocSlot() {
    if (next_free_ < total_slots_) {
        return next_free_++;
    }

    // Evict oldest (LRU front)
    if (lru_.empty()) return 0; // fallback

    std::string evicted = lru_.front();
    auto it = entries_.find(evicted);
    int slot = -1;
    if (it != entries_.end()) {
        slot = it->second.page * PER_PAGE +
               (it->second.slot_y / SLOT_H) * PAGE_COLS +
               (it->second.slot_x / SLOT_W);
        lru_iters_.erase(evicted);
        entries_.erase(evicted);
    }
    lru_.pop_front();

    if (slot == -1) {
        slot = 0; // fallback
    }
    return slot;
}

void ThumbnailAtlas::slotToPixelOrigin(int slot, int& px, int& py, int& page_idx) const {
    page_idx = slot / PER_PAGE;
    int page_slot = slot % PER_PAGE;
    int row = page_slot / PAGE_COLS;
    int col = page_slot % PAGE_COLS;
    px = col * SLOT_W;
    py = row * SLOT_H;
}

void ThumbnailAtlas::touchLRU(const std::string& id) {
    auto it = lru_iters_.find(id);
    if (it != lru_iters_.end()) {
        lru_.erase(it->second);
    }
    lru_.push_back(id);
    lru_iters_[id] = std::prev(lru_.end());
}

void ThumbnailAtlas::upload(const std::string& videoId,
                            const uint8_t* argb8888, int src_w, int src_h) {
    if (!argb8888 || src_w <= 0 || src_h <= 0) return;

    int slot = -1;
    auto it = entries_.find(videoId);
    if (it != entries_.end()) {
        ThumbAtlasEntry& entry = it->second;
        slot = entry.page * PER_PAGE +
               (entry.slot_y / SLOT_H) * PAGE_COLS +
               (entry.slot_x / SLOT_W);
        touchLRU(videoId);
    } else {
        slot = allocSlot();
        int px = 0, py = 0, page_idx = 0;
        slotToPixelOrigin(slot, px, py, page_idx);
        if (!ensurePage(page_idx)) return;

        ThumbAtlasEntry entry;
        entry.page = page_idx;
        entry.slot_x = px;
        entry.slot_y = py;
        entry.valid = true;

        entries_[videoId] = entry;
        touchLRU(videoId);
    }

    // Crop source image to 16:9 and scale to SLOT_W x SLOT_H using nearest neighbor
    std::vector<uint32_t> slot_pixels(SLOT_W * SLOT_H);
    int crop_w = src_w;
    int crop_h = src_w * 9 / 16;
    int crop_x = 0;
    int crop_y = (src_h - crop_h) / 2;
    if (crop_h > src_h) {
        crop_h = src_h;
        crop_w = src_h * 16 / 9;
        crop_x = (src_w - crop_w) / 2;
        crop_y = 0;
    }

    const uint32_t* src32 = reinterpret_cast<const uint32_t*>(argb8888);
    for (int dy = 0; dy < SLOT_H; ++dy) {
        int sy = crop_y + (dy * crop_h) / SLOT_H;
        sy = std::max(0, std::min(src_h - 1, sy));
        for (int dx = 0; dx < SLOT_W; ++dx) {
            int sx = crop_x + (dx * crop_w) / SLOT_W;
            sx = std::max(0, std::min(src_w - 1, sx));
            slot_pixels[dy * SLOT_W + dx] = src32[sy * src_w + sx];
        }
    }

    ThumbAtlasEntry& entry = entries_[videoId];
    SDL_Rect dstRect{entry.slot_x, entry.slot_y, SLOT_W, SLOT_H};
    SDL_UpdateTexture(pages_[entry.page].tex, &dstRect, slot_pixels.data(), SLOT_W * 4);
}

const ThumbAtlasEntry* ThumbnailAtlas::get(const std::string& videoId) const {
    auto it = entries_.find(videoId);
    if (it != entries_.end() && it->second.valid) {
        return &it->second;
    }
    return nullptr;
}

bool ThumbnailAtlas::isLoaded(const std::string& videoId) const {
    auto it = entries_.find(videoId);
    return (it != entries_.end() && it->second.valid);
}

bool ThumbnailAtlas::render(SDL_Renderer* renderer,
                            const std::string& videoId,
                            const SDL_Rect& dst) const {
    auto it = entries_.find(videoId);
    if (it == entries_.end() || !it->second.valid) return false;

    // Touch LRU (non-const operation on logical cache)
    const_cast<ThumbnailAtlas*>(this)->touchLRU(videoId);

    const ThumbAtlasEntry& entry = it->second;
    SDL_Rect srcRect{entry.slot_x, entry.slot_y, SLOT_W, SLOT_H};
    SDL_RenderCopy(renderer, pages_[entry.page].tex, &srcRect, &dst);
    return true;
}

void ThumbnailAtlas::clear() {
    for (auto& p : pages_) {
        if (p.tex) {
            SDL_DestroyTexture(p.tex);
            p.tex = nullptr;
        }
    }
    entries_.clear();
    lru_.clear();
    lru_iters_.clear();
    next_free_ = 0;
}

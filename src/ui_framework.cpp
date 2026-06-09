#include "ui_framework.hpp"
#include "renderer_utils.hpp"
#include <cmath>
#include <algorithm>

namespace ui {

VideoCard::VideoCard(ImageManager* im, const YouTubeVideo& video)
    : im_(im), video(video) {
    focusable = true;
    bounds.w = 300; 
    bounds.h = 240; 
}

void VideoCard::update(float dt) {
    (void)dt;
    targetScale = 1.0f;
    scale = 1.0f;
    if (focused) {
        focusedTime_ += dt;
    } else {
        focusedTime_ = 0.0f;
    }
}

void VideoCard::render(SDL_Renderer* renderer, float offsetX, float offsetY) {
    float cx = bounds.x + offsetX + bounds.w / 2.0f;
    float cy = bounds.y + offsetY + bounds.h / 2.0f;
    
    float w = bounds.w * scale;
    float h = bounds.h * scale;
    float x = cx - w / 2.0f;
    float y = cy - h / 2.0f;
    
    SDL_Rect cardRect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};

    SDL_Texture* thumb = im_->getThumbnail(video.id);
    bool horizontal = (bounds.w > 400);
    int thumbW = horizontal ? 160 : static_cast<int>(bounds.w);
    int thumbH = horizontal ? 90 : static_cast<int>(bounds.w * (9.0f / 16.0f));

    // Card background — when previewing in portrait layout, skip thumbnail area
    // so the mpv GLES video (rendered in the first pass) shows through.
    if (is_previewing && !horizontal) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, 26, 26, 26, 255);
        SDL_Rect textSection{cardRect.x, cardRect.y + thumbH, cardRect.w, cardRect.h - thumbH};
        SDL_RenderFillRect(renderer, &textSection);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    } else {
        fillRoundedRect(renderer, cardRect, 8, {26, 26, 26, 255});
    }

    SDL_Rect thumbRect{cardRect.x, cardRect.y, thumbW, thumbH};
    
    if (is_previewing) {
        // Do nothing: GLES video frame was already drawn to this region in the first pass
    } else {
        if (thumb) {
            int texW = 0, texH = 0;
            SDL_QueryTexture(thumb, nullptr, nullptr, &texW, &texH);
            
            int srcW = texW;
            int srcH = texW * 9 / 16;
            int srcX = 0;
            int srcY = (texH - srcH) / 2;
            if (srcH > texH) {
                srcH = texH;
                srcW = texH * 16 / 9;
                srcX = (texW - srcW) / 2;
                srcY = 0;
            }
            SDL_Rect srcRect{srcX, srcY, srcW, srcH};
            
            SDL_RenderCopy(renderer, thumb, &srcRect, &thumbRect);
        } else {
            SDL_SetRenderDrawColor(renderer, 37, 37, 37, 255); // Fallback thumb background (#252525)
            SDL_RenderFillRect(renderer, &thumbRect);
        }
    }
    
    int maxPixelW = horizontal ? (static_cast<int>(bounds.w) - thumbW - 24) : (static_cast<int>(bounds.w) - 20);
    int textX = cardRect.x + (horizontal ? thumbW + 12 : 10);
    int textY = cardRect.y + (horizontal ? 8 : thumbH + 8);
    if (!layout_cached_) {
        int tempMaxW = horizontal ? (static_cast<int>(bounds.w) - thumbW - 24) : (static_cast<int>(bounds.w) - 20);
        
        // 1. Title layout cache
        getTextSize(video.title, 2, &titleW_, nullptr);
        if (titleW_ > tempMaxW) {
            std::string ell = "...";
            int ellW = 0;
            getTextSize(ell, 2, &ellW, nullptr);
            int targetW = tempMaxW - ellW;
            size_t len = utf8Length(video.title);
            truncated_title_ = video.title;
            while (len > 0) {
                std::string temp = utf8Slice(video.title, 0, len);
                int tempW = 0;
                getTextSize(temp, 2, &tempW, nullptr);
                if (tempW <= targetW) {
                    truncated_title_ = temp + ell;
                    break;
                }
                len--;
            }
        } else {
            truncated_title_ = video.title;
        }
        
        // 2. Meta layout cache
        std::string meta = video.author;
        if (!video.view_count_string.empty()) meta += " | " + video.view_count_string;
        getTextSize(meta, 1, &metaW_, nullptr);
        if (metaW_ > tempMaxW) {
            std::string ell = "...";
            int ellW = 0;
            getTextSize(ell, 1, &ellW, nullptr);
            int targetW = tempMaxW - ellW;
            size_t len = utf8Length(meta);
            truncated_meta_ = meta;
            while (len > 0) {
                std::string temp = utf8Slice(meta, 0, len);
                int tempW = 0;
                getTextSize(temp, 1, &tempW, nullptr);
                if (tempW <= targetW) {
                    truncated_meta_ = temp + ell;
                    break;
                }
                len--;
            }
        } else {
            truncated_meta_ = meta;
        }
        
        layout_cached_ = true;
    }
    
    if (titleW_ > maxPixelW) {
        if (focused && focusedTime_ > 1.5f) {
            int maxScroll = titleW_ - maxPixelW + 20;
            int scrollOffset = static_cast<int>((focusedTime_ - 1.5f) * 35.0f);
            if (scrollOffset > maxScroll) {
                if (scrollOffset > maxScroll + 35) {
                    focusedTime_ = 1.5f;
                    scrollOffset = 0;
                } else {
                    scrollOffset = maxScroll;
                }
            }
            
            SDL_Rect textClip{textX, textY, maxPixelW, 30};
            SDL_Rect oldClip;
            SDL_RenderGetClipRect(renderer, &oldClip);
            SDL_bool hasOldClip = SDL_RenderIsClipEnabled(renderer);
            
            SDL_Rect activeClip = textClip;
            if (hasOldClip) {
                int cx1 = std::max(activeClip.x, oldClip.x);
                int cy1 = std::max(activeClip.y, oldClip.y);
                int cx2 = std::min(activeClip.x + activeClip.w, oldClip.x + oldClip.w);
                int cy2 = std::min(activeClip.y + activeClip.h, oldClip.y + oldClip.h);
                if (cx2 > cx1 && cy2 > cy1) {
                    activeClip = {cx1, cy1, cx2 - cx1, cy2 - cy1};
                } else {
                    activeClip = {0, 0, 0, 0};
                }
            }
            SDL_RenderSetClipRect(renderer, &activeClip);
            drawText(renderer, textX - scrollOffset, textY, video.title, 2, {240, 240, 240, 255});
            SDL_RenderSetClipRect(renderer, hasOldClip ? &oldClip : nullptr);
        } else {
            drawText(renderer, textX, textY, truncated_title_, 2, {240, 240, 240, 255});
        }
    } else {
        drawText(renderer, textX, textY, video.title, 2, {240, 240, 240, 255});
    }
    
    drawText(renderer, textX, textY + 25, truncated_meta_, 1, {150, 150, 150, 255});
    
    if (!video.duration_string.empty()) {
        int durW = 0, durH = 0;
        getTextSize(video.duration_string, 1, &durW, &durH);
        int pillW = durW + 8;
        int pillH = durH + 4;
        int pillX = cardRect.x + thumbW - pillW - 6;
        int pillY = cardRect.y + thumbH - pillH - 6;
        
        SDL_Rect pillRect{pillX, pillY, pillW, pillH};
        fillRoundedRect(renderer, pillRect, 3, {0, 0, 0, 180});
        drawText(renderer, pillX + 4, pillY + 2, video.duration_string, 1, {255, 255, 255, 255});
    }

    // maskRoundedCorners paints background-colour pixels into the corners; when
    // previewing this would overwrite the mpv video corners, so skip it.
    if (!is_previewing) {
        maskRoundedCorners(renderer, cardRect, 8, {15, 15, 15, 255});
    }

    // Draw card border on top of masked corners
    if (focused) {
        drawRoundedRect(renderer, cardRect, 8, {48, 48, 52, 255});
    }
}

void GridContainer::addCard(std::shared_ptr<VideoCard> card) {
    cards.push_back(card);
    int idx = cards.size() - 1;
    int row = idx / columns;
    int col = idx % columns;
    
    card->bounds.w = (bounds.w - padding * (columns + 1)) / static_cast<float>(columns);
    if (columns == 1) {
        card->bounds.h = 90.0f;
    } else {
        card->bounds.h = card->bounds.w * (9.0f / 16.0f) + 54.0f;
    }
    
    card->bounds.x = bounds.x + padding + col * (card->bounds.w + padding);
    card->bounds.y = bounds.y + padding + row * (card->bounds.h + padding);
}

void GridContainer::pruneOldCards(int maxCards, int& focusedCardIdx) {
    if (static_cast<int>(cards.size()) <= maxCards) return;
    
    int pruneRows = (static_cast<int>(cards.size()) - maxCards + columns - 1) / columns;
    int pruneCount = pruneRows * columns;
    if (pruneCount >= static_cast<int>(cards.size())) return;
    
    float cardH = (columns == 1) ? 90.0f : ((bounds.w - padding * (columns + 1)) / static_cast<float>(columns) * (9.0f / 16.0f) + 54.0f);
    float rowHeight = cardH + padding;
    float removedHeight = pruneRows * rowHeight;
    
    if (scrollY < removedHeight) {
        return;
    }
    
    cards.erase(cards.begin(), cards.begin() + pruneCount);
    
    scrollY = std::max(0.0f, scrollY - removedHeight);
    targetScrollY = std::max(0.0f, targetScrollY - removedHeight);
    
    focusedCardIdx = std::max(0, focusedCardIdx - pruneCount);
    
    for (size_t idx = 0; idx < cards.size(); ++idx) {
        auto& card = cards[idx];
        int row = idx / columns;
        int col = idx % columns;
        
        card->bounds.w = (bounds.w - padding * (columns + 1)) / static_cast<float>(columns);
        card->bounds.h = cardH;
        card->bounds.x = bounds.x + padding + col * (card->bounds.w + padding);
        card->bounds.y = bounds.y + padding + row * (card->bounds.h + padding);
    }
}

void GridContainer::update(float dt) {
    (void)dt;
    scrollY = targetScrollY;
    for (auto& c : cards) c->update(dt);
}

SDL_Rect GridContainer::viewportRect(float offsetX, float offsetY) const {
    return {
        static_cast<int>(bounds.x + offsetX),
        static_cast<int>(bounds.y + offsetY),
        static_cast<int>(bounds.w),
        static_cast<int>(bounds.h)
    };
}

void GridContainer::render(SDL_Renderer* renderer, float offsetX, float offsetY) {
    const SDL_Rect clip = viewportRect(offsetX, offsetY);
    SDL_RenderSetClipRect(renderer, &clip);
    for (auto& c : cards) {
        float cy = c->bounds.y + offsetY - scrollY;
        if (cy + c->bounds.h > bounds.y && cy < bounds.y + bounds.h) {
            c->render(renderer, offsetX, offsetY - scrollY);
        }
    }
    SDL_RenderSetClipRect(renderer, nullptr);
}

void FocusManager::setGrid(std::shared_ptr<GridContainer> grid) {
    grid_ = grid;
    focusedCardIdx_ = 0;
    updateTargetFocus();
    currentFocusRing_ = targetFocusRing_;
}

void FocusManager::updateTargetFocus() {
    if (!grid_ || grid_->cards.empty()) return;
    focusedCardIdx_ = std::clamp(focusedCardIdx_, 0, static_cast<int>(grid_->cards.size()) - 1);
    
    for (auto& c : grid_->cards) c->focused = false;
    
    auto card = grid_->cards[focusedCardIdx_];
    card->focused = true;
    
    targetFocusRing_ = card->bounds;
    float headerOffset = grid_->bounds.y;
    float screenH = grid_->bounds.y + grid_->bounds.h;
    float cy = card->bounds.y - grid_->scrollY;
    
    if (focusedCardIdx_ < grid_->columns) {
        grid_->targetScrollY = 0.0f;
    } else if (cy < headerOffset) {
        grid_->targetScrollY = card->bounds.y - headerOffset;
    } else if (cy + card->bounds.h > screenH) {
        grid_->targetScrollY = card->bounds.y + card->bounds.h - screenH;
    }
    grid_->targetScrollY = std::max(0.0f, grid_->targetScrollY);
}

void FocusManager::handleInput(int dx, int dy) {
    if (!grid_ || grid_->cards.empty()) return;
    int maxCols = grid_->columns;
    int row = focusedCardIdx_ / maxCols;
    int col = focusedCardIdx_ % maxCols;
    
    col += dx;
    if (col < 0) col = 0;
    if (col >= maxCols) col = maxCols - 1;
    
    row += dy;
    if (row < 0) row = 0;
    
    int newIdx = row * maxCols + col;
    if (newIdx >= static_cast<int>(grid_->cards.size())) {
        newIdx = grid_->cards.size() - 1;
    }
    
    if (newIdx != focusedCardIdx_) {
        focusedCardIdx_ = newIdx;
        updateTargetFocus();
        
        if (newIdx >= static_cast<int>(grid_->cards.size()) - (maxCols * 2)) {
            if (grid_->onScrolledToBottom) grid_->onScrolledToBottom();
        }
    }
}

void FocusManager::update(float dt) {
    (void)dt;
    if (grid_) grid_->update(dt);
    
    if (grid_ && !grid_->cards.empty()) {
        float scroll = grid_->scrollY;
        currentFocusRing_.x = targetFocusRing_.x;
        currentFocusRing_.y = targetFocusRing_.y - scroll;
        currentFocusRing_.w = targetFocusRing_.w;
        currentFocusRing_.h = targetFocusRing_.h;
    }
}

void FocusManager::renderFocusRing(SDL_Renderer* renderer, float offsetX, float offsetY) {
    if (!grid_ || grid_->cards.empty()) return;
    const SDL_Rect clip = grid_->viewportRect(offsetX, offsetY);
    SDL_RenderSetClipRect(renderer, &clip);
    
    float w = currentFocusRing_.w;
    float h = currentFocusRing_.h;
    float cx = currentFocusRing_.x + offsetX + currentFocusRing_.w / 2.0f;
    float cy = currentFocusRing_.y + offsetY + currentFocusRing_.h / 2.0f;
    
    SDL_Rect ring{
        static_cast<int>(cx - w / 2.0f) - 4,
        static_cast<int>(cy - h / 2.0f) - 4,
        static_cast<int>(w) + 8,
        static_cast<int>(h) + 8
    };
    
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    drawRoundedRect(renderer, ring, 10, {255, 255, 255, 220});
    ring.x -= 1; ring.y -= 1; ring.w += 2; ring.h += 2;
    drawRoundedRect(renderer, ring, 11, {255, 48, 48, 255});
    SDL_RenderSetClipRect(renderer, nullptr);
}

std::shared_ptr<VideoCard> FocusManager::getFocusedCard() const {
    if (!grid_ || grid_->cards.empty()) return nullptr;
    return grid_->cards[focusedCardIdx_];
}

void FocusManager::clickFocused() {
    auto card = getFocusedCard();
    if (card && card->onClick) card->onClick();
}

void FocusManager::pruneGridIfNeeded(int maxCards) {
    if (!grid_) return;
    grid_->pruneOldCards(maxCards, focusedCardIdx_);
    updateTargetFocus();
}

} // namespace ui

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

    bool horizontal = (bounds.w > 400);
    int thumbW = horizontal ? 160 : static_cast<int>(bounds.w);
    int thumbH = horizontal ? 90 : static_cast<int>(bounds.w * (9.0f / 16.0f));

    // Card background — when previewing, skip thumbnail area
    // so the mpv GLES video (rendered in the first pass) shows through.
    if (is_previewing) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, theme::SURFACE.r, theme::SURFACE.g, theme::SURFACE.b, 255);
        if (horizontal) {
            SDL_Rect textSection{cardRect.x + thumbW, cardRect.y, cardRect.w - thumbW, cardRect.h};
            SDL_RenderFillRect(renderer, &textSection);
        } else {
            SDL_Rect textSection{cardRect.x, cardRect.y + thumbH, cardRect.w, cardRect.h - thumbH};
            SDL_RenderFillRect(renderer, &textSection);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    } else {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, theme::SURFACE.r, theme::SURFACE.g, theme::SURFACE.b, 255);
        SDL_RenderFillRect(renderer, &cardRect);
        // Subtle 1px top accent line for visual separation
        SDL_SetRenderDrawColor(renderer, theme::HAIRLINE.r, theme::HAIRLINE.g, theme::HAIRLINE.b, 255);
        SDL_Rect topLine{cardRect.x, cardRect.y, cardRect.w, 1};
        SDL_RenderFillRect(renderer, &topLine);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    }

    SDL_Rect thumbRect{cardRect.x, cardRect.y, thumbW, thumbH};
    
    if (is_previewing) {
        // Do nothing: GLES video frame was already drawn to this region in the first pass
    } else {
        if (!im_->renderThumbnail(renderer, video.id, thumbRect)) {
            SDL_SetRenderDrawColor(renderer, theme::THUMB_BG.r, theme::THUMB_BG.g, theme::THUMB_BG.b, 255); // Fallback thumb background
            SDL_RenderFillRect(renderer, &thumbRect);
        }
    }
       int textX = cardRect.x + (horizontal ? thumbW + 12 : 10);
    int textY = cardRect.y + (horizontal ? 8 : thumbH + 8);
    if (!layout_cached_) {
        int tempMaxW = horizontal ? (static_cast<int>(bounds.w) - thumbW - 24) : (static_cast<int>(bounds.w) - 20);
        maxPixelW_ = tempMaxW;
        
        // 1. Title layout cache
        getTextSize(video.title, 2, &titleW_, nullptr);
        if (titleW_ > tempMaxW) {
            std::string ell = "...";
            int ellW = 0;
            getTextSize(ell, 2, &ellW, nullptr);
            int targetW = tempMaxW - ellW;
            size_t len = utf8Length(video.title);
            
            size_t low = 0;
            size_t high = len;
            size_t best_len = 0;
            while (low <= high) {
                size_t mid = low + (high - low) / 2;
                std::string temp = utf8Slice(video.title, 0, mid);
                int tempW = 0;
                getTextSize(temp, 2, &tempW, nullptr);
                if (tempW <= targetW) {
                    best_len = mid;
                    low = mid + 1;
                } else {
                    if (mid == 0) break;
                    high = mid - 1;
                }
            }
            truncated_title_ = utf8Slice(video.title, 0, best_len) + ell;
        } else {
            truncated_title_ = video.title;
        }
        
        // 2. Author/Channel layout cache
        std::string author_str = video.author;
        getTextSize(author_str, 1, &authorW_, nullptr);
        if (authorW_ > tempMaxW) {
            std::string ell = "...";
            int ellW = 0;
            getTextSize(ell, 1, &ellW, nullptr);
            int targetW = tempMaxW - ellW;
            size_t len = utf8Length(author_str);
            size_t low = 0, high = len, best_len = 0;
            while (low <= high) {
                size_t mid = low + (high - low) / 2;
                std::string temp = utf8Slice(author_str, 0, mid);
                int tempW = 0;
                getTextSize(temp, 1, &tempW, nullptr);
                if (tempW <= targetW) {
                    best_len = mid;
                    low = mid + 1;
                } else {
                    if (mid == 0) break;
                    high = mid - 1;
                }
            }
            truncated_author_ = utf8Slice(author_str, 0, best_len) + ell;
        } else {
            truncated_author_ = author_str;
        }

        // 3. Views & Date layout cache
        std::string views_date = video.view_count_string;
        if (!video.uploaded_ago_string.empty()) {
            if (!views_date.empty()) views_date += " • "; // safe separator dot/dash
            views_date += video.uploaded_ago_string;
        }
        getTextSize(views_date, 1, &viewsDateW_, nullptr);
        if (viewsDateW_ > tempMaxW) {
            std::string ell = "...";
            int ellW = 0;
            getTextSize(ell, 1, &ellW, nullptr);
            int targetW = tempMaxW - ellW;
            size_t len = utf8Length(views_date);
            size_t low = 0, high = len, best_len = 0;
            while (low <= high) {
                size_t mid = low + (high - low) / 2;
                std::string temp = utf8Slice(views_date, 0, mid);
                int tempW = 0;
                getTextSize(temp, 1, &tempW, nullptr);
                if (tempW <= targetW) {
                    best_len = mid;
                    low = mid + 1;
                } else {
                    if (mid == 0) break;
                    high = mid - 1;
                }
            }
            truncated_views_date_ = utf8Slice(views_date, 0, best_len) + ell;
        } else {
            truncated_views_date_ = views_date;
        }
        
        getTextSize("Ay", 2, nullptr, &titleH_);  // cap_height for scale-2 font
        getTextSize("Ay", 1, nullptr, &metaH_);   // cap_height for scale-1 font
        if (!video.duration_string.empty()) {
            getTextSize(video.duration_string, 1, &durW_, &durH_);
        }
        
        layout_cached_ = true;
    }
    
    const int lineGap = 5;

    if (titleW_ > maxPixelW_) {
        if (focused && focusedTime_ > 1.5f && focusedTime_ < 25.0f) {
            int maxScroll = titleW_ - maxPixelW_ + 20;
            int scrollOffset = static_cast<int>((focusedTime_ - 1.5f) * 35.0f);
            if (scrollOffset > maxScroll) {
                if (scrollOffset > maxScroll + 35) {
                    focusedTime_ = 1.5f;
                    scrollOffset = 0;
                } else {
                    scrollOffset = maxScroll;
                }
            }
            
            SDL_Rect textClip{textX, textY, maxPixelW_, titleH_ + 4};
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
            drawText(renderer, textX - scrollOffset, textY, video.title, 2, theme::TEXT);
            SDL_RenderSetClipRect(renderer, hasOldClip ? &oldClip : nullptr);
        } else {
            drawText(renderer, textX, textY, truncated_title_, 2, theme::TEXT);
        }
    } else {
        drawText(renderer, textX, textY, video.title, 2, theme::TEXT);
    }
    
    // Draw Channel Name
    drawText(renderer, textX, textY + titleH_ + lineGap, truncated_author_, 1, theme::TEXT_2);

    // Draw Views & Date line
    drawText(renderer, textX, textY + titleH_ + lineGap + metaH_ + lineGap, truncated_views_date_, 1, theme::TEXT_3);
    
    if (!video.duration_string.empty() && !is_previewing) {
        int pillW = durW_ + 10;
        int pillH = durH_ + 6;
        int pillX = cardRect.x + thumbW - pillW - 5;
        int pillY = cardRect.y + thumbH - pillH - 5;
        int textPillY = pillY + (pillH - durH_) / 2;
        
        SDL_Rect pillRect{pillX, pillY, pillW, pillH};
        fillRoundedRect(renderer, pillRect, theme::RADIUS_PILL, theme::BLACK.a8(210));
        drawText(renderer, pillX + 5, textPillY, video.duration_string, 1, theme::WHITE);
    }

    // maskRoundedCorners paints background-colour pixels into the corners; when
    // previewing this would overwrite the mpv video corners, so skip it.
    if (!is_previewing) {
        maskRoundedCorners(renderer, cardRect, theme::RADIUS_CARD, theme::BG);
    }

    // Draw card border on top of masked corners
    if (focused) {
        drawRoundedRect(renderer, cardRect, theme::RADIUS_CARD, theme::BORDER);
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
        // thumbnail + (8 top pad) + title (28) + 5 gap + channel (14) + 5 gap
        // + views/date (14) + (8 bottom pad) = thumb + 82.  Bottom padding now
        // matches the top so the views/date line isn't visually clipped.
        card->bounds.h = card->bounds.w * (9.0f / 16.0f) + 82.0f;
    }

    card->bounds.x = bounds.x + padding + col * (card->bounds.w + padding);
    card->bounds.y = bounds.y + padding + row * (card->bounds.h + padding);
}

void GridContainer::pruneOldCards(int maxCards, int& focusedCardIdx) {
    if (static_cast<int>(cards.size()) <= maxCards) return;
    
    int pruneRows = (static_cast<int>(cards.size()) - maxCards + columns - 1) / columns;
    int pruneCount = pruneRows * columns;
    if (pruneCount >= static_cast<int>(cards.size())) return;
    
    float cardH = (columns == 1) ? 90.0f : ((bounds.w - padding * (columns + 1)) / static_cast<float>(columns) * (9.0f / 16.0f) + 82.0f);
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
    // Smooth scroll lerp — feels premium, especially on the handheld touchpad
    const float lerpSpeed = 18.0f;
    float alpha = std::min(1.0f, dt * lerpSpeed);
    scrollY = scrollY + (targetScrollY - scrollY) * alpha;
    // Snap to rest when very close to avoid sub-pixel jitter
    if (std::abs(scrollY - targetScrollY) < 0.5f) scrollY = targetScrollY;
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
    if (grid_) {
        gridFocusIndices_[grid_.get()] = focusedCardIdx_;
        for (auto& c : grid_->cards) {
            c->focused = false;
        }
    }
    
    grid_ = grid;
    
    if (grid_) {
        auto it = gridFocusIndices_.find(grid_.get());
        if (it != gridFocusIndices_.end()) {
            focusedCardIdx_ = it->second;
        } else {
            focusedCardIdx_ = 0;
        }
        updateTargetFocus();
        currentFocusRing_ = targetFocusRing_;
    } else {
        focusedCardIdx_ = 0;
        currentFocusRing_ = {0, 0, 0, 0};
        targetFocusRing_ = {0, 0, 0, 0};
    }
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
        
        // Speculative prefetching: load more cards when user gets 70% of the way through current grid
        if (newIdx >= static_cast<int>(grid_->cards.size()) * 7 / 10) {
            if (grid_->onScrolledToBottom) grid_->onScrolledToBottom();
        }
    }
}

void FocusManager::update(float dt) {
    if (grid_) {
        grid_->update(dt);
        if (!grid_->cards.empty()) {
            focusedCardIdx_ = std::max(0, std::min(focusedCardIdx_, static_cast<int>(grid_->cards.size()) - 1));
            auto card = grid_->cards[focusedCardIdx_];
            if (card) {
                card->focused = true;
                targetFocusRing_ = card->bounds;
            }
            float scroll = grid_->scrollY;
            currentFocusRing_.x = targetFocusRing_.x;
            currentFocusRing_.y = targetFocusRing_.y - scroll;
            currentFocusRing_.w = targetFocusRing_.w;
            currentFocusRing_.h = targetFocusRing_.h;
        }
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
    // Outer glow ring
    SDL_Rect glow{
        ring.x - 2,
        ring.y - 2,
        ring.w + 4,
        ring.h + 4
    };
    drawRoundedRect(renderer, glow, 12, theme::ACCENT.a8(70));
    drawRoundedRect(renderer, ring, 10, theme::WHITE.a8(200));
    ring.x -= 1; ring.y -= 1; ring.w += 2; ring.h += 2;
    drawRoundedRect(renderer, ring, 11, theme::ACCENT);
    SDL_RenderSetClipRect(renderer, nullptr);
}

std::shared_ptr<VideoCard> FocusManager::getFocusedCard() const {
    if (!grid_ || grid_->cards.empty()) return nullptr;
    int idx = std::max(0, std::min(focusedCardIdx_, static_cast<int>(grid_->cards.size()) - 1));
    return grid_->cards[idx];
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

void FocusManager::setFocusedIndex(int index) {
    if (!grid_ || grid_->cards.empty()) return;
    focusedCardIdx_ = std::clamp(index, 0, static_cast<int>(grid_->cards.size()) - 1);
    updateTargetFocus();
    currentFocusRing_ = targetFocusRing_;
}

void FocusManager::resetGridFocus(std::shared_ptr<GridContainer> grid) {
    if (grid) {
        gridFocusIndices_[grid.get()] = 0;
        if (grid_ == grid) {
            focusedCardIdx_ = 0;
            updateTargetFocus();
        }
    }
}

} // namespace ui

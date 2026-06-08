#include "ui_framework.hpp"
#include "renderer_utils.hpp"
#include <cmath>
#include <algorithm>

namespace ui {

static float lerp(float a, float b, float dt, float speed = 10.0f) {
    return b; // Removed lerp entirely per user request for performance
}

VideoCard::VideoCard(ImageManager* im, const YouTubeVideo& video)
    : im_(im), video(video) {
    focusable = true;
    bounds.w = 300; 
    bounds.h = 240; 
}

void VideoCard::update(float /*dt*/) {
    // Scaling removed to save CPU. 
    // The focus border is sufficient for handheld feedback.
    targetScale = 1.0f;
    scale = 1.0f;
}

void VideoCard::render(SDL_Renderer* renderer, float offsetX, float offsetY) {
    float cx = bounds.x + offsetX + bounds.w / 2.0f;
    float cy = bounds.y + offsetY + bounds.h / 2.0f;
    
    float w = bounds.w * scale;
    float h = bounds.h * scale;
    float x = cx - w / 2.0f;
    float y = cy - h / 2.0f;
    
    SDL_Rect cardRect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};
    
    SDL_SetRenderDrawColor(renderer, 40, 44, 50, 255);
    SDL_RenderFillRect(renderer, &cardRect);
    
    SDL_Texture* thumb = im_->getThumbnail(video.id);
    bool horizontal = (bounds.w > 400);
    
    int thumbW = horizontal ? 160 : static_cast<int>(bounds.w);
    int thumbH = horizontal ? 90 : static_cast<int>(bounds.w * (9.0f / 16.0f));
    
    if (thumb) {
        SDL_Rect thumbRect{cardRect.x, cardRect.y, thumbW, thumbH};
        SDL_RenderCopy(renderer, thumb, nullptr, &thumbRect);
    } else {
        SDL_SetRenderDrawColor(renderer, 30, 32, 38, 255);
        SDL_Rect thumbRect{cardRect.x, cardRect.y, thumbW, thumbH};
        SDL_RenderFillRect(renderer, &thumbRect);
    }
    
    std::string title = video.title;
    int maxChars = horizontal ? ((bounds.w - thumbW) / 12) : (bounds.w / 12);
    if (title.length() > static_cast<size_t>(maxChars)) title = title.substr(0, maxChars - 3) + "...";
    
    int textX = cardRect.x + (horizontal ? thumbW + 10 : 5);
    int textY = cardRect.y + (horizontal ? 5 : thumbH + 5);
    drawText(renderer, textX, textY, title, 2, {240, 240, 240, 255});
    
    std::string meta = video.author;
    if (!video.view_count_string.empty()) meta += " | " + video.view_count_string;
    if (meta.length() > static_cast<size_t>(maxChars + 5)) meta = meta.substr(0, maxChars + 2) + "...";
    drawText(renderer, textX, textY + 25, meta, 1, {150, 150, 150, 255});
    
    if (!video.duration_string.empty()) {
        drawTextShadow(renderer, cardRect.x + thumbW - 35, cardRect.y + thumbH - 15, video.duration_string, 1, {255, 255, 255, 255});
    }
}

void GridContainer::addCard(std::shared_ptr<VideoCard> card) {
    cards.push_back(card);
    int idx = cards.size() - 1;
    int row = idx / columns;
    int col = idx % columns;
    
    card->bounds.w = (640.0f - padding * (columns + 1)) / columns;
    if (columns == 1) {
        card->bounds.h = 90.0f;
    } else {
        card->bounds.h = card->bounds.w * (9.0f / 16.0f) + 40.0f;
    }
    
    card->bounds.x = bounds.x + padding + col * (card->bounds.w + padding);
    card->bounds.y = bounds.y + padding + row * (card->bounds.h + padding) + 30.0f; // 30px offset for title
}

void GridContainer::update(float dt) {
    scrollY = targetScrollY;
    for (auto& c : cards) c->update(dt);
}

void GridContainer::render(SDL_Renderer* renderer, float offsetX, float offsetY) {
    drawTextShadow(renderer, bounds.x + offsetX + 20, bounds.y + offsetY - scrollY + 5, title, 2, {255, 255, 255, 255});
    for (auto& c : cards) {
        float cy = c->bounds.y + offsetY - scrollY;
        if (cy + c->bounds.h > 0 && cy < 480) { 
            c->render(renderer, offsetX, offsetY - scrollY);
        }
    }
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
    
    float screenH = 480.0f;
    float headerOffset = grid_->bounds.y + 40.0f; // Space for header
    float cy = card->bounds.y - grid_->scrollY;
    
    if (cy < headerOffset) {
        grid_->targetScrollY = card->bounds.y - headerOffset;
    } else if (cy + card->bounds.h > screenH - 20.0f) {
        grid_->targetScrollY = card->bounds.y + card->bounds.h - screenH + 20.0f;
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
    
    auto card = grid_->cards[focusedCardIdx_];
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
    
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &ring);
    ring.x -= 1; ring.y -= 1; ring.w += 2; ring.h += 2;
    SDL_SetRenderDrawColor(renderer, 100, 150, 255, 150);
    SDL_RenderDrawRect(renderer, &ring);
}

std::shared_ptr<VideoCard> FocusManager::getFocusedCard() const {
    if (!grid_ || grid_->cards.empty()) return nullptr;
    return grid_->cards[focusedCardIdx_];
}

void FocusManager::clickFocused() {
    auto card = getFocusedCard();
    if (card && card->onClick) card->onClick();
}

} // namespace ui

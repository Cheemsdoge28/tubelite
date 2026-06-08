#include "ui_framework.hpp"
#include "renderer_utils.hpp"
#include <cmath>
#include <algorithm>

namespace ui {

static float lerp(float a, float b, float dt, float speed = 10.0f) {
    return a + (b - a) * (1.0f - std::exp(-speed * dt));
}

VideoCard::VideoCard(ImageManager* im, const YouTubeVideo& video)
    : im_(im), video(video) {
    focusable = true;
    bounds.w = 160; 
    bounds.h = 130; 
}

void VideoCard::update(float dt) {
    if (focused) targetScale = 1.08f;
    else targetScale = 1.0f;
    scale = lerp(scale, targetScale, dt, 15.0f);
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
    int thumbH = static_cast<int>(90 * scale);
    if (thumb) {
        SDL_Rect thumbRect{cardRect.x, cardRect.y, cardRect.w, thumbH};
        SDL_RenderCopy(renderer, thumb, nullptr, &thumbRect);
    } else {
        SDL_SetRenderDrawColor(renderer, 30, 32, 38, 255);
        SDL_Rect thumbRect{cardRect.x, cardRect.y, cardRect.w, thumbH};
        SDL_RenderFillRect(renderer, &thumbRect);
    }
    
    std::string title = video.title;
    if (title.length() > 25) title = title.substr(0, 22) + "...";
    drawText(renderer, cardRect.x + 5, cardRect.y + thumbH + 5, title, 1, {240, 240, 240, 255});
    
    std::string meta = video.author;
    if (!video.view_count_string.empty()) meta += " | " + video.view_count_string;
    if (meta.length() > 30) meta = meta.substr(0, 27) + "...";
    drawText(renderer, cardRect.x + 5, cardRect.y + thumbH + 18, meta, 1, {150, 150, 150, 255});
    
    if (!video.duration_string.empty()) {
        drawTextShadow(renderer, cardRect.x + cardRect.w - 35, cardRect.y + thumbH - 15, video.duration_string, 1, {255, 255, 255, 255});
    }
}

void HorizontalRail::addCard(std::shared_ptr<VideoCard> card) {
    cards.push_back(card);
    float x = 20.0f;
    for (auto& c : cards) {
        c->bounds.x = x;
        c->bounds.y = bounds.y + 30.0f;
        x += c->bounds.w + 15.0f;
    }
}

void HorizontalRail::update(float dt) {
    scrollX = lerp(scrollX, targetScrollX, dt, 10.0f);
    for (auto& c : cards) c->update(dt);
}

void HorizontalRail::render(SDL_Renderer* renderer, float offsetX, float offsetY) {
    drawTextShadow(renderer, bounds.x + offsetX + 20, bounds.y + offsetY + 5, title, 2, {255, 255, 255, 255});
    for (auto& c : cards) {
        float cx = c->bounds.x + offsetX - scrollX;
        if (cx + c->bounds.w > 0 && cx < 640) { 
            c->render(renderer, offsetX - scrollX, offsetY);
        }
    }
}

void FocusManager::setViews(const std::vector<std::shared_ptr<HorizontalRail>>& rails) {
    rails_ = rails;
    focusedRailIdx_ = 0;
    focusedCardIdx_ = 0;
    updateTargetFocus();
    currentFocusRing_ = targetFocusRing_;
}

void FocusManager::updateTargetFocus() {
    if (rails_.empty()) return;
    focusedRailIdx_ = std::clamp(focusedRailIdx_, 0, static_cast<int>(rails_.size()) - 1);
    auto rail = rails_[focusedRailIdx_];
    
    if (rail->cards.empty()) return;
    focusedCardIdx_ = std::clamp(focusedCardIdx_, 0, static_cast<int>(rail->cards.size()) - 1);
    
    for (auto& r : rails_) {
        for (auto& c : r->cards) c->focused = false;
    }
    
    auto card = rail->cards[focusedCardIdx_];
    card->focused = true;
    
    targetFocusRing_ = card->bounds;
    targetFocusRing_.y = rail->bounds.y + 30.0f;
    
    float screenW = 640.0f;
    float cx = card->bounds.x - rail->scrollX;
    if (cx < 40.0f) rail->targetScrollX = card->bounds.x - 40.0f;
    else if (cx + card->bounds.w > screenW - 40.0f) {
        rail->targetScrollX = card->bounds.x + card->bounds.w - screenW + 40.0f;
    }
    rail->targetScrollX = std::max(0.0f, rail->targetScrollX);
}

void FocusManager::handleInput(int dx, int dy) {
    if (rails_.empty()) return;
    if (dx != 0) {
        focusedCardIdx_ += dx;
        updateTargetFocus();
    }
    if (dy != 0) {
        focusedRailIdx_ += dy;
        updateTargetFocus();
    }
}

void FocusManager::update(float dt) {
    for (auto& r : rails_) r->update(dt);
    
    if (!rails_.empty() && !rails_[focusedRailIdx_]->cards.empty()) {
        auto rail = rails_[focusedRailIdx_];
        float scroll = rail->scrollX;
        currentFocusRing_.x = lerp(currentFocusRing_.x, targetFocusRing_.x - scroll, dt, 15.0f);
        currentFocusRing_.y = lerp(currentFocusRing_.y, targetFocusRing_.y, dt, 15.0f);
        currentFocusRing_.w = lerp(currentFocusRing_.w, targetFocusRing_.w, dt, 15.0f);
        currentFocusRing_.h = lerp(currentFocusRing_.h, targetFocusRing_.h, dt, 15.0f);
    }
}

void FocusManager::renderFocusRing(SDL_Renderer* renderer, float offsetX, float offsetY) {
    if (rails_.empty() || rails_[focusedRailIdx_]->cards.empty()) return;
    
    auto card = rails_[focusedRailIdx_]->cards[focusedCardIdx_];
    float scale = card->scale;
    float w = currentFocusRing_.w * scale;
    float h = currentFocusRing_.h * scale;
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
    if (rails_.empty()) return nullptr;
    auto rail = rails_[focusedRailIdx_];
    if (rail->cards.empty()) return nullptr;
    return rail->cards[focusedCardIdx_];
}

void FocusManager::clickFocused() {
    auto card = getFocusedCard();
    if (card && card->onClick) card->onClick();
}

} // namespace ui

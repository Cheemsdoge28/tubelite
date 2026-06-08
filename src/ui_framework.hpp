#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include "youtube_api.hpp"
#include "image_manager.hpp"

namespace ui {

struct Rect {
    float x, y, w, h;
    SDL_Rect toSDL() const { 
        return {static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)}; 
    }
};

class View {
public:
    virtual ~View() = default;
    virtual void render(SDL_Renderer* renderer, float offsetX, float offsetY) = 0;
    virtual void update(float dt) {}
    
    Rect bounds{0, 0, 0, 0};
    bool focusable = false;
    bool focused = false;
    float scale = 1.0f;
    float targetScale = 1.0f;
    
    std::function<void()> onFocus;
    std::function<void()> onBlur;
    std::function<void()> onClick;
};

class VideoCard : public View {
public:
    VideoCard(ImageManager* im, const YouTubeVideo& video);
    void render(SDL_Renderer* renderer, float offsetX, float offsetY) override;
    void update(float dt) override;
    
    YouTubeVideo video;
private:
    ImageManager* im_;
};

class HorizontalRail : public View {
public:
    void addCard(std::shared_ptr<VideoCard> card);
    void render(SDL_Renderer* renderer, float offsetX, float offsetY) override;
    void update(float dt) override;
    
    std::vector<std::shared_ptr<VideoCard>> cards;
    float scrollX = 0.0f;
    float targetScrollX = 0.0f;
    std::string title;
};

class FocusManager {
public:
    void setViews(const std::vector<std::shared_ptr<HorizontalRail>>& rails);
    void handleInput(int dx, int dy);
    void update(float dt);
    void renderFocusRing(SDL_Renderer* renderer, float offsetX, float offsetY);
    
    std::shared_ptr<VideoCard> getFocusedCard() const;
    void clickFocused();

private:
    std::vector<std::shared_ptr<HorizontalRail>> rails_;
    int focusedRailIdx_ = 0;
    int focusedCardIdx_ = 0;
    Rect currentFocusRing_{0,0,0,0};
    Rect targetFocusRing_{0,0,0,0};
    
    void updateTargetFocus();
};

} // namespace ui

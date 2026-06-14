#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include "youtube_api.hpp"
#include "image_manager.hpp"
#include <unordered_map>


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
    virtual void update(float /*dt*/) {}
    
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
    
    ImageManager* im_;
    YouTubeVideo video;
    float focusedTime_ = 0.0f;

    bool layout_cached_ = false;
    int titleW_ = 0;
    int authorW_ = 0;
    int viewsDateW_ = 0;
    int titleH_ = 0;
    int metaH_ = 0;
    int durW_ = 0;
    int durH_ = 0;
    int maxPixelW_ = 0;
    std::string truncated_title_ = "";
    std::string truncated_author_ = "";
    std::string truncated_views_date_ = "";
    bool is_previewing = false;
};

class GridContainer : public View {
public:
    void addCard(std::shared_ptr<VideoCard> card);
    void pruneOldCards(int maxCards, int& focusedCardIdx);
    void render(SDL_Renderer* renderer, float offsetX, float offsetY) override;
    void update(float dt) override;
    SDL_Rect viewportRect(float offsetX, float offsetY) const;
    
    std::vector<std::shared_ptr<VideoCard>> cards;
    float scrollY = 0.0f;
    float targetScrollY = 0.0f;
    std::string title;
    
    int columns = 2;
    float padding = 15.0f;
    std::function<void()> onScrolledToBottom;
};

class FocusManager {
public:
    void setGrid(std::shared_ptr<GridContainer> grid);
    void handleInput(int dx, int dy);
    void update(float dt);
    void renderFocusRing(SDL_Renderer* renderer, float offsetX, float offsetY);
    
    std::shared_ptr<VideoCard> getFocusedCard() const;
    void clickFocused();
    void pruneGridIfNeeded(int maxCards);
    void setFocusedIndex(int index);

private:
    std::shared_ptr<GridContainer> grid_;
    int focusedCardIdx_ = 0;
    Rect currentFocusRing_{0,0,0,0};
    Rect targetFocusRing_{0,0,0,0};
    
    std::unordered_map<GridContainer*, int> gridFocusIndices_;
    
    void updateTargetFocus();
};

} // namespace ui

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
    // Wire-up.  Caller sets image manager + per-video activation callback ONCE
    // so addVideo() / lazy materialization can build cards without per-call
    // boilerplate.  These are independent of any specific video.
    void setImageManager(ImageManager* im) { im_ = im; }
    void setActivateCallback(std::function<void(const YouTubeVideo&)> cb) { on_activate_ = std::move(cb); }

    // Preferred path: append metadata only.  The VideoCard is NOT allocated;
    // it materializes the first time the item enters the viewport.  This keeps
    // the per-item cost down to roughly the size of YouTubeVideo (~strings)
    // instead of a fully-built VideoCard (cached layout + lambda + flags).
    void addVideo(const YouTubeVideo& v);

    // Legacy: push a pre-built card.  Still supported for the rare case where
    // the caller needs to attach a non-default onClick.  Most callers should
    // use addVideo + setActivateCallback instead.
    void addCard(std::shared_ptr<VideoCard> card);

    // Drop every video AND every materialized card.  Use this when the feed
    // kind changes (e.g. Trending → Subscriptions toggle).
    void clear();

    int videoCount() const { return static_cast<int>(videos.size()); }

    // Lazily build the VideoCard at videos[idx] if not already.  Always
    // returns a non-null pointer (assuming idx is in range) — call this
    // before touching cards[idx] from outside the render loop.
    std::shared_ptr<VideoCard> ensureCard(int idx);

    // Release VideoCard objects for indices far from the [first,last] viewport
    // window.  Called automatically by render() so memory stays bounded as
    // the user scrolls through hundreds of items.
    void releaseOffscreenCards(int firstVisible, int lastVisible, int margin);

    void pruneOldCards(int maxCards, int& focusedCardIdx);
    void render(SDL_Renderer* renderer, float offsetX, float offsetY) override;
    void update(float dt) override;
    SDL_Rect viewportRect(float offsetX, float offsetY) const;

    // Authoritative model.  Every entry has a parallel slot in `cards`
    // (which is nullptr unless materialized).
    std::vector<YouTubeVideo> videos;

    // Parallel array — cards.size() == videos.size().  Most entries are
    // nullptr; only currently-visible (or recently-visible) items hold a
    // VideoCard.  External readers must null-check before dereferencing.
    std::vector<std::shared_ptr<VideoCard>> cards;

    float scrollY = 0.0f;
    float targetScrollY = 0.0f;
    std::string title;

    int columns = 2;
    float padding = 15.0f;
    std::function<void()> onScrolledToBottom;

private:
    ImageManager* im_ = nullptr;
    std::function<void(const YouTubeVideo&)> on_activate_;
    void layoutCardAt(VideoCard& card, int idx) const;
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
    void resetGridFocus(std::shared_ptr<GridContainer> grid);

private:
    std::shared_ptr<GridContainer> grid_;
    int focusedCardIdx_ = 0;
    Rect currentFocusRing_{0,0,0,0};
    Rect targetFocusRing_{0,0,0,0};
    
    std::unordered_map<GridContainer*, int> gridFocusIndices_;
    
    void updateTargetFocus();
};

} // namespace ui

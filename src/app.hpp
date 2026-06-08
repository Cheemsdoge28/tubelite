#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include <mutex>
#include <functional>
#include "state.hpp"
#include "mpv_player.hpp"
#include "youtube_api.hpp"
#include "keyboard_overlay.hpp"
#include "status_overlay.hpp"
#include "image_manager.hpp"
#include "ui_framework.hpp"
#include <memory>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

class App {
public:
    App() = default;
    ~App();

    bool initialize();
    void run();

private:
    void shutdown();
    bool createWindow();
    void openController();
    void closeController();

    void handleEvent(SDL_Event& event);
    void handleKey(SDL_Keycode key);
    void handleControllerButton(SDL_GameControllerButton button, bool down);
    void handleControllerAxis(const SDL_ControllerAxisEvent& caxis);
    void handleJoyHat(Uint8 value);
    void handleJoyAxis(const SDL_JoyAxisEvent& jaxis);
    void handleJoyButton(Uint8 button, SDL_JoystickID instanceId, bool down);

    void updateSticks();
    void updateKeyboardCursorBlinkState();
    void renderFrame();

    void openKeyboard();
    void closeKeyboard(bool commit);
    void activateKeyboardGo();
    void activateSelectedKey();

    void doSearch(const std::string& query);
    void playVideo(const YouTubeVideo& video);
    void stopBrowsePreviewState();
    void leavePlayback();
    void showPlaybackToast(const std::string& text, bool withProgress = false);
    std::shared_ptr<ui::GridContainer> activeGrid() const;
    std::string streamCacheKey(const std::string& videoId, int maxHeight) const;
    void renderBrowseHeader(int width, int height, const std::string& title, const std::string& subtitle, float scrollY, bool searchScreen);
    void renderBrowseLoadingState(int width, int height, const std::string& text);

    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    SDL_GameController* controller_{nullptr};
    SDL_Joystick* joystick_{nullptr};

    TubeState state_;
    MpvPlayer mpv_player_;
    YouTubeAPI youtube_api_;
    KeyboardOverlay keyboard_;
    StatusOverlay status_;

    bool uiDirty_{true};
    bool lastKeyboardCursorVisible_{true};
    bool homeLoadFailed_{false};

    YouTubeVideo current_video_;

    std::unique_ptr<ImageManager> image_manager_;
    ui::FocusManager focus_manager_;
    std::shared_ptr<ui::GridContainer> home_grid_;
    std::shared_ptr<ui::GridContainer> search_grid_;
    
    std::string current_search_query_;
    std::string loading_status_text_{"Resolving Stream..."};
    int search_page_{1};
    int home_page_{1};

    int lastStickDirX_{0};
    int lastStickDirY_{0};
    std::chrono::steady_clock::time_point nextStickNavAt_;

    std::vector<YouTubeVideo> cached_trending_videos_;
    std::chrono::steady_clock::time_point trending_cache_time_;

    std::shared_ptr<ui::VideoCard> preview_card_{nullptr};
    bool is_playing_preview_{false};
    bool is_loading_preview_{false};
    std::unordered_map<std::string, std::string> stream_url_cache_;
    std::unordered_set<std::string> stream_prefetch_inflight_;
    
    int last_playback_seconds_{-1};
    std::mutex queue_mutex_;
    std::vector<std::function<void()>> main_thread_queue_;
    void queueOnMainThread(std::function<void()> cb);
    void processMainThreadQueue();

    std::vector<int> event_fds_;
    std::thread input_thread_;
    bool input_thread_running_{false};
    void startInputThread();
    void stopInputThread();
    
    std::chrono::steady_clock::time_point volume_overlay_timeout_;
    std::chrono::steady_clock::time_point speed_overlay_timeout_;

    void loadHomeFeeds();
    void loadMoreHomeFeeds();
    void loadMoreSearchResults();
    void updateHoverPreviews();
};

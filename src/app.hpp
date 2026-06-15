#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include <mutex>
#include <functional>
#include <optional>
#include "state.hpp"
#include "mpv_player.hpp"
#include "youtube_api.hpp"
#include "keyboard_overlay.hpp"
#include "status_overlay.hpp"
#include "storyboard.hpp"
#include "image_manager.hpp"
#include "thumbnail_atlas.hpp"
#include "ui_framework.hpp"
#include "state_manager.hpp"
#include "compositor.hpp"
#include <memory>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

class App {
    friend class Compositor;
public:
    App() : state_manager_(state_) {}
    ~App();

    bool initialize();
    void run();
    static void getSystemMemoryAndStorage(double& ram_used_mb, double& storage_free_gb, double& storage_total_gb);

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

    void updateSticks(float dt);
    void updateKeyboardCursorBlinkState();
    void renderFrame();

    void openKeyboard();
    void closeKeyboard(bool commit);
    void activateKeyboardGo();
    void activateSelectedKey();

    void doSearch(const std::string& query);
    void playVideo(const YouTubeVideo& video, bool forceFullscreen = true);
    void toggleMiniplayer();
    void stopBrowsePreviewState();
    void leavePlayback();
    void showPlaybackToast(const std::string& text, bool withProgress = false);
    void showPlaybackUi();
    bool isInputLocked() const;
    std::shared_ptr<ui::GridContainer> activeGrid() const;
    std::string streamCacheKey(const std::string& videoId, int maxHeight) const;
    std::optional<std::string> getCachedStreamUrl(const std::string& key);
    void setCachedStreamUrl(const std::string& key, const std::string& url);
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
    StoryboardManager storyboard_;

    bool uiDirty_{true};
    bool lastKeyboardCursorVisible_{true};
    bool homeLoadFailed_{false};
    Uint32 play_flash_start_time_{0};
    StateManager state_manager_;
    std::unique_ptr<Compositor> compositor_;

    YouTubeVideo current_video_;

    std::unique_ptr<ImageManager> image_manager_;
    std::unique_ptr<ThumbnailAtlas> thumb_atlas_;
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
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> stream_url_cache_times_;
    std::unordered_set<std::string> stream_prefetch_inflight_;
    
    int last_playback_seconds_{-1};
    float scrub_hold_time_{0.0f};
    std::mutex queue_mutex_;
    std::vector<std::function<void()>> main_thread_queue_;
    void queueOnMainThread(std::function<void()> cb);
    void processMainThreadQueue();

    std::vector<int> event_fds_;
    std::thread input_thread_;
    bool input_thread_running_{false};
    void startInputThread();
    void stopInputThread();
    
    std::chrono::steady_clock::time_point playback_ui_timeout_;
    std::chrono::steady_clock::time_point volume_overlay_timeout_;
    std::chrono::steady_clock::time_point speed_overlay_timeout_;
    std::chrono::steady_clock::time_point last_fps_update_;
    std::chrono::steady_clock::time_point last_frame_time_;
    int frame_count_{0};
    float current_fps_{0.0f};
    float render_latency_ms_{0.0f};

    int headerTitleW_Home_{0};
    int headerTitleH_Home_{0};
    int headerTitleW_Search_{0};
    int headerTitleH_Search_{0};

    std::string home_feed_query_{"trending"};
    std::vector<YouTubeVideo> playback_history_;
    void saveHistory();
    void loadHistory();
    void addToHistory(const YouTubeVideo& video);

    void saveHomeCache();
    bool loadHomeCache();

    void loadHomeFeeds();
    void loadMoreHomeFeeds();
    void loadMoreSearchResults();
    void updateHoverPreviews();
    void handleVideoEnded();
};

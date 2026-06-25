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
    friend class SettingsModal;
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
    void handleKeyUp(SDL_Keycode key);
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

    // Dump a JSON snapshot of the profiler + sidecar/image telemetry + current
    // state to `path`.  Triggered by F11.  Path defaults to the fast tmpfs
    // location on Linux so it survives an emulator launch.
    std::string dumpProfileSnapshot(const std::string& path = "");
    void showPlaybackUi();
    bool isInputLocked() const;
    std::shared_ptr<ui::GridContainer> activeGrid() const;
    std::shared_ptr<ui::GridContainer> getPlaybackGrid() const;
    std::string streamCacheKey(const std::string& videoId, int maxHeight) const;
    std::optional<std::string> getCachedStreamUrl(const std::string& key);
    void setCachedStreamUrl(const std::string& key, const std::string& url);
    // Metadata cached alongside the stream URL (same key) so a cache hit can
    // restore stats/description without a second resolver round-trip.
    void cacheStreamMeta(const std::string& key, const VideoPlaybackMetadata& meta);
    std::optional<VideoPlaybackMetadata> getCachedStreamMeta(const std::string& key);
    // Cache encoding: "video_url|subtitle_url|audio_url" — last two may be empty.
    static void splitCachedStream(const std::string& cached,
                                  std::string& video_url,
                                  std::string& subtitle_url,
                                  std::string& audio_url);
    void renderBrowseLoadingState(int width, int height, const std::string& text);


    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    // Cached SDL window dimensions.  The handheld is fixed 640x480
    // KMSDRM fullscreen (no resize possible), so calling
    // SDL_GetWindowSize per frame in renderFrame() (and per keystroke
    // in keyboard input handlers) is pure overhead.  Initialised right
    // after createWindow() in App::initialize and refreshed defensively
    // on SDL_WINDOWEVENT_SIZE_CHANGED.
    int cached_window_w_{640};
    int cached_window_h_{480};
    SDL_GameController* controller_{nullptr};
    SDL_Joystick* joystick_{nullptr};

    // ── Screen-off / power-save mode (X double-tap in the player) ────────────
    // Turns off the panel backlight and switches the CPU to the
    // conservative governor while audio keeps playing — a "listen with
    // the screen off" mode for music.  The backlight + governor are
    // ALWAYS restored on exit (player exit, app shutdown) so the device
    // never gets left dark.
    bool        screenOff_{false};            // currently in screen-off mode
    Uint32      screenOffArmMs_{0};           // first-tap timestamp (0 = not armed)
    int         savedBacklight_{-1};          // backlight value before we zeroed it (-1 = none saved)
    std::string savedGovernor_;               // CPU governor before we forced conservative
    void backlightWrite(int value);           // write /sys/class/backlight brightness
    int  backlightRead();                     // read current brightness (-1 on failure)
    void cpuGovernorWrite(const std::string& gov);
    std::string cpuGovernorRead();
    void enterScreenOff();                    // save state, blank panel, conservative gov
    void exitScreenOff();                     // restore backlight + governor (idempotent)
    void requestScreenOffToggle();            // X-press: arm, confirm, or cancel

    // Player volume step (dir = -1 down / +1 up).  Reads mpv's actual
    // volume so it matches the daemon and never drifts.  Bound to L2/R2.
    void adjustPlayerVolume(int dir);

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
    // Timestamp of the last "new video surface appeared" event (fullscreen
    // start, next track, miniplayer open, preview start). Drives a short
    // fade-from-black transition in the compositor. 0 = no animation pending.
    Uint32 video_fade_start_time_{0};
    void triggerVideoFade() { video_fade_start_time_ = SDL_GetTicks(); uiDirty_ = true; }
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
    // Set when onScrolledToBottom fires while isSearching=true so we retry
    // immediately once the current page load finishes.
    bool pendingMoreHome_{false};
    bool pendingMoreSearch_{false};

    int lastStickDirX_{0};
    int lastStickDirY_{0};
    std::chrono::steady_clock::time_point nextStickNavAt_;

    std::vector<YouTubeVideo> cached_trending_videos_;
    std::chrono::steady_clock::time_point trending_cache_time_;
    // Which feed kind populated cached_trending_videos_.  "" before first
    // load; "trending" or "subscriptions" thereafter.  Used so a toggle in
    // the settings modal doesn't keep showing the old feed's cache.
    std::string cached_home_kind_;

    std::shared_ptr<ui::VideoCard> preview_card_{nullptr};
    bool is_playing_preview_{false};
    bool is_loading_preview_{false};
    std::unordered_map<std::string, std::string> stream_url_cache_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> stream_url_cache_times_;
    // Resolve-time metadata keyed identically to stream_url_cache_, evicted in
    // lockstep.  A cache hit reads stats/description from here instead of
    // re-invoking the stream resolver (that backfill used to race the daemon
    // handoff when the user exited mid-resolve).
    std::unordered_map<std::string, VideoPlaybackMetadata> stream_meta_cache_;
    std::unordered_set<std::string> stream_prefetch_inflight_;
    // Hard backstop against a runaway preview-prefetch loop: a cacheKey that
    // just failed is not re-requested until this time passes.  Without it, a
    // failing video could be hammered hundreds of times/sec.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> stream_prefetch_fail_until_;

    int last_playback_seconds_{-1};
    float scrub_hold_time_{0.0f};
    std::string prefetched_next_video_id_;
    void prefetchNextVideo();
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
    VideoPlaybackMetadata active_video_metadata_;
    std::optional<double> last_seek_time_;
    std::chrono::steady_clock::time_point last_seek_time_point_;
    std::vector<std::string> wrapped_description_lines_;
    int description_scroll_row_{0};
    bool select_held_{false};
    bool select_action_triggered_{false};
    bool auth_initial_check_done_{false};

    int headerTitleW_Home_{0};
    int headerTitleH_Home_{0};
    int headerTitleW_Search_{0};
    int headerTitleH_Search_{0};

    std::string home_feed_query_{"trending"};
    std::vector<YouTubeVideo> playback_history_;
    void saveHistory();
    void loadHistory();
    void addToHistory(const YouTubeVideo& video);

    void saveSettings();
    void loadSettings();
    void saveDaemonQueue();
    void playNextTrack();
    void playPreviousTrack();

    void saveHomeCache();
    bool loadHomeCache();

    // Browse-state persistence — last screen, focused index, search query
    // + results, so reopening the app drops the user right back where
    // they left off (no re-search, no scroll-back-to-here).  Thumbnails
    // are NOT saved — they get re-fetched on demand by the lazy image
    // manager, which is bandwidth-cheap compared to re-running a search.
    void saveBrowseState();
    // Returns true if state was successfully restored (so the caller can
    // skip the cold-fetch loadHomeFeeds(), whose async callback would
    // otherwise clear and replace the just-restored grid contents).
    bool loadBrowseState();
    // Gates per-transition saveBrowseState() calls during App::initialize.
    // Without this gate, the reabsorb path's transitionTo(Home) would
    // fire saveBrowseState BEFORE loadBrowseState ran, snapshotting an
    // empty in-memory state (screen=home, no search videos) and
    // clobbering the on-disk file.  Flipped to true once loadBrowseState
    // has completed.
    bool browse_state_ready_{false};

    // Daemon reabsorption — on launch, check whether the background
    // daemon is currently playing.  If yes, transfer the playback into
    // our own mpv at the same offset with a soft audio fade-in, open
    // the miniplayer, and kill the daemon.  Mirrors the exit-fade in
    // shutdown() for symmetry.  Returns true if reabsorption happened.
    bool reabsorbDaemonPlayback();

    void loadHomeFeeds();
    void loadMoreHomeFeeds();
    void loadMoreSearchResults();
    void updateHoverPreviews();
    void handleVideoEnded();
    // mpv reported the current file failed to load (stale/poisoned cached URL).
    // Evict the bad entry and re-resolve once as a real play.
    void handlePlaybackLoadFailure();
    // Video id that already got an automatic post-failure re-resolve; cleared
    // once playback actually progresses, so a genuinely dead stream can't loop.
    std::string load_retry_video_id_;
};

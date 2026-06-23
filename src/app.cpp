#include "app.hpp"
#include "daemon.hpp"
#include "json.hpp"
#include "profiler.hpp"
#include "renderer_utils.hpp"
#include "settings.hpp"
#include "settings_modal.hpp"
#include "stb_image.h"
#include "ui_sounds.hpp"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#include <csignal>

// Stop the tubed backend service (SIGTERM). Used when the app exits and is NOT
// handing off to the background daemon, so neither tubed nor any yt-dlp it
// spawned is left running.
static void stopTubed() {
    std::ifstream ifs("/dev/shm/tubed.pid");
    pid_t pid = 0;
    if (ifs) ifs >> pid;
    if (pid > 0) ::kill(pid, SIGTERM);
}
#endif

static std::string getAppDataPath(const std::string& filename) {
#ifdef _WIN32
    return filename;
#else
    // Resolve the install-dir prefix once — std::filesystem::exists is
    // a stat() under the hood and this function is called from every
    // saveBrowseState / loadBrowseState / saveDaemonQueue / saveHistory
    // / saveHomeCache call site (now firing on every screen transition,
    // so on the order of several Hz when the user is navigating).  The
    // base dir doesn't move during a session, so cache the prefix on
    // first call and short-circuit thereafter.
    static const std::string base = []() {
        if (std::filesystem::exists("/roms/tools/tubelite"))
            return std::string("/roms/tools/tubelite/");
        return std::string();
    }();
    return base + filename;
#endif
}

void App::getSystemMemoryAndStorage(double& ram_used_mb, double& storage_free_gb, double& storage_total_gb) {
    ram_used_mb = 0.0;
    storage_free_gb = 0.0;
    storage_total_gb = 0.0;

    // RAM RSS
#ifdef _WIN32
    ram_used_mb = 0.0;
#else
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "VmRSS:", 6) == 0) {
                long rss_kb = 0;
                if (std::sscanf(line + 6, "%ld", &rss_kb) == 1) {
                    ram_used_mb = rss_kb / 1024.0;
                }
                break;
            }
        }
        std::fclose(f);
    }
#endif

    // Storage
#ifdef _WIN32
    ULARGE_INTEGER freeBytes, totalBytes, totalFreeBytes;
    if (GetDiskFreeSpaceExA(".", &freeBytes, &totalBytes, &totalFreeBytes)) {
        storage_free_gb = (double)freeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
        storage_total_gb = (double)totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    }
#else
    struct statvfs stat;
    if (statvfs(".", &stat) == 0) {
        storage_free_gb = (double)(stat.f_frsize * stat.f_bavail) / (1024.0 * 1024.0 * 1024.0);
        storage_total_gb = (double)(stat.f_frsize * stat.f_blocks) / (1024.0 * 1024.0 * 1024.0);
    }
#endif
}

// static void logInfo(const std::string& msg) { std::cout << "[INFO] " << msg << std::endl; }
static void logError(const std::string& msg) { std::cerr << "[ERROR] " << msg << std::endl; }


App::~App() { shutdown(); }

bool App::initialize() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        logError(std::string("SDL_Init failed: ") + SDL_GetError());
        return false;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    if (!createWindow()) return false;

    // UI sounds: initialize after SDL is up but before any user-facing
    // interaction can fire.  Failure is non-fatal — sounds become a no-op.
    ui_sounds::init();

    if (!initFonts(renderer_)) {
        logError("Failed to initialize TTF fonts, falling back to pixel font");
    }
    getTextSize("TubeLite", 3, &headerTitleW_Home_, &headerTitleH_Home_);
    getTextSize("Search", 2, &headerTitleW_Search_, &headerTitleH_Search_);
    
    openController();
    image_manager_ = std::make_unique<ImageManager>(renderer_);
    thumb_atlas_ = std::make_unique<ThumbnailAtlas>(renderer_, 3);
    image_manager_->setAtlas(thumb_atlas_.get());
    
    // Both grids share the same activation behaviour (clicking a card plays
    // the video).  Wire it ONCE here so addVideo() can build cards on the
    // fly without per-call boilerplate at every feed-loader call site.
    auto activate = [this](const YouTubeVideo& v) { playVideo(v); };

    home_grid_ = std::make_shared<ui::GridContainer>();
    home_grid_->title = "Trending Now";
    home_grid_->columns = 2;
    home_grid_->bounds = {0, 100, 640, 332};
    home_grid_->setImageManager(image_manager_.get());
    home_grid_->setActivateCallback(activate);
    home_grid_->onScrolledToBottom = [this]() {
        if (state_.isLoadingVideo || state_.isSearching) {
            pendingMoreHome_ = true; // retry once current load finishes
        } else {
            loadMoreHomeFeeds();
        }
    };

    search_grid_ = std::make_shared<ui::GridContainer>();
    search_grid_->title = "";
    search_grid_->columns = 2;
    search_grid_->bounds = {0, 100, 640, 332};
    search_grid_->setImageManager(image_manager_.get());
    search_grid_->setActivateCallback(activate);
    search_grid_->onScrolledToBottom = [this]() {
        if (state_.isLoadingVideo || state_.isSearching) {
            pendingMoreSearch_ = true; // retry once current load finishes
        } else {
            loadMoreSearchResults();
        }
    };
    compositor_ = std::make_unique<Compositor>(renderer_);

    state_manager_.setTransitionCallback([this](TubeState::Screen oldScreen, TubeState::Screen newScreen, bool oldMiniplayer, bool newMiniplayer) {
        bool stoppedPlayback = (oldScreen == TubeState::Screen::Playback && newScreen != TubeState::Screen::Playback && !newMiniplayer) ||
                               (oldMiniplayer && !newMiniplayer && newScreen != TubeState::Screen::Playback);
        if (stoppedPlayback) {
            mpv_player_.stop();
            storyboard_.stop();
            // NOTE: do NOT clear the thumbnail cache here. The atlas is bounded,
            // so clearing frees little memory but forces every visible grid
            // thumbnail to re-download/re-decode — the "churn" on closing the
            // miniplayer. Keeping it cached returns to a fully-drawn grid.
        }
        
        // Stop storyboard extraction during miniplayer to avoid RK3326 resource contention
        if (newMiniplayer && !oldMiniplayer) {
            storyboard_.stop();
            triggerVideoFade();  // animate the miniplayer appearing
        }
        
        // Resume/restart storyboard extraction if returning to fullscreen playback from miniplayer
        if (oldMiniplayer && !newMiniplayer && newScreen == TubeState::Screen::Playback) {
            const std::string cacheKey = streamCacheKey(current_video_.id, state_.maxQualityHeight);
            auto cachedOpt = getCachedStreamUrl(cacheKey);
            if (cachedOpt.has_value() && !cachedOpt.value().empty()) {
                std::string cached_val = cachedOpt.value();
                std::string stream_url = cached_val;
                size_t pipe_pos = cached_val.find('|');
                if (pipe_pos != std::string::npos) {
                    stream_url = cached_val.substr(0, pipe_pos);
                }
                if (!current_video_.is_live) {
                    storyboard_.start(stream_url, current_video_.duration_seconds);
                }
            }
        }
        
        if (newScreen == TubeState::Screen::Search) {
            focus_manager_.setGrid(search_grid_);
        } else if (newScreen == TubeState::Screen::Home) {
            focus_manager_.setGrid(home_grid_);
        }

        // Persist browse state on every screen transition so kills /
        // crashes / OOM during a session don't lose where the user
        // was.  Previously saveBrowseState() only ran on clean exit
        // (App::run end), so anything that killed the process before
        // that — sigkill from ES, OOM, daemon-spawn race — discarded
        // the entire search/home state.  This write is ~2 KB to
        // /roms/tools/tubelite/browse_state.json; cheap on transitions
        // that happen at most a few times a minute.
        //
        // Gated on browse_state_ready_ so that the reabsorb path's
        // transitionTo(Home) during App::initialize (which runs BEFORE
        // loadBrowseState) doesn't snapshot a still-empty in-memory
        // state and overwrite the on-disk file the user actually left
        // behind.
        if (browse_state_ready_) saveBrowseState();

        uiDirty_ = true;
    });

    if (!mpv_player_.initialize(window_, renderer_)) {
        logError("MPV init failed");
        return false;
    }
    loadSettings();
    loadHistory();
    // Restore browse state BEFORE reabsorb so reabsorb can drop the user
    // back onto the SCREEN they were last on (e.g. their search results)
    // with the now-playing miniplayer over it — instead of always
    // forcing Home.  loadBrowseState() transitions to the saved screen
    // and hydrates the grids; reabsorb then keeps that screen.
    bool restored = loadBrowseState();
    // Reabsorb: if the daemon is currently playing, transfer its track
    // into our own mpv with continuous audio.  Doing killExistingDaemon()
    // before this would cut audio for a beat while mpv spins up.
    // reabsorbDaemonPlayback() handles the daemon kill (via fade-out
    // signal) itself once playback has been picked up, and preserves the
    // browse screen restored above.
    bool reabsorbed = reabsorbDaemonPlayback();
    if (!reabsorbed) killExistingDaemon();
    // An empty restore (no saved state / cold install) falls through to
    // the standard trending fetch; a successful restore short-circuits it
    // so the async callback can't clobber the hydrated grids.
    if (!restored) {
        loadHomeFeeds();
    }
    // Now safe for transition-driven saves to fire — the in-memory
    // state matches (or super-sedes) the on-disk file.
    browse_state_ready_ = true;
    SDL_StartTextInput();

    // Prime the cached window dimensions now that the window exists.
    // All subsequent reads (renderFrame, input handlers) use the
    // cached values; refresh hook lives in handleEvent for
    // SDL_WINDOWEVENT_SIZE_CHANGED — defensive, the fullscreen window
    // on this device never actually resizes.
    SDL_GetWindowSize(window_, &cached_window_w_, &cached_window_h_);
    keyboard_.preload(renderer_, state_, cached_window_w_, cached_window_h_);

    return true;
}

void App::run() {
    using namespace std::chrono;
    const milliseconds frameTarget(16); // ~60fps
    last_fps_update_ = steady_clock::now();
    last_frame_time_ = steady_clock::now();
    
    while (state_.running) {
        Profiler::instance().beginFrame();
        auto start = steady_clock::now();

        auto now_dt = steady_clock::now();
        float dt = duration<float>(now_dt - last_frame_time_).count();
        last_frame_time_ = now_dt;
        if (dt > 0.1f) dt = 0.1f; // Clamp to prevent spikes after waking up

        { PROFILE_SCOPE("main_queue"); processMainThreadQueue(); }

        // Auth status: poll tubed until we get a real answer (handles cold
        // tubed start where the first request races the daemon).  Throttled
        // to ~2 Hz instead of per-frame — internal coalescing made the
        // per-frame version cheap, but it still chewed atomics + thread
        // spawns every 16 ms for no gain over polling every 500 ms.
        if (!youtube_api_.authChecked()) {
            static auto lastAuthPoll = steady_clock::now() - seconds(1);
            if (now_dt - lastAuthPoll >= milliseconds(500)) {
                youtube_api_.refreshAuthStatus();
                auth_initial_check_done_ = true;
                lastAuthPoll = now_dt;
            }
        }
        if (state_.authed != youtube_api_.isAuthed() ||
            state_.authChecked != youtube_api_.authChecked()) {
            const bool wasAuthed = state_.authed;
            state_.authed      = youtube_api_.isAuthed();
            state_.authChecked = youtube_api_.authChecked();
            uiDirty_ = true;
            // Late sign-in: cookies just became valid (e.g. user dropped a
            // fresh cookies.txt and pressed A on the sign-in help modal to
            // re-check).  If the user opted into Subscriptions but we'd
            // been showing the trending fallback, reload Home now so the
            // change is immediate instead of next restart.
            if (!wasAuthed && state_.authed &&
                state_.homeFeedKind == "subscriptions" &&
                state_.currentScreen == TubeState::Screen::Home) {
                cached_home_kind_.clear();   // force re-fetch (not from cache)
                loadHomeFeeds();
            }
        }

        // Handle Playback HUD timeout and fadeout
        if (state_.currentScreen == TubeState::Screen::Playback) {
            double remaining = duration<float>(playback_ui_timeout_ - start).count();
            if (state_.isLoadingVideo || state_.isScrubbing) {
                playback_ui_timeout_ = start + seconds(5);
                if (!state_.showUi) {
                    state_.showUi = true;
                    uiDirty_ = true;
                }
            } else {
                if (remaining > 0.0f) {
                    if (!state_.showUi) {
                        state_.showUi = true;
                        uiDirty_ = true;
                    }
                    if (remaining < 0.5f) {
                        uiDirty_ = true;
                    }
                } else {
                    if (state_.showUi && !state_.showDescriptionDrawer) {
                        state_.showUi = false;
                        uiDirty_ = true;
                    }
                }
            }
        }

        if (state_.currentScreen != TubeState::Screen::Playback) {
            if (image_manager_ && image_manager_->update()) {
                uiDirty_ = true;
            }
        }
        
        auto focusedCard = focus_manager_.getFocusedCard();
        if (focusedCard && state_.inputMode != TubeState::InputMode::SearchText && focusedCard->focusedTime_ > 1.5f && focusedCard->focusedTime_ < 25.0f && focusedCard->titleW_ > focusedCard->maxPixelW_) {
            uiDirty_ = true;
        }
        
        if (play_flash_start_time_ > 0) {
            if (SDL_GetTicks() - play_flash_start_time_ < 400) {
                uiDirty_ = true;
            } else {
                play_flash_start_time_ = 0;
            }
        }

        auto currentGrid = activeGrid();
        bool gridScrolling = false;
        if (currentGrid && std::abs(currentGrid->scrollY - currentGrid->targetScrollY) > 0.05f) {
            gridScrolling = true;
            uiDirty_ = true;
        }
        
        bool active = uiDirty_ || gridScrolling || mpv_player_.isPlaying() || is_playing_preview_ || is_loading_preview_ || state_.isScrubbing || (state_.inputMode == TubeState::InputMode::SearchText) || state_.isSearching || state_.isLoadingVideo || (state_.currentScreen == TubeState::Screen::Playback && state_.showUi);
        // (Removed a forced 60fps spin during the 0.85s post-navigation dwell:
        // nothing animates then — the focus ring is instant and the title
        // marquee only starts at 1.5s and raises uiDirty itself. The 100ms
        // event-wait below still advances focusedTime to trigger previews, so
        // browsing now idles the CPU instead of busy-rendering.)

        if (!active) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!main_thread_queue_.empty()) active = true;
        }
        
        {
            PROFILE_SCOPE("sdl_events");
            SDL_Event event;
            if (!active) {
                if (SDL_WaitEventTimeout(&event, 100)) {
                    handleEvent(event);
                }
            }
            while (SDL_PollEvent(&event)) { handleEvent(event); }
        }

        { PROFILE_SCOPE("sticks");          updateSticks(dt); }
        { PROFILE_SCOPE("kbd_blink");       updateKeyboardCursorBlinkState(); }
        { PROFILE_SCOPE("hover_previews");  updateHoverPreviews(); }

        // Lazy retry of SDL audio init: on devices where
        // EmulationStation (or whatever spawned us) is holding the
        // codec exclusively at our startup moment, the eager
        // ui_sounds::init() at App::initialize hits EBUSY through
        // dmix's slave open and disables itself.  Once another
        // dmix-using client (mpv on first playback, or retroarch
        // backgrounded) has created the shared dmix region, SDL can
        // JOIN it without re-acquiring hw — so retry periodically
        // until it sticks.  Throttled to 1 s so we don't pound
        // SDL_InitSubSystem / SDL_OpenAudioDevice every frame; once
        // it succeeds the early-out in ui_sounds::init() makes the
        // call effectively free.
        if (!ui_sounds::isInitialized()) {
            static Uint32 last_ui_sounds_retry_ms = 0;
            const Uint32 now_ms = SDL_GetTicks();
            if (now_ms - last_ui_sounds_retry_ms >= 1000) {
                last_ui_sounds_retry_ms = now_ms;
                ui_sounds::init();
            }
        }

        {
            PROFILE_SCOPE("mpv_update");
            if (mpv_player_.update()) {
                uiDirty_ = true;
                PROFILE_COUNT("mpv_new_frame");
            }
        }
        if (mpv_player_.checkAndClearEnded()) {
            handleVideoEnded();
        } else if (mpv_player_.isPlaying()) {
            double pos = mpv_player_.getPlaybackTime();
            double dur = mpv_player_.getDuration();
            if (dur > 0.0 && pos > 0.0 && dur - pos <= 90.0) {
                prefetchNextVideo();
            }
        }
        { PROFILE_SCOPE("focus_update");    focus_manager_.update(dt); }

        // Safety net: if we somehow left the player while the screen was
        // blanked (track ended → home, reabsorb, etc.), restore the
        // backlight immediately so the device is never left dark on a
        // non-player screen.
        if (screenOff_ && state_.currentScreen != TubeState::Screen::Playback) {
            exitScreenOff();
            uiDirty_ = true;
        }

        // No-render power-save: while the panel is off, skip the entire
        // render path (FBO composite, present) — only mpv audio decode
        // keeps running above.  This is most of the CPU/GPU saving on top
        // of the conservative governor + zero backlight.
        if (!screenOff_) {
            renderFrame();
        }

        // Calculate FPS
        frame_count_++;
        auto now = steady_clock::now();
        auto fps_elapsed = duration_cast<milliseconds>(now - last_fps_update_);
        if (fps_elapsed >= seconds(1)) {
            current_fps_ = frame_count_ / (fps_elapsed.count() / 1000.0f);
            frame_count_ = 0;
            last_fps_update_ = now;
        }
        
        static float sleep_error_accum = 0.0f;
        auto loop_end = steady_clock::now();
        float work_time = duration<float>(loop_end - start).count();

        // ── Battery-friendly dynamic frame pacing ────────────────────────────
        // Render at the highest rate ONLY when the user is actually
        // watching moving video; otherwise drop the loop rate so the
        // RK3326 spends most of its time idle.  Tiers:
        //   * screen-off  → 10 fps  (only pump mpv audio; nothing visual)
        //   * playing video fullscreen, UNpaused → 60 fps (smooth)
        //   * everything else (browsing, paused, miniplayer, menus) → 30 fps
        // 30 fps is indistinguishable for a card UI / paused frame but
        // roughly halves composite + CPU wakeups vs a constant 60.
        float target_fps;
        if (screenOff_) {
            target_fps = 10.0f;
        } else if (state_.currentScreen == TubeState::Screen::Playback &&
                   mpv_player_.isPlaying()) {
            target_fps = 60.0f;
        } else {
            target_fps = 30.0f;
        }
        float target_frame_time = 1.0f / target_fps;

        float sleep_sec = target_frame_time - work_time + sleep_error_accum;
        if (sleep_sec > 0.001f) {
            auto sleep_start = steady_clock::now();
            SDL_Delay(static_cast<Uint32>(sleep_sec * 1000.0f));
            float actual_sleep = duration<float>(steady_clock::now() - sleep_start).count();
            sleep_error_accum = sleep_sec - actual_sleep;
        } else {
            sleep_error_accum = sleep_sec;
        }
        
        // Clamp to prevent runaway accumulation during load stalls/crashes
        if (sleep_error_accum < -0.05f) sleep_error_accum = -0.05f;

        Profiler::instance().endFrame();
    }

    // Always drop a final profile snapshot at shutdown — costs ~1 ms and gives
    // us a guaranteed snapshot to look at without the user having to remember
    // to press SELECT+Y / F11 before quitting.
#ifdef _WIN32
    dumpProfileSnapshot("tubelite_profile_shutdown.json");
#else
    dumpProfileSnapshot("/dev/shm/tubelite_profile_shutdown.json");
#endif

    saveSettings();
    saveBrowseState();
    // Spawn daemon if audio is playing OR if the user is in miniplayer/fullscreen mode
    // (covers paused state and search-screen miniplayer where isPlaying() may be false).
    bool daemonEligible = mpv_player_.isPlaying()
        || state_.miniplayerActive
        || state_.currentScreen == TubeState::Screen::Playback;
    if (state_.backgroundDaemonEnabled && daemonEligible && !current_video_.id.empty()) {
#ifndef _WIN32
        // Clear any stale readiness flag before launching the new daemon.
        ::unlink("/dev/shm/tubelite_daemon_audio.live");
#endif
        saveDaemonQueue();
        spawnDaemon();

#ifndef _WIN32
        // Seamless handoff: keep our own audio playing and fade it out while the
        // daemon spins up and buffers. We exit the instant the daemon reports
        // its audio is live (or after a short cap), so playback never goes
        // silent during the cross-process handover.
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(3500);
        const auto fadeStart = steady_clock::now();
        const int startVol = state_.volume;          // 0..100
        const float fadeSecs = 1.2f;
        while (steady_clock::now() < deadline) {
            mpv_player_.update();                     // keep decoding/audio flowing
            float t = duration<float>(steady_clock::now() - fadeStart).count();
            int v = (t >= fadeSecs) ? 0 : static_cast<int>(startVol * (1.0f - t / fadeSecs));
            mpv_player_.setVolume(v);
            if (::access("/dev/shm/tubelite_daemon_audio.live", F_OK) == 0) break;
            SDL_Delay(25);
        }
#endif
    } else {
#ifndef _WIN32
        // No background playback is taking over, so nothing will use the backend
        // — shut tubed down so it (and any yt-dlp it spawned) can't linger.
        stopTubed();
#endif
    }
}

void App::queueOnMainThread(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    main_thread_queue_.push_back(cb);
}

void App::processMainThreadQueue() {
    std::vector<std::function<void()>> local_queue;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        local_queue.swap(main_thread_queue_);
    }
    for (const auto& cb : local_queue) {
        cb();
    }
}

// ── Screen-off / power-save mode ────────────────────────────────────────────
//
// Backlight is controlled via /sys/class/backlight/backlight/brightness
// (confirmed on this R36S: max_brightness 255, writable).  CPU governor
// via /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor.  All ops
// are best-effort and no-op on Windows / missing sysfs.

void App::backlightWrite(int value) {
#ifndef _WIN32
    std::ofstream f("/sys/class/backlight/backlight/brightness");
    if (f) f << value;
#else
    (void)value;
#endif
}

int App::backlightRead() {
#ifndef _WIN32
    std::ifstream f("/sys/class/backlight/backlight/brightness");
    int v = -1;
    if (f && (f >> v)) return v;
#endif
    return -1;
}

void App::cpuGovernorWrite(const std::string& gov) {
#ifndef _WIN32
    // Write the governor to every CPU's cpufreq node.  Globbing without
    // <glob.h> for portability: cpu0..cpu7 covers RK3326's 4 cores with
    // headroom.
    for (int i = 0; i < 8; ++i) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) +
                           "/cpufreq/scaling_governor";
        if (!std::filesystem::exists(path)) continue;
        std::ofstream f(path);
        if (f) f << gov;
    }
#else
    (void)gov;
#endif
}

std::string App::cpuGovernorRead() {
#ifndef _WIN32
    std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    std::string g;
    if (f && (f >> g)) return g;
#endif
    return "";
}

void App::enterScreenOff() {
    if (screenOff_) return;
    // Snapshot what we're about to change so exit can restore exactly.
    savedBacklight_ = backlightRead();
    savedGovernor_  = cpuGovernorRead();
    // Conservative governor minimises clocks while audio decode keeps
    // ticking; "powersave" would be even lower but can underrun audio
    // on RK3326, so conservative is the safe floor.
    cpuGovernorWrite("conservative");
    backlightWrite(0);
    screenOff_ = true;
    screenOffArmMs_ = 0;
    std::cerr << "[App] screen-off mode ON (backlight was "
              << savedBacklight_ << ", governor was '" << savedGovernor_ << "')\n";
}

void App::exitScreenOff() {
    // Idempotent + defensive: always restore even if we think we're not
    // in screen-off, in case a previous run left the panel dark.
    if (savedBacklight_ >= 0) {
        backlightWrite(savedBacklight_);
    } else {
        // No saved value (e.g. restore-on-exit safety net) — force the
        // panel back to full so the device is never left dark.
        backlightWrite(255);
    }
    if (!savedGovernor_.empty()) cpuGovernorWrite(savedGovernor_);
    if (screenOff_)
        std::cerr << "[App] screen-off mode OFF (backlight restored to "
                  << (savedBacklight_ >= 0 ? savedBacklight_ : 255) << ")\n";
    screenOff_ = false;
    screenOffArmMs_ = 0;
    savedBacklight_ = -1;
    savedGovernor_.clear();
}

void App::adjustPlayerVolume(int dir) {
    // Read mpv's ACTUAL volume (double-native) so steps never drift out
    // of sync after fades / reabsorb — exactly mirrors the daemon's
    // FN+L2/R2 handler.  ±5 per press, clamp 0..100.
    double cur = mpv_player_.getPropertyDouble("volume");
    if (cur <= 0.0 && state_.volume > 0) cur = state_.volume; // mpv not ready yet
    int v = (int)cur + dir * 5;
    v = std::max(0, std::min(100, v));
    state_.volume = v;
    mpv_player_.setVolume(v);
    showPlaybackToast("Volume " + std::to_string(v) + "%");
    uiDirty_ = true;
}

void App::requestScreenOffToggle() {
    // Only meaningful during playback (audio is the point of the mode).
    if (state_.currentScreen != TubeState::Screen::Playback) return;

    if (screenOff_) {
        // Any X press while dark turns the screen back on immediately.
        exitScreenOff();
        uiDirty_ = true;
        return;
    }

    const Uint32 now = SDL_GetTicks();
    const Uint32 kConfirmWindowMs = 2500;
    if (screenOffArmMs_ != 0 && (now - screenOffArmMs_) <= kConfirmWindowMs) {
        // Second tap within the window → commit.
        screenOffArmMs_ = 0;
        enterScreenOff();
        uiDirty_ = true;
    } else {
        // First tap → arm + show the big confirmation prompt.  The
        // prompt is drawn in renderFrame() while screenOffArmMs_ is set.
        screenOffArmMs_ = now;
        showPlaybackToast("Press X again to turn the SCREEN OFF "
                          "(audio keeps playing). Any other button cancels.");
        uiDirty_ = true;
    }
}

void App::shutdown() {
    // SAFETY: restore the backlight + governor before tearing anything
    // down, so quitting while in screen-off mode can never leave the
    // device dark.
    exitScreenOff();
    SDL_StopTextInput();
    closeController();
    keyboard_.destroyTexture();
    status_.destroyTexture();
    storyboard_.stop();
    mpv_player_.shutdown();
    thumb_atlas_.reset();
    cleanupFonts();
    ui_sounds::shutdown();
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_ = nullptr;   }
    SDL_Quit();
}

bool App::createWindow() {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    // Force SDL to use its OpenGL ES 2.0 renderer (backed by EGL).
    // This is REQUIRED before SDL_CreateWindow so the KMSDRM backend
    // initialises EGL instead of its raw framebuffer path.
    // mpv's render context will then attach to the same EGL context.
    // Do NOT add SDL_WINDOW_OPENGL here — that flag fights with the
    // opengles2 renderer's internal EGL surface setup and causes a segfault.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");

    window_ = SDL_CreateWindow("tubelite", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               640, 480, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (window_ == nullptr) {
        logError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        // opengles2 unavailable — try without vsync
        renderer_ = SDL_CreateRenderer(window_, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    }
    if (renderer_ == nullptr) {
        logError(std::string("SDL_CreateRenderer (opengles2) failed: ") + SDL_GetError());
        logError("Falling back to software renderer (mpv video will be disabled)");
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer_ == nullptr) {
        logError(std::string("SDL_CreateRenderer failed entirely: ") + SDL_GetError());
        return false;
    }

    // Log which renderer SDL chose
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer_, &info) == 0)
        std::cerr << "[SDL] renderer: " << info.name << "\n";

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);
    return true;
}


void App::openController() {
    if (controller_ != nullptr) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            controller_ = SDL_GameControllerOpen(i);
            if (controller_ != nullptr) return;
        }
    }
    if (SDL_NumJoysticks() > 0) joystick_ = SDL_JoystickOpen(0);
}

void App::closeController() {
    if (controller_ != nullptr) { SDL_GameControllerClose(controller_); controller_ = nullptr; }
    if (joystick_ != nullptr)   { SDL_JoystickClose(joystick_);         joystick_ = nullptr;   }
}

std::shared_ptr<ui::GridContainer> App::activeGrid() const {
    if (state_.currentScreen == TubeState::Screen::Search) return search_grid_;
    if (state_.currentScreen == TubeState::Screen::Home) return home_grid_;
    return nullptr;
}

std::shared_ptr<ui::GridContainer> App::getPlaybackGrid() const {
    TubeState::Screen browseScreen = state_.currentScreen;
    if (browseScreen == TubeState::Screen::Playback) {
        browseScreen = state_manager_.getPreviousBrowseScreen();
    }
    return (browseScreen == TubeState::Screen::Search) ? search_grid_ : home_grid_;
}

bool App::isInputLocked() const {
    if (state_.isLoadingVideo) return true;
    if (state_.isSearching) {
        auto grid = activeGrid();
        if (!grid || grid->videos.empty()) {
            return true;
        }
    }
    return false;
}

std::string App::streamCacheKey(const std::string& videoId, int maxHeight) const {
    return videoId + "#" + std::to_string(maxHeight);
}

void App::splitCachedStream(const std::string& cached,
                            std::string& video_url,
                            std::string& subtitle_url,
                            std::string& audio_url) {
    video_url.clear(); subtitle_url.clear(); audio_url.clear();
    size_t p1 = cached.find('|');
    if (p1 == std::string::npos) { video_url = cached; return; }
    video_url = cached.substr(0, p1);
    size_t p2 = cached.find('|', p1 + 1);
    if (p2 == std::string::npos) {
        subtitle_url = cached.substr(p1 + 1);
    } else {
        subtitle_url = cached.substr(p1 + 1, p2 - p1 - 1);
        audio_url    = cached.substr(p2 + 1);
    }
}

std::optional<std::string> App::getCachedStreamUrl(const std::string& key) {
    auto it = stream_url_cache_.find(key);
    if (it == stream_url_cache_.end()) return std::nullopt;
    
    auto timeIt = stream_url_cache_times_.find(key);
    if (timeIt != stream_url_cache_times_.end()) {
        auto age = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - timeIt->second).count();
        if (age >= 120) {
            stream_url_cache_.erase(it);
            stream_url_cache_times_.erase(timeIt);
            return std::nullopt;
        }
    }
    return it->second;
}

void App::setCachedStreamUrl(const std::string& key, const std::string& url) {
    if (url.empty()) {
        stream_url_cache_.erase(key);
        stream_url_cache_times_.erase(key);
        return;
    }
    stream_url_cache_[key] = url;
    stream_url_cache_times_[key] = std::chrono::steady_clock::now();

    // Bound the cache. Each resolved entry holds a long googlevideo URL (~1-2 KB);
    // left unbounded it grows for every card you dwell on across a browse session,
    // adding steady RAM pressure on the 1 GB device — which then swaps and makes
    // the whole UI feel like it "slows down over time". Evict oldest over the cap.
    constexpr size_t kMaxStreamCache = 192;
    while (stream_url_cache_.size() > kMaxStreamCache && !stream_url_cache_times_.empty()) {
        auto oldest = stream_url_cache_times_.begin();
        for (auto it = stream_url_cache_times_.begin(); it != stream_url_cache_times_.end(); ++it) {
            if (it->second < oldest->second) oldest = it;
        }
        stream_url_cache_.erase(oldest->first);
        stream_url_cache_times_.erase(oldest);
    }
}

void App::stopBrowsePreviewState() {
    if (is_playing_preview_) {
        mpv_player_.stop();
        mpv_player_.setMute(state_.muted);
        mpv_player_.destroyPreviewTexture();
    }
    if (preview_card_) {
        preview_card_->is_previewing = false;
        preview_card_ = nullptr;
    }
    is_playing_preview_ = false;
    is_loading_preview_ = false;
}

void App::leavePlayback() {
    state_manager_.stopPlayback();
    state_.showUi = true;
    last_playback_seconds_ = -1;
}

void App::showPlaybackToast(const std::string& text, bool withProgress) {
    mpv_player_.showText(text);
    if (withProgress) {
        mpv_player_.showProgress();
    }
}

std::string App::dumpProfileSnapshot(const std::string& path) {
    // Choose a default path: fast tmpfs on Linux (survives only until reboot,
    // but easy to find / scp), CWD on Windows.
    std::string outPath = path;
    if (outPath.empty()) {
#ifdef _WIN32
        outPath = "tubelite_profile.json";
#else
        outPath = "/dev/shm/tubelite_profile.json";
#endif
    }

    using nlohmann::json;
    json root;

    // ── Wall-clock + frame stats ─────────────────────────────────────────────
    const auto& prof = Profiler::instance();
    root["wall_ts_ms"]      = (uint64_t)SDL_GetTicks();
    root["frame_id"]        = prof.frameId();
    root["last_frame_ms"]   = prof.lastFrameMs();
    root["current_fps"]     = current_fps_;
    root["render_latency_ms"] = render_latency_ms_;
    // Profiler self-overhead so we can subtract measurement noise from sections.
    root["profiler_overhead_ms"]      = prof.avgOverheadMs();
    root["profiler_per_scope_ns"]     = prof.perScopeOverheadNs();
    root["profiler_scope_calls"]      = prof.lastTotalScopeCalls();

    // ── Frame history (oldest first; ms) ─────────────────────────────────────
    float hist[Profiler::HIST_FRAMES];
    int   histN = 0;
    prof.getFrameHistory(hist, histN);
    auto histArr = json::array();
    for (int i = 0; i < histN; ++i) histArr.push_back(hist[i]);
    root["frame_history_ms"] = std::move(histArr);

    // ── Profiler sections (sorted by avg ms descending) ──────────────────────
    struct Row { const char* name; float avg_ms; float max_ms; float avg_calls; };
    std::vector<Row> rows;
    rows.reserve(prof.sectionCount());
    for (int i = 0; i < prof.sectionCount(); ++i) {
        const auto& s = prof.section(i);
        rows.push_back({s.name, s.avg_ns / 1.0e6f, s.max_ns / 1.0e6f, s.avg_calls});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b){ return a.avg_ms > b.avg_ms; });
    auto secArr = json::array();
    for (const auto& r : rows) {
        secArr.push_back({
            {"name",      r.name ? r.name : "?"},
            {"avg_ms",    r.avg_ms},
            {"max_ms",    r.max_ms},
            {"avg_calls", r.avg_calls},
        });
    }
    root["sections"] = std::move(secArr);

    // ── tubed (YouTubeAPI) telemetry ─────────────────────────────────────────
    const auto& yt = youtube_api_.telemetry();
    root["tubed"] = {
        {"searches_inflight",   yt.searches_inflight.load()},
        {"streams_inflight",    yt.streams_inflight.load()},
        {"previews_inflight",   yt.previews_inflight.load()},
        {"searches_total",      yt.searches_total.load()},
        {"streams_total",       yt.streams_total.load()},
        {"previews_total",      yt.previews_total.load()},
        {"streams_failed",      yt.streams_failed.load()},
        {"previews_cancelled",  yt.previews_cancelled.load()},
        {"last_search_ms",      yt.last_search_ms.load()},
        {"last_stream_ms",      yt.last_stream_ms.load()},
        {"last_preview_ms",     yt.last_preview_ms.load()},
        {"ema_search_ms",       yt.ema_search_ms_x10.load() / 10.0},
        {"ema_stream_ms",       yt.ema_stream_ms_x10.load() / 10.0},
        {"ema_preview_ms",      yt.ema_preview_ms_x10.load() / 10.0},
        {"tubed_wait_ms_total", yt.tubed_wait_ms_total.load()},
    };

    // ── Image manager telemetry ──────────────────────────────────────────────
    if (image_manager_) {
        const auto& im = image_manager_->telemetry();
        root["images"] = {
            {"downloads_inflight",       im.downloads_inflight.load()},
            {"queue_depth",              im.queue_depth.load()},
            {"texture_queue_depth",      im.texture_queue_depth.load()},
            {"cache_size",               im.cache_size.load()},
            {"thumbnails_loaded_total",  im.thumbnails_loaded_total.load()},
            {"thumbnails_failed_total",  im.thumbnails_failed_total.load()},
        };
    }

    // ── State context — what was happening when the snapshot was taken ───────
    const char* screen = "?";
    switch (state_.currentScreen) {
        case TubeState::Screen::Home:     screen = "Home";     break;
        case TubeState::Screen::Search:   screen = "Search";   break;
        case TubeState::Screen::Playback: screen = "Playback"; break;
    }
    size_t q_size = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        q_size = main_thread_queue_.size();
    }
    root["state"] = {
        {"screen",                screen},
        {"current_video_id",      current_video_.id},
        {"is_playing",            mpv_player_.isPlaying()},
        {"is_playing_preview",    is_playing_preview_},
        {"is_loading_preview",    is_loading_preview_},
        {"miniplayer_active",     state_.miniplayerActive},
        {"is_scrubbing",          state_.isScrubbing},
        {"is_loading_video",      state_.isLoadingVideo},
        {"is_searching",          state_.isSearching},
        {"show_ui",               state_.showUi},
        {"show_description",      state_.showDescriptionDrawer},
        {"speed",                 state_.speed},
        {"main_queue_size",       q_size},
        {"vo_drops",              mpv_player_.getPropertyInt("vo-drop-frame-count")},
        {"decoder_drops",         mpv_player_.getPropertyInt("decoder-frame-drop-count")},
    };

    // ── System (RAM / storage) ───────────────────────────────────────────────
    double ram_mb = 0.0, storage_free = 0.0, storage_total = 0.0;
    getSystemMemoryAndStorage(ram_mb, storage_free, storage_total);
    root["system"] = {
        {"ram_rss_mb",      ram_mb},
        {"storage_free_gb", storage_free},
        {"storage_total_gb", storage_total},
    };

    // ── Write to disk ────────────────────────────────────────────────────────
    std::ofstream ofs(outPath);
    if (!ofs) {
        std::cerr << "[profile] FAILED to open " << outPath << " for write\n";
        return {};
    }
    ofs << root.dump(2);
    ofs.close();

    std::cerr << "[profile] wrote snapshot to " << outPath
              << " (sections=" << prof.sectionCount()
              << ", frames=" << histN
              << ", frame_id=" << prof.frameId() << ")\n";
    return outPath;
}

void App::showPlaybackUi() {
    playback_ui_timeout_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (!state_.showUi) {
        state_.showUi = true;
        uiDirty_ = true;
    }
}

void App::renderBrowseLoadingState(int width, int height, const std::string& text) {
    float time = SDL_GetTicks() / 1000.0f;
    drawSpinner(renderer_, width / 2, height / 2 - 15, 20, time);
    drawTextCentered(renderer_, width / 2, height / 2 + 20, text, 2, theme::TEXT_3);
    uiDirty_ = true;
}




void App::openKeyboard() {
    state_.inputMode = TubeState::InputMode::SearchText;
    state_.textBuffer.clear();
    state_.textCursor = 0;
    state_.keyboardSelectedIndex = 0;
    keyboard_.resetRepeatState();
    uiDirty_ = true;
}

void App::closeKeyboard(bool commit) {
    state_.inputMode = TubeState::InputMode::None;
    state_.leftTrigger = 0.0f;
    state_.rightTrigger = 0.0f;
    keyboard_.resetRepeatState();
    if (commit && !state_.textBuffer.empty()) doSearch(state_.textBuffer);
    uiDirty_ = true;
}

void App::activateKeyboardGo() {
    if (state_.inputMode == TubeState::InputMode::SearchText) closeKeyboard(true);
}

void App::activateSelectedKey() {
    if (state_.inputMode != TubeState::InputMode::SearchText) return;
    const auto* key = keyboard_.getSelectedKey(state_,
                                              cached_window_w_,
                                              cached_window_h_);
    if (key == nullptr) return;
    
    std::string value = key->value;
    if (value == "__BACKSPACE__")     KeyboardOverlay::eraseActiveBufferChar(state_);
    else if (value == "__MODE__")     keyboard_.toggleMode(state_, uiDirty_);
    else if (value == "__LEFT__")     KeyboardOverlay::moveActiveCursor(state_, -1);
    else if (value == "__RIGHT__")    KeyboardOverlay::moveActiveCursor(state_, 1);
    else if (value == "__ENTER__")    { activateKeyboardGo(); return; }
    else if (value == "__CANCEL__")   { closeKeyboard(false); return; }
    else                              KeyboardOverlay::insertActiveText(state_, value);
    uiDirty_ = true;
}

void App::doSearch(const std::string& query) {
    stopBrowsePreviewState();
    state_manager_.transitionTo(TubeState::Screen::Search);
    state_.isSearching = true;
    state_.isLoadingVideo = false;
    uiDirty_ = true;
    current_search_query_ = query;
    search_page_ = 1;
    pendingMoreSearch_ = false;
    
    search_grid_->clear();
    focus_manager_.resetGridFocus(search_grid_);
    focus_manager_.setGrid(search_grid_);
    
    if (image_manager_) image_manager_->clearCache();

    int reqPage = search_page_;
    std::string reqQuery = query;
    youtube_api_.search(query, reqPage, [this, reqQuery, reqPage](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqQuery, reqPage, results, finished]() {
            if (state_.currentScreen != TubeState::Screen::Search || current_search_query_ != reqQuery || search_page_ != reqPage) return;
            
            if (finished) {
                state_.isSearching = false;
                // Whole search completed — persist so a crash before
                // exit doesn't lose this set of results.
                if (browse_state_ready_) saveBrowseState();
                uiDirty_ = true;
                return;
            }

            if (!results.empty()) {
                bool isFirst = search_grid_->videos.empty();
                for (const auto& v : results) {
                    search_grid_->addVideo(v);
                }
                if (isFirst) {
                    focus_manager_.setGrid(search_grid_);
                    // Snapshot as soon as the first page lands.  Each
                    // subsequent page is appended; the "finished" branch
                    // above re-saves at the end for the full set.  This
                    // first-page save covers the common case of a user
                    // glancing at results and getting interrupted mid-
                    // scroll.
                    if (browse_state_ready_) saveBrowseState();
                }
                uiDirty_ = true;
            }
        });
    });
}

void App::playVideo(const YouTubeVideo& video, bool forceFullscreen) {
    if (state_.currentScreen == TubeState::Screen::Playback && current_video_.id == video.id) return;
    // If same video is already loading, do not restart it
    if (state_.isLoadingVideo && current_video_.id == video.id) return;
    // If a different video is loading, cancel it and start the new one
    state_.isLoadingVideo = false;
    
    bool keepMiniplayer = state_.miniplayerActive && !forceFullscreen;
    if (!keepMiniplayer) {
        state_manager_.setMiniplayerActive(false);
    }
    
    addToHistory(video);
    stopBrowsePreviewState();
    mpv_player_.stop();
    storyboard_.stop();
    // Keep the (bounded) thumbnail atlas warm across playback so returning to
    // the grid doesn't re-decode every thumbnail.

    current_video_ = video;
    state_.isLoadingVideo = true;
    state_.showDescriptionDrawer = false;
    description_scroll_row_ = 0;
    active_video_metadata_ = VideoPlaybackMetadata();
    wrapped_description_lines_.clear();
    loading_status_text_ = "Resolving Stream...";
    last_playback_seconds_ = -1;
    prefetched_next_video_id_.clear();
    uiDirty_ = true;

    const std::string cacheKey = streamCacheKey(video.id, state_.maxQualityHeight);
    auto cachedOpt = getCachedStreamUrl(cacheKey);
    if (cachedOpt.has_value() && !cachedOpt.value().empty()) {
        state_.isLoadingVideo = false;
        if (keepMiniplayer) {
            state_manager_.transitionTo(state_.currentScreen);
            state_manager_.setMiniplayerActive(true);
            state_.showUi = true;
        } else {
            state_manager_.transitionTo(TubeState::Screen::Playback);
            state_.showUi = false;
        }
        mpv_player_.setMute(state_.muted);
        mpv_player_.setVolume(state_.volume);
        mpv_player_.setSpeed(state_.speed);

        std::string stream_url, subtitle_url, audio_url;
        splitCachedStream(cachedOpt.value(), stream_url, subtitle_url, audio_url);

        mpv_player_.play(stream_url, subtitle_url, audio_url);
        mpv_player_.showText("Loading " + std::to_string(state_.maxQualityHeight) + "p");
        triggerVideoFade();
        if (!keepMiniplayer) {
            state_.showUi = false;
        }
        uiDirty_ = true;

        // Start storyboard extraction
        if (!video.is_live) {
            storyboard_.start(stream_url, video.duration_seconds);
        }

        // The cached URL carries no metadata, which previously left the player's
        // stat row stuck on "loading stats". Fetch stats/description from the
        // backend — a tubed cache hit, so it's fast and spawns no yt-dlp.
        youtube_api_.getStreamUrl(video.id, state_.maxQualityHeight,
            [this, video](bool ok, const std::string&, const std::string&, const std::string&, const VideoPlaybackMetadata& meta) {
                if (!ok) return;
                queueOnMainThread([this, video, meta]() {
                    if (current_video_.id != video.id) return;
                    active_video_metadata_ = meta;
                    wrapped_description_lines_ = wrapText(meta.description, 280, 1);
                    uiDirty_ = true;
                });
            },
            /*isPreview=*/false, /*isLive=*/video.is_live);
        return;
    }

    youtube_api_.getStreamUrl(video.id, state_.maxQualityHeight, [this, video, cacheKey, keepMiniplayer](bool success, const std::string& url, const std::string& subtitle_url, const std::string& audio_url, const VideoPlaybackMetadata& meta) {
        queueOnMainThread([this, video, cacheKey, success, url, subtitle_url, audio_url, meta, keepMiniplayer]() {
            if (!state_.isLoadingVideo || current_video_.id != video.id) return;
            state_.isLoadingVideo = false;
            if (success) {
                active_video_metadata_ = meta;
                wrapped_description_lines_ = wrapText(meta.description, 280, 1);
                setCachedStreamUrl(cacheKey, url + "|" + subtitle_url + "|" + audio_url);
                if (keepMiniplayer) {
                    state_manager_.transitionTo(state_.currentScreen);
                    state_manager_.setMiniplayerActive(true);
                    state_.showUi = true;
                } else {
                    state_manager_.transitionTo(TubeState::Screen::Playback);
                    state_.showUi = false;
                }
                mpv_player_.setMute(state_.muted);
                mpv_player_.setVolume(state_.volume);
                mpv_player_.setSpeed(state_.speed);
                mpv_player_.play(url, subtitle_url, audio_url);
                mpv_player_.showText("Loading " + std::to_string(state_.maxQualityHeight) + "p");
                triggerVideoFade();

                // Start storyboard extraction.  Live streams don't have a
                // storyboard image, so skip it — saves a yt-dlp run.
                if (!video.is_live) {
                    storyboard_.start(url, video.duration_seconds);
                }
            } else {
                setCachedStreamUrl(cacheKey, ""); // Cache failure
                loading_status_text_ = "Stream Resolve Failed — Press A to retry";
                state_.isLoadingVideo = false;
            }
            uiDirty_ = true;
        });
    },
    /*isPreview=*/false, /*isLive=*/video.is_live);
}

void App::updateSticks(float dt) {
    if (state_.inputMode == TubeState::InputMode::SearchText) {
        const int w = cached_window_w_;
        const int h = cached_window_h_;
        if (keyboard_.updateSelectionFromDpad(state_, w, h, uiDirty_)) return;
        if (keyboard_.updateSelectionFromStick(state_, w, h, uiDirty_)) return;
        keyboard_.updateCursorFromTriggers(state_, uiDirty_, [this](int delta) {
            KeyboardOverlay::moveActiveCursor(state_, delta);
        });
    } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
        int dirX = 0, dirY = 0;
        if (state_.dpadLeftPressed)       dirX = -1;
        else if (state_.dpadRightPressed) dirX = 1;
        else if (state_.dpadUpPressed)    dirY = -1;
        else if (state_.dpadDownPressed)  dirY = 1;
        
        if (dirX == 0 && dirY == 0) {
            const float threshold = 0.5f;
            const float absX = std::abs(state_.leftStickX);
            const float absY = std::abs(state_.leftStickY);
            if (absX >= threshold || absY >= threshold) {
                if (absX >= absY) dirX = (state_.leftStickX > 0.0f) ? 1 : -1;
                else              dirY = (state_.leftStickY > 0.0f) ? 1 : -1;
            }
        }
        
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (dirX != 0 || dirY != 0) {
            if (dirX != lastStickDirX_ || dirY != lastStickDirY_ || now >= nextStickNavAt_) {
                lastStickDirX_ = dirX;
                lastStickDirY_ = dirY;
                nextStickNavAt_ = now + milliseconds(140);
                focus_manager_.handleInput(dirX, dirY);
                uiDirty_ = true;
            }
        } else {
            lastStickDirX_ = 0;
            lastStickDirY_ = 0;
        }
    } else if (state_.currentScreen == TubeState::Screen::Playback) {
        // (Volume is on the L2/R2 BUTTONS now — handled in
        // handleJoyButton — because the R36S exposes L2/R2 as digital
        // buttons 6/7, not analog trigger axes.  The old analog-trigger
        // path here never fired on this device.)

        // Repeating scroll for description drawer
        if (state_.showDescriptionDrawer) {
            static auto lastDescScroll = std::chrono::steady_clock::now();
            auto scrollNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(scrollNow - lastDescScroll).count() > 80) {
                const Uint8* keys = SDL_GetKeyboardState(nullptr);
                bool kbUp = keys[SDL_SCANCODE_UP];
                bool kbDown = keys[SDL_SCANCODE_DOWN];
                
                if (state_.dpadUpPressed || kbUp || state_.leftStickY < -0.5f) {
                    description_scroll_row_ = std::max(0, description_scroll_row_ - 1);
                    uiDirty_ = true;
                    lastDescScroll = scrollNow;
                } else if (state_.dpadDownPressed || kbDown || state_.leftStickY > 0.5f) {
                    description_scroll_row_++;
                    uiDirty_ = true;
                    lastDescScroll = scrollNow;
                }
            }
        }

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        bool kbLeft = keys[SDL_SCANCODE_LEFT];
        bool kbRight = keys[SDL_SCANCODE_RIGHT];
        bool wantsScrub = !current_video_.is_live && !state_.showDescriptionDrawer && (state_.dpadLeftPressed || state_.dpadRightPressed || kbLeft || kbRight || std::abs(state_.leftStickX) > 0.2f);
        if (wantsScrub) {
            showPlaybackUi();
            if (!state_.isScrubbing) {
                state_.isScrubbing = true;
                state_.scrubTargetTime = mpv_player_.getPlaybackTime();
                mpv_player_.pause();
                scrub_hold_time_ = 0.0f;
            }
            
            double delta = 0.0;
            if (state_.dpadLeftPressed || kbLeft) {
                delta = -0.5; // 0.5 seconds base rate
            } else if (state_.dpadRightPressed || kbRight) {
                delta = 0.5;
            } else if (std::abs(state_.leftStickX) > 0.2f) {
                delta = state_.leftStickX * 1.0;
            }
            
            if (delta != 0.0) {
                scrub_hold_time_ += dt;
                double multiplier = 1.0 + static_cast<double>(scrub_hold_time_ * scrub_hold_time_ * 15.0);
                if (multiplier > 100.0) multiplier = 100.0;
                
                state_.scrubTargetTime += delta * multiplier;
                state_.scrubTargetTime = std::max(0.0, std::min(mpv_player_.getDuration(), state_.scrubTargetTime));
                uiDirty_ = true;
            }
        } else {
            if (state_.isScrubbing) {
                mpv_player_.seekAbsoluteKeyframes(state_.scrubTargetTime);
                last_seek_time_ = state_.scrubTargetTime;
                last_seek_time_point_ = std::chrono::steady_clock::now();
                mpv_player_.resume();
                play_flash_start_time_ = SDL_GetTicks();
                state_.isScrubbing = false;
                scrub_hold_time_ = 0.0f;
                uiDirty_ = true;
            }
        }
    }
}

void App::updateKeyboardCursorBlinkState() {
    if (state_.inputMode != TubeState::InputMode::SearchText) return;
    using namespace std::chrono;
    const auto phase = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 400;
    bool visible = (phase % 2) == 0;
    if (visible != lastKeyboardCursorVisible_) {
        lastKeyboardCursorVisible_ = visible;
        uiDirty_ = true;
    }
}

void App::renderFrame() {
    PROFILE_SCOPE("renderFrame");
    auto render_start = std::chrono::steady_clock::now();

    // Use cached dimensions — window is fixed 640x480 fullscreen and
    // SDL_GetWindowSize traverses through SDL's display state for what
    // amounts to two reads against a never-changing pair of ints.
    const int width  = cached_window_w_;
    const int height = cached_window_h_;
    bool shouldPresent = uiDirty_ || state_.isLoadingVideo || state_.isScrubbing || state_.isSearching || (state_.miniplayerActive && mpv_player_.isPlaying());
    if (!shouldPresent) {
        PROFILE_COUNT("frame_skipped");
        return;
    }

    mpv_player_.beginFrame();

    { PROFILE_SCOPE("compositor"); compositor_->render(this, width, height); }

    uiDirty_ = false;

    auto render_end = std::chrono::steady_clock::now();
    render_latency_ms_ = std::chrono::duration<float, std::milli>(render_end - render_start).count();
}


void App::handleEvent(SDL_Event& event) {
    // ── Screen-off mode intercept ────────────────────────────────────────────
    // While the panel is blanked, ANY real input wakes the screen and is
    // consumed (so the wake press doesn't also trigger an action).
    // Non-input events (quit/window) still pass through below.
    if (screenOff_) {
        bool wakeInput = (event.type == SDL_KEYDOWN
                       || event.type == SDL_CONTROLLERBUTTONDOWN
                       || event.type == SDL_JOYBUTTONDOWN
                       || event.type == SDL_JOYHATMOTION);
        if (!wakeInput && event.type == SDL_CONTROLLERAXISMOTION)
            wakeInput = std::abs(event.caxis.value) > 12000;
        if (!wakeInput && event.type == SDL_JOYAXISMOTION)
            wakeInput = std::abs(event.jaxis.value) > 12000;
        if (wakeInput) {
            exitScreenOff();
            showPlaybackToast("Screen on");
            uiDirty_ = true;
            return;
        }
        if (event.type == SDL_QUIT) state_.running = false;
        return;   // suppress all other input while dark
    }

    // Arm-cancel: while the "press X again" prompt is showing, ANY non-X
    // button press cancels it (and still performs that button's action).
    if (screenOffArmMs_ != 0) {
        const bool anyPress = (event.type == SDL_KEYDOWN
                            || event.type == SDL_CONTROLLERBUTTONDOWN
                            || event.type == SDL_JOYBUTTONDOWN);
        const bool isX =
            (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_x) ||
            (event.type == SDL_CONTROLLERBUTTONDOWN &&
             event.cbutton.button == SDL_CONTROLLER_BUTTON_X) ||
            (event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 2);
        if (anyPress && !isX) {
            screenOffArmMs_ = 0;
            showPlaybackToast("Screen-off cancelled");
            uiDirty_ = true;
        }
    }

    if (state_.currentScreen == TubeState::Screen::Playback) {
        // Wake the HUD on ACTUAL user input.  Axis motion is filtered to
        // outside-deadzone values only — sticks at rest constantly emit
        // motion events from drift/noise, which were keeping the 5 s
        // auto-hide timer in a permanent reset loop so the HUD never
        // disappeared on its own.  Threshold ~24 % of full range matches
        // a deliberate stick push.
        bool wake = (event.type == SDL_KEYDOWN
                  || event.type == SDL_CONTROLLERBUTTONDOWN
                  || event.type == SDL_JOYBUTTONDOWN
                  || event.type == SDL_JOYHATMOTION);
        if (!wake && event.type == SDL_CONTROLLERAXISMOTION) {
            wake = std::abs(event.caxis.value) > 8000;
        }
        if (!wake && event.type == SDL_JOYAXISMOTION) {
            wake = std::abs(event.jaxis.value) > 8000;
        }
        if (wake) showPlaybackUi();
    }

    // Refresh the cached window size on resize (defensive — the
    // fullscreen KMSDRM window doesn't actually resize, but if the
    // build is ever run on desktop the cache must not go stale).
    if (event.type == SDL_WINDOWEVENT &&
        (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
         event.window.event == SDL_WINDOWEVENT_RESIZED)) {
        SDL_GetWindowSize(window_, &cached_window_w_, &cached_window_h_);
    }

    switch (event.type) {
    case SDL_QUIT: state_.running = false; break;
    case SDL_KEYDOWN: handleKey(event.key.keysym.sym); break;
    case SDL_KEYUP: handleKeyUp(event.key.keysym.sym); break;
    case SDL_CONTROLLERDEVICEADDED: openController(); break;
    case SDL_CONTROLLERDEVICEREMOVED: closeController(); openController(); break;
    case SDL_CONTROLLERBUTTONDOWN: handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), true); break;
    case SDL_CONTROLLERBUTTONUP: handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), false); break;
    case SDL_JOYHATMOTION:
        if (controller_ == nullptr) handleJoyHat(event.jhat.value);
        break;
    case SDL_JOYAXISMOTION:
        if (controller_ == nullptr) handleJoyAxis(event.jaxis);
        break;
    case SDL_JOYBUTTONDOWN:
        if (event.jbutton.button == 16) {
            select_held_ = true;
            select_action_triggered_ = false;
        }
        if (controller_ == nullptr) handleJoyButton(event.jbutton.button, event.jbutton.which, true);
        break;
    case SDL_JOYBUTTONUP:
        if (event.jbutton.button == 16) {
            select_held_ = false;
        }
        if (controller_ == nullptr) handleJoyButton(event.jbutton.button, event.jbutton.which, false);
        break;
    case SDL_CONTROLLERAXISMOTION: handleControllerAxis(event.caxis); break;
    case SDL_TEXTINPUT:
        if (state_.inputMode == TubeState::InputMode::SearchText) {
            KeyboardOverlay::insertActiveText(state_, KeyboardOverlay::transformTypedText(state_, event.text.text));
            uiDirty_ = true;
        }
        break;
    }
}

void App::toggleMiniplayer() {
    static Uint32 lastToggleTime = 0;
    Uint32 now = SDL_GetTicks();
    if (now - lastToggleTime < 250) {
        return;
    }
    lastToggleTime = now;

    state_manager_.toggleMiniplayer();
    uiDirty_ = true;
}

void App::handleKeyUp(SDL_Keycode key) {
    if (key == SDLK_TAB || key == SDLK_F3 || key == SDLK_LSHIFT) {
        select_held_ = false;
        if (!select_action_triggered_) {
            toggleMiniplayer();
        }
    }
}

void App::handleKey(SDL_Keycode key) {
    if (state_.currentScreen == TubeState::Screen::Playback) {
        showPlaybackUi();
    }

    // Sign-in help modal captures keys while open (dev/keyboard parity with the
    // SEL+X / A / B controller flow). F1 toggles it open.
    if (state_.showSignInHelp) {
        if (key == SDLK_RETURN || key == SDLK_a) {
            youtube_api_.refreshAuthStatus();
        } else if (key == SDLK_ESCAPE || key == SDLK_b) {
            state_.showSignInHelp = false;
            youtube_api_.refreshAuthStatus();
        }
        uiDirty_ = true;
        return;
    }
    if (key == SDLK_F1) {
        state_.showSignInHelp = true;
        youtube_api_.refreshAuthStatus();
        ui_sounds::play(ui_sounds::Sound::Select);
        uiDirty_ = true;
        return;
    }

    // Settings modal — captures all keys while open.  F2 toggles it.
    // Returning false from handleKey() means "close" — caller persists.
    if (state_.showSettingsModal) {
        std::string oldHomeFeed = state_.homeFeedKind;
        if (!SettingsModal::handleKey(this, key)) {
            state_.showSettingsModal = false;
            saveSettings();   // persist whatever the user just changed
            // If they switched the Home tab kind, reload it so the change is
            // visible immediately instead of next launch.
            if (state_.homeFeedKind != oldHomeFeed &&
                state_.currentScreen == TubeState::Screen::Home) {
                loadHomeFeeds();
            }
        }
        uiDirty_ = true;
        return;
    }
    if (key == SDLK_F2) {
        state_.showSettingsModal = true;
        ui_sounds::play(ui_sounds::Sound::Select);
        uiDirty_ = true;
        return;
    }

    if (key == SDLK_TAB || key == SDLK_F3 || key == SDLK_LSHIFT) {
        select_held_ = true;
        select_action_triggered_ = false;
        return;
    }

    if (select_held_) {
        if (key == SDLK_a || key == SDLK_RETURN) {
            if (state_.currentScreen == TubeState::Screen::Playback) {
                state_.showDescriptionDrawer = !state_.showDescriptionDrawer;
                description_scroll_row_ = 0;
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        } else if (key == SDLK_RIGHT) {
            if (state_.currentScreen == TubeState::Screen::Playback || state_.miniplayerActive) {
                playNextTrack();
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        } else if (key == SDLK_LEFT) {
            if (state_.currentScreen == TubeState::Screen::Playback || state_.miniplayerActive) {
                playPreviousTrack();
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        }
    }

    if (state_.inputMode == TubeState::InputMode::SearchText) {
        if (key == SDLK_RETURN)    activateKeyboardGo();
        else if (key == SDLK_BACKSPACE) KeyboardOverlay::eraseActiveBufferChar(state_);
        else if (key == SDLK_LEFT)      KeyboardOverlay::moveActiveCursor(state_, -1);
        else if (key == SDLK_RIGHT)     KeyboardOverlay::moveActiveCursor(state_, 1);
        else if (key == SDLK_UP)        { keyboard_.moveSelection(state_, 0, -1, cached_window_w_, cached_window_h_, uiDirty_); }
        else if (key == SDLK_DOWN)      { keyboard_.moveSelection(state_, 0, 1, cached_window_w_, cached_window_h_, uiDirty_); }
        else if (key == SDLK_ESCAPE)    closeKeyboard(false);
        return;
    }
    switch (key) {
    case SDLK_q:
        state_.running = false;
        break;
    case SDLK_ESCAPE:
        if (state_.isLoadingVideo) {
            state_.isLoadingVideo = false;
            uiDirty_ = true;
        } else if (state_.miniplayerActive || state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                state_.showDescriptionDrawer = false;
                uiDirty_ = true;
            } else {
                leavePlayback();
            }
        } else {
            state_.running = false;
        }
        break;
    case SDLK_TAB:
    case SDLK_BACKSPACE:
        toggleMiniplayer();
        break;
    case SDLK_p:
    case SDLK_SPACE:
        if (state_.miniplayerActive || state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                showPlaybackToast("Playing");
            }
            uiDirty_ = true;
        }
        break;
    case SDLK_y:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.toggleSubtitles();
            showPlaybackToast("Subtitles Toggled");
        } else {
            openKeyboard();
        }
        break;
    case SDLK_s:
        // (mpv stats overlay bind removed — it conflicted with the
        // debug overlay and audio-track UX.)
        break;
    case SDLK_r:
        if (state_.currentScreen == TubeState::Screen::Home) {
            cached_trending_videos_.clear();
            loadHomeFeeds();
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            if (!current_search_query_.empty()) {
                doSearch(current_search_query_);
                uiDirty_ = true;
            }
        }
        break;
    case SDLK_x:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            // X in the player toggles screen-off / power-save mode
            // (double-tap to confirm).  Replaces the removed mpv stats
            // overlay.
            requestScreenOffToggle();
        } else if (state_.currentScreen == TubeState::Screen::Home ||
                   state_.currentScreen == TubeState::Screen::Search) {
            // Keyboard parity with the controller X binding: open sign-in
            // help.  Replaces the old dead resolution cycler.
            state_.showSignInHelp = true;
            youtube_api_.refreshAuthStatus();
            ui_sounds::play(ui_sounds::Sound::Select);
            uiDirty_ = true;
        }
        break;
    case SDLK_UP:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            // D-pad Up only scrolls the description drawer now.  The old
            // "cycle subtitle track" action was an unwanted double-bind
            // that fired whenever the user nudged Up during playback.
            if (state_.showDescriptionDrawer) {
                description_scroll_row_ = std::max(0, description_scroll_row_ - 1);
                uiDirty_ = true;
            }
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(0, -1);
        }
        break;
    case SDLK_DOWN:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            // D-pad Down only scrolls the description drawer now (the
            // old "cycle audio track" double-bind is removed).
            if (state_.showDescriptionDrawer) {
                description_scroll_row_++;
                uiDirty_ = true;
            }
        } else if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(0, 1);
        }
        break;
    case SDLK_LEFT:
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(-1, 0);
        }
        break;
    case SDLK_RIGHT:
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.handleInput(1, 0);
        }
        break;
    case SDLK_RETURN:
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.clickFocused();
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                play_flash_start_time_ = SDL_GetTicks();
                showPlaybackToast("Playing");
            }
        }
        break;
    case SDLK_PAGEUP:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::min(2.0, state_.speed + 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        }
        break;
    case SDLK_PAGEDOWN:
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::max(0.25, state_.speed - 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        }
        break;
    case SDLK_F12:
        state_.showDebugOverlay = !state_.showDebugOverlay;
        uiDirty_ = true;
        break;
    case SDLK_F11: {
        std::string p = dumpProfileSnapshot();
        if (!p.empty()) {
            if (state_.currentScreen == TubeState::Screen::Playback) {
                showPlaybackToast("Profile dumped: " + p);
            }
        }
        uiDirty_ = true;
        break;
    }
    default: break;
    }
}

void App::handleControllerButton(SDL_GameControllerButton button, bool down) {
    if (down && state_.currentScreen == TubeState::Screen::Playback) {
        showPlaybackUi();
    }

    // Sign-in help modal: while open it captures input — A re-checks auth, B
    // closes.  All other buttons are swallowed so nothing navigates behind it.
    if (state_.showSignInHelp) {
        if (down && button == SDL_CONTROLLER_BUTTON_A) {
            youtube_api_.refreshAuthStatus();   // user just dropped cookies.txt
        } else if (down && button == SDL_CONTROLLER_BUTTON_B) {
            state_.showSignInHelp = false;
            youtube_api_.refreshAuthStatus();   // re-check on close
        }
        if (down) { uiDirty_ = true; return; }
        return;
    }

    if (button == SDL_CONTROLLER_BUTTON_BACK) {
        select_held_ = down;
        if (down) {
            select_action_triggered_ = false;
        } else {
            if (!select_action_triggered_) {
                toggleMiniplayer();
            }
        }
        return;
    }

    if (down && select_held_) {
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (state_.miniplayerActive) {
                if (mpv_player_.isPlaying()) {
                    mpv_player_.pause();
                    showPlaybackToast("Paused");
                } else {
                    mpv_player_.resume();
                    showPlaybackToast("Playing");
                }
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            } else if (state_.currentScreen == TubeState::Screen::Playback) {
                state_.showDescriptionDrawer = !state_.showDescriptionDrawer;
                description_scroll_row_ = 0;
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
            if (state_.currentScreen == TubeState::Screen::Playback || state_.miniplayerActive) {
                playNextTrack();
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        } else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
            if (state_.currentScreen == TubeState::Screen::Playback || state_.miniplayerActive) {
                playPreviousTrack();
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        } else if (button == SDL_CONTROLLER_BUTTON_B) {
            if (state_.miniplayerActive) {
                leavePlayback();
                select_action_triggered_ = true;
                uiDirty_ = true;
                return;
            }
        } else if (button == SDL_CONTROLLER_BUTTON_Y) {
            // SELECT+Y → open the settings modal.
            // (Previously bound to a debug profile-dump; F11 still does that
            // on a keyboard, and the user-facing settings modal is the more
            // valuable chord here.)
            state_.showSettingsModal = true;
            ui_sounds::play(ui_sounds::Sound::Select);
            select_action_triggered_ = true;
            uiDirty_ = true;
            return;
        }
    }

    // Settings modal: forward controller buttons to the modal's key handler
    // so D-pad / A / B map to the same actions as the keyboard path.
    if (state_.showSettingsModal && down) {
        std::string oldHomeFeed = state_.homeFeedKind;
        bool stillOpen = true;
        switch (button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:    stillOpen = SettingsModal::handleKey(this, SDLK_UP); break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  stillOpen = SettingsModal::handleKey(this, SDLK_DOWN); break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  stillOpen = SettingsModal::handleKey(this, SDLK_LEFT); break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: stillOpen = SettingsModal::handleKey(this, SDLK_RIGHT); break;
            case SDL_CONTROLLER_BUTTON_A:          stillOpen = SettingsModal::handleKey(this, SDLK_a); break;
            case SDL_CONTROLLER_BUTTON_B:          stillOpen = SettingsModal::handleKey(this, SDLK_b); break;
            default: return;   // ignore other buttons while modal is open
        }
        if (!stillOpen) {
            state_.showSettingsModal = false;
            saveSettings();
            if (state_.homeFeedKind != oldHomeFeed &&
                state_.currentScreen == TubeState::Screen::Home) {
                loadHomeFeeds();
            }
        }
        uiDirty_ = true;
        return;
    }

    if (button == SDL_CONTROLLER_BUTTON_START && controller_ != nullptr && SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_BACK)) {
        state_.running = false;
        return;
    }

    if (button == SDL_CONTROLLER_BUTTON_DPAD_UP)    state_.dpadUpPressed = down;
    if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  state_.dpadDownPressed = down;
    if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  state_.dpadLeftPressed = down;
    if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) state_.dpadRightPressed = down;

    if (!down) return;

    if (state_.inputMode == TubeState::InputMode::SearchText) {
        if (button == SDL_CONTROLLER_BUTTON_A)             activateSelectedKey();
        else if (button == SDL_CONTROLLER_BUTTON_X)        KeyboardOverlay::eraseActiveBufferChar(state_);
        else if (button == SDL_CONTROLLER_BUTTON_Y)        KeyboardOverlay::insertActiveText(state_, " ");
        else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) keyboard_.toggleMode(state_, uiDirty_);
        else if (button == SDL_CONTROLLER_BUTTON_START)    activateKeyboardGo();
        else if (button == SDL_CONTROLLER_BUTTON_B)        closeKeyboard(false);
        return;
    }

    if (button == SDL_CONTROLLER_BUTTON_Y) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            mpv_player_.toggleSubtitles();
            showPlaybackToast("Subtitles Toggled");
        } else {
            openKeyboard();
        }
    } else if (button == SDL_CONTROLLER_BUTTON_A) {
        if ((state_.currentScreen == TubeState::Screen::Home || state_.currentScreen == TubeState::Screen::Search) && !isInputLocked()) {
            focus_manager_.clickFocused();
        } else if (state_.currentScreen == TubeState::Screen::Playback) {
            if (mpv_player_.isPlaying()) {
                mpv_player_.pause();
                showPlaybackToast("Paused");
            } else {
                mpv_player_.resume();
                play_flash_start_time_ = SDL_GetTicks();
                showPlaybackToast("Playing");
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_B) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                state_.showDescriptionDrawer = false;
                ui_sounds::play(ui_sounds::Sound::Back);
                uiDirty_ = true;
            } else {
                ui_sounds::play(ui_sounds::Sound::Back);
                leavePlayback();
            }
        } else if (state_.isLoadingVideo) {
            state_.isLoadingVideo = false;
            ui_sounds::play(ui_sounds::Sound::Back);
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            ui_sounds::play(ui_sounds::Sound::Back);
            state_manager_.transitionTo(TubeState::Screen::Home);
        }
    } else if (button == SDL_CONTROLLER_BUTTON_X) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            // X in the player toggles screen-off / power-save mode
            // (double-tap to confirm).
            requestScreenOffToggle();
        } else if (state_.currentScreen == TubeState::Screen::Home ||
                   state_.currentScreen == TubeState::Screen::Search) {
            // X opens the sign-in help modal directly (replaced the old
            // SEL+X chord and the dead resolution cycler).  If SELECT is
            // held we mark the chord as "consumed" so SELECT release
            // doesn't ALSO toggle the miniplayer.
            state_.showSignInHelp = true;
            youtube_api_.refreshAuthStatus();
            ui_sounds::play(ui_sounds::Sound::Select);
            if (select_held_) select_action_triggered_ = true;
            uiDirty_ = true;
        }
    } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
        if (state_.currentScreen == TubeState::Screen::Home) {
            cached_trending_videos_.clear();
            loadHomeFeeds();
            uiDirty_ = true;
        } else if (state_.currentScreen == TubeState::Screen::Search) {
            if (!current_search_query_.empty()) {
                doSearch(current_search_query_);
                uiDirty_ = true;
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
        // L3 toggles OUR debug overlay everywhere — including during
        // playback.  It used to call mpv's built-in stats overlay in
        // the player, which "absorbed" the debug-overlay toggle the
        // user actually wanted.  The mpv stats overlay is removed.
        state_.showDebugOverlay = !state_.showDebugOverlay;
        uiDirty_ = true;
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
        // Description-drawer scroll only (subtitle-track cycle removed).
        if (state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                description_scroll_row_ = std::max(0, description_scroll_row_ - 1);
                uiDirty_ = true;
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        // Description-drawer scroll only (audio-track cycle removed).
        if (state_.currentScreen == TubeState::Screen::Playback) {
            if (state_.showDescriptionDrawer) {
                description_scroll_row_++;
                uiDirty_ = true;
            }
        }
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
        // Handled via updateSticks() scrubbing
    } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        // Handled via updateSticks() scrubbing
    } else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::max(0.25, state_.speed - 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        } else {
            state_.backgroundDaemonEnabled = !state_.backgroundDaemonEnabled;
            saveSettings();
            uiDirty_ = true;
        }
    } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        if (state_.currentScreen == TubeState::Screen::Playback) {
            state_.speed = std::min(2.0, state_.speed + 0.25);
            mpv_player_.setSpeed(state_.speed);
            showPlaybackToast("Speed " + std::to_string(state_.speed).substr(0, 4) + "x");
        } else {
            state_.showUi = !state_.showUi;
            uiDirty_ = true;
        }
    }
}

void App::handleJoyHat(Uint8 value) {
    state_.dpadUpPressed    = (value & SDL_HAT_UP) != 0;
    state_.dpadDownPressed  = (value & SDL_HAT_DOWN) != 0;
    state_.dpadLeftPressed  = (value & SDL_HAT_LEFT) != 0;
    state_.dpadRightPressed = (value & SDL_HAT_RIGHT) != 0;

    if (state_.inputMode == TubeState::InputMode::SearchText) {
        if (value & SDL_HAT_UP)    { keyboard_.moveSelection(state_, 0, -1, cached_window_w_, cached_window_h_, uiDirty_); }
        if (value & SDL_HAT_DOWN)  { keyboard_.moveSelection(state_, 0, 1, cached_window_w_, cached_window_h_, uiDirty_); }
        if (value & SDL_HAT_LEFT)  { keyboard_.moveSelection(state_, -1, 0, cached_window_w_, cached_window_h_, uiDirty_); }
        if (value & SDL_HAT_RIGHT) { keyboard_.moveSelection(state_, 1, 0, cached_window_w_, cached_window_h_, uiDirty_); }
    }
}

void App::handleJoyAxis(const SDL_JoyAxisEvent& jaxis) {
    float normalized = (float)jaxis.value / 32767.0f;
    if (std::abs(jaxis.value) < 10000) normalized = 0.0f;
    if (jaxis.axis == 0) state_.leftStickX = normalized;
    else if (jaxis.axis == 1) state_.leftStickY = normalized;
    else if (jaxis.axis == 2) state_.rightStickX = normalized;
    else if (jaxis.axis == 3) state_.rightStickY = normalized;
}

void App::handleJoyButton(Uint8 button, SDL_JoystickID instanceId, bool down) {
    // Diagnostic — read off raw joystick button indices so ambiguous
    // physical buttons (notably L2/R2, which on some R36S images arrive
    // as digital buttons rather than analog triggers) can be mapped
    // correctly.  Only logs on press to avoid doubling the spam.
    if (down) std::cerr << "[app] joybutton index=" << (int)button << "\n";
    switch (button) {
    case 0: handleControllerButton(SDL_CONTROLLER_BUTTON_B, down); break;
    case 1: handleControllerButton(SDL_CONTROLLER_BUTTON_A, down); break;
    case 2: handleControllerButton(SDL_CONTROLLER_BUTTON_X, down); break;
    case 3: handleControllerButton(SDL_CONTROLLER_BUTTON_Y, down); break;
    case 4: handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, down); break;
    case 5: handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, down); break;
    case 8: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_UP, down); break;
    case 9: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, down); break;
    case 10: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, down); break;
    case 11: handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, down); break;
    case 12: 
    case 13:
        if (down) {
            SDL_Joystick* joy = SDL_JoystickFromInstanceID(instanceId);
            if (joy && SDL_JoystickGetButton(joy, 12) && SDL_JoystickGetButton(joy, 13)) { state_.running = false; break; }
        }
        if (button == 12) handleControllerButton(SDL_CONTROLLER_BUTTON_BACK, down);
        else handleControllerButton(SDL_CONTROLLER_BUTTON_START, down);
        break;
    // R36S button indices:
    //   6 = L2, 7 = R2        (triggers exposed as digital buttons)
    //   14 = L3, 15 = R3      (stick clicks — NOT in the abbreviated
    //                          mapping table, but the device has them)
    // The old code conflated 6+14 → LEFTSTICK and 7+15 → RIGHTSTICK,
    // which is why BOTH L2 and L3 were hitting the stats overlay.  Split
    // them: L2/R2 → volume, L3/R3 → their real stick-click bindings.
    case 6:  // L2 → volume down (player only)
        if (down && state_.currentScreen == TubeState::Screen::Playback)
            adjustPlayerVolume(-1);
        break;
    case 7:  // R2 → volume up (player only)
        if (down && state_.currentScreen == TubeState::Screen::Playback)
            adjustPlayerVolume(+1);
        break;
    case 14: // L3 → debug overlay toggle (handleControllerButton routes it)
        handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, down);
        break;
    case 15: // R3 → reload feed
        handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, down);
        break;
    case 16:
        select_held_ = down;
        if (down) {
            select_action_triggered_ = false;
        }
        break;
    default: break;
    }
}

void App::handleControllerAxis(const SDL_ControllerAxisEvent& caxis) {
    float normalized = static_cast<float>(caxis.value) / 32767.0f;
    if (std::abs(caxis.value) < 8000) normalized = 0.0f;
    switch (caxis.axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:       state_.leftStickX = normalized; break;
    case SDL_CONTROLLER_AXIS_LEFTY:       state_.leftStickY = normalized; break;
    case SDL_CONTROLLER_AXIS_RIGHTX:      state_.rightStickX = normalized; break;
    case SDL_CONTROLLER_AXIS_RIGHTY:      state_.rightStickY = normalized; break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: state_.leftTrigger = normalized; break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: state_.rightTrigger = normalized; break;
    default: break;
    }
}

void App::loadHomeFeeds() {
    stopBrowsePreviewState();
    home_page_ = 1;
    homeLoadFailed_ = false;
    state_.isLoadingVideo = false;

    using namespace std::chrono;
    auto now = steady_clock::now();

    // Decide which feed to load up-front so cache lookups can be kind-aware.
    // Subscriptions requires sign-in; without cookies we silently fall back
    // to trending (and the title makes that visible).
    const bool wantsSubs = (state_.homeFeedKind == "subscriptions");
    const bool useSubs   = wantsSubs && state_.authed;
    const std::string kind = useSubs ? "subscriptions" : "trending";
    std::cout << "[App] loadHomeFeeds: homeFeedKind='" << state_.homeFeedKind
              << "' authed=" << state_.authed
              << " → kind='" << kind << "'"
              << (wantsSubs && !useSubs ? " (subs requested but guest — fallback)" : "")
              << std::endl;

    // If the on-disk cache was for a different kind, discard it.  Otherwise
    // toggling Trending↔Subscriptions in the modal would keep showing the
    // old feed's cards even after we tried to switch.
    if (!cached_home_kind_.empty() && cached_home_kind_ != kind) {
        cached_trending_videos_.clear();
        home_grid_->clear();
        focus_manager_.resetGridFocus(home_grid_);
        focus_manager_.setGrid(home_grid_);
    }

    // Load from disk cache if memory cache is empty AND for this kind.
    if (cached_trending_videos_.empty()) {
        loadHomeCache();
        // loadHomeCache restores trending only (the disk cache is shared);
        // if we're loading subscriptions, treat it as empty so we fetch.
        if (kind == "subscriptions") {
            cached_trending_videos_.clear();
        }
    }

    // If cache is fresh AND matches the requested kind, render from cache.
    if (!cached_trending_videos_.empty() && cached_home_kind_ == kind &&
            duration_cast<minutes>(now - trending_cache_time_).count() < 30) {
        state_.isSearching = false;
        if (home_grid_->videos.empty()) {
            for (const auto& v : cached_trending_videos_) {
                home_grid_->addVideo(v);
            }
            focus_manager_.setGrid(home_grid_);
        }
        home_grid_->title = (kind == "subscriptions") ? "Subscriptions" : "Trending";
        uiDirty_ = true;
        return;
    }

    // Stale-While-Revalidate: If we have no cache, we must clear and show loading
    if (cached_trending_videos_.empty()) {
        home_grid_->clear();
        focus_manager_.resetGridFocus(home_grid_);
        focus_manager_.setGrid(home_grid_);
    }

    state_.isSearching = true;
    uiDirty_ = true;
    if (wantsSubs && !state_.authed) {
        home_grid_->title = "Trending (Sign in for Subscriptions)";
    } else {
        home_grid_->title = useSubs ? "Subscriptions" : "Trending";
    }
    home_feed_query_ = useSubs ? "subs" : "trending";
    pendingMoreHome_ = false;

    int reqPage = home_page_;
    // Streamed/incremental population: render each card the instant it
    // arrives from the YouTubeAPI callback chain instead of waiting for
    // the whole batch.  Net effect: the UI starts showing results within
    // a couple of seconds even when the full page takes 15+ seconds.
    //
    // The first arriving card flips focus to the home grid (so the
    // loading overlay can clear) and marks the load as a success early
    // — the user can scroll/click while subsequent items stream in.
    // cached_trending_videos_ is cleared once at the start of this page
    // so we don't accumulate stale entries on top of a refresh.
    if (!cached_trending_videos_.empty()) {
        // Was a cache-hit render before; refreshing → clear and rebuild
        // so we don't accumulate stale entries on top of fresh ones.
        home_grid_->clear();
        cached_trending_videos_.clear();
    }
    // Flag shared between callback firings: first arriving chunk flips
    // the loading overlay off + binds focus to the grid.
    auto first_chunk_flag = std::make_shared<bool>(false);

    auto cb = [this, reqPage, kind, first_chunk_flag]
              (const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqPage, results, finished, kind, first_chunk_flag]() {
            if (state_.currentScreen != TubeState::Screen::Home || home_page_ != reqPage) return;

            // Stream cards in as they arrive — DO NOT wait for finished.
            // Each per-result callback fires from the YouTubeAPI worker
            // thread; here we just append to the grid and let the
            // virtualized renderer pick it up next frame.
            if (!results.empty()) {
                for (const auto& v : results) {
                    home_grid_->addVideo(v);
                    cached_trending_videos_.push_back(v);
                }
                if (!*first_chunk_flag) {
                    *first_chunk_flag = true;
                    focus_manager_.setGrid(home_grid_);
                    // First item landed — clear the loading overlay so
                    // the user can start interacting even though more
                    // items are still streaming.
                    state_.isSearching = false;
                    homeLoadFailed_    = false;
                }
                uiDirty_ = true;
            }

            if (finished) {
                std::cout << "[App] loadHomeFeeds: kind='" << kind
                          << "' finished; grid has "
                          << home_grid_->videos.size() << " items" << std::endl;
                if (home_grid_->videos.empty()) {
                    // No results at all — surface the failure state.
                    homeLoadFailed_ = true;
                } else {
                    trending_cache_time_ = std::chrono::steady_clock::now();
                    cached_home_kind_    = kind;
                    saveHomeCache();
                }
                state_.isSearching = false;
                uiDirty_ = true;
            }
        });
    };

    // Trending and subscriptions BOTH go through fetchFeed now — they
    // share the same flat-playlist + dump-json + retry pipeline.
    std::cout << "[App] loadHomeFeeds: dispatching fetchFeed(" << kind
              << ") page=" << reqPage << std::endl;
    youtube_api_.fetchFeed(kind, reqPage, cb);
}

void App::loadMoreHomeFeeds() {
    if (state_.isLoadingVideo || state_.isSearching) return;
    state_.isSearching = true;
    uiDirty_ = true;
    home_page_++;

    int reqPage = home_page_;
    const bool useSubs = (state_.homeFeedKind == "subscriptions") && state_.authed;
    auto cb = [this, reqPage](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqPage, results, finished]() {
            if (state_.currentScreen != TubeState::Screen::Home || home_page_ != reqPage) return;
            
            if (finished) {
                state_.isSearching = false;
                focus_manager_.pruneGridIfNeeded(500);
                uiDirty_ = true;
                if (pendingMoreHome_) {
                    pendingMoreHome_ = false;
                    loadMoreHomeFeeds();
                }
                return;
            }
            
            if (!results.empty()) {
                // Build id set once (O(history)) rather than scanning per card (O(history×cards)).
                std::unordered_set<std::string> historyIds;
                historyIds.reserve(playback_history_.size());
                for (const auto& hv : playback_history_) historyIds.insert(hv.id);
                for (const auto& v : results) {
                    if (historyIds.count(v.id)) continue;
                    home_grid_->addVideo(v);
                    cached_trending_videos_.push_back(v);
                }
                // Cache cap bumped from 150 → 500 now that the grid is
                // virtualized — only visible-window cards are materialized,
                // so 500 metadata rows are cheap (~50 KB vs the old ~500 KB
                // of fully-built VideoCards).
                constexpr size_t kMaxTrendingCache = 500;
                if (cached_trending_videos_.size() > kMaxTrendingCache) {
                    cached_trending_videos_.erase(
                        cached_trending_videos_.begin(),
                        cached_trending_videos_.begin() +
                            (cached_trending_videos_.size() - kMaxTrendingCache));
                }
                uiDirty_ = true;
            }
        });
    };
    // Same dispatch as loadHomeFeeds — trending and subscriptions both
    // go through fetchFeed for a uniform pipeline.
    youtube_api_.fetchFeed(useSubs ? "subscriptions" : "trending", reqPage, cb);
}

void App::loadMoreSearchResults() {
    if (state_.isLoadingVideo || state_.isSearching || current_search_query_.empty()) return;
    state_.isSearching = true;
    uiDirty_ = true;
    search_page_++;
    
    int reqPage = search_page_;
    std::string reqQuery = current_search_query_;
    youtube_api_.search(current_search_query_, reqPage, [this, reqQuery, reqPage](const std::vector<YouTubeVideo>& results, bool finished) {
        queueOnMainThread([this, reqQuery, reqPage, results, finished]() {
            if (state_.currentScreen != TubeState::Screen::Search || current_search_query_ != reqQuery || search_page_ != reqPage) return;
            
            if (finished) {
                state_.isSearching = false;
                focus_manager_.pruneGridIfNeeded(500);
                uiDirty_ = true;
                if (pendingMoreSearch_) {
                    pendingMoreSearch_ = false;
                    loadMoreSearchResults();
                }
                return;
            }
            
            if (!results.empty()) {
                for (const auto& v : results) {
                    search_grid_->addVideo(v);
                }
                uiDirty_ = true;
            }
        });
    });
}

// Legacy presentation queue methods removed.

void App::updateHoverPreviews() {
    // Respect the user's modal preference.  When disabled, ensure any
    // in-flight preview is torn down so we don't leak an mpv pipeline
    // after the user flips the toggle off mid-session.
    if (!state_.hoverPreviewsEnabled) {
        if (is_playing_preview_ || preview_card_) stopBrowsePreviewState();
        return;
    }
    if ((state_.currentScreen != TubeState::Screen::Home && state_.currentScreen != TubeState::Screen::Search) || state_.isLoadingVideo || state_.inputMode == TubeState::InputMode::SearchText || state_.miniplayerActive) {
        stopBrowsePreviewState();
        return;
    }

    auto focusedCard = focus_manager_.getFocusedCard();
    if (!focusedCard) {
        stopBrowsePreviewState();
        return;
    }

    if (preview_card_ != focusedCard) {
        stopBrowsePreviewState();
        preview_card_ = focusedCard;
        return;
    }

    if (is_playing_preview_) {
        auto grid = activeGrid();
        if (grid) {
            const float screenY = focusedCard->bounds.y - grid->scrollY;
            const bool horizontal = (focusedCard->bounds.w > 400);
            const int thumbW = horizontal ? 160 : static_cast<int>(focusedCard->bounds.w);
            const int thumbH = horizontal ? 90 : static_cast<int>(focusedCard->bounds.w * (9.0f / 16.0f));
            
            const bool fullyVisible =
                screenY >= grid->bounds.y &&
                screenY + thumbH <= grid->bounds.y + grid->bounds.h &&
                focusedCard->bounds.x >= grid->bounds.x &&
                focusedCard->bounds.x + thumbW <= grid->bounds.x + grid->bounds.w;


            if (!fullyVisible || screenY < 96.0f) {
                stopBrowsePreviewState();
                return;
            }
        }
        return;
    }

    const std::string cacheKey = streamCacheKey(focusedCard->video.id, state_.maxQualityHeight);
    auto grid = activeGrid();
    // Resolve ONLY the currently focused card, and only once it has been dwelt
    // on. Previously this also prefetched the next TWO adjacent cards, which
    // launched up to three concurrent yt-dlp processes per focus change — those
    // pegged all four A35 cores and stalled the UI. The dwell gate means simply
    // scrolling past cards resolves nothing; you must linger to warm a preview.
    if (grid && focusedCard->focusedTime_ >= 0.45f) {
        // Skip if this key failed recently (hard backstop against a runaway
        // re-request loop), is already in flight, already resolved, OR is
        // a live stream (live URLs rotate; pre-resolving them just wastes
        // a yt-dlp run and almost always produces a stale URL by the time
        // the user actually clicks).
        auto failIt = stream_prefetch_fail_until_.find(cacheKey);
        bool cooling = (failIt != stream_prefetch_fail_until_.end() &&
                        std::chrono::steady_clock::now() < failIt->second);
        if (!cooling &&
            !focusedCard->video.is_live &&
            !getCachedStreamUrl(cacheKey).has_value() &&
            stream_prefetch_inflight_.find(cacheKey) == stream_prefetch_inflight_.end()) {
            stream_prefetch_inflight_.insert(cacheKey);
            is_loading_preview_ = true;
            youtube_api_.getStreamUrl(focusedCard->video.id, state_.maxQualityHeight, [this, cacheKey](bool success, const std::string& url, const std::string& subtitle_url, const std::string& audio_url, const VideoPlaybackMetadata& /*meta*/) {
                queueOnMainThread([this, cacheKey, success, url, subtitle_url, audio_url]() {
                    stream_prefetch_inflight_.erase(cacheKey);
                    is_loading_preview_ = false;
                    if (success && !url.empty()) {
                        setCachedStreamUrl(cacheKey, url + "|" + subtitle_url + "|" + audio_url);
                        stream_prefetch_fail_until_.erase(cacheKey);
                    } else {
                        setCachedStreamUrl(cacheKey, ""); // Cache failure
                        // Back off 30s before this key may be re-requested.
                        stream_prefetch_fail_until_[cacheKey] =
                            std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    }
                    uiDirty_ = true;
                });
            }, /*isPreview=*/true, /*isLive=*/false, focusedCard->video.id);
        }
    }

    // 2. Play preview if focused for >= 0.85s and url is cached
    if (focusedCard->focusedTime_ < 0.85f || is_loading_preview_) {
        return;
    }

    auto cachedOpt = getCachedStreamUrl(cacheKey);
    if (cachedOpt.has_value() && !cachedOpt.value().empty()) {
        auto grid = activeGrid();
        if (!grid) return;

        const float screenY = focusedCard->bounds.y - grid->scrollY;
        const bool horizontal = (focusedCard->bounds.w > 400);
        const int thumbW = horizontal ? 160 : static_cast<int>(focusedCard->bounds.w);
        const int thumbH = horizontal ? 90 : static_cast<int>(focusedCard->bounds.w * (9.0f / 16.0f));
        const bool fullyVisible =
            screenY >= grid->bounds.y &&
            screenY + thumbH <= grid->bounds.y + grid->bounds.h &&
            focusedCard->bounds.x >= grid->bounds.x &&
            focusedCard->bounds.x + thumbW <= grid->bounds.x + grid->bounds.w;


        if (!fullyVisible || screenY < 96.0f) {
            return;
        }

        mpv_player_.setMute(true);
        std::string stream_url, subtitle_url_unused, audio_url;
        splitCachedStream(cachedOpt.value(), stream_url, subtitle_url_unused, audio_url);
        // Previews are muted so no audio_url needed.  The video-only DASH
        // stream still plays back fine in mpv as a silent track.
        mpv_player_.play(stream_url);
        is_playing_preview_ = true;
        focusedCard->is_previewing = true;
        triggerVideoFade();  // animate the thumbnail preview starting
        uiDirty_ = true;
    }
}

void App::saveSettings() {
    // Single source of truth: all persistent fields go through settings::save.
    // Previously this method also did a second write that only contained
    // backgroundDaemonEnabled — that second write CLOBBERED everything the
    // settings modal just saved.  Hence "settings not persistent": the
    // modal wrote correctly, then this method immediately overwrote it.
    Settings s;
    s.maxQualityHeight        = state_.maxQualityHeight;
    s.homeFeedKind            = state_.homeFeedKind;
    s.volume                  = state_.volume;
    s.showDebugOverlay        = state_.showDebugOverlay;
    s.backgroundDaemonEnabled = state_.backgroundDaemonEnabled;
    s.hoverPreviewsEnabled    = state_.hoverPreviewsEnabled;
    s.autoplayNextEnabled     = state_.autoplayNextEnabled;
    s.uiSoundsEnabled         = state_.uiSoundsEnabled;
    settings::save(s);
}

void App::loadSettings() {
    Settings s;
    settings::load(s);                     // populates only what's on disk
    SettingsModal::apply(this, s);         // mirror into state_ (also primes ui_sounds)
    state_.backgroundDaemonEnabled = s.backgroundDaemonEnabled;
}

void App::saveBrowseState() {
    // Persist enough to drop the user back where they were on next
    // launch: which screen, search query, search/home page, focused
    // index, and the actual videos that were on screen so we don't have
    // to re-run the search/feed call.  Thumbnails are NOT serialized —
    // they're cheap to re-fetch via the image manager and would bloat
    // the file by an order of magnitude.
    try {
        nlohmann::json j;
        // Save the underlying BROWSE screen, not the literal current
        // screen — when the user is in fullscreen Playback (or a
        // miniplayer over a browse screen), currentScreen is Playback,
        // and the old code defaulted that to "home", silently losing the
        // Search context the user came from.  getPreviousBrowseScreen()
        // recovers Search vs Home in that case.
        TubeState::Screen browseScreen = state_.currentScreen;
        if (browseScreen == TubeState::Screen::Playback)
            browseScreen = state_manager_.getPreviousBrowseScreen();
        const char* screenName =
            (browseScreen == TubeState::Screen::Search) ? "search" : "home";
        j["screen"]        = screenName;
        j["search_query"]  = current_search_query_;
        j["search_page"]   = search_page_;
        j["home_page"]     = home_page_;
        // Focused index in the grid the user was looking at when they exited.
        auto activeIdx = [&]() -> int {
            auto grid = activeGrid();
            if (!grid || grid->videos.empty()) return 0;
            // FocusManager stores the index per-grid; just use the
            // currently focused card to recover.
            auto focused = focus_manager_.getFocusedCard();
            if (!focused) return 0;
            for (size_t i = 0; i < grid->videos.size(); ++i) {
                if (grid->videos[i].id == focused->video.id) return static_cast<int>(i);
            }
            return 0;
        };
        j["focused_index"] = activeIdx();

        // Snapshot of the active grid's videos so the cards reappear
        // exactly as the user left them.  We dump both grids if both have
        // content — loading is cheap and the user might toggle between
        // them.  Cap each at the in-memory ceiling (500) to keep the
        // file small enough for the SD card.
        auto serializeGrid = [](const std::shared_ptr<ui::GridContainer>& g) {
            nlohmann::json arr = nlohmann::json::array();
            if (!g) return arr;
            const size_t cap = std::min<size_t>(g->videos.size(), 500);
            for (size_t i = 0; i < cap; ++i) {
                const auto& v = g->videos[i];
                arr.push_back({
                    {"id", v.id}, {"title", v.title}, {"author", v.author},
                    {"duration_string", v.duration_string},
                    {"view_count_string", v.view_count_string},
                    {"uploaded_ago_string", v.uploaded_ago_string},
                    {"duration_seconds", v.duration_seconds},
                    {"is_live", v.is_live},
                });
            }
            return arr;
        };
        j["home_videos"]   = serializeGrid(home_grid_);
        j["search_videos"] = serializeGrid(search_grid_);

        std::ofstream ofs(getAppDataPath("browse_state.json"));
        if (ofs) ofs << j.dump();   // compact; not meant for hand-editing
    } catch (...) {}
}

bool App::loadBrowseState() {
    try {
        std::ifstream ifs(getAppDataPath("browse_state.json"));
        if (!ifs) return false;
        nlohmann::json j;
        ifs >> j;

        // Hydrate grids first so the focus/screen restore below can see
        // populated containers.
        auto hydrate = [](std::shared_ptr<ui::GridContainer>& g, const nlohmann::json& arr) {
            if (!g || !arr.is_array()) return;
            for (const auto& item : arr) {
                YouTubeVideo v;
                v.id                  = item.value("id", std::string());
                if (v.id.empty()) continue;
                v.title               = item.value("title", std::string());
                v.author              = item.value("author", std::string());
                v.duration_string     = item.value("duration_string", std::string());
                v.view_count_string   = item.value("view_count_string", std::string());
                v.uploaded_ago_string = item.value("uploaded_ago_string", std::string());
                v.duration_seconds    = item.value("duration_seconds", 0);
                v.is_live             = item.value("is_live", false);
                g->addVideo(v);
            }
        };
        if (j.contains("home_videos"))   hydrate(home_grid_,   j["home_videos"]);
        if (j.contains("search_videos")) hydrate(search_grid_, j["search_videos"]);

        current_search_query_ = j.value("search_query", std::string());
        search_page_          = std::max(1, j.value("search_page", 1));
        home_page_            = std::max(1, j.value("home_page", 1));

        // Seed the trending in-memory cache from the restored home grid
        // so loadMoreHomeFeeds works correctly without a re-fetch — it
        // relies on cached_trending_videos_ for pagination state, and
        // without this the "load page 2" path would start from page 1
        // and duplicate everything.
        cached_trending_videos_ = home_grid_->videos;
        trending_cache_time_    = std::chrono::steady_clock::now();
        cached_home_kind_       = (state_.homeFeedKind == "subscriptions" && state_.authed)
                                      ? "subscriptions" : "trending";

        // Restore which screen was active so the user lands back on it.
        std::string screen = j.value("screen", std::string("home"));
        if (screen == "search" && !search_grid_->videos.empty()) {
            state_manager_.transitionTo(TubeState::Screen::Search);
            focus_manager_.setGrid(search_grid_);
            search_grid_->title = current_search_query_.empty()
                                      ? std::string("Search")
                                      : "Search: " + current_search_query_;
        } else {
            focus_manager_.setGrid(home_grid_);
            home_grid_->title = (state_.homeFeedKind == "subscriptions" && state_.authed)
                                    ? "Subscriptions" : "Trending";
        }

        // Focus the same card.  Out-of-range gets clamped by setFocusedIndex.
        int focusedIdx = j.value("focused_index", 0);
        focus_manager_.setFocusedIndex(focusedIdx);
        uiDirty_ = true;
        // Successful restore — tell caller it can skip the cold-fetch
        // loadHomeFeeds() that would clobber what we just hydrated.
        return !home_grid_->videos.empty() || !search_grid_->videos.empty();
    } catch (...) {}
    return false;
}

bool App::reabsorbDaemonPlayback() {
#ifdef _WIN32
    return false;
#else
    // Look for the daemon's state snapshot.  If absent or unreadable,
    // there's nothing to reabsorb — return so the caller can do the
    // standard cold start.
    std::ifstream ifs("/dev/shm/tubelite_daemon_state.json");
    if (!ifs) return false;

    nlohmann::json j;
    try { ifs >> j; } catch (...) { return false; }
    if (!j.is_object()) return false;

    YouTubeVideo v;
    v.id     = j.value("id", std::string());
    v.title  = j.value("title", std::string());
    v.author = j.value("author", std::string());
    v.duration_seconds = j.value("duration", 0.0);
    if (v.id.empty()) return false;
    const double position  = std::max(0.0, j.value("position", 0.0));
    const bool   wasPlaying = j.value("playing", true);
    const double speed      = std::max(0.25, std::min(2.0, j.value("speed", 1.0)));

    std::cerr << "[App] reabsorbing daemon playback: id=" << v.id
              << " pos=" << position << "s playing=" << wasPlaying
              << " speed=" << speed << "x\n";

    // ── Retrieve stream URLs ──
    // Read stream_url / subtitle_url / audio_url directly from the daemon state.
    // This is the most fresh, valid URL set since the daemon is playing them.
    // Fall back to daemon_queue.json and then to tubed resolve if needed.
    std::string streamUrl  = j.value("stream_url", std::string());
    std::string subUrl     = j.value("subtitle_url", std::string());
    std::string audioUrl   = j.value("audio_url", std::string());

    if (streamUrl.empty()) {
        try {
            std::ifstream qifs(getAppDataPath("daemon_queue.json"));
            if (qifs) {
                nlohmann::json q;
                qifs >> q;
                if (q.contains("videos") && q["videos"].is_array()) {
                    int cidx = q.value("current_index", 0);
                    const auto& vids = q["videos"];
                    auto tryEntry = [&](const nlohmann::json& entry) -> bool {
                        if (entry.value("id", std::string()) != v.id) return false;
                        std::string u = entry.value("stream_url", std::string());
                        if (u.empty()) return false;
                        streamUrl = u;
                        subUrl    = entry.value("subtitle_url", std::string());
                        audioUrl  = entry.value("audio_url", std::string());
                        return true;
                    };
                    bool gotUrlFromQueue = false;
                    if (cidx >= 0 && cidx < (int)vids.size()) {
                        gotUrlFromQueue = tryEntry(vids[cidx]);
                    }
                    if (!gotUrlFromQueue) {
                        for (const auto& entry : vids) {
                            if (tryEntry(entry)) { gotUrlFromQueue = true; break; }
                        }
                    }
                }
            }
        } catch (...) {}
    }

    if (streamUrl.empty()) {
        std::cerr << "[App] reabsorb: no pre-resolved URL, resolving via tubed\n";
        VideoPlaybackMetadata meta;
        std::atomic<bool> done{false}, ok{false};
        youtube_api_.getStreamUrl(v.id, state_.maxQualityHeight,
            [&done, &ok, &streamUrl, &subUrl, &audioUrl, &meta]
            (bool success, const std::string& u, const std::string& sub,
             const std::string& audio, const VideoPlaybackMetadata& m) {
                streamUrl = u; subUrl = sub; audioUrl = audio; meta = m;
                ok.store(success); done.store(true);
            },
            /*isPreview=*/false, /*isLive=*/false);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            SDL_Delay(20);
        }
        if (!ok.load() || streamUrl.empty()) {
            std::cerr << "[App] reabsorb: stream resolve failed, falling through\n";
            return false;
        }
    }

    // ── Transfer playback ─────────────────────────────────────────────────────
    current_video_ = v;
    // Populate metadata from cache if available (avoids blocking re-fetch).
    {
        VideoPlaybackMetadata meta;
        auto cachedOpt = getCachedStreamUrl(streamCacheKey(v.id, state_.maxQualityHeight));
        active_video_metadata_ = meta;
        wrapped_description_lines_ = wrapText(meta.description, 280, 1);
    }
    setCachedStreamUrl(streamCacheKey(v.id, state_.maxQualityHeight),
                       streamUrl + "|" + subUrl + "|" + audioUrl);

    mpv_player_.setVolume(0);
    mpv_player_.setMute(state_.muted);
    state_.speed = speed;
    mpv_player_.setSpeed(state_.speed);
    mpv_player_.play(streamUrl, subUrl, audioUrl);
    if (position > 0.1) mpv_player_.setPendingSeekPosition(position);

    // Signal the daemon to fade out IMMEDIATELY (it ramps down over
    // ~800 ms then exits).  Doing this in parallel with our mpv's
    // buffering window means the user perceives a smooth crossfade
    // instead of a several-second black-screen freeze before the
    // window appears.  Previously we waited up to 4 s for our mpv
    // to start producing before signalling — that compounded with
    // a 1 s fade-in to give a worst-case 5 s blank startup; the
    // common path was ~1-2 s of black screen even with a cached URL.
    {
        std::ofstream sig("/dev/shm/tubelite_daemon_fadeout");
        if (!sig) killExistingDaemon();
    }

    // Land back on whatever browse screen loadBrowseState() restored
    // (Search or Home) with the miniplayer over it — don't force Home
    // and lose the user's search context.  If we're somehow not on a
    // browse screen, fall back to Home.
    TubeState::Screen target = state_.currentScreen;
    if (target != TubeState::Screen::Search &&
        target != TubeState::Screen::Home) {
        target = TubeState::Screen::Home;
    }
    state_manager_.transitionTo(target);
    state_manager_.setMiniplayerActive(true);
    state_.showUi = true;
    if (!wasPlaying) {
        mpv_player_.pause();
    }

    // Crossfade window: ~800 ms, matching the daemon's own fade-out
    // duration in fadeOutAndExit().  We tick mpv.update() each step so
    // the demuxer + decoder make progress, and ramp our volume up
    // proportionally — so as soon as audio actually starts flowing,
    // it comes in at the correct level for the current point in the
    // ramp instead of a sudden full-volume jump.  If our mpv is still
    // buffering at the end of the window we return anyway and let the
    // main loop drive it — the SDL window is what the user is waiting
    // to see, not full audio.
    using namespace std::chrono;
    {
        const auto fadeStart = steady_clock::now();
        const float fadeSecs = 0.8f;
        const int targetVol = state_.volume;
        while (true) {
            mpv_player_.update();
            float t = duration<float>(steady_clock::now() - fadeStart).count();
            if (t >= fadeSecs) { mpv_player_.setVolume(targetVol); break; }
            mpv_player_.setVolume(static_cast<int>(targetVol * (t / fadeSecs)));
            SDL_Delay(25);
        }
    }
    uiDirty_ = true;
    return true;
#endif
}

void App::saveDaemonQueue() {
    try {
        nlohmann::json j;
        auto attachResolvedStream = [this](nlohmann::json& v, const std::string& videoId) {
            std::optional<std::string> cached = getCachedStreamUrl(streamCacheKey(videoId, 360));
            if (!cached && state_.maxQualityHeight != 360) {
                cached = getCachedStreamUrl(streamCacheKey(videoId, state_.maxQualityHeight));
            }
            if (!cached || cached->empty()) {
                return;
            }

            std::string url, sub_url, audio_url;
            splitCachedStream(*cached, url, sub_url, audio_url);
            if (url.empty()) {
                return;
            }

            v["stream_url"] = url;
            v["subtitle_url"] = sub_url;
            v["audio_url"] = audio_url;
        };

        std::shared_ptr<ui::GridContainer> grid = getPlaybackGrid();
        if (!grid || grid->videos.empty()) {
            nlohmann::json v;
            v["id"] = current_video_.id;
            v["title"] = current_video_.title;
            v["author"] = current_video_.author;
            v["duration_seconds"] = current_video_.duration_seconds;
            v["duration_string"] = current_video_.duration_string;
            // Pass any already-resolved stream we have so the daemon can start instantly.
            attachResolvedStream(v, current_video_.id);
            j["videos"].push_back(v);
            j["current_index"] = 0;
        } else {
            int current_idx = 0;
            for (size_t i = 0; i < grid->videos.size(); ++i) {
                nlohmann::json v;
                const auto& vid = grid->videos[i];
                v["id"] = vid.id;
                v["title"] = vid.title;
                v["author"] = vid.author;
                v["duration_seconds"] = vid.duration_seconds;
                v["duration_string"] = vid.duration_string;
                // Prefer the daemon's native 360p cache, but fall back to any
                // already-resolved playback stream before forcing a re-resolve.
                attachResolvedStream(v, vid.id);
                if (vid.id == current_video_.id) {
                    current_idx = static_cast<int>(i);
                }
                j["videos"].push_back(v);
            }
            j["current_index"] = current_idx;
        }
        j["current_position"] = mpv_player_.getPlaybackTime();
        j["speed"]            = state_.speed;
        
        std::ofstream ofs(getAppDataPath("daemon_queue.json"));
        if (ofs) {
            ofs << j.dump(4);
        }
    } catch (...) {}
}


void App::playNextTrack() {
    std::shared_ptr<ui::GridContainer> grid = getPlaybackGrid();
    if (grid && !grid->videos.empty()) {
        for (size_t i = 0; i < grid->videos.size(); ++i) {
            if (grid->videos[i].id == current_video_.id) {
                if (i + 1 < grid->videos.size()) {
                    auto nextVideo = grid->videos[i + 1];
                    focus_manager_.setFocusedIndex(i + 1);
                    playVideo(nextVideo, !state_.miniplayerActive);
                    showPlaybackToast("Next Track");
                } else {
                    showPlaybackToast("End of Playlist");
                }
                break;
            }
        }
    }
}

void App::playPreviousTrack() {
    std::shared_ptr<ui::GridContainer> grid = getPlaybackGrid();
    if (grid && !grid->videos.empty()) {
        for (size_t i = 0; i < grid->videos.size(); ++i) {
            if (grid->videos[i].id == current_video_.id) {
                if (i > 0) {
                    auto prevVideo = grid->videos[i - 1];
                    focus_manager_.setFocusedIndex(i - 1);
                    playVideo(prevVideo, !state_.miniplayerActive);
                    showPlaybackToast("Previous Track");
                } else {
                    showPlaybackToast("Start of Playlist");
                }
                break;
            }
        }
    }
}

void App::saveHistory() {
    try {
        nlohmann::json j = nlohmann::json::array();
        int count = 0;
        for (auto it = playback_history_.rbegin(); it != playback_history_.rend(); ++it) {
            if (count++ >= 50) break;
            nlohmann::json item;
            item["id"] = it->id;
            item["title"] = it->title;
            item["author"] = it->author;
            item["duration_seconds"] = it->duration_seconds;
            item["duration_string"] = it->duration_string;
            item["view_count_string"] = it->view_count_string;
            item["uploaded_ago_string"] = it->uploaded_ago_string;
            j.push_back(item);
        }
        std::ofstream ofs(getAppDataPath("history.json"));
        if (ofs) {
            ofs << j.dump(4);
        }
    } catch (...) {}
}

void App::loadHistory() {
    playback_history_.clear();
    try {
        std::ifstream ifs(getAppDataPath("history.json"));
        if (ifs) {
            nlohmann::json j;
            ifs >> j;
            if (j.is_array()) {
                std::vector<YouTubeVideo> temp;
                for (const auto& item : j) {
                    YouTubeVideo v;
                    v.id = item.value("id", "");
                    v.title = item.value("title", "");
                    v.author = item.value("author", "");
                    v.duration_seconds = item.value("duration_seconds", 0);
                    v.duration_string = item.value("duration_string", "");
                    v.view_count_string = item.value("view_count_string", "");
                    v.uploaded_ago_string = item.value("uploaded_ago_string", "");
                    if (!v.id.empty()) {
                        temp.push_back(v);
                    }
                }
                std::reverse(temp.begin(), temp.end());
                playback_history_ = temp;
            }
        }
    } catch (...) {}
}

void App::addToHistory(const YouTubeVideo& video) {
    playback_history_.erase(
        std::remove_if(playback_history_.begin(), playback_history_.end(),
                       [&video](const YouTubeVideo& v) { return v.id == video.id; }),
        playback_history_.end()
    );
    playback_history_.push_back(video);
    saveHistory();
}

void App::saveHomeCache() {
    try {
        nlohmann::json j;
        j["timestamp"] = static_cast<long long>(std::time(nullptr));
        nlohmann::json videosArray = nlohmann::json::array();
        for (const auto& v : cached_trending_videos_) {
            nlohmann::json item;
            item["id"] = v.id;
            item["title"] = v.title;
            item["author"] = v.author;
            item["duration_seconds"] = v.duration_seconds;
            item["duration_string"] = v.duration_string;
            item["view_count_string"] = v.view_count_string;
            item["uploaded_ago_string"] = v.uploaded_ago_string;
            videosArray.push_back(item);
        }
        j["videos"] = videosArray;
        std::ofstream ofs(getAppDataPath("home_cache.json"));
        if (ofs) {
            ofs << j.dump(4);
        }
    } catch (...) {}
}

bool App::loadHomeCache() {
    try {
        std::ifstream ifs(getAppDataPath("home_cache.json"));
        if (!ifs) return false;
        nlohmann::json j;
        ifs >> j;
        if (!j.is_object() || !j.contains("videos") || !j["videos"].is_array()) return false;
        
        long long timestamp = j.value("timestamp", 0LL);
        
        std::vector<YouTubeVideo> temp;
        for (const auto& item : j["videos"]) {
            YouTubeVideo v;
            v.id = item.value("id", "");
            v.title = item.value("title", "");
            v.author = item.value("author", "");
            v.duration_seconds = item.value("duration_seconds", 0);
            v.duration_string = item.value("duration_string", "");
            v.view_count_string = item.value("view_count_string", "");
            v.uploaded_ago_string = item.value("uploaded_ago_string", "");
            if (!v.id.empty()) {
                temp.push_back(v);
            }
        }
        
        if (temp.empty()) return false;
        
        cached_trending_videos_ = temp;
        home_grid_->clear();
        for (const auto& v : cached_trending_videos_) {
            home_grid_->addVideo(v);
        }
        
        focus_manager_.setGrid(home_grid_);
        
        long long now_epoch = static_cast<long long>(std::time(nullptr));
        long long diff_seconds = now_epoch - timestamp;
        if (diff_seconds < 0) diff_seconds = 0;
        
        trending_cache_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(diff_seconds);
        
        return true;
    } catch (...) {
        return false;
    }
}

void App::handleVideoEnded() {
    // Respect the modal toggle: when autoplay is off, just stop on end so
    // the user can decide what to do next instead of being whisked to the
    // following video.
    if (!state_.autoplayNextEnabled) {
        leavePlayback();
        return;
    }
    std::shared_ptr<ui::GridContainer> grid = getPlaybackGrid();
    bool playedNext = false;

    if (grid && !grid->videos.empty()) {
        for (size_t i = 0; i < grid->videos.size(); ++i) {
            if (grid->videos[i].id == current_video_.id) {
                if (i + 1 < grid->videos.size()) {
                    auto nextVideo = grid->videos[i + 1];
                    focus_manager_.setFocusedIndex(i + 1);
                    playVideo(nextVideo, !state_.miniplayerActive);
                    playedNext = true;
                }
                break;
            }
        }
    }
    
    if (!playedNext) {
        mpv_player_.seekAbsoluteKeyframes(0.0);
        mpv_player_.pause();
        uiDirty_ = true;
    }
}

void App::prefetchNextVideo() {
    if (prefetched_next_video_id_ == current_video_.id) return;
    
    // Find the next video in the active grid
    std::shared_ptr<ui::GridContainer> grid = getPlaybackGrid();
    if (!grid || grid->videos.empty()) return;
    
    YouTubeVideo nextVideo;
    bool foundCurrent = false;
    for (size_t i = 0; i < grid->videos.size(); ++i) {
        if (grid->videos[i].id == current_video_.id) {
            if (i + 1 < grid->videos.size()) {
                nextVideo = grid->videos[i + 1];
                foundCurrent = true;
            }
            break;
        }
    }
    
    if (!foundCurrent) return;

    // Mark as prefetched so we don't spam requests
    prefetched_next_video_id_ = current_video_.id;

    const std::string nextCacheKey = streamCacheKey(nextVideo.id, state_.maxQualityHeight);
    if (getCachedStreamUrl(nextCacheKey).has_value()) {
        // Already cached!
        return;
    }
    // Skip prefetch for live streams — their URLs are time-limited and
    // tend to be stale by the time the current video ends.
    if (nextVideo.is_live) return;

    // Start background prefetch request (using isPreview=true so it doesn't cancel main playback requests)
    std::cerr << "[prefetch] Prefetching next video stream URL: " << nextVideo.title << "\n";
    youtube_api_.getStreamUrl(nextVideo.id, state_.maxQualityHeight, [this, nextCacheKey](bool success, const std::string& url, const std::string& subtitle_url, const std::string& audio_url, const VideoPlaybackMetadata& /*meta*/) {
        queueOnMainThread([this, nextCacheKey, success, url, subtitle_url, audio_url]() {
            if (success && !url.empty()) {
                setCachedStreamUrl(nextCacheKey, url + "|" + subtitle_url + "|" + audio_url);
                std::cerr << "[prefetch] Next video stream URL cached successfully.\n";
            }
        });
    }, /*isPreview=*/true, /*isLive=*/false, "autoplay_" + current_video_.id);
}


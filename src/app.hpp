#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include <unordered_map>
#include "state.hpp"
#include "mpv_player.hpp"
#include "youtube_api.hpp"
#include "keyboard_overlay.hpp"
#include "status_overlay.hpp"
#include "image_manager.hpp"
#include "ui_framework.hpp"
#include <memory>

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

    std::vector<YouTubeVideo> search_results_;
    int selected_result_idx_{0};
    YouTubeVideo current_video_;

    std::unique_ptr<ImageManager> image_manager_;
    ui::FocusManager focus_manager_;
    std::shared_ptr<ui::GridContainer> home_grid_;
    std::shared_ptr<ui::GridContainer> search_grid_;
    
    std::string current_search_query_;
    int search_page_{1};
    int home_page_{1};
    
    void loadHomeFeeds();
    void loadMoreHomeFeeds();
    void loadMoreSearchResults();
};

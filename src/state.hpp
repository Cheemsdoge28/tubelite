#pragma once
#include <string>

struct TubeState {
    enum class Screen { Home, Search, Playback };
    enum class InputMode { None, SearchText };
    enum class KeyboardMode { Lowercase, Uppercase, Symbols };

    Screen currentScreen{Screen::Home};
    InputMode inputMode{InputMode::None};
    KeyboardMode keyboardMode{KeyboardMode::Lowercase};

    std::string textBuffer;
    int textCursor{0};
    bool running{true};
    bool isLoadingVideo{false};
    bool isSearching{false};
    int keyboardSelectedIndex{0};
    bool replaceBufferOnNextInput{false};
    bool showUi{true};
    int maxQualityHeight{720};
    int volume{100};
    bool muted{false};
    double speed{1.0};
    bool isScrubbing{false};
    double scrubTargetTime{0.0};
    bool showDebugOverlay{false};
    bool miniplayerActive{false};
    bool showDescriptionDrawer{false};
    bool showSignInHelp{false};      // modal: how to sign in via cookies.txt
    bool showSettingsModal{false};   // modal: Settings (quality, home feed, etc.)
    int  settingsModalIndex{0};      // selected row inside the settings modal
    bool showQueuePanel{false};      // player: explicit up-next queue (START)
    int  queueSelectedIndex{0};      // selected row inside the queue panel
    bool showCardMenu{false};        // browse: Play Next / Add to Queue for focused card (START)
    int  cardMenuIndex{0};           // selected row inside the card action menu
    bool authed{false};              // mirror of YouTubeAPI::isAuthed() (cookies present)
    bool authChecked{false};         // has tubed been queried at least once
    bool backgroundDaemonEnabled{true};
    std::string homeFeedKind{"trending"};  // "trending" | "subscriptions" (modal-controlled)
    bool hoverPreviewsEnabled{true};       // mute card hover-previews (saves CPU/network)
    bool autoplayNextEnabled{true};        // auto-advance to next track on video end
    bool uiSoundsEnabled{true};            // procedural click/select/back UI sounds

    float leftStickX{0.0f};
    float leftStickY{0.0f};
    float rightStickX{0.0f};
    float rightStickY{0.0f};
    float leftTrigger{0.0f};
    float rightTrigger{0.0f};
    bool dpadUpPressed{false};
    bool dpadDownPressed{false};
    bool dpadLeftPressed{false};
    bool dpadRightPressed{false};
};

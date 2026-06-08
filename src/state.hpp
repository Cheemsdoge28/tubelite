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
    int keyboardSelectedIndex{0};
    bool replaceBufferOnNextInput{false};
    bool showUi{true};

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

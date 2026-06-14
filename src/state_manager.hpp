#pragma once
#include "state.hpp"
#include <functional>

class StateManager {
public:
    StateManager(TubeState& state) : state_(state) {}

    using TransitionCallback = std::function<void(
        TubeState::Screen oldScreen, TubeState::Screen newScreen,
        bool oldMiniplayer, bool newMiniplayer)>;

    void setTransitionCallback(TransitionCallback cb) { transition_cb_ = cb; }

    void transitionTo(TubeState::Screen nextScreen);
    void setMiniplayerActive(bool active);
    void stopPlayback();

    TubeState::Screen getPreviousBrowseScreen() const { return previousBrowseScreen_; }
    void setPreviousBrowseScreen(TubeState::Screen screen) { previousBrowseScreen_ = screen; }

private:
    TubeState& state_;
    TransitionCallback transition_cb_;
    TubeState::Screen previousBrowseScreen_{TubeState::Screen::Home};
};

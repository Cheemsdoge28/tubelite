#include "state_manager.hpp"

void StateManager::transitionTo(TubeState::Screen nextScreen) {
    TubeState::Screen oldScreen = state_.currentScreen;
    bool oldMiniplayer = state_.miniplayerActive;

    if (oldScreen == TubeState::Screen::Home || oldScreen == TubeState::Screen::Search) {
        previousBrowseScreen_ = oldScreen;
    }

    state_.currentScreen = nextScreen;

    if (transition_cb_) {
        transition_cb_(oldScreen, nextScreen, oldMiniplayer, state_.miniplayerActive);
    }
}

void StateManager::setMiniplayerActive(bool active) {
    TubeState::Screen oldScreen = state_.currentScreen;
    bool oldMiniplayer = state_.miniplayerActive;

    state_.miniplayerActive = active;

    if (transition_cb_) {
        transition_cb_(oldScreen, state_.currentScreen, oldMiniplayer, active);
    }
}

void StateManager::stopPlayback() {
    TubeState::Screen oldScreen = state_.currentScreen;
    bool oldMiniplayer = state_.miniplayerActive;

    state_.miniplayerActive = false;
    state_.isLoadingVideo = false;

    if (state_.currentScreen == TubeState::Screen::Playback) {
        state_.currentScreen = previousBrowseScreen_;
    }

    if (transition_cb_) {
        transition_cb_(oldScreen, state_.currentScreen, oldMiniplayer, false);
    }
}

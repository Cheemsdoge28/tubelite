#pragma once
#include <SDL2/SDL.h>

#include "settings.hpp"

class App;

// SettingsModal: full-screen overlay for editing user-facing preferences
// (quality cap, home-tab feed kind, volume default, debug overlay).
//
// Rendering and input are split so the modal stays an independent module:
//   * render(): draws the modal on top of whatever is already on screen.
//   * handleKey(): consumes one SDL keycode; returns true if the modal
//     stays open, false if the caller should close it (and save).
class SettingsModal {
public:
    // Static so the existing Compositor can call it without holding state.
    // The modal itself stores its cursor position inside `App` (state_.settingsModalIndex)
    // so it survives re-renders without an instance lifetime.
    static void render(App* app, SDL_Renderer* renderer, int width, int height);

    // Returns true if the modal should remain open after this key.  When it
    // returns false, the caller closes the modal AND calls settings::save()
    // so the change persists to disk.
    static bool handleKey(App* app, SDL_Keycode key);

    // Mirror state from Settings into TubeState (called on load and after
    // every modal-driven change so the rest of the app sees fresh values).
    static void apply(App* app, const Settings& s);
};

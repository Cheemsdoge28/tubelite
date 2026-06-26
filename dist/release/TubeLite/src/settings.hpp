#pragma once

#include <string>

// Persisted user preferences.  Loaded once at startup, written when the
// settings modal changes a value.  The on-disk format is JSON so the user
// can hand-edit if needed; missing keys fall back to the defaults below.
//
// Path: /roms/tools/tubelite/settings.json (preferred — survives SD-card
// remounts), with cwd-relative fallback for dev builds.
struct Settings {
    // Stream quality: max video height tubed will ask yt-dlp for.  Realistic
    // values today are 144 / 240 / 360 — tubed is muxed-only, and YouTube
    // serves itag 18 (360p) as the highest muxed format for almost all
    // videos.  Anything above 360 is effectively the same as 360.
    int  maxQualityHeight = 360;

    // What the Home tab shows.  "trending" = anonymous trending feed
    // (works without sign-in).  "subscriptions" = personalized signed-in
    // subscriptions feed via tubed `feed` op (requires cookies.txt).
    std::string homeFeedKind = "trending";

    // Default player volume (0-100).  Persists across sessions so users
    // don't have to re-set it every launch.
    int  volume = 100;

    // Show debug overlay on launch.
    bool showDebugOverlay = false;

    // Background-daemon mode (audio continues when app is sent to background).
    bool backgroundDaemonEnabled = true;

    // Show a short video preview when a card is focused for a moment.
    // Off by default for low-bandwidth users; saves a yt-dlp call + a
    // mpv decode pipeline per hovered card.
    bool hoverPreviewsEnabled = true;

    // Auto-advance to the next video in the current grid when the current
    // one finishes.  Off mirrors classic YouTube "one-and-done" behaviour.
    bool autoplayNextEnabled = true;

    // Procedural UI sounds (focus tick, A/B chirps, settings toggle clicks).
    // Synthesized at startup — no audio assets shipped.  See ui_sounds.cpp.
    bool uiSoundsEnabled = true;
};

namespace settings {
    // Load from disk into out (returns false if no file existed; defaults
    // already in out are preserved).
    bool load(Settings& out);

    // Write current values to disk.  Best-effort: failure to write is
    // logged but doesn't crash the app.
    bool save(const Settings& s);

    // The resolved on-disk path used by load/save.  Useful for the modal
    // to display "Saved to <path>" feedback.
    std::string path();
}

#pragma once
//
// ui_sounds.hpp — tiny procedurally-generated UI feedback layer.
//
// Why procedural: no asset files to ship, no licensing, no decoder, no
// extra dependency.  At init we synthesize a handful of short tones with
// soft envelopes (40-100 ms each) and store them as int16 PCM in RAM.
// At play() time we just queue a voice that gets mixed into the output by
// SDL's audio callback.  Total RAM cost is ~24 KB for all sounds.
//
// All calls are thread-safe (lock-free atomics for voice state).  Safe to
// call from any thread including SDL event handlers.
//

namespace ui_sounds {

enum class Sound {
    Tick,    // focus moved (very subtle click)
    Select,  // A pressed / confirm (warm two-tone chime)
    Back,    // B pressed / cancel  (descending two-tone)
    Toggle,  // settings toggle flipped (sharp click)
};

// Initialise SDL audio + synthesize all waveforms.  Returns false if SDL
// failed to open an audio device — caller should treat sounds as no-op.
bool init();

// Close audio device.  Safe to call even if init() failed.
void shutdown();

// Play `s`.  Re-entrant; multiple concurrent plays mix in the output.
// Volume is 0.0..1.0; capped by the global user volume.  No-op when
// sounds are disabled via setEnabled(false) or when init() failed.
void play(Sound s, float volume = 1.0f);

// User toggle from settings modal.  Disabled = play() returns immediately.
void setEnabled(bool enabled);
bool isEnabled();

} // namespace ui_sounds

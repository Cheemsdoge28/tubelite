#include "ui_sounds.hpp"

#include <SDL2/SDL.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace ui_sounds {

namespace {

// ── Audio config ────────────────────────────────────────────────────────────
constexpr int kSampleRate = 22050;        // half CD rate — UI sounds are
                                          // short and warm; saves CPU + RAM.
constexpr int kChannels   = 1;            // mono — UI sounds don't need stereo
constexpr int kBufSamples = 512;          // ~23 ms latency; plenty responsive
constexpr int kMaxVoices  = 6;            // simultaneous plays before drop

// Pre-synthesized waveforms, int16 mono.  Indexed by Sound enum value.
struct Waveform {
    std::vector<int16_t> samples;
};
Waveform g_waveforms[4];

// Voice slot: one currently-playing sound instance.
struct Voice {
    std::atomic<int>       wave{-1};   // -1 = inactive
    std::atomic<uint32_t>  pos{0};
    std::atomic<int>       vol_x256{256};  // 0-256, fixed point
};
Voice g_voices[kMaxVoices];

SDL_AudioDeviceID g_dev = 0;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_initialized{false};

// ── Envelope helper ────────────────────────────────────────────────────────
// Linear attack-hold-release envelope.  Inputs in seconds.  Returns 0..1.
float envelope(float t, float dur, float attack, float release) {
    if (t < attack)             return t / attack;
    if (t > dur - release)      return std::max(0.0f, (dur - t) / release);
    return 1.0f;
}

// Mix a sine wave at `freq` for `dur` seconds into the output buffer,
// modulated by an envelope.  `gain` is 0..1.
void mixSine(std::vector<float>& buf, float freq, float dur,
             float attack, float release, float gain, float startTime = 0.0f) {
    const int startSample = static_cast<int>(startTime * kSampleRate);
    const int totalSamples = static_cast<int>(dur * kSampleRate);
    const float omega = 2.0f * 3.14159265f * freq / kSampleRate;
    for (int i = 0; i < totalSamples; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float env = envelope(t, dur, attack, release);
        const float v = std::sin(omega * i) * env * gain;
        const int idx = startSample + i;
        if (idx >= 0 && idx < (int)buf.size()) buf[idx] += v;
    }
}

// Convert mixed float buffer (-1..1 range expected) to int16 PCM, with
// soft clipping at ±0.95 so we never produce hard clicks even if mixing
// overruns.
std::vector<int16_t> toPCM(const std::vector<float>& f) {
    std::vector<int16_t> out(f.size());
    for (size_t i = 0; i < f.size(); ++i) {
        float v = f[i];
        // tanh-style soft clip — cheap polynomial approximation
        if      (v >  0.95f) v =  0.95f - (v - 0.95f) * 0.1f;
        else if (v < -0.95f) v = -0.95f - (v + 0.95f) * 0.1f;
        out[i] = static_cast<int16_t>(v * 30000.0f);
    }
    return out;
}

// ── Sound designs ──────────────────────────────────────────────────────────
//
// Kept short (≤120 ms) and quiet (gain ≤0.4) so they never compete with
// video playback audio.  Envelopes are soft to avoid clicky transients
// that scream "synth beep" — premium-feeling UI sounds always have an
// attack ≥ a couple ms and a longer release than attack.

// Design rules (v2 — gentler):
//   * Frequencies in the 500-1200 Hz range; anything above 2 kHz sounds
//     "beep-y" through small handheld speakers.
//   * Long releases (40-80 ms) and slow attacks (4-8 ms) → no audible
//     transient click that registers as harsh.
//   * Peak gain ≤0.10 — half the old levels.  Multiple rapid plays still
//     read clearly because of the soft envelope.
//   * Two stacked sines an octave or fifth apart sound "rounded" without
//     adding apparent loudness.

Waveform makeTick() {                       // focus move
    constexpr float dur = 0.050f;           // 50 ms (was 30 ms — slower decay)
    std::vector<float> buf(static_cast<size_t>(dur * kSampleRate), 0.0f);
    // Low fundamental + octave, soft.  No high frequencies that clash.
    mixSine(buf, 600.0f,  dur, 0.005f, 0.045f, 0.08f);
    mixSine(buf, 1200.0f, dur, 0.005f, 0.045f, 0.04f);
    return Waveform{toPCM(buf)};
}

Waveform makeSelect() {                     // confirm / A
    constexpr float dur = 0.140f;
    std::vector<float> buf(static_cast<size_t>(dur * kSampleRate), 0.0f);
    // Warm rising fifth: low A4 (440) → E5 (660).  Lower than before so
    // it sits below voice/dialogue range.
    mixSine(buf, 440.0f, 0.110f, 0.006f, 0.090f, 0.11f, 0.000f);
    mixSine(buf, 660.0f, 0.110f, 0.008f, 0.090f, 0.08f, 0.030f);
    return Waveform{toPCM(buf)};
}

Waveform makeBack() {                       // cancel / B
    constexpr float dur = 0.120f;
    std::vector<float> buf(static_cast<size_t>(dur * kSampleRate), 0.0f);
    // Descending fifth: E5 (660) → A4 (440).
    mixSine(buf, 660.0f, 0.090f, 0.006f, 0.075f, 0.10f, 0.000f);
    mixSine(buf, 440.0f, 0.100f, 0.008f, 0.085f, 0.08f, 0.025f);
    return Waveform{toPCM(buf)};
}

Waveform makeToggle() {                     // settings switch
    constexpr float dur = 0.060f;
    std::vector<float> buf(static_cast<size_t>(dur * kSampleRate), 0.0f);
    // Single warm click — 800 Hz with a soft octave below.  No upper
    // harmonic at 3 kHz like the old version; that's what made it sharp.
    mixSine(buf, 800.0f, dur, 0.004f, 0.050f, 0.10f);
    mixSine(buf, 400.0f, dur, 0.004f, 0.050f, 0.05f);
    return Waveform{toPCM(buf)};
}

// ── SDL audio callback ─────────────────────────────────────────────────────
// Mixes all active voices into the output buffer.  Runs on the SDL audio
// thread; touches only atomics + the (immutable after init) waveform data.
void audioCallback(void* /*ud*/, Uint8* stream, int byteLen) {
    int16_t* out = reinterpret_cast<int16_t*>(stream);
    const int samples = byteLen / 2;
    std::memset(stream, 0, byteLen);

    for (auto& voice : g_voices) {
        const int wIdx = voice.wave.load(std::memory_order_acquire);
        if (wIdx < 0) continue;
        const auto& w = g_waveforms[wIdx];
        uint32_t pos = voice.pos.load(std::memory_order_relaxed);
        const int vol = voice.vol_x256.load(std::memory_order_relaxed);
        const uint32_t avail = (pos < w.samples.size())
                               ? (uint32_t)w.samples.size() - pos : 0;
        const uint32_t take = std::min((uint32_t)samples, avail);
        for (uint32_t i = 0; i < take; ++i) {
            const int32_t mix = (int32_t)out[i] +
                                (int32_t)w.samples[pos + i] * vol / 256;
            out[i] = (int16_t)std::max(-32768, std::min(32767, (int)mix));
        }
        pos += take;
        if (pos >= w.samples.size()) {
            voice.wave.store(-1, std::memory_order_release);
            voice.pos.store(0,  std::memory_order_relaxed);
        } else {
            voice.pos.store(pos, std::memory_order_relaxed);
        }
    }
}

} // namespace

bool init() {
    if (g_initialized.load()) return true;

    // Force SDL onto the ALSA backend so the device-name string below
    // is interpreted as an ALSA PCM name and not (e.g.) Pulse / pipewire.
    // The macro `SDL_HINT_AUDIODRIVER` was only added in SDL 2.0.18 and
    // this device's SDL is older; the hint string itself ("SDL_AUDIODRIVER")
    // has been the same since 2.0.0, so we use the literal directly.
    // No-op on builds that only have ALSA, harmless elsewhere.
    SDL_SetHint("SDL_AUDIODRIVER", "alsa");

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::cerr << "[ui_sounds] SDL_INIT_AUDIO failed: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = kSampleRate;
    want.format   = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples  = kBufSamples;
    want.callback = audioCallback;

    // Open SDL's audio device through ALSA's shared `plug:dmix` PCM so
    // UI sounds don't grab the codec exclusively.  Without this, SDL
    // passes nullptr → ALSA `default` → on the RK817/ArkOS device that
    // resolves to `hw:0,0`, which opens the codec O_EXCL-style and
    // blocks both mpv and any other audio client (`fuser -v` shows
    // tubelite holding `F...m` mmap on pcmC0D0p).  `plug:dmix` is the
    // same shared PCM mpv uses (see mpv_player.cpp), and dmix allows
    // multiple openers via /dev/shm/asound.<key> coordination.
    //
    // If the host doesn't have `plug:dmix` available (unlikely on
    // anything with default ALSA install), fall back to nullptr so UI
    // sounds still work — they just won't share the device.
    SDL_AudioSpec have{};
    g_dev = SDL_OpenAudioDevice("plug:dmix", 0, &want, &have,
                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (g_dev == 0) {
        std::cerr << "[ui_sounds] SDL_OpenAudioDevice(plug:dmix) failed: "
                  << SDL_GetError() << " — falling back to default\n";
        g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                                    SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    }
    if (g_dev == 0) {
        std::cerr << "[ui_sounds] SDL_OpenAudioDevice failed: "
                  << SDL_GetError() << "\n";
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    g_waveforms[(int)Sound::Tick]   = makeTick();
    g_waveforms[(int)Sound::Select] = makeSelect();
    g_waveforms[(int)Sound::Back]   = makeBack();
    g_waveforms[(int)Sound::Toggle] = makeToggle();

    SDL_PauseAudioDevice(g_dev, 0);
    g_initialized.store(true);
    std::cerr << "[ui_sounds] initialized — "
              << "rate=" << have.freq << " ch=" << (int)have.channels
              << " buf=" << have.samples << "\n";
    return true;
}

void shutdown() {
    if (!g_initialized.exchange(false)) return;
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void play(Sound s, float volume) {
    if (!g_initialized.load() || !g_enabled.load()) return;
    const int sIdx = static_cast<int>(s);
    if (sIdx < 0 || sIdx >= 4) return;
    const int vol_x256 = std::max(0, std::min(256, (int)(volume * 256.0f)));
    if (vol_x256 == 0) return;

    // Find a free voice slot.  Lock-free: claim by CAS'ing the wave
    // index from -1 to our sIdx.  If all voices are busy we drop the
    // sound — better than queueing latency on a UI tick.
    for (auto& voice : g_voices) {
        int expected = -1;
        if (voice.wave.compare_exchange_strong(expected, sIdx,
                                               std::memory_order_acq_rel)) {
            voice.pos.store(0, std::memory_order_relaxed);
            voice.vol_x256.store(vol_x256, std::memory_order_relaxed);
            return;
        }
    }
}

void setEnabled(bool enabled) { g_enabled.store(enabled); }
bool isEnabled()              { return g_enabled.load(); }

} // namespace ui_sounds

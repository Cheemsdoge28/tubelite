#pragma once
//
//  theme.hpp — TubeLite unified design system
//  ---------------------------------------------------------------------------
//  Single source of truth for the look of the whole application: the SDL
//  browse/search/playback UI *and* the DRM-overlay background daemon. Both
//  rendering stacks include this header so the now-playing card, the grid
//  cards, the playback HUD, the keyboard and the status bar all speak the same
//  visual language — same palette, same radii, same accent.
//
//  Design intent: a refined deep navy-black surface with a single confident
//  YouTube-red accent. Everything else (greys, status colours) is tuned to sit
//  calmly against that base so the red always reads as "the important thing".
//
//  Performance notes for the rk3326 (Mali-G31, quad A35):
//    * Everything here is `constexpr` — zero runtime cost, folded at compile
//      time, no static initialisation, no per-frame allocation.
//    * `Rgba` is a 4-byte POD. The daemon's software rasteriser reads .r/.g/.b
//      directly; the SDL side gets a zero-overhead implicit SDL_Color cast.
//    * Keeping one palette means caches (header layer, status bar, keyboard,
//      miniplayer chrome) stay valid longer because colours no longer drift
//      between code paths.
//
//  This header is intentionally free of any SDL include. The SDL_Color
//  convenience cast is only compiled into translation units that have already
//  included SDL (guarded on SDL's own include sentinel, SDL_h_), so the daemon
//  — which never links the SDL render path — pulls in nothing extra.
//
#include <cstdint>

namespace theme {

// ── Core colour type ────────────────────────────────────────────────────────
// 8-bit RGBA. Trivial, copyable, constexpr-friendly.
struct Rgba {
    uint8_t r, g, b, a;

    constexpr Rgba() : r(0), g(0), b(0), a(255) {}
    constexpr Rgba(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
        : r(r_), g(g_), b(b_), a(a_) {}

    // Return the same colour with an explicit alpha (0-255).
    constexpr Rgba a8(uint8_t na) const { return Rgba(r, g, b, na); }

    // Return the same colour with its alpha scaled by `f` (0.0-1.0).
    // Used for fade-in/out and translucency without spelling out new literals.
    constexpr Rgba fade(float f) const {
        return Rgba(r, g, b,
                    static_cast<uint8_t>(static_cast<float>(a) *
                                         (f < 0.f ? 0.f : (f > 1.f ? 1.f : f))));
    }

#ifdef SDL_h_
    // Zero-cost implicit bridge so `theme::ACCENT` drops straight into any
    // call site expecting an SDL_Color. Only exists where SDL is in scope.
    operator SDL_Color() const { return SDL_Color{r, g, b, a}; }
#endif
};

// ── Surfaces (darkest → lightest) ───────────────────────────────────────────
inline constexpr Rgba BG       {12, 13, 18};   // screen background, deep navy-black
inline constexpr Rgba BAR      {16, 18, 22};   // top/bottom chrome bars (status, HUD)
inline constexpr Rgba PANEL    {24, 28, 34};   // hint-bar / drawer / HUD panel fills
inline constexpr Rgba SURFACE  {26, 28, 32};   // card body, miniplayer strip
inline constexpr Rgba CHIP     {42, 48, 56};   // hint pills, key chips, small buttons
inline constexpr Rgba RAISED   {34, 38, 48};   // gradient lift / hovered surface
inline constexpr Rgba TRACK    {58, 64, 78};   // progress track, inactive fills
inline constexpr Rgba BUFFERED {92, 98, 112};  // buffered-ahead portion of progress
inline constexpr Rgba DIVIDER  {32, 36, 44};   // structural 1-2px lines on bars/panels
inline constexpr Rgba HAIRLINE {52, 58, 70};   // lighter 1px separators on cards/chips
inline constexpr Rgba BORDER   {48, 52, 62};   // resting card / element border
inline constexpr Rgba THUMB_BG {30, 32, 38};   // thumbnail placeholder

// ── Accent ──────────────────────────────────────────────────────────────────
inline constexpr Rgba ACCENT        {255, 48, 48};  // the one red
inline constexpr Rgba ACCENT_BRIGHT {255, 85, 85};  // hover / search title

// ── Text (most prominent → faintest) ────────────────────────────────────────
inline constexpr Rgba TEXT       {240, 242, 245};  // titles, primary content
inline constexpr Rgba TEXT_2     {165, 172, 186};  // channel / secondary
inline constexpr Rgba TEXT_3     {140, 144, 158};  // views, dates, tertiary
inline constexpr Rgba TEXT_ON    {214, 220, 230};  // text sitting on panels/HUD
inline constexpr Rgba TEXT_MUTED {120, 126, 140};  // faint hints, captions

// ── Status / semantic accents (badges, hint chips) ──────────────────────────
inline constexpr Rgba GREEN  {64, 214, 96};
inline constexpr Rgba YELLOW {255, 214, 64};
inline constexpr Rgba BLUE   {64, 148, 255};
inline constexpr Rgba PURPLE {191, 64, 255};

// ── Neutrals for shadows / scrims / highlights ──────────────────────────────
inline constexpr Rgba BLACK {0, 0, 0};
inline constexpr Rgba WHITE {255, 255, 255};

// ── Geometry tokens ─────────────────────────────────────────────────────────
inline constexpr int RADIUS_CARD  = 8;   // cards, miniplayer, focus ring
inline constexpr int RADIUS_PANEL = 6;   // panels, drawers, badges
inline constexpr int RADIUS_PILL  = 4;   // hint pills, duration pill
inline constexpr int RADIUS_SM    = 3;   // small chips, scrub label

} // namespace theme

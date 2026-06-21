#include "settings_modal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "app.hpp"
#include "renderer_utils.hpp"
#include "theme.hpp"

namespace {

// Selectable quality steps.  Anything above 360 is currently aspirational
// (yt-dlp is muxed-only via ios+android client), but the steps remain in
// case DASH support returns later.
constexpr int kQualitySteps[] = {144, 240, 360, 480, 720};

enum RowKind {
    KIND_HEADER,        // non-focusable section label
    KIND_CYCLE,         // < value >   (max quality, home tab)
    KIND_TOGGLE,        // ON/OFF pill (hover previews, autoplay, etc.)
    KIND_SLIDER,        // ▰▰▰ N      (volume)
    KIND_ACTION,        // > Reset to defaults
};

struct Row {
    RowKind kind;
    const char* label;
};

// One source of truth for the modal layout.  Edit here to add/remove rows.
const Row kRows[] = {
    {KIND_HEADER, "PLAYBACK"},
    {KIND_CYCLE,  "Max quality"},
    {KIND_TOGGLE, "Hover previews"},
    {KIND_TOGGLE, "Autoplay next"},
    {KIND_HEADER, "HOME"},
    {KIND_CYCLE,  "Home tab"},
    {KIND_HEADER, "AUDIO"},
    {KIND_SLIDER, "Default volume"},
    {KIND_HEADER, "ADVANCED"},
    {KIND_TOGGLE, "Background daemon"},
    {KIND_TOGGLE, "Debug overlay"},
    {KIND_ACTION, "Reset to defaults"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// Tight daemon-style spacing — matches the now-playing card's compact feel.
constexpr int kRowH     = 18;     // data rows
constexpr int kHeaderH  = 16;     // headers
constexpr int kHeaderSp = 4;      // space above each header
constexpr int kAccentW  = 3;      // left accent bar width (daemon uses 4 on 380px)
constexpr int kPadL     = 14;     // card left padding (after accent bar)
constexpr int kPadR     = 14;     // card right padding
constexpr int kTopH     = 30;     // SETTINGS title strip
constexpr int kBotH     = 28;     // footer hint bar

int clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

int qualityStepIndex(int height) {
    int best = 0, bestDist = 9999;
    for (int i = 0; i < (int)(sizeof(kQualitySteps) / sizeof(kQualitySteps[0])); ++i) {
        int d = std::abs(kQualitySteps[i] - height);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

bool isFocusable(int rowIdx) {
    return kRows[rowIdx].kind != KIND_HEADER;
}

int firstFocusableFrom(int start, int direction) {
    int i = start;
    for (int step = 0; step < kRowCount; ++step) {
        i = (i + direction + kRowCount) % kRowCount;
        if (isFocusable(i)) return i;
    }
    return start;
}

// Daemon-style 1px hairline rule.
void hrule(SDL_Renderer* r, int x, int y, int w, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    SDL_Rect line{x, y, w, 1};
    SDL_RenderFillRect(r, &line);
}

// ON/OFF status pill matching the daemon's badge styling: low-alpha tinted
// fill + slightly stronger border + bright text.
void drawPill(SDL_Renderer* r, int xRight, int y, bool on) {
    const char* label = on ? "ON" : "OFF";
    int tw = 0, th = 0;
    getTextSize(label, 1, &tw, &th);
    const int padX = 7;
    SDL_Rect pill{xRight - tw - padX * 2, y - 1, tw + padX * 2, th + 2};
    SDL_Color tint = on ? SDL_Color(theme::GREEN) : SDL_Color(theme::TEXT_MUTED);
    fillRoundedRect(r, pill, theme::RADIUS_PILL, SDL_Color{tint.r, tint.g, tint.b, 38});
    drawRoundedRect(r, pill, theme::RADIUS_PILL, SDL_Color{tint.r, tint.g, tint.b, 180});
    drawText(r, pill.x + padX, y, label, 1, tint);
}

// Slim slider: 90px pill track, accent-red fill, numeric to the right.
void drawSlider(SDL_Renderer* r, int xRight, int y, int value) {
    std::string num = std::to_string(value);
    int nw = 0, nh = 0;
    getTextSize(num, 1, &nw, &nh);
    const int barW = 90;
    const int barH = 5;
    const int gap  = 8;
    SDL_Rect track{xRight - nw - gap - barW, y + (nh - barH) / 2, barW, barH};
    fillRoundedRect(r, track, barH / 2, theme::TRACK.a8(220));
    SDL_Rect fill{track.x, track.y, (track.w * clamp(value, 0, 100)) / 100, track.h};
    fillRoundedRect(r, fill, barH / 2, theme::ACCENT);
    drawText(r, xRight - nw, y, num, 1, theme::TEXT_ON);
}

// Cycle value with < > chevrons only when focused (avoids visual noise on
// unfocused rows).  Chevron-row width is tight: each glyph + 4px.
void drawCycleValue(SDL_Renderer* r, int xRight, int y, const std::string& value, bool focused) {
    int vw = 0, vh = 0;
    getTextSize(value, 1, &vw, &vh);
    SDL_Color col = focused ? SDL_Color(theme::ACCENT_BRIGHT) : SDL_Color(theme::TEXT_ON);
    if (focused) {
        drawText(r, xRight - vw - 18, y, "<", 1, theme::TEXT_2);
        drawText(r, xRight + 6,       y, ">", 1, theme::TEXT_2);
    }
    drawText(r, xRight - vw - 4, y, value, 1, col);
}

} // namespace

void SettingsModal::apply(App* app, const Settings& s) {
    app->state_.maxQualityHeight        = s.maxQualityHeight;
    app->state_.homeFeedKind            = s.homeFeedKind;
    app->state_.volume                  = s.volume;
    app->state_.showDebugOverlay        = s.showDebugOverlay;
    app->state_.backgroundDaemonEnabled = s.backgroundDaemonEnabled;
    app->state_.hoverPreviewsEnabled    = s.hoverPreviewsEnabled;
    app->state_.autoplayNextEnabled     = s.autoplayNextEnabled;
}

void SettingsModal::render(App* app, SDL_Renderer* renderer, int width, int height) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect scrim{0, 0, width, height};
    fillRoundedRect(renderer, scrim, 0, theme::BLACK.a8(210));

    // Auto-sized card from the row table.
    int contentH = kTopH + kBotH;
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].kind == KIND_HEADER) contentH += kHeaderH + (i == 0 ? 0 : kHeaderSp);
        else                              contentH += kRowH;
    }
    const int cw = std::min(width  - 24, 420);
    const int ch = std::min(height - 16, contentH);
    const int cx = (width  - cw) / 2;
    const int cy = (height - ch) / 2;

    // Diffuse shadow (daemon-style, two passes).
    {
        SDL_Rect s1{cx + 2, cy + 4, cw, ch};
        SDL_Rect s2{cx + 1, cy + 2, cw, ch + 1};
        fillRoundedRect(renderer, s1, theme::RADIUS_CARD, theme::BLACK.a8(70));
        fillRoundedRect(renderer, s2, theme::RADIUS_CARD, theme::BLACK.a8(40));
    }

    // Card body
    SDL_Rect card{cx, cy, cw, ch};
    fillRoundedRect(renderer, card, theme::RADIUS_CARD, theme::SURFACE);

    // Subtle top-half lift (faked gradient via stacked low-alpha bands).
    for (int i = 0; i < 12; ++i) {
        int alpha = 14 - i;
        if (alpha <= 0) break;
        SDL_Rect band{cx + 1, cy + 1 + i * 2, cw - 2, 2};
        fillRoundedRect(renderer, band, 0, theme::RAISED.a8(alpha));
    }

    // Left accent bar (the daemon's signature)
    SDL_Rect accent{cx, cy, kAccentW, ch};
    fillRoundedRect(renderer, accent, 2, theme::ACCENT);
    // Glow to the right of the bar — short horizontal gradient.
    for (int i = 0; i < 16; ++i) {
        int alpha = 30 - i * 2;
        if (alpha <= 0) break;
        SDL_Rect g{cx + kAccentW + i, cy + 1, 1, ch - 2};
        fillRoundedRect(renderer, g, 0, theme::ACCENT.a8(alpha));
    }

    // Card border
    drawRoundedRect(renderer, card, theme::RADIUS_CARD, theme::BORDER);

    // Header strip: title left, inline shortcut summary right.
    int x = cx + kAccentW + kPadL;
    int y = cy + 8;
    drawText(renderer, x, y, "Settings", 2, theme::TEXT);
    {
        const char* sub = "B close";
        int sw = 0, sh = 0;
        getTextSize(sub, 1, &sw, &sh);
        drawText(renderer, cx + cw - kPadR - sw, y + 4, sub, 1, theme::TEXT_MUTED);
    }
    hrule(renderer, cx + 8, cy + kTopH - 1, cw - 16, theme::HAIRLINE.a8(110));

    // Cursor housekeeping (in case the row table changed across runs)
    int& cursor = app->state_.settingsModalIndex;
    if (!isFocusable(cursor)) cursor = firstFocusableFrom(cursor, +1);
    cursor = clamp(cursor, 0, kRowCount - 1);

    // Rows
    y = cy + kTopH + 2;
    const int valX = cx + cw - kPadR;
    for (int i = 0; i < kRowCount; ++i) {
        const Row& row = kRows[i];

        if (row.kind == KIND_HEADER) {
            if (i != 0) y += kHeaderSp;
            // Section header in accent — small, uppercase, with a tiny rule
            // under it to mirror the daemon's hairline-divided sections.
            drawText(renderer, x, y + 2, row.label, 1, theme::ACCENT);
            int hw = 0, hh = 0;
            getTextSize(row.label, 1, &hw, &hh);
            hrule(renderer, x + hw + 8, y + 8,
                  (cx + cw - kPadR) - (x + hw + 8),
                  theme::HAIRLINE.a8(70));
            y += kHeaderH;
            continue;
        }

        const bool focused = (i == cursor);
        if (focused) {
            // High-contrast selection: warm tinted background + bright
            // accent left-stub.  Replaces the old PANEL.a8(180) which was
            // nearly invisible against SURFACE (same hue).
            SDL_Rect bg{cx + kAccentW + 4, y - 1, cw - kAccentW - 8, kRowH - 1};
            fillRoundedRect(renderer, bg, theme::RADIUS_SM, theme::ACCENT.a8(36));
            SDL_Rect stub{cx + kAccentW + 4, y - 1, 2, kRowH - 1};
            fillRoundedRect(renderer, stub, 1, theme::ACCENT);
        }
        SDL_Color labelCol = focused ? SDL_Color(theme::TEXT) : SDL_Color(theme::TEXT_ON);
        drawText(renderer, x + 4, y + 3, row.label, 1, labelCol);

        const int valY = y + 3;
        if (row.kind == KIND_CYCLE) {
            std::string val;
            if (std::strcmp(row.label, "Max quality") == 0) {
                val = std::to_string(app->state_.maxQualityHeight) + "p";
            } else if (std::strcmp(row.label, "Home tab") == 0) {
                val = (app->state_.homeFeedKind == "subscriptions") ? "Subscriptions" : "Trending";
            }
            drawCycleValue(renderer, valX, valY, val, focused);
        } else if (row.kind == KIND_TOGGLE) {
            bool v = false;
            if      (std::strcmp(row.label, "Hover previews")    == 0) v = app->state_.hoverPreviewsEnabled;
            else if (std::strcmp(row.label, "Autoplay next")     == 0) v = app->state_.autoplayNextEnabled;
            else if (std::strcmp(row.label, "Background daemon") == 0) v = app->state_.backgroundDaemonEnabled;
            else if (std::strcmp(row.label, "Debug overlay")     == 0) v = app->state_.showDebugOverlay;
            drawPill(renderer, valX, valY, v);
        } else if (row.kind == KIND_SLIDER) {
            drawSlider(renderer, valX, valY, app->state_.volume);
        } else if (row.kind == KIND_ACTION) {
            const char* hint = focused ? "A to confirm" : "";
            int hw = 0, hh = 0;
            getTextSize(hint, 1, &hw, &hh);
            drawText(renderer, valX - hw, valY, hint, 1,
                     focused ? SDL_Color(theme::ACCENT_BRIGHT) : SDL_Color(theme::TEXT_MUTED));
        }
        y += kRowH;
    }

    // Footer hint bar (daemon-style: hairline above, compact chips)
    hrule(renderer, cx + 8, cy + ch - kBotH + 2, cw - 16, theme::HAIRLINE.a8(90));
    std::vector<HintItem> hints = {
        {"DPAD",  theme::TEXT_ON, "MOVE"},
        {"LF/RT", theme::BLUE,    "EDIT"},
        {"A",     theme::ACCENT,  "TOGGLE"},
        {"B",     theme::YELLOW,  "CLOSE"},
    };
    drawHintButtons(renderer, hints, cy + ch - 22, 18, 1, 2 * cx + cw,
                    theme::PANEL.a8(210), theme::CHIP.a8(180), theme::TEXT_ON);

    app->uiDirty_ = true;
}

bool SettingsModal::handleKey(App* app, SDL_Keycode key) {
    int& cursor = app->state_.settingsModalIndex;
    if (!isFocusable(cursor)) cursor = firstFocusableFrom(cursor, +1);
    cursor = clamp(cursor, 0, kRowCount - 1);

    auto applyStep = [&](int delta) {
        const Row& row = kRows[cursor];
        const char* label = row.label;
        switch (row.kind) {
            case KIND_CYCLE:
                if (std::strcmp(label, "Max quality") == 0) {
                    int qi = qualityStepIndex(app->state_.maxQualityHeight) + delta;
                    qi = clamp(qi, 0, (int)(sizeof(kQualitySteps) / sizeof(kQualitySteps[0])) - 1);
                    app->state_.maxQualityHeight = kQualitySteps[qi];
                } else if (std::strcmp(label, "Home tab") == 0) {
                    app->state_.homeFeedKind =
                        (app->state_.homeFeedKind == "trending") ? "subscriptions" : "trending";
                }
                break;
            case KIND_TOGGLE:
                if      (std::strcmp(label, "Hover previews")    == 0) app->state_.hoverPreviewsEnabled    = !app->state_.hoverPreviewsEnabled;
                else if (std::strcmp(label, "Autoplay next")     == 0) app->state_.autoplayNextEnabled     = !app->state_.autoplayNextEnabled;
                else if (std::strcmp(label, "Background daemon") == 0) app->state_.backgroundDaemonEnabled = !app->state_.backgroundDaemonEnabled;
                else if (std::strcmp(label, "Debug overlay")     == 0) app->state_.showDebugOverlay        = !app->state_.showDebugOverlay;
                break;
            case KIND_SLIDER:
                app->state_.volume = clamp(app->state_.volume + delta * 5, 0, 100);
                break;
            case KIND_ACTION:
                if (std::strcmp(label, "Reset to defaults") == 0 && delta > 0) {
                    Settings defaults;
                    SettingsModal::apply(app, defaults);
                }
                break;
            default: break;
        }
        app->uiDirty_ = true;
    };

    switch (key) {
        case SDLK_UP:
            cursor = firstFocusableFrom(cursor, -1);
            app->uiDirty_ = true;
            return true;
        case SDLK_DOWN:
            cursor = firstFocusableFrom(cursor, +1);
            app->uiDirty_ = true;
            return true;
        case SDLK_LEFT:   applyStep(-1); return true;
        case SDLK_RIGHT:  applyStep(+1); return true;
        case SDLK_RETURN:
        case SDLK_a:      applyStep(+1); return true;
        case SDLK_ESCAPE:
        case SDLK_b:      return false;
        default:          return true;
    }
}

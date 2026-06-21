#include "settings_modal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "app.hpp"
#include "renderer_utils.hpp"
#include "theme.hpp"
#include "ui_sounds.hpp"

namespace {

// ── Spacing rhythm ──────────────────────────────────────────────────────────
// Everything below is a multiple of 4 px.  Picking ONE base unit and sticking
// to it is what makes a UI feel ordered instead of arbitrary; previous
// attempts free-handed each metric (22 px row here, 16 px header there,
// 18 px gap somewhere else) and that's why nothing aligned.
constexpr int U = 4;        // base unit

constexpr int kCardW       = 440;
constexpr int kAccentW     = 3;
constexpr int kCardPadL    = 4 * U;      // 16 px content inset past accent bar
constexpr int kCardPadR    = 4 * U;
constexpr int kCardPadT    = 3 * U;      // 12 px from card top to title baseline
constexpr int kCardPadB    = 3 * U;

constexpr int kTitleH      = 7 * U;      // 28 px — title + breathing room
constexpr int kDividerGap  = 2 * U;      // 8 px between title and content
constexpr int kSectionGap  = 3 * U;      // 12 px above each section header
constexpr int kHeaderH     = 5 * U;      // 20 px — section label row
constexpr int kRowH        = 7 * U;      // 28 px — comfortable, touch-friendly
constexpr int kHintH       = 9 * U;      // 36 px — footer hint bar

// Quality cycle options.
constexpr int kQualitySteps[] = {144, 240, 360, 480, 720};

enum RowKind {
    KIND_HEADER,        // non-focusable section label
    KIND_CYCLE,         // < value >
    KIND_TOGGLE,        // ON/OFF pill
    KIND_SLIDER,        // ▰▰▰ N
    KIND_ACTION,        // > confirm action
};

struct Row {
    RowKind kind;
    const char* label;
};

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
    {KIND_TOGGLE, "UI sounds"},
    {KIND_TOGGLE, "Debug overlay"},
    {KIND_ACTION, "Reset to defaults"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

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

// Row layout height (varies by kind).
int rowHeight(int rowIdx) {
    if (kRows[rowIdx].kind != KIND_HEADER) return kRowH;
    // First header has no extra gap; subsequent headers get a section gap.
    return kHeaderH + (rowIdx == 0 ? 0 : kSectionGap);
}

// ── Component drawing ──────────────────────────────────────────────────────

// Right-aligned ON/OFF pill — green = on, muted grey = off.
void drawPill(SDL_Renderer* r, int xRight, int yCenter, bool on) {
    const char* label = on ? "ON" : "OFF";
    int tw = 0, th = 0;
    getTextSize(label, 1, &tw, &th);
    constexpr int padX = 8, padY = 3;
    SDL_Rect pill{xRight - tw - padX * 2, yCenter - th / 2 - padY,
                  tw + padX * 2, th + padY * 2};
    SDL_Color tint = on ? SDL_Color(theme::GREEN) : SDL_Color(theme::TEXT_MUTED);
    fillRoundedRect(r, pill, theme::RADIUS_PILL, SDL_Color{tint.r, tint.g, tint.b, 36});
    drawRoundedRect(r, pill, theme::RADIUS_PILL, SDL_Color{tint.r, tint.g, tint.b, 170});
    drawText(r, pill.x + padX, pill.y + padY, label, 1, tint);
}

// Slider with track + accent fill + right-aligned numeric.  Volume only.
void drawSlider(SDL_Renderer* r, int xRight, int yCenter, int value) {
    std::string num = std::to_string(value);
    int nw = 0, nh = 0;
    getTextSize(num, 1, &nw, &nh);
    constexpr int barW = 110;
    constexpr int barH = 6;
    constexpr int gap  = 3 * U;     // 12 px between bar and number
    SDL_Rect track{xRight - nw - gap - barW, yCenter - barH / 2, barW, barH};
    fillRoundedRect(r, track, barH / 2, theme::TRACK.a8(220));
    SDL_Rect fill{track.x, track.y, (track.w * clamp(value, 0, 100)) / 100, track.h};
    fillRoundedRect(r, fill, barH / 2, theme::ACCENT);
    drawText(r, xRight - nw, yCenter - nh / 2, num, 1, theme::TEXT);
}

// Cycle value with discreet < > chevrons.  Chevrons appear only when
// focused, since on unfocused rows they'd be visual noise on every line.
void drawCycleValue(SDL_Renderer* r, int xRight, int yCenter,
                    const std::string& value, bool focused) {
    int vw = 0, vh = 0;
    getTextSize(value, 1, &vw, &vh);
    SDL_Color col = focused ? SDL_Color(theme::ACCENT_BRIGHT) : SDL_Color(theme::TEXT);
    const int yText = yCenter - vh / 2;
    if (focused) {
        // Chevrons sit 16 px away from the value on each side — wide enough
        // that the value doesn't feel cramped between them.
        drawText(r, xRight - vw - 4 * U - 6, yText, "<", 1, theme::TEXT_2);
        drawText(r, xRight + 6,              yText, ">", 1, theme::TEXT_2);
    }
    drawText(r, xRight - vw - 4, yText, value, 1, col);
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
    app->state_.uiSoundsEnabled         = s.uiSoundsEnabled;
    ui_sounds::setEnabled(s.uiSoundsEnabled);
}

void SettingsModal::render(App* app, SDL_Renderer* renderer, int width, int height) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect scrim{0, 0, width, height};
    fillRoundedRect(renderer, scrim, 0, theme::BLACK.a8(190));

    // ── Card sizing (sum of all the spacing constants — no magic numbers) ──
    int rowsBlockH = 0;
    for (int i = 0; i < kRowCount; ++i) rowsBlockH += rowHeight(i);
    const int cw = std::min(width  - 24, kCardW);
    const int ch = std::min(height - 16,
                            kCardPadT + kTitleH + kDividerGap +
                            rowsBlockH + kCardPadB + kHintH);
    const int cx = (width  - cw) / 2;
    const int cy = (height - ch) / 2;

    // Daemon-card chrome: shadow + body + lift + accent bar + glow + border.
    drawDaemonCard(renderer, SDL_Rect{cx, cy, cw, ch}, theme::RADIUS_CARD);

    // ── Title strip ────────────────────────────────────────────────────────
    int contentLeft  = cx + kAccentW + kCardPadL;
    int contentRight = cx + cw - kCardPadR;
    int titleTop     = cy + kCardPadT;

    // Title text — sentence-case "Settings", primary text colour.  Larger
    // type but not red — red is reserved for active accents.
    {
        int th = 0;
        getTextSize("Settings", 2, nullptr, &th);
        drawText(renderer, contentLeft, titleTop + (kTitleH - th) / 2,
                 "Settings", 2, theme::TEXT);
    }
    // Right-side micro-hint sized at 1× so it doesn't compete with the
    // title for attention.  Sits on the same vertical centerline.
    {
        const char* sub = "B close";
        int sw = 0, sh = 0;
        getTextSize(sub, 1, &sw, &sh);
        drawText(renderer, contentRight - sw,
                 titleTop + (kTitleH - sh) / 2, sub, 1, theme::TEXT_MUTED);
    }
    // Single hairline divider exactly kDividerGap/2 below the title block.
    drawHairline(renderer, contentLeft, titleTop + kTitleH + kDividerGap / 2,
                 contentRight - contentLeft, theme::HAIRLINE.a8(90));

    // Cursor housekeeping
    int& cursor = app->state_.settingsModalIndex;
    if (!isFocusable(cursor)) cursor = firstFocusableFrom(cursor, +1);
    cursor = clamp(cursor, 0, kRowCount - 1);

    // ── Rows ───────────────────────────────────────────────────────────────
    int y = titleTop + kTitleH + kDividerGap;
    for (int i = 0; i < kRowCount; ++i) {
        const Row& row = kRows[i];
        const int rh = rowHeight(i);

        if (row.kind == KIND_HEADER) {
            // Section label: small, uppercase, accent-red, sitting in the
            // section's gap region so it reads as a label not a row.
            int extra = (i == 0) ? 0 : kSectionGap;
            int labelTop = y + extra;
            int gh = 0;
            getTextSize(row.label, 1, nullptr, &gh);
            int labelY = labelTop + (kHeaderH - gh) / 2;
            drawText(renderer, contentLeft, labelY, row.label, 1, theme::ACCENT);
            // Hairline to the right of the label, vertically centered with
            // it — daemon-style "section-separator" pattern.
            int lw = 0;
            getTextSize(row.label, 1, &lw, nullptr);
            drawHairline(renderer, contentLeft + lw + 2 * U,
                         labelY + gh / 2,
                         contentRight - (contentLeft + lw + 2 * U),
                         theme::HAIRLINE.a8(70));
            y += rh;
            continue;
        }

        const bool focused = (i == cursor);
        const int yCenter = y + kRowH / 2;

        // Selection background: NEUTRAL elevated surface so the red accent
        // bar (which appears on the SELECTED row's left edge) stays the
        // only red element and reads loud.  Previous version tinted the
        // background red too, which made the bar disappear into the wash.
        if (focused) {
            SDL_Rect bg{cx + kAccentW + 2 * U,
                        y + 2,
                        cw - kAccentW - 4 * U,
                        kRowH - 4};
            fillRoundedRect(renderer, bg, theme::RADIUS_PANEL, theme::RAISED);
            // 2 px bright accent strip on the left of the highlight band.
            SDL_Rect stub{bg.x, bg.y, 2, bg.h};
            fillRoundedRect(renderer, stub, 1, theme::ACCENT);
        }

        // Label
        int lh = 0;
        getTextSize(row.label, 1, nullptr, &lh);
        SDL_Color labelCol = focused ? SDL_Color(theme::TEXT) : SDL_Color(theme::TEXT_ON);
        drawText(renderer, contentLeft + 2 * U, yCenter - lh / 2,
                 row.label, 1, labelCol);

        // Value
        if (row.kind == KIND_CYCLE) {
            std::string val;
            if (std::strcmp(row.label, "Max quality") == 0) {
                val = std::to_string(app->state_.maxQualityHeight) + "p";
            } else if (std::strcmp(row.label, "Home tab") == 0) {
                val = (app->state_.homeFeedKind == "subscriptions")
                          ? "Subscriptions" : "Trending";
            }
            drawCycleValue(renderer, contentRight, yCenter, val, focused);
        } else if (row.kind == KIND_TOGGLE) {
            bool v = false;
            if      (std::strcmp(row.label, "Hover previews")    == 0) v = app->state_.hoverPreviewsEnabled;
            else if (std::strcmp(row.label, "Autoplay next")     == 0) v = app->state_.autoplayNextEnabled;
            else if (std::strcmp(row.label, "Background daemon") == 0) v = app->state_.backgroundDaemonEnabled;
            else if (std::strcmp(row.label, "UI sounds")         == 0) v = app->state_.uiSoundsEnabled;
            else if (std::strcmp(row.label, "Debug overlay")     == 0) v = app->state_.showDebugOverlay;
            drawPill(renderer, contentRight, yCenter, v);
        } else if (row.kind == KIND_SLIDER) {
            drawSlider(renderer, contentRight, yCenter, app->state_.volume);
        } else if (row.kind == KIND_ACTION) {
            const char* hint = focused ? "A to confirm" : "";
            int hw = 0, hh = 0;
            getTextSize(hint, 1, &hw, &hh);
            drawText(renderer, contentRight - hw, yCenter - hh / 2, hint, 1,
                     focused ? SDL_Color(theme::ACCENT_BRIGHT)
                             : SDL_Color(theme::TEXT_MUTED));
        }
        y += rh;
    }

    // ── Footer hint bar ────────────────────────────────────────────────────
    drawHairline(renderer, contentLeft, cy + ch - kHintH,
                 contentRight - contentLeft, theme::HAIRLINE.a8(90));
    std::vector<HintItem> hints = {
        {"DPAD",  theme::TEXT_ON, "MOVE"},
        {"LF/RT", theme::BLUE,    "EDIT"},
        {"A",     theme::ACCENT,  "TOGGLE"},
        {"B",     theme::YELLOW,  "CLOSE"},
    };
    drawHintButtons(renderer, hints,
                    cy + ch - kHintH + 2 * U,   // 8 px below the hairline
                    5 * U,                       // 20 px chip height
                    1, 2 * cx + cw,
                    theme::PANEL.a8(210),
                    theme::CHIP.a8(180),
                    theme::TEXT_ON);

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
                    qi = clamp(qi, 0,
                               (int)(sizeof(kQualitySteps) / sizeof(kQualitySteps[0])) - 1);
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
                else if (std::strcmp(label, "UI sounds")         == 0) {
                    app->state_.uiSoundsEnabled = !app->state_.uiSoundsEnabled;
                    ui_sounds::setEnabled(app->state_.uiSoundsEnabled);
                }
                else if (std::strcmp(label, "Debug overlay")     == 0) app->state_.showDebugOverlay        = !app->state_.showDebugOverlay;
                ui_sounds::play(ui_sounds::Sound::Toggle);
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
            ui_sounds::play(ui_sounds::Sound::Tick);
            app->uiDirty_ = true;
            return true;
        case SDLK_DOWN:
            cursor = firstFocusableFrom(cursor, +1);
            ui_sounds::play(ui_sounds::Sound::Tick);
            app->uiDirty_ = true;
            return true;
        case SDLK_LEFT:   applyStep(-1); return true;
        case SDLK_RIGHT:  applyStep(+1); return true;
        case SDLK_RETURN:
        case SDLK_a:      applyStep(+1); return true;
        case SDLK_ESCAPE:
        case SDLK_b:
            ui_sounds::play(ui_sounds::Sound::Back);
            return false;
        default:          return true;
    }
}

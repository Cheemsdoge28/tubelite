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

// Row layout — single ordered list with a `kind` per row so render/handleKey
// stay table-driven instead of growing switches.
enum RowKind {
    KIND_HEADER,        // non-focusable section label
    KIND_CYCLE,         // ◄ value ►   (max quality, home tab)
    KIND_TOGGLE,        // ON/OFF pill (hover previews, autoplay, etc.)
    KIND_SLIDER,        // ▰▰▰▱▱ N    (volume)
    KIND_ACTION,        // > Reset to defaults
};

struct Row {
    RowKind kind;
    const char* label;     // displayed text
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
constexpr int kRowH     = 22;     // pixels per row
constexpr int kHeaderH  = 20;     // headers are slightly tighter
constexpr int kTopH     = 38;     // SETTINGS title block
constexpr int kBotH     = 36;     // footer hint reserve

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
    // Walk in `direction` (±1) until we land on a focusable row.  Wraps.
    int i = start;
    for (int step = 0; step < kRowCount; ++step) {
        i = (i + direction + kRowCount) % kRowCount;
        if (isFocusable(i)) return i;
    }
    return start;
}

// Render a small ON/OFF pill.  ON = green tint, OFF = dim panel.
void drawTogglePill(SDL_Renderer* r, int xRight, int y, bool on) {
    const char* label = on ? "ON" : "OFF";
    int tw = 0, th = 0;
    getTextSize(label, 1, &tw, &th);
    const int padX = 8, padY = 4;
    SDL_Rect pill{xRight - tw - padX * 2, y - padY / 2,
                  tw + padX * 2, th + padY};
    SDL_Color bg     = on ? theme::GREEN.a8(200) : theme::PANEL.a8(200);
    SDL_Color border = on ? theme::GREEN          : theme::CHIP;
    SDL_Color text   = on ? theme::BLACK          : theme::TEXT_2;
    fillRoundedRect(r, pill, 8, bg);
    drawRoundedRect(r, pill, 8, border);
    drawText(r, pill.x + padX, y, label, 1, text);
}

// Render a slim horizontal slider showing `value/100`, plus the numeric.
void drawSlider(SDL_Renderer* r, int xRight, int y, int value) {
    std::string num = std::to_string(value);
    int nw = 0, nh = 0;
    getTextSize(num, 1, &nw, &nh);
    const int barW = 100;
    const int barH = 6;
    const int gap  = 8;
    SDL_Rect track{xRight - nw - gap - barW, y + (nh - barH) / 2, barW, barH};
    fillRoundedRect(r, track, 3, theme::PANEL.a8(200));
    SDL_Rect fill{track.x, track.y, (track.w * clamp(value, 0, 100)) / 100, track.h};
    fillRoundedRect(r, fill, 3, theme::ACCENT);
    drawText(r, xRight - nw, y, num, 1, theme::TEXT_ON);
}

// Render the value side of a CYCLE row with ◄ ► chevrons when focused.
void drawCycleValue(SDL_Renderer* r, int xRight, int y, const std::string& value, bool focused) {
    int vw = 0, vh = 0;
    getTextSize(value, 1, &vw, &vh);
    const int chevW = 10;
    SDL_Color col = focused ? SDL_Color(theme::ACCENT) : SDL_Color(theme::TEXT_ON);
    if (focused) {
        drawText(r, xRight - vw - chevW * 2 - 4, y, "<", 1, theme::TEXT_2);
        drawText(r, xRight + 2,                   y, ">", 1, theme::TEXT_2);
    }
    drawText(r, xRight - vw - chevW,             y, value, 1, col);
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

    // Auto-sized card from the row table so adding rows doesn't push
    // content past the hint bar.  Headers are tighter than data rows.
    int contentH = kTopH + kBotH;
    for (int i = 0; i < kRowCount; ++i) {
        contentH += (kRows[i].kind == KIND_HEADER) ? kHeaderH : kRowH;
    }
    const int cw = std::min(width  - 24, 480);
    const int ch = std::min(height - 16, contentH);
    const int cx = (width  - cw) / 2;
    const int cy = (height - ch) / 2;

    SDL_Rect card{cx, cy, cw, ch};
    fillRoundedRect(renderer, card, theme::RADIUS_CARD, theme::SURFACE);
    drawRoundedRect(renderer, card, theme::RADIUS_CARD, theme::ACCENT.a8(220));

    // Header strip
    int x = cx + 18;
    int y = cy + 12;
    drawTextShadow(renderer, x, y, "SETTINGS", 2, theme::ACCENT);
    {
        const char* sub = "UP/DN pick   LF/RT adjust   B close";
        int sw = 0, sh = 0;
        getTextSize(sub, 1, &sw, &sh);
        drawText(renderer, cx + cw - 18 - sw, y + 4, sub, 1, theme::TEXT_2);
    }
    // Thin divider under the header
    SDL_Rect divider{cx + 12, cy + kTopH - 4, cw - 24, 1};
    SDL_SetRenderDrawColor(renderer, theme::HAIRLINE.r, theme::HAIRLINE.g,
                           theme::HAIRLINE.b, 200);
    SDL_RenderFillRect(renderer, &divider);

    // Make sure the cursor is on a focusable row (in case row table changed
    // from a previous run, or someone set it to a header index).
    int& cursor = app->state_.settingsModalIndex;
    if (!isFocusable(cursor)) cursor = firstFocusableFrom(cursor, +1);
    cursor = clamp(cursor, 0, kRowCount - 1);

    // Render rows
    y = cy + kTopH;
    const int valX = cx + cw - 18;   // right edge for value rendering
    for (int i = 0; i < kRowCount; ++i) {
        const Row& row = kRows[i];
        if (row.kind == KIND_HEADER) {
            drawText(renderer, x, y + 4, row.label, 1, theme::ACCENT);
            y += kHeaderH;
            continue;
        }

        const bool focused = (i == cursor);
        if (focused) {
            SDL_Rect highlight{cx + 8, y - 2, cw - 16, kRowH - 2};
            fillRoundedRect(renderer, highlight, 6, theme::PANEL.a8(180));
        }
        SDL_Color labelCol = focused ? SDL_Color(theme::TEXT_ON) : SDL_Color(theme::TEXT_2);
        drawText(renderer, x + 4, y + 4, row.label, 1, labelCol);

        const int valY = y + 4;
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
            drawTogglePill(renderer, valX, valY, v);
        } else if (row.kind == KIND_SLIDER) {
            drawSlider(renderer, valX, valY, app->state_.volume);
        } else if (row.kind == KIND_ACTION) {
            const char* hint = focused ? "A to confirm  >" : ">";
            int hw = 0, hh = 0;
            getTextSize(hint, 1, &hw, &hh);
            drawText(renderer, valX - hw, valY, hint, 1,
                     focused ? SDL_Color(theme::ACCENT) : SDL_Color(theme::TEXT_2));
        }
        y += kRowH;
    }

    // Footer hint bar
    std::vector<HintItem> hints = {
        {"UP/DN", theme::TEXT_ON, "PICK"},
        {"LF/RT", theme::BLUE,    "ADJUST"},
        {"A",     theme::ACCENT,  "TOGGLE"},
        {"B",     theme::YELLOW,  "CLOSE"},
    };
    drawHintButtons(renderer, hints, cy + ch - 26, 20, 1, 2 * cx + cw,
                    theme::PANEL.a8(200), theme::CHIP.a8(180), theme::TEXT_ON);

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
                if      (std::strcmp(label, "Hover previews")    == 0) app->state_.hoverPreviewsEnabled = !app->state_.hoverPreviewsEnabled;
                else if (std::strcmp(label, "Autoplay next")     == 0) app->state_.autoplayNextEnabled  = !app->state_.autoplayNextEnabled;
                else if (std::strcmp(label, "Background daemon") == 0) app->state_.backgroundDaemonEnabled = !app->state_.backgroundDaemonEnabled;
                else if (std::strcmp(label, "Debug overlay")     == 0) app->state_.showDebugOverlay     = !app->state_.showDebugOverlay;
                break;
            case KIND_SLIDER:
                app->state_.volume = clamp(app->state_.volume + delta * 5, 0, 100);
                break;
            case KIND_ACTION:
                // "Reset to defaults" — restore every field to its factory
                // value.  We use a fresh Settings{} and feed it through the
                // shared apply path so any future setting that gets a
                // default gets picked up here automatically.
                if (std::strcmp(label, "Reset to defaults") == 0 && delta > 0) {
                    Settings defaults;     // struct-init = defaults
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
        case SDLK_a:      applyStep(+1); return true;   // A toggles/advances/confirms
        case SDLK_ESCAPE:
        case SDLK_b:      return false;                 // B closes (caller saves)
        default:          return true;
    }
}

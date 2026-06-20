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

// Stable row ordering.  Add new rows here and to render() / handleKey().
enum Row : int {
    ROW_QUALITY = 0,
    ROW_HOME_FEED,
    ROW_VOLUME,
    ROW_DEBUG_OVERLAY,
    ROW_COUNT
};

int clampIndex(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

int qualityStepIndex(int height) {
    int best = 0;
    int bestDist = 9999;
    for (int i = 0; i < (int)(sizeof(kQualitySteps)/sizeof(kQualitySteps[0])); ++i) {
        int d = std::abs(kQualitySteps[i] - height);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

} // namespace

void SettingsModal::apply(App* app, const Settings& s) {
    app->state_.maxQualityHeight = s.maxQualityHeight;
    app->state_.homeFeedKind     = s.homeFeedKind;
    app->state_.volume           = s.volume;
    app->state_.showDebugOverlay = s.showDebugOverlay;
}

void SettingsModal::render(App* app, SDL_Renderer* renderer, int width, int height) {
    // Dim background.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect scrim{0, 0, width, height};
    fillRoundedRect(renderer, scrim, 0, theme::BLACK.a8(200));

    const int cw = std::min(width - 40, 460);
    const int ch = std::min(height - 40, 260);
    const int cx = (width - cw) / 2;
    const int cy = (height - ch) / 2;
    SDL_Rect card{cx, cy, cw, ch};
    fillRoundedRect(renderer, card, theme::RADIUS_CARD, theme::SURFACE);
    drawRoundedRect(renderer, card, theme::RADIUS_CARD, theme::ACCENT.a8(220));

    int x = cx + 20;
    int y = cy + 16;

    drawTextShadow(renderer, x, y, "SETTINGS", 3, theme::ACCENT);
    y += 36;

    const int idx = clampIndex(app->state_.settingsModalIndex, 0, ROW_COUNT - 1);

    auto drawRow = [&](int rowIdx, const char* label, const std::string& value) {
        SDL_Color labelCol = (rowIdx == idx) ? SDL_Color(theme::ACCENT) : SDL_Color(theme::TEXT_ON);
        SDL_Color valueCol = (rowIdx == idx) ? SDL_Color(theme::YELLOW) : SDL_Color(theme::TEXT_2);
        if (rowIdx == idx) {
            SDL_Rect highlight{cx + 8, y - 4, cw - 16, 24};
            fillRoundedRect(renderer, highlight, 6, theme::PANEL.a8(180));
        }
        drawText(renderer, x, y, label, 1, labelCol);
        // Right-align value.
        int vw = 0, vh = 0;
        getTextSize(value, 1, &vw, &vh);
        drawText(renderer, cx + cw - 20 - vw, y, value, 1, valueCol);
        y += 28;
    };

    drawRow(ROW_QUALITY,       "Max quality",     std::to_string(app->state_.maxQualityHeight) + "p");
    drawRow(ROW_HOME_FEED,     "Home tab",        app->state_.homeFeedKind == "subscriptions"
                                                        ? "Subscriptions"
                                                        : "Trending");
    drawRow(ROW_VOLUME,        "Default volume",  std::to_string(app->state_.volume));
    drawRow(ROW_DEBUG_OVERLAY, "Debug overlay",   app->state_.showDebugOverlay ? "ON" : "OFF");

    std::vector<HintItem> hints = {
        {"UP/DN", theme::TEXT_ON, "PICK"},
        {"LF/RT", theme::BLUE,    "ADJUST"},
        {"B",     theme::YELLOW,  "CLOSE"},
    };
    drawHintButtons(renderer, hints, cy + ch - 26, 20, 1, 2 * cx + cw,
                    theme::PANEL.a8(200), theme::CHIP.a8(180), theme::TEXT_ON);

    app->uiDirty_ = true;
}

bool SettingsModal::handleKey(App* app, SDL_Keycode key) {
    int& idx = app->state_.settingsModalIndex;
    idx = clampIndex(idx, 0, ROW_COUNT - 1);

    auto step = [&](int delta) {
        switch (idx) {
            case ROW_QUALITY: {
                int qi = qualityStepIndex(app->state_.maxQualityHeight) + delta;
                qi = clampIndex(qi, 0, (int)(sizeof(kQualitySteps)/sizeof(kQualitySteps[0])) - 1);
                app->state_.maxQualityHeight = kQualitySteps[qi];
                break;
            }
            case ROW_HOME_FEED:
                app->state_.homeFeedKind =
                    (app->state_.homeFeedKind == "trending") ? "subscriptions" : "trending";
                break;
            case ROW_VOLUME: {
                int v = app->state_.volume + delta * 5;
                app->state_.volume = clampIndex(v, 0, 100);
                break;
            }
            case ROW_DEBUG_OVERLAY:
                app->state_.showDebugOverlay = !app->state_.showDebugOverlay;
                break;
            default: break;
        }
        app->uiDirty_ = true;
    };

    switch (key) {
        case SDLK_UP:        idx = (idx + ROW_COUNT - 1) % ROW_COUNT; app->uiDirty_ = true; return true;
        case SDLK_DOWN:      idx = (idx + 1) % ROW_COUNT;             app->uiDirty_ = true; return true;
        case SDLK_LEFT:      step(-1); return true;
        case SDLK_RIGHT:     step(+1); return true;
        case SDLK_RETURN:
        case SDLK_a:         step(+1); return true;   // A toggles/advances
        case SDLK_ESCAPE:
        case SDLK_b:         return false;            // B closes (caller saves)
        default:             return true;
    }
}

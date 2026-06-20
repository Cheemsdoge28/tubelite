#include "settings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "json.hpp"

namespace settings {

static std::string resolvePath() {
    // Prefer the well-known install location (writable on ArkOS) so the
    // file survives across builds and SD remounts.  Fall back to cwd
    // for dev builds where the install dir doesn't exist.
    namespace fs = std::filesystem;
    if (fs::exists("/roms/tools/tubelite")) {
        return "/roms/tools/tubelite/settings.json";
    }
    return "settings.json";
}

std::string path() { return resolvePath(); }

bool load(Settings& out) {
    std::ifstream f(resolvePath());
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) return false;
        if (j.contains("maxQualityHeight") && j["maxQualityHeight"].is_number_integer())
            out.maxQualityHeight = j["maxQualityHeight"].get<int>();
        if (j.contains("homeFeedKind") && j["homeFeedKind"].is_string())
            out.homeFeedKind = j["homeFeedKind"].get<std::string>();
        if (j.contains("volume") && j["volume"].is_number_integer())
            out.volume = j["volume"].get<int>();
        if (j.contains("showDebugOverlay") && j["showDebugOverlay"].is_boolean())
            out.showDebugOverlay = j["showDebugOverlay"].get<bool>();
        // Legacy snake_case key kept readable so older on-disk files still load.
        if (j.contains("backgroundDaemonEnabled") && j["backgroundDaemonEnabled"].is_boolean())
            out.backgroundDaemonEnabled = j["backgroundDaemonEnabled"].get<bool>();
        else if (j.contains("background_daemon_enabled") && j["background_daemon_enabled"].is_boolean())
            out.backgroundDaemonEnabled = j["background_daemon_enabled"].get<bool>();
        if (j.contains("hoverPreviewsEnabled") && j["hoverPreviewsEnabled"].is_boolean())
            out.hoverPreviewsEnabled = j["hoverPreviewsEnabled"].get<bool>();
        if (j.contains("autoplayNextEnabled") && j["autoplayNextEnabled"].is_boolean())
            out.autoplayNextEnabled = j["autoplayNextEnabled"].get<bool>();
        return true;
    } catch (...) {
        return false;
    }
}

bool save(const Settings& s) {
    nlohmann::json j;
    j["maxQualityHeight"]        = s.maxQualityHeight;
    j["homeFeedKind"]            = s.homeFeedKind;
    j["volume"]                  = s.volume;
    j["showDebugOverlay"]        = s.showDebugOverlay;
    j["backgroundDaemonEnabled"] = s.backgroundDaemonEnabled;
    j["hoverPreviewsEnabled"]    = s.hoverPreviewsEnabled;
    j["autoplayNextEnabled"]     = s.autoplayNextEnabled;
    // Also write the legacy snake_case key so older code paths still read
    // a sane value before they migrate.
    j["background_daemon_enabled"] = s.backgroundDaemonEnabled;
    std::ofstream f(resolvePath());
    if (!f) {
        std::cerr << "settings: failed to open " << resolvePath() << " for write\n";
        return false;
    }
    f << j.dump(2);
    return f.good();
}

} // namespace settings

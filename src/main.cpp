#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <limits>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include "mpv_player.hpp"
#include "youtube_api.hpp"

#include <fstream>
#include <mutex>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#include <libloaderapi.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#endif

namespace {

struct LaunchOptions {
    std::string initialUrl{"https://www.google.com"};
    std::filesystem::path executablePath;
};

// Simple thread-safe logger that writes to a file and stderr.
static std::ofstream g_logFile;
static std::mutex g_logMutex;

static std::string nowString() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tt = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static void initLogging() {
    const char* env = std::getenv("FIRE4ARKOS_LOG");
    std::string path = env ? env : "/tmp/fire4arkos.log";
    // On Windows, default to current folder if /tmp likely doesn't exist
#ifdef _WIN32
    if (path.rfind("/tmp/", 0) == 0) path = std::string(".") + "/fire4arkos.log";
#endif
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logFile.open(path, std::ios::app);
    if (g_logFile) {
        g_logFile << "[I] " << nowString() << " - Logger started\n";
    }
}

static void logMessage(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::string line = std::string("[") + level + "] " + nowString() + " - " + msg;
    if (g_logFile) g_logFile << line << '\n';
    // also mirror to stderr for real-time visibility
    std::cerr << line << '\n';
}

static void logInfo(const std::string& msg) { logMessage("I", msg); }
static void logWarn(const std::string& msg) { logMessage("W", msg); }
static void logError(const std::string& msg) { logMessage("E", msg); }

static bool envFlagEnabled(const char* name, bool defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }

    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return !(text == "0" || text == "false" || text == "no" || text == "off" || text.empty());
}

static int envInt(const char* name, int defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return defaultValue;
    }
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return defaultValue;
    }
    if (parsed < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
    if (parsed > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(parsed);
}

static std::filesystem::path executableDirectory(const std::filesystem::path& argv0) {
#ifdef _WIN32
    std::vector<char> buffer(MAX_PATH);
    DWORD len = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size()) {
        return std::filesystem::path(std::string(buffer.data(), len)).parent_path();
    }
#endif

    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(argv0, ec);
    if (!ec && resolved.has_parent_path()) {
        return resolved.parent_path();
    }

    if (argv0.has_parent_path()) {
        return std::filesystem::absolute(argv0, ec).parent_path();
    }

    return std::filesystem::current_path(ec);
}

static std::string percentEncode(const std::string& input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(input.size() * 3);

    for (unsigned char ch : input) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }

        encoded.push_back('%');
        encoded.push_back(hex[(ch >> 4) & 0x0F]);
        encoded.push_back(hex[ch & 0x0F]);
    }

    return encoded;
}



struct TubeState {
    enum class Screen {
        Home,
        Search,
        Playback
    };
    
    enum class InputMode {
        None,
        SearchText
    };

    enum class KeyboardMode {
        Lowercase,
        Uppercase,
        Symbols
    };

    Screen currentScreen{Screen::Home};
    InputMode inputMode{InputMode::None};
    KeyboardMode keyboardMode{KeyboardMode::Lowercase};
    
    std::string textBuffer;
    int textCursor{0};
    int scrollOffset{0};
    bool running{true};
    int keyboardSelectedIndex{0};
    bool replaceBufferOnNextInput{false};
    bool showUi{true};
    float cursorX{320.0f};
    float cursorY{240.0f};
    float leftStickX{0.0f};
    float leftStickY{0.0f};
    float rightStickX{0.0f};
    float rightStickY{0.0f};
    float leftTrigger{0.0f};
    float rightTrigger{0.0f};
    bool dpadUpPressed{false};
    bool dpadDownPressed{false};
    bool dpadLeftPressed{false};
    bool dpadRightPressed{false};
    bool l3Pressed{false};
};


    enum class KeyboardMode {
        Lowercase,
        Uppercase,
        Symbols
    };

    std::string currentUrl{"https://www.google.com"};
    std::vector<std::string> history;
    std::vector<std::string> forwardStack;
    std::string urlBuffer{currentUrl};
    std::string textBuffer;
    int scrollOffset{0};
    bool requestReload{false};
    bool running{true};
    InputMode inputMode{InputMode::None};
    KeyboardMode keyboardMode{KeyboardMode::Lowercase};
    int keyboardSelectedIndex{0};
    int textCursor{0};
    bool replaceBufferOnNextInput{false};
    bool pageTextSelectionArmed{false};
    bool showUi{true};
    float cursorX{320.0f};
    float cursorY{240.0f};
    float lastSentCursorX{320.0f};
    float lastSentCursorY{240.0f};
    float leftStickX{0.0f};
    float leftStickY{0.0f};
    float rightStickX{0.0f};
    float rightStickY{0.0f};
    float leftStickHoldSeconds{0.0f};
    std::chrono::steady_clock::time_point leftStickHoldStart{};
    std::chrono::steady_clock::time_point leftStickLastUpdate{};
    float leftTrigger{0.0f};
    float rightTrigger{0.0f};
    bool dpadUpPressed{false};
    bool dpadDownPressed{false};
    bool dpadLeftPressed{false};
    bool dpadRightPressed{false};
    bool l3Pressed{false};  // L3 (left stick click) for drag selection
    std::chrono::steady_clock::time_point clickSuppressUntil{};  // Suppress mousemove IPC until this time
};

class App final {
public:
    explicit App(LaunchOptions options)
        : backend_(executableDirectory(options.executablePath)) {
        maxPerformance_ = envFlagEnabled("FIRE4ARKOS_MAX_PERF", false);
        forceVsync_ = envFlagEnabled("FIRE4ARKOS_FORCE_VSYNC", false);
        noSleep_ = envFlagEnabled("FIRE4ARKOS_NO_SLEEP", false);
        frameSkip_ = std::max(1, envInt("FIRE4ARKOS_FRAME_SKIP", 2));
        volumeStepPercent_ = std::clamp(envInt("FIRE4ARKOS_VOLUME_STEP", 5), 1, 20);
        state_.currentUrl = options.initialUrl;
        state_.urlBuffer = options.initialUrl;
    }

    
    bool initialize() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
            std::cerr << "SDL_Init failed: " << SDL_GetError() << '
';
            return false;
        }
        SDL_GameControllerEventState(SDL_ENABLE);
        if (!createWindow()) return false;
        openController();
        if (!mpv_player_.initialize(window_, renderer_)) {
            std::cerr << "MPV init failed
";
            return false;
        }
        SDL_StartTextInput();
        updateTitle();
        return true;
    }

    
    void run() {
        while (state_.running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handleEvent(event);
            }
            updateKeyboardCursorBlinkState();
            mpv_player_.update();
            updateSticks(); // mostly for virtual keyboard repeats
            renderFrame();
            SDL_Delay(16); // roughly 60fps
        }
    }

    
    void shutdown() {
        SDL_StopTextInput();
        closeController();
        destroyUiTextures();
        mpv_player_.shutdown();
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

private:
    bool createWindow() {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");

        const bool useVsync = forceVsync_ || !maxPerformance_;
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, useVsync ? "1" : "0");

        window_ = SDL_CreateWindow(
            "R36S Browser",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            640,
            480,
            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);

        if (window_ == nullptr) {
            std::string err = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
            std::cerr << err << '\n';
            logError(err);
            return false;
        }

        Uint32 rendererFlags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE;
        if (useVsync) {
            rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
        }

        renderer_ = SDL_CreateRenderer(window_, -1, rendererFlags);
        if (renderer_ == nullptr) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }

        if (renderer_ == nullptr) {
            std::string err = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
            std::cerr << err << '\n';
            logError(err);
            return false;
        }

        logRendererInfo();

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        SDL_ShowCursor(SDL_DISABLE);
        backend_.resize(width, height);
        return true;
    }

    void logRendererInfo() {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer_, &info) != 0) {
            logWarn(std::string("SDL_GetRendererInfo failed: ") + SDL_GetError());
            return;
        }

        std::ostringstream ss;
        ss << "Renderer: " << info.name;
        if (info.flags & SDL_RENDERER_ACCELERATED)  ss << " [accelerated]";
        if (info.flags & SDL_RENDERER_SOFTWARE)     ss << " [software]";
        if (info.flags & SDL_RENDERER_PRESENTVSYNC) ss << " [vsync]";
        if (info.flags & SDL_RENDERER_TARGETTEXTURE) ss << " [target-texture]";
        ss << " max=" << info.max_texture_width << "x" << info.max_texture_height;
        logInfo(ss.str());
        std::cout << ss.str() << '\n';

        // Log and select preferred pixel format for framebuffer texture
        // Priority: ARGB8888 (matches Xvfb BGRX on little-endian) > XRGB8888 > first available
        preferredTextureFormat_ = SDL_PIXELFORMAT_UNKNOWN;
        std::ostringstream fmtss;
        fmtss << "Texture formats:";
        for (Uint32 i = 0; i < info.num_texture_formats; ++i) {
            Uint32 fmt = info.texture_formats[i];
            fmtss << ' ' << SDL_GetPixelFormatName(fmt);
            if (preferredTextureFormat_ == SDL_PIXELFORMAT_UNKNOWN) {
                preferredTextureFormat_ = fmt;
            }
            if (fmt == SDL_PIXELFORMAT_ARGB8888) {
                preferredTextureFormat_ = fmt;
            }
        }
        logInfo(fmtss.str());
        std::cout << fmtss.str() << '\n';

        std::ostringstream selss;
        selss << "Selected texture format: " << SDL_GetPixelFormatName(preferredTextureFormat_);
        logInfo(selss.str());
        std::cout << selss.str() << '\n';
    }

    void handleEvent(const SDL_Event& event) {
        switch (event.type) {
        case SDL_QUIT:
            state_.running = false;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            openController();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            closeController();
            openController();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), true);
            break;
        case SDL_CONTROLLERBUTTONUP:
            handleControllerButton(static_cast<SDL_GameControllerButton>(event.cbutton.button), false);
            break;
        case SDL_JOYHATMOTION:
            if (controller_ == nullptr) {
                handleJoyHat(event.jhat.value);
            }
            break;
        case SDL_JOYBUTTONDOWN:
            // Process raw joystick buttons if no controller is open,
            // OR if it's one of our special hardware buttons (Fn=16, Select=12, Start=13, L3=14, R3=15)
            // that might not be mapped in the standard GameController profile.
            if (controller_ == nullptr ||
                event.jbutton.button == 16 || event.jbutton.button == 12 || event.jbutton.button == 13 ||
                event.jbutton.button == 14 || event.jbutton.button == 15) {
                handleJoyButton(event.jbutton.button, event.jbutton.which, true);
            }
            break;
        case SDL_JOYBUTTONUP:
            if (controller_ == nullptr ||
                event.jbutton.button == 16 || event.jbutton.button == 12 || event.jbutton.button == 13 ||
                event.jbutton.button == 14 || event.jbutton.button == 15) {
                handleJoyButton(event.jbutton.button, event.jbutton.which, false);
            }
            break;
        case SDL_CONTROLLERAXISMOTION:
            handleControllerAxis(event.caxis);
            break;
        case SDL_JOYAXISMOTION:
            if (controller_ == nullptr) {
                handleJoyAxis(event.jaxis);
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                backend_.resize(event.window.data1, event.window.data2);
                uiDirty_ = true;
            }
            break;
        case SDL_TEXTINPUT:
            handleTextInput(event.text.text);
            break;
        case SDL_KEYDOWN:
            handleKey(event.key.keysym.sym);
            break;
        default:
            break;
        }
    }

    
    void handleKey(SDL_Keycode key) {
        if (hasActiveKeyboard()) {
            handleKeyboardOverlayKey(key);
            return;
        }
        switch (key) {
        case SDLK_q:
        case SDLK_ESCAPE:
            state_.running = false;
            break;
        case SDLK_y:
            openKeyboard(TubeState::InputMode::SearchText);
            break;
        case SDLK_UP:
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) {
                selected_result_idx_--;
            }
            break;
        case SDLK_DOWN:
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < search_results_.size() - 1) {
                selected_result_idx_++;
            }
            break;
        case SDLK_RETURN:
            if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) {
                playVideo(search_results_[selected_result_idx_]);
            }
            break;
        }
    }

    void handleKeyboardOverlayKey(SDL_Keycode key) {
        switch (key) {
        case SDLK_UP:
            moveKeyboardSelection(0, -1);
            break;
        case SDLK_DOWN:
            moveKeyboardSelection(0, 1);
            break;
        case SDLK_LEFT:
            moveKeyboardSelection(-1, 0);
            break;
        case SDLK_RIGHT:
            moveKeyboardSelection(1, 0);
            break;
        case SDLK_RETURN:
            activateSelectedKey();
            break;
        case SDLK_ESCAPE:
            closeKeyboard(false);
            break;
        case SDLK_BACKSPACE:
            eraseActiveBufferChar();
            updateTitle();
            break;
        case SDLK_SPACE:
            insertActiveText(" ");
            updateTitle();
            break;
        case SDLK_TAB:
            if (state_.inputMode == TubeState::InputMode::SearchText) {
                applyBufferedPageText();
                backend_.pressKey("Tab");
            }
            break;
        default:
            break;
        }
    }

    
    void handleControllerButton(SDL_GameControllerButton button, bool down) {
        if (button == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_BACK)) {
            state_.running = false;
            return;
        }
        if (!down) return;
        
        if (hasActiveKeyboard()) {
            if (button == SDL_CONTROLLER_BUTTON_A) { closeKeyboard(false); return; }
            if (button == SDL_CONTROLLER_BUTTON_X) { eraseActiveBufferChar(); return; }
            if (button == SDL_CONTROLLER_BUTTON_Y) { insertActiveText(" "); return; }
            if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) { toggleKeyboardMode(); return; }
            if (button == SDL_CONTROLLER_BUTTON_START) { activateKeyboardGo(); return; }
            return;
        }

        if (button == SDL_CONTROLLER_BUTTON_Y) {
            openKeyboard(TubeState::InputMode::SearchText);
            return;
        }
        
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (state_.currentScreen == TubeState::Screen::Search && !search_results_.empty()) {
                playVideo(search_results_[selected_result_idx_]);
            } else if (state_.currentScreen == TubeState::Screen::Playback) {
                if (mpv_player_.isPlaying()) mpv_player_.pause(); else mpv_player_.resume();
            }
            return;
        }
        
        if (button == SDL_CONTROLLER_BUTTON_B) {
            if (state_.currentScreen == TubeState::Screen::Playback) {
                mpv_player_.stop();
                state_.currentScreen = TubeState::Screen::Home;
            } else if (state_.currentScreen == TubeState::Screen::Search) {
                state_.currentScreen = TubeState::Screen::Home;
            }
            return;
        }

        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) selected_result_idx_--;
        } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < search_results_.size() - 1) selected_result_idx_++;
        }
    }

    void handleControllerAxis(const SDL_ControllerAxisEvent& caxis) {
        float normalized = (float)caxis.value / 32767.0f;
        if (std::abs(caxis.value) < 8000) normalized = 0.0f;
        
        switch (caxis.axis) {
            case SDL_CONTROLLER_AXIS_LEFTX:  state_.leftStickX = normalized; break;
            case SDL_CONTROLLER_AXIS_LEFTY:  state_.leftStickY = normalized; break;
            case SDL_CONTROLLER_AXIS_RIGHTX: state_.rightStickX = normalized; break;
            case SDL_CONTROLLER_AXIS_RIGHTY: state_.rightStickY = normalized; break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                state_.leftTrigger = normalized;
                if (!hasActiveKeyboard() && normalized > 0.5f) {
                    static auto lastZoomOut = std::chrono::steady_clock::now();
                    if (std::chrono::steady_clock::now() - lastZoomOut > std::chrono::milliseconds(500)) {
                        backend_.sendCommand("zoom:out");
                        lastZoomOut = std::chrono::steady_clock::now();
                    }
                }
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                state_.rightTrigger = normalized;
                if (!hasActiveKeyboard() && normalized > 0.5f) {
                    static auto lastZoomIn = std::chrono::steady_clock::now();
                    if (std::chrono::steady_clock::now() - lastZoomIn > std::chrono::milliseconds(500)) {
                        backend_.sendCommand("zoom:in");
                        lastZoomIn = std::chrono::steady_clock::now();
                    }
                }
                break;
            default: break;
        }
    }

    void handleJoyHat(Uint8 value) {
        if (value & SDL_HAT_UP) {
            if (hasActiveKeyboard()) {
                moveKeyboardSelection(0, -1);
                return;
            }
            backend_.scrollBy(-5);
            state_.scrollOffset -= 5;
        }
        if (value & SDL_HAT_DOWN) {
            if (hasActiveKeyboard()) {
                moveKeyboardSelection(0, 1);
                return;
            }
            backend_.scrollBy(5);
            state_.scrollOffset += 5;
        }
        if (value & SDL_HAT_LEFT) {
            if (hasActiveKeyboard()) {
                moveKeyboardSelection(-1, 0);
                return;
            }
            backend_.scrollBy(-1);
        }
        if (value & SDL_HAT_RIGHT) {
            if (hasActiveKeyboard()) {
                moveKeyboardSelection(1, 0);
                return;
            }
            backend_.scrollBy(1);
        }
    }

    void handleJoyButton(Uint8 button, SDL_JoystickID instanceId, bool down) {
        static bool debugInput = (std::getenv("FIRE4ARKOS_DEBUG_INPUT") != nullptr);
        if (debugInput && down) {
            std::ostringstream ss;
            ss << "[DEBUG-INPUT] Raw Button Down: " << (int)button << " (instance " << instanceId << ")";
            logInfo(ss.str());
        }

        switch (button) {
        case 0: // South face button (B) -> Trigger SDL A action
            handleControllerButton(SDL_CONTROLLER_BUTTON_A, down);
            break;
        case 1: // East face button (A) -> Select key in keyboard, L3-style click otherwise
            if (hasActiveKeyboard()) {
                handleControllerButton(SDL_CONTROLLER_BUTTON_B, down);
            } else {
                handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, down);
            }
            break;
        case 2: // X (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_X, down);
            break;
        case 3: // Y (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_Y, down);
            break;
        case 4: // L1 (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, down);
            break;
        case 5: // R1 (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, down);
            break;
        case 6: // L2 (R36S)
            if (down) {
                if (hasActiveKeyboard()) {
                    moveActiveCursor(-1);
                    updateTitle();
                } else {
                    backend_.sendCommand("zoom:out");
                }
            }
            break;
        case 7: // R2 (R36S)
            if (down) {
                if (hasActiveKeyboard()) {
                    moveActiveCursor(1);
                    updateTitle();
                } else {
                    backend_.sendCommand("zoom:in");
                }
            }
            break;
        case 8: // D-Pad Up (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_UP, down);
            break;
        case 9: // D-Pad Down (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, down);
            break;
        case 10: // D-Pad Left (R36S)
            if (!fnPressed_) {
                handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, down);
            }
            break;
        case 11: // D-Pad Right (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, down);
            break;
        case 12: // Select (R36S)
        case 13: // Start (R36S)
            // Check if both are pressed for exit
            if (down) {
                SDL_Joystick* joy = SDL_JoystickFromInstanceID(instanceId);
                if (joy && SDL_JoystickGetButton(joy, 12) && SDL_JoystickGetButton(joy, 13)) {
                    state_.running = false;
                    break;
                }
            }
            if (button == 12) {
                handleControllerButton(SDL_CONTROLLER_BUTTON_BACK, down);
            } else {
                handleControllerButton(SDL_CONTROLLER_BUTTON_START, down);
            }
            break;
        case 14: // L3 (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, down);
            break;
        case 15: // R3 (R36S)
            handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, down);
            break;
        case 16: // FN button (R36S) — hold for D-pad volume control
            fnPressed_ = down;
            if (down) {
                volumeOverlayTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
                uiDirty_ = true;
            }
            break;
        default:
            break;
        }
    }

    void handleJoyAxis(const SDL_JoyAxisEvent& jaxis) {
        float normalized = (float)jaxis.value / 32767.0f;
        if (std::abs(jaxis.value) < 10000) normalized = 0.0f;
        
        if (jaxis.axis == 0) state_.leftStickX = normalized;
        else if (jaxis.axis == 1) state_.leftStickY = normalized;
        else if (jaxis.axis == 2) state_.rightStickX = normalized;
        else if (jaxis.axis == 3) state_.rightStickY = normalized;
    }

    bool updateSticks() {
        if (hasActiveKeyboard()) {
            bool keyboardMoved = updateKeyboardSelectionFromStick();
            keyboardMoved = updateKeyboardSelectionFromDpad() || keyboardMoved;
            keyboardMoved = updateKeyboardCursorFromTriggers() || keyboardMoved;
            return keyboardMoved;
        }

        // Smooth cursor movement with quadratic acceleration
        bool moved = false;
        const auto now = std::chrono::steady_clock::now();
        bool suppressed = now < state_.clickSuppressUntil;
        bool stickActive = (state_.leftStickX != 0.0f || state_.leftStickY != 0.0f);

        if (suppressed || !stickActive) {
            state_.leftStickHoldSeconds = 0.0f;
            state_.leftStickHoldStart = {};
            state_.leftStickLastUpdate = {};
        }

        if (!suppressed && stickActive) {
            if (state_.leftStickHoldStart == std::chrono::steady_clock::time_point{}) {
                state_.leftStickHoldStart = now;
                state_.leftStickLastUpdate = now;
                state_.leftStickHoldSeconds = 0.0f;
            } else {
                const auto delta = std::chrono::duration<float>(now - state_.leftStickLastUpdate).count();
                state_.leftStickHoldSeconds += std::max(0.0f, delta);
                state_.leftStickLastUpdate = now;
            }

            const float rampSeconds = 1.2f;
            float rampT = std::min(state_.leftStickHoldSeconds / rampSeconds, 1.0f);
            rampT = rampT * rampT; // Ease-in for precision, faster after holding.

            float baseSpeed = 3.5f;
            float maxSpeed = 8.0f;
            if (framebuffer_.width > 0 && framebuffer_.width <= 320) {
                baseSpeed = 2.5f; // Slower for low-res logical space
                maxSpeed = 6.0f;
            }

            float speed = baseSpeed + (maxSpeed - baseSpeed) * rampT;

            // Quadratic acceleration: input^2 * speed for fine control
            float velX = (state_.leftStickX * std::abs(state_.leftStickX)) * speed;
            float velY = (state_.leftStickY * std::abs(state_.leftStickY)) * speed;

            state_.cursorX += velX;
            state_.cursorY += velY;
            
            int w, h;
            SDL_GetWindowSize(window_, &w, &h);
            if (state_.cursorX < 0) state_.cursorX = 0;
            if (state_.cursorX > w - 1) state_.cursorX = w - 1;
            if (state_.cursorY < 0) state_.cursorY = 0;
            if (state_.cursorY > h - 1) state_.cursorY = h - 1;
            {
                // Only send mousemove IPC if not in the post-click suppression window.
                // This prevents the stick from racing a click command and moving the
                // cursor to a stale position.
                bool suppressed = std::chrono::steady_clock::now() < state_.clickSuppressUntil;
                
                // During L3 drag, always send mousemove (drag needs continuous position updates)
                if (state_.l3Pressed || !suppressed) {
                    static auto lastMove = std::chrono::steady_clock::now();
                    auto moveInterval = state_.l3Pressed ? std::chrono::milliseconds(16) : std::chrono::milliseconds(30);
                    if (std::chrono::steady_clock::now() - lastMove > moveInterval) {
                        backend_.mouseMoveAt((int)state_.cursorX, (int)state_.cursorY);
                        lastMove = std::chrono::steady_clock::now();
                    }
                }
            }

            moved = true;
        }

        if (state_.rightStickY != 0.0f) {
            static auto lastScroll = std::chrono::steady_clock::now();
            static auto lastVolume = std::chrono::steady_clock::now();
            
            if (fnPressed_) {
                // Adjust volume when FN is held: Up on stick (Y < 0) increases, Down (Y > 0) decreases
                if (std::chrono::steady_clock::now() - lastVolume > std::chrono::milliseconds(150)) {
                    int delta = state_.rightStickY < 0 ? volumeStepPercent_ : -volumeStepPercent_;
                    adjustSystemVolume(delta);
                    volumeOverlayTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
                    uiDirty_ = true;
                    lastVolume = std::chrono::steady_clock::now();
                    moved = true;
                }
            } else {
                if (std::chrono::steady_clock::now() - lastScroll > std::chrono::milliseconds(100)) {
                    int scrollAmt = state_.rightStickY > 0 ? 3 : -3;
                    backend_.scrollBy(scrollAmt);
                    lastScroll = std::chrono::steady_clock::now();
                    moved = true;
                }
            }
        }

        return moved;
    }

    int scaleInputX(int windowX, int windowWidth) const {
        if (windowWidth <= 0) return windowX;
        const int targetWidth = std::max(1, framebuffer_.width > 0 ? framebuffer_.width : 640);
        return static_cast<int>(std::clamp<long long>(
            static_cast<long long>(windowX) * targetWidth / windowWidth,
            0,
            static_cast<long long>(targetWidth - 1)));
    }

    int scaleInputY(int windowY, int windowHeight) const {
        if (windowHeight <= 0) return windowY;
        const int targetHeight = std::max(1, framebuffer_.height > 0 ? framebuffer_.height : 480);
        return static_cast<int>(std::clamp<long long>(
            static_cast<long long>(windowY) * targetHeight / windowHeight,
            0,
            static_cast<long long>(targetHeight - 1)));
    }

    bool hasActiveKeyboard() const {
        return state_.inputMode != TubeState::InputMode::None;
    }

    std::string& activeBuffer() {
        return state_.inputMode == TubeState::InputMode::SearchText ? state_.urlBuffer : state_.textBuffer;
    }

    const std::string& activeBuffer() const {
        return state_.inputMode == TubeState::InputMode::SearchText ? state_.urlBuffer : state_.textBuffer;
    }

    void openKeyboard(TubeState::InputMode mode) {
        state_.inputMode = mode;
        if (mode == TubeState::InputMode::SearchText) {
            state_.urlBuffer = state_.currentUrl;
            state_.replaceBufferOnNextInput = true;
            state_.pageTextSelectionArmed = false;
        } else if (mode == TubeState::InputMode::SearchText) {
            state_.textBuffer.clear();
            state_.textCursor = 0;
            state_.replaceBufferOnNextInput = false;
            state_.pageTextSelectionArmed = true;
            backend_.pressKey("ctrl+a");
        }
        state_.keyboardSelectedIndex = 0;
        state_.textCursor = static_cast<int>(activeBuffer().size());
        resetKeyboardInputRepeat();
        updateTitle();
    }

    void closeKeyboard(bool keepBuffer) {
        if (!keepBuffer) {
            if (state_.inputMode == TubeState::InputMode::SearchText) {
                state_.urlBuffer = state_.currentUrl;
            } else if (state_.inputMode == TubeState::InputMode::SearchText) {
                state_.textBuffer.clear();
                state_.textCursor = 0;
            }
        }
        state_.inputMode = TubeState::InputMode::None;
        state_.replaceBufferOnNextInput = false;
        state_.pageTextSelectionArmed = false;
        state_.leftTrigger = 0.0f;
        state_.rightTrigger = 0.0f;
        resetKeyboardInputRepeat();
        updateTitle();
    }

    void navigateBack() {
        backend_.goBack();
        if (state_.history.size() > 1) {
            state_.forwardStack.push_back(state_.currentUrl);
            state_.currentUrl = state_.history[state_.history.size() - 2];
            state_.history.pop_back();
            state_.urlBuffer = state_.currentUrl;
            state_.requestReload = true;
        }
    }

    void handleTextInput(const char* text) {
        if (!hasActiveKeyboard()) {
            return;
        }
        insertActiveText(transformTypedText(text));
        updateTitle();
    }

    void eraseActiveBufferChar() {
        auto& buffer = activeBuffer();
        if (state_.replaceBufferOnNextInput) {
            buffer.clear();
            state_.textCursor = 0;
            state_.replaceBufferOnNextInput = false;
            return;
        }
        if (state_.inputMode == TubeState::InputMode::SearchText && buffer.empty()) {
            backend_.pressKey("BackSpace");
            state_.pageTextSelectionArmed = false;
            return;
        }
        if (state_.textCursor > 0 && !buffer.empty()) {
            buffer.erase(static_cast<size_t>(state_.textCursor - 1), 1);
            --state_.textCursor;
        }
    }

    struct KeyboardKey {
        const char* label;
        const char* value;
        int widthUnits;
    };

    struct KeyboardKeyGeometry {
        SDL_Rect bounds{};
        std::string label;
        std::string value;
        int row{0};
        int col{0};
        int index{0};
    };

    struct KeyboardOverlayLayout {
        SDL_Rect panel{};
        int statusBarHeight{48};
        std::vector<KeyboardKeyGeometry> keys;
    };

    static constexpr int kKeyboardGridColumns = 12;
    static constexpr bool kKeyboardWrapAround = true;

    const std::vector<std::vector<KeyboardKey>>& keyboardLayout() const {
        static const std::vector<std::vector<KeyboardKey>> lowerLayout = {
            {{"1", "1", 1}, {"2", "2", 1}, {"3", "3", 1}, {"4", "4", 1}, {"5", "5", 1}, {"6", "6", 1},
             {"7", "7", 1}, {"8", "8", 1}, {"9", "9", 1}, {"0", "0", 1}, {"-", "-", 1}, {".", ".", 1}},
            {{"q", "q", 1}, {"w", "w", 1}, {"e", "e", 1}, {"r", "r", 1}, {"t", "t", 1}, {"y", "y", 1},
             {"u", "u", 1}, {"i", "i", 1}, {"o", "o", 1}, {"p", "p", 1}, {"/", "/", 1}, {":", ":", 1}},
            {{"a", "a", 1}, {"s", "s", 1}, {"d", "d", 1}, {"f", "f", 1}, {"g", "g", 1}, {"h", "h", 1},
             {"j", "j", 1}, {"k", "k", 1}, {"l", "l", 1}, {"_", "_", 1}, {"@", "@", 1}, {"?", "?", 1}},
            {{"z", "z", 1}, {"x", "x", 1}, {"c", "c", 1}, {"v", "v", 1}, {"b", "b", 1}, {"n", "n", 1},
             {"m", "m", 1}, {"&", "&", 1}, {"=", "=", 1}, {"+", "+", 1}, {"#", "#", 1}, {"%", "%", 1}},
            {{"MODE", "__MODE__", 2}, {"SPACE", " ", 3}, {"BKSP", "__BACKSPACE__", 2}, {"<", "__LEFT__", 1},
             {">", "__RIGHT__", 1}, {"GO", "__ENTER__", 1}, {"ESC", "__CANCEL__", 2}}
        };
        static const std::vector<std::vector<KeyboardKey>> upperLayout = {
            {{"1", "1", 1}, {"2", "2", 1}, {"3", "3", 1}, {"4", "4", 1}, {"5", "5", 1}, {"6", "6", 1},
             {"7", "7", 1}, {"8", "8", 1}, {"9", "9", 1}, {"0", "0", 1}, {"-", "-", 1}, {".", ".", 1}},
            {{"Q", "Q", 1}, {"W", "W", 1}, {"E", "E", 1}, {"R", "R", 1}, {"T", "T", 1}, {"Y", "Y", 1},
             {"U", "U", 1}, {"I", "I", 1}, {"O", "O", 1}, {"P", "P", 1}, {"/", "/", 1}, {":", ":", 1}},
            {{"A", "A", 1}, {"S", "S", 1}, {"D", "D", 1}, {"F", "F", 1}, {"G", "G", 1}, {"H", "H", 1},
             {"J", "J", 1}, {"K", "K", 1}, {"L", "L", 1}, {"_", "_", 1}, {"@", "@", 1}, {"?", "?", 1}},
            {{"Z", "Z", 1}, {"X", "X", 1}, {"C", "C", 1}, {"V", "V", 1}, {"B", "B", 1}, {"N", "N", 1},
             {"M", "M", 1}, {"&", "&", 1}, {"=", "=", 1}, {"+", "+", 1}, {"#", "#", 1}, {"%", "%", 1}},
            {{"MODE", "__MODE__", 2}, {"SPACE", " ", 3}, {"BKSP", "__BACKSPACE__", 2}, {"<", "__LEFT__", 1},
             {">", "__RIGHT__", 1}, {"GO", "__ENTER__", 1}, {"ESC", "__CANCEL__", 2}}
        };
        static const std::vector<std::vector<KeyboardKey>> symbolsLayout = {
            {{"1", "1", 1}, {"2", "2", 1}, {"3", "3", 1}, {"4", "4", 1}, {"5", "5", 1}, {"6", "6", 1},
             {"7", "7", 1}, {"8", "8", 1}, {"9", "9", 1}, {"0", "0", 1}, {"[", "[", 1}, {"]", "]", 1}},
            {{"!", "!", 1}, {"@", "@", 1}, {"#", "#", 1}, {"$", "$", 1}, {"%", "%", 1}, {"^", "^", 1},
             {"&", "&", 1}, {"*", "*", 1}, {"(", "(", 1}, {")", ")", 1}, {"{", "{", 1}, {"}", "}", 1}},
            {{"<", "<", 1}, {">", ">", 1}, {"/", "/", 1}, {"\\", "\\", 1}, {"|", "|", 1}, {"_", "_", 1},
             {"+", "+", 1}, {"=", "=", 1}, {"~", "~", 1}, {";", ";", 1}, {":", ":", 1}, {"`", "`", 1}},
            {{"'", "'", 1}, {"\"", "\"", 1}, {",", ",", 1}, {".", ".", 1}, {"?", "?", 1}, {"-", "-", 1},
             {"@", "@", 1}, {"#", "#", 1}, {"%", "%", 1}, {"&", "&", 1}, {"*", "*", 1}, {"=", "=", 1}},
            {{"MODE", "__MODE__", 2}, {"SPACE", " ", 3}, {"BKSP", "__BACKSPACE__", 2}, {"<", "__LEFT__", 1},
             {">", "__RIGHT__", 1}, {"GO", "__ENTER__", 1}, {"ESC", "__CANCEL__", 2}}
        };

        switch (state_.keyboardMode) {
        case TubeState::KeyboardMode::Uppercase:
            return upperLayout;
        case TubeState::KeyboardMode::Symbols:
            return symbolsLayout;
        case TubeState::KeyboardMode::Lowercase:
        default:
            return lowerLayout;
        }
    }

    KeyboardOverlayLayout buildKeyboardOverlayLayout(int width, int height) const {
        KeyboardOverlayLayout layoutInfo;
        const int outerMargin = 14;
        const int panelPadding = (width < 480) ? 8 : 10;
        const int rowHeight = (height < 360) ? 26 : 30;
        const int rowGap = (height < 360) ? 6 : 7;
        const int topContent = panelPadding + 18 + 4 + 16 + 8;
        const int hintHeight = 0;
        const int gridHeight = static_cast<int>(keyboardLayout().size()) * rowHeight +
                               (static_cast<int>(keyboardLayout().size()) - 1) * rowGap;
        const int panelHeight = topContent + gridHeight + hintHeight + panelPadding * 2;
        const int panelY = std::max(outerMargin, height - layoutInfo.statusBarHeight - panelHeight - 12);
        layoutInfo.panel = {outerMargin, panelY, std::max(120, width - outerMargin * 2), panelHeight};

        const int cellWidth = std::max(20, (layoutInfo.panel.w - panelPadding * 2) / kKeyboardGridColumns);
        int keyIndex = 0;
        int y = layoutInfo.panel.y + topContent;
        const auto& rows = keyboardLayout();
        for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const auto& row = rows[rowIndex];
            int unitsUsed = 0;
            for (const auto& key : row) {
                unitsUsed += std::max(1, key.widthUnits);
            }
            int x = layoutInfo.panel.x + panelPadding + std::max(0, ((layoutInfo.panel.w - panelPadding * 2) - unitsUsed * cellWidth) / 2);
            for (size_t colIndex = 0; colIndex < row.size(); ++colIndex) {
                const auto& key = row[colIndex];
                KeyboardKeyGeometry geometry;
                geometry.bounds = {x, y, std::max(18, std::max(1, key.widthUnits) * cellWidth - 6), rowHeight};
                geometry.label = key.label;
                geometry.value = key.value;
                geometry.row = static_cast<int>(rowIndex);
                geometry.col = static_cast<int>(colIndex);
                geometry.index = keyIndex++;
                layoutInfo.keys.push_back(std::move(geometry));
                x += std::max(1, key.widthUnits) * cellWidth;
            }
            y += rowHeight + rowGap;
        }

        return layoutInfo;
    }

    const KeyboardKeyGeometry* selectedKeyboardKey(const KeyboardOverlayLayout& layoutInfo) const {
        if (layoutInfo.keys.empty()) {
            return nullptr;
        }
        const int index = std::clamp(state_.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
        return &layoutInfo.keys[static_cast<size_t>(index)];
    }

    std::string transformTypedText(const char* text) const {
        std::string transformed{text};
        if (state_.keyboardMode == TubeState::KeyboardMode::Uppercase) {
            for (char& ch : transformed) {
                if (std::isalpha(static_cast<unsigned char>(ch))) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
            }
        }
        return transformed;
    }

    void insertActiveText(const std::string& text) {
        auto& buffer = activeBuffer();
        if (state_.replaceBufferOnNextInput) {
            buffer.clear();
            state_.textCursor = 0;
            state_.replaceBufferOnNextInput = false;
        }
        state_.textCursor = std::clamp(state_.textCursor, 0, static_cast<int>(buffer.size()));
        buffer.insert(static_cast<size_t>(state_.textCursor), text);
        state_.textCursor += static_cast<int>(text.size());
    }

    void moveActiveCursor(int delta) {
        if (state_.replaceBufferOnNextInput) {
            state_.textCursor = (delta < 0) ? 0 : static_cast<int>(activeBuffer().size());
            state_.replaceBufferOnNextInput = false;
            return;
        }
        if (state_.inputMode == TubeState::InputMode::SearchText && activeBuffer().empty()) {
            backend_.pressKey(delta < 0 ? "Left" : "Right");
            state_.pageTextSelectionArmed = false;
            return;
        }
        state_.textCursor = std::clamp(state_.textCursor + delta, 0, static_cast<int>(activeBuffer().size()));
    }

    void ensureKeyboardSelectionValid() {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        if (layoutInfo.keys.empty()) {
            state_.keyboardSelectedIndex = 0;
            return;
        }
        state_.keyboardSelectedIndex = std::clamp(state_.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
    }

    void toggleKeyboardMode() {
        switch (state_.keyboardMode) {
        case TubeState::KeyboardMode::Lowercase:
            state_.keyboardMode = TubeState::KeyboardMode::Uppercase;
            break;
        case TubeState::KeyboardMode::Uppercase:
            state_.keyboardMode = TubeState::KeyboardMode::Symbols;
            break;
        case TubeState::KeyboardMode::Symbols:
        default:
            state_.keyboardMode = TubeState::KeyboardMode::Lowercase;
            break;
        }
        ensureKeyboardSelectionValid();
        uiDirty_ = true;
    }

    void resetKeyboardInputRepeat() {
        keyboardNavDirectionX_ = 0;
        keyboardNavDirectionY_ = 0;
        keyboardNavStartedAt_ = std::chrono::steady_clock::time_point{};
        keyboardNavNextAt_ = std::chrono::steady_clock::time_point{};
        keyboardDpadDirectionX_ = 0;
        keyboardDpadDirectionY_ = 0;
        keyboardDpadStartedAt_ = std::chrono::steady_clock::time_point{};
        keyboardDpadNextAt_ = std::chrono::steady_clock::time_point{};
        triggerCursorDirection_ = 0;
        triggerCursorStartedAt_ = std::chrono::steady_clock::time_point{};
        triggerCursorNextAt_ = std::chrono::steady_clock::time_point{};
    }

    static int keyCenterX(const KeyboardKeyGeometry& key) {
        return key.bounds.x + key.bounds.w / 2;
    }

    static int keyCenterY(const KeyboardKeyGeometry& key) {
        return key.bounds.y + key.bounds.h / 2;
    }

    int repeatIntervalMs(std::chrono::steady_clock::time_point startedAt, int baseMs, int minMs) const {
        if (startedAt == std::chrono::steady_clock::time_point{}) {
            return baseMs;
        }
        const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        return std::max(minMs, baseMs - static_cast<int>(heldMs / 250) * 18);
    }

    int resolveWrappedKeyboardSelection(const KeyboardOverlayLayout& layoutInfo, int directionX, int directionY) const {
        if (!kKeyboardWrapAround || layoutInfo.keys.empty()) {
            return -1;
        }
        const KeyboardKeyGeometry* current = selectedKeyboardKey(layoutInfo);
        if (current == nullptr) {
            return 0;
        }
        int bestIndex = -1;
        int bestScore = std::numeric_limits<int>::max();
        for (const auto& candidate : layoutInfo.keys) {
            if (candidate.index == current->index) {
                continue;
            }
            int score = std::numeric_limits<int>::max();
            if (directionX > 0) {
                score = candidate.bounds.x * 10 + std::abs(keyCenterY(candidate) - keyCenterY(*current));
            } else if (directionX < 0) {
                score = (layoutInfo.panel.x + layoutInfo.panel.w - candidate.bounds.x) * 10 +
                        std::abs(keyCenterY(candidate) - keyCenterY(*current));
            } else if (directionY > 0) {
                score = candidate.bounds.y * 10 + std::abs(keyCenterX(candidate) - keyCenterX(*current));
            } else if (directionY < 0) {
                score = (layoutInfo.panel.y + layoutInfo.panel.h - candidate.bounds.y) * 10 +
                        std::abs(keyCenterX(candidate) - keyCenterX(*current));
            }
            if (score < bestScore) {
                bestScore = score;
                bestIndex = candidate.index;
            }
        }
        return bestIndex;
    }

    void moveKeyboardSelection(int directionX, int directionY) {
        if (!hasActiveKeyboard()) {
            return;
        }
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        if (layoutInfo.keys.empty()) {
            return;
        }
        state_.keyboardSelectedIndex = std::clamp(state_.keyboardSelectedIndex, 0, static_cast<int>(layoutInfo.keys.size()) - 1);
        const KeyboardKeyGeometry& current = layoutInfo.keys[static_cast<size_t>(state_.keyboardSelectedIndex)];

        int bestIndex = -1;
        float bestScore = std::numeric_limits<float>::max();
        for (const auto& candidate : layoutInfo.keys) {
            if (candidate.index == current.index) {
                continue;
            }
            const float dx = static_cast<float>(keyCenterX(candidate) - keyCenterX(current));
            const float dy = static_cast<float>(keyCenterY(candidate) - keyCenterY(current));
            if (directionX > 0 && dx <= 0.0f) continue;
            if (directionX < 0 && dx >= 0.0f) continue;
            if (directionY > 0 && dy <= 0.0f) continue;
            if (directionY < 0 && dy >= 0.0f) continue;

            const float primary = (directionX != 0) ? std::abs(dx) : std::abs(dy);
            const float secondary = (directionX != 0) ? std::abs(dy) : std::abs(dx);
            const float score = primary + secondary * 2.6f;
            if (score < bestScore) {
                bestScore = score;
                bestIndex = candidate.index;
            }
        }

        if (bestIndex < 0) {
            bestIndex = resolveWrappedKeyboardSelection(layoutInfo, directionX, directionY);
        }
        if (bestIndex >= 0) {
            state_.keyboardSelectedIndex = bestIndex;
        }
        uiDirty_ = true;
    }

    bool updateKeyboardSelectionFromStick() {
        const float absX = std::abs(state_.leftStickX);
        const float absY = std::abs(state_.leftStickY);
        const float threshold = 0.45f;
        int dirX = 0;
        int dirY = 0;
        if (absX >= threshold || absY >= threshold) {
            if (absX >= absY) {
                dirX = (state_.leftStickX > 0.0f) ? 1 : -1;
            } else {
                dirY = (state_.leftStickY > 0.0f) ? 1 : -1;
            }
        }

        if (dirX == 0 && dirY == 0) {
            keyboardNavDirectionX_ = 0;
            keyboardNavDirectionY_ = 0;
            keyboardNavStartedAt_ = std::chrono::steady_clock::time_point{};
            keyboardNavNextAt_ = std::chrono::steady_clock::time_point{};
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (dirX != keyboardNavDirectionX_ || dirY != keyboardNavDirectionY_) {
            keyboardNavDirectionX_ = dirX;
            keyboardNavDirectionY_ = dirY;
            keyboardNavStartedAt_ = now;
            keyboardNavNextAt_ = now + std::chrono::milliseconds(220);
            moveKeyboardSelection(dirX, dirY);
            return true;
        }

        if (now >= keyboardNavNextAt_) {
            moveKeyboardSelection(dirX, dirY);
            keyboardNavNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(keyboardNavStartedAt_, 135, 70));
            return true;
        }
        return false;
    }

    bool updateKeyboardSelectionFromDpad() {
        int dirX = 0;
        int dirY = 0;

        if (state_.dpadLeftPressed != state_.dpadRightPressed) {
            dirX = state_.dpadRightPressed ? 1 : -1;
        } else if (state_.dpadUpPressed != state_.dpadDownPressed) {
            dirY = state_.dpadDownPressed ? 1 : -1;
        }

        if (dirX == 0 && dirY == 0) {
            keyboardDpadDirectionX_ = 0;
            keyboardDpadDirectionY_ = 0;
            keyboardDpadStartedAt_ = std::chrono::steady_clock::time_point{};
            keyboardDpadNextAt_ = std::chrono::steady_clock::time_point{};
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (dirX != keyboardDpadDirectionX_ || dirY != keyboardDpadDirectionY_) {
            keyboardDpadDirectionX_ = dirX;
            keyboardDpadDirectionY_ = dirY;
            keyboardDpadStartedAt_ = now;
            keyboardDpadNextAt_ = now + std::chrono::milliseconds(220);
            moveKeyboardSelection(dirX, dirY);
            return true;
        }

        if (now >= keyboardDpadNextAt_) {
            moveKeyboardSelection(dirX, dirY);
            keyboardDpadNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(keyboardDpadStartedAt_, 135, 70));
            return true;
        }
        return false;
    }

    bool updateKeyboardCursorFromTriggers() {
        int direction = 0;
        if (state_.leftTrigger > 0.55f && state_.rightTrigger <= 0.55f) {
            direction = -1;
        } else if (state_.rightTrigger > 0.55f && state_.leftTrigger <= 0.55f) {
            direction = 1;
        }

        if (direction == 0) {
            triggerCursorDirection_ = 0;
            triggerCursorStartedAt_ = std::chrono::steady_clock::time_point{};
            triggerCursorNextAt_ = std::chrono::steady_clock::time_point{};
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (direction != triggerCursorDirection_) {
            triggerCursorDirection_ = direction;
            triggerCursorStartedAt_ = now;
            triggerCursorNextAt_ = now + std::chrono::milliseconds(220);
            moveActiveCursor(direction);
            updateTitle();
            return true;
        }

        if (now >= triggerCursorNextAt_) {
            moveActiveCursor(direction);
            updateTitle();
            triggerCursorNextAt_ = now + std::chrono::milliseconds(repeatIntervalMs(triggerCursorStartedAt_, 140, 90));
            return true;
        }
        return false;
    }

    void applyBufferedPageText() {
        if (!state_.textBuffer.empty()) {
            backend_.typeText(state_.textBuffer);
            state_.textBuffer.clear();
            state_.textCursor = 0;
            state_.pageTextSelectionArmed = false;
        } else if (state_.pageTextSelectionArmed) {
            backend_.pressKey("BackSpace");
            state_.pageTextSelectionArmed = false;
        }
        updateTitle();
    }

    void activateKeyboardGo() {
        if (!hasActiveKeyboard()) {
            return;
        }
        if (state_.inputMode == TubeState::InputMode::SearchText) {
            commitUrlEdit();
            return;
        }
        applyBufferedPageText();
        backend_.pressKey("Return");
        closeKeyboard(true);
    }

    void activateSelectedKey() {
        if (!hasActiveKeyboard()) {
            return;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        const KeyboardKeyGeometry* key = selectedKeyboardKey(layoutInfo);
        if (key == nullptr) {
            return;
        }
        const std::string value = key->value;

        if (value == "__BACKSPACE__") {
            eraseActiveBufferChar();
        } else if (value == "__MODE__") {
            toggleKeyboardMode();
            return;
        } else if (value == "__LEFT__") {
            moveActiveCursor(-1);
        } else if (value == "__RIGHT__") {
            moveActiveCursor(1);
        } else if (value == "__ENTER__") {
            activateKeyboardGo();
            return;
        } else if (value == "__CANCEL__") {
            closeKeyboard(false);
            return;
        } else {
            insertActiveText(value);
        }

        updateTitle();
    }

    void openController() {
        if (controller_ != nullptr) {
            return;
        }

        const int joystickCount = SDL_NumJoysticks();
        for (int index = 0; index < joystickCount; ++index) {
            if (SDL_IsGameController(index)) {
                controller_ = SDL_GameControllerOpen(index);
                if (controller_ != nullptr) {
                    std::ostringstream ss;
                    ss << "Opened SDL game controller: " << SDL_GameControllerName(controller_);
                    logInfo(ss.str());
                    return;
                }
            }
        }

        if (joystickCount > 0) {
            joystick_ = SDL_JoystickOpen(0);
            if (joystick_ != nullptr) {
                std::ostringstream ss;
                ss << "Opened SDL joystick fallback: " << SDL_JoystickName(joystick_);
                logInfo(ss.str());
            }
        }
    }

    void closeController() {
        if (controller_ != nullptr) {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
        if (joystick_ != nullptr) {
            SDL_JoystickClose(joystick_);
            joystick_ = nullptr;
        }
    }

    
    void doSearch(const std::string& query) {
        state_.currentScreen = TubeState::Screen::Search;
        search_results_.clear();
        selected_result_idx_ = 0;
        youtube_api_.search(query, [this](bool success, const std::vector<YouTubeVideo>& results) {
            if (success) {
                search_results_ = results;
            }
        });
    }

    void playVideo(const YouTubeVideo& video) {
        current_video_ = video;
        youtube_api_.getStreamUrl(video.id, [this](bool success, const std::string& url) {
            if (success) {
                state_.currentScreen = TubeState::Screen::Playback;
                mpv_player_.play(url);
            }
        });
    }
    
    void commitUrlEdit() {
        if (!state_.textBuffer.empty()) {
            doSearch(state_.textBuffer);
        }
        state_.inputMode = TubeState::InputMode::None;
    }

    void commitUrlEdit_old() {
        if (state_.urlBuffer.empty()) {
            state_.urlBuffer = state_.currentUrl;
            state_.inputMode = TubeState::InputMode::None;
            updateTitle();
            return;
        }

        state_.currentUrl = normalizeUrl(state_.urlBuffer);
        state_.history.push_back(state_.currentUrl);
        state_.forwardStack.clear();
        state_.requestReload = true;
        state_.inputMode = TubeState::InputMode::None;
        state_.textCursor = static_cast<int>(state_.urlBuffer.size());
        updateTitle();
    }

    static std::string normalizeUrl(std::string url) {
        auto hasScheme = url.find("://") != std::string::npos;
        if (!hasScheme) {
            url = "https://" + url;
        }
        return url;
    }

    void updateTitle() {
        std::string title;
        if (state_.inputMode == TubeState::InputMode::SearchText) {
            title = "URL: " + state_.urlBuffer;
        } else if (state_.inputMode == TubeState::InputMode::SearchText) {
            title = "Type: " + state_.textBuffer;
        } else {
            title = "Page: " + state_.currentUrl;
        }
        SDL_SetWindowTitle(window_, title.c_str());
        uiDirty_ = true;
    }

    std::string keyboardModeLabel() const {
        switch (state_.keyboardMode) {
        case TubeState::KeyboardMode::Uppercase:
            return "UPPER";
        case TubeState::KeyboardMode::Symbols:
            return "SYMBOLS";
        case TubeState::KeyboardMode::Lowercase:
        default:
            return "LOWER";
        }
    }

    std::string keyboardPreviewText() const {
        std::string preview = activeBuffer();
        const int cursor = state_.replaceBufferOnNextInput
            ? 0
            : std::clamp(state_.textCursor, 0, static_cast<int>(preview.size()));
        if (keyboardCursorVisible()) {
            preview.insert(static_cast<size_t>(cursor), "|");
        } else {
            preview.insert(static_cast<size_t>(cursor), " ");
        }
        constexpr int maxChars = 38;
        if (static_cast<int>(preview.size()) > maxChars) {
            int start = std::clamp(cursor - (maxChars / 2), 0, static_cast<int>(preview.size()) - maxChars);
            preview = preview.substr(static_cast<size_t>(start), maxChars);
        }
        return preview;
    }

    bool keyboardCursorVisible() const {
        using namespace std::chrono;
        const auto phase = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 400;
        return (phase % 2) == 0;
    }

    void updateKeyboardCursorBlinkState() {
        if (!hasActiveKeyboard()) {
            lastKeyboardCursorVisible_ = keyboardCursorVisible();
            return;
        }

        const bool visible = keyboardCursorVisible();
        if (visible != lastKeyboardCursorVisible_) {
            lastKeyboardCursorVisible_ = visible;
            uiDirty_ = true;
        }
    }

    static std::array<uint8_t, 7> glyphFor(char ch) {
        switch (static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ch)))) {
        case 'A': return {14, 17, 17, 31, 17, 17, 17};
        case 'B': return {30, 17, 17, 30, 17, 17, 30};
        case 'C': return {14, 17, 16, 16, 16, 17, 14};
        case 'D': return {30, 17, 17, 17, 17, 17, 30};
        case 'E': return {31, 16, 16, 30, 16, 16, 31};
        case 'F': return {31, 16, 16, 30, 16, 16, 16};
        case 'G': return {14, 17, 16, 23, 17, 17, 15};
        case 'H': return {17, 17, 17, 31, 17, 17, 17};
        case 'I': return {31, 4, 4, 4, 4, 4, 31};
        case 'J': return {7, 2, 2, 2, 18, 18, 12};
        case 'K': return {17, 18, 20, 24, 20, 18, 17};
        case 'L': return {16, 16, 16, 16, 16, 16, 31};
        case 'M': return {17, 27, 21, 17, 17, 17, 17};
        case 'N': return {17, 25, 21, 19, 17, 17, 17};
        case 'O': return {14, 17, 17, 17, 17, 17, 14};
        case 'P': return {30, 17, 17, 30, 16, 16, 16};
        case 'Q': return {14, 17, 17, 17, 21, 18, 13};
        case 'R': return {30, 17, 17, 30, 20, 18, 17};
        case 'S': return {15, 16, 16, 14, 1, 1, 30};
        case 'T': return {31, 4, 4, 4, 4, 4, 4};
        case 'U': return {17, 17, 17, 17, 17, 17, 14};
        case 'V': return {17, 17, 17, 17, 17, 10, 4};
        case 'W': return {17, 17, 17, 17, 21, 21, 10};
        case 'X': return {17, 17, 10, 4, 10, 17, 17};
        case 'Y': return {17, 17, 10, 4, 4, 4, 4};
        case 'Z': return {31, 1, 2, 4, 8, 16, 31};
        case '0': return {14, 17, 19, 21, 25, 17, 14};
        case '1': return {4, 12, 4, 4, 4, 4, 14};
        case '2': return {14, 17, 1, 2, 4, 8, 31};
        case '3': return {30, 1, 1, 6, 1, 1, 30};
        case '4': return {2, 6, 10, 18, 31, 2, 2};
        case '5': return {31, 16, 16, 30, 1, 1, 30};
        case '6': return {14, 16, 16, 30, 17, 17, 14};
        case '7': return {31, 1, 2, 4, 8, 8, 8};
        case '8': return {14, 17, 17, 14, 17, 17, 14};
        case '9': return {14, 17, 17, 15, 1, 1, 14};
        case '-': return {0, 0, 0, 31, 0, 0, 0};
        case '.': return {0, 0, 0, 0, 0, 6, 6};
        case '/': return {1, 2, 2, 4, 8, 8, 16};
        case ':': return {0, 6, 6, 0, 6, 6, 0};
        case '_': return {0, 0, 0, 0, 0, 0, 31};
        case '?': return {14, 17, 1, 2, 4, 0, 4};
        case '@': return {14, 17, 1, 13, 21, 21, 14};
        case '&': return {12, 18, 20, 8, 21, 18, 13};
        case '=': return {0, 31, 0, 31, 0, 0, 0};
        case '+': return {0, 4, 4, 31, 4, 4, 0};
        case '#': return {10, 10, 31, 10, 31, 10, 10};
        case '%': return {24, 25, 2, 4, 8, 19, 3};
        case '!': return {4, 4, 4, 4, 4, 0, 4};
        case '$': return {4, 15, 20, 14, 5, 30, 4};
        case '^': return {4, 10, 17, 0, 0, 0, 0};
        case '*': return {0, 17, 10, 31, 10, 17, 0};
        case '(': return {2, 4, 8, 8, 8, 4, 2};
        case ')': return {8, 4, 2, 2, 2, 4, 8};
        case '[': return {14, 8, 8, 8, 8, 8, 14};
        case ']': return {14, 2, 2, 2, 2, 2, 14};
        case '{': return {2, 4, 4, 8, 4, 4, 2};
        case '}': return {8, 4, 4, 2, 4, 4, 8};
        case '<': return {2, 4, 8, 16, 8, 4, 2};
        case '>': return {8, 4, 2, 1, 2, 4, 8};
        case ';': return {0, 4, 4, 0, 4, 4, 8};
        case '\'': return {4, 4, 2, 0, 0, 0, 0};
        case '"': return {10, 10, 4, 0, 0, 0, 0};
        case '\\': return {16, 8, 8, 4, 2, 2, 1};
        case '|': return {4, 4, 4, 4, 4, 4, 4};
        case '~': return {0, 0, 13, 18, 0, 0, 0};
        case '`': return {8, 4, 2, 0, 0, 0, 0};
        case ' ': return {0, 0, 0, 0, 0, 0, 0};
        default: return {0, 0, 0, 0, 0, 0, 0};
        }
    }

    void drawGlyph(int x, int y, char ch, int scale, SDL_Color color) {
        SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
        const auto glyph = glyphFor(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[static_cast<size_t>(row)] >> (4 - col)) & 1U) {
                    SDL_Rect pixel{x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer_, &pixel);
                }
            }
        }
    }

    void drawText(int x, int y, const std::string& text, int scale, SDL_Color color) {
        int cursor = x;
        for (char ch : text) {
            drawGlyph(cursor, y, ch, scale, color);
            cursor += scale * 6;
        }
    }

    void drawTextShadow(int x, int y, const std::string& text, int scale, SDL_Color color) {
        const int shadowOffset = std::max(1, scale / 2);
        const SDL_Color shadow{8, 10, 12, 200};
        drawText(x + shadowOffset, y + shadowOffset, text, scale, shadow);
        drawText(x, y, text, scale, color);
    }

    SDL_Texture* createTargetTexture(int width, int height) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height);
        if (texture == nullptr) {
            texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
        }
        if (texture != nullptr) {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        }
        return texture;
    }

    void destroyUiTextures() {
        if (keyboardOverlayTexture_ != nullptr) {
            SDL_DestroyTexture(keyboardOverlayTexture_);
            keyboardOverlayTexture_ = nullptr;
        }
        if (statusOverlayTexture_ != nullptr) {
            SDL_DestroyTexture(statusOverlayTexture_);
            statusOverlayTexture_ = nullptr;
        }
        if (loadingOverlayTexture_ != nullptr) {
            SDL_DestroyTexture(loadingOverlayTexture_);
            loadingOverlayTexture_ = nullptr;
        }
    }

    void renderKeyboardOverlay(int width, int height) {
        if (!hasActiveKeyboard()) {
            if (keyboardOverlayTexture_ != nullptr) {
                SDL_DestroyTexture(keyboardOverlayTexture_);
                keyboardOverlayTexture_ = nullptr;
                keyboardOverlayWidth_ = 0;
                keyboardOverlayHeight_ = 0;
            }
            return;
        }

        ensureKeyboardSelectionValid();
        const auto layoutInfo = buildKeyboardOverlayLayout(width, height);
        if (keyboardOverlayTexture_ == nullptr ||
            keyboardOverlayWidth_ != layoutInfo.panel.w ||
            keyboardOverlayHeight_ != layoutInfo.panel.h ||
            uiDirty_) {
            if (keyboardOverlayTexture_ != nullptr) {
                SDL_DestroyTexture(keyboardOverlayTexture_);
                keyboardOverlayTexture_ = nullptr;
            }

            keyboardOverlayWidth_ = layoutInfo.panel.w;
            keyboardOverlayHeight_ = layoutInfo.panel.h;
            keyboardOverlayTexture_ = createTargetTexture(layoutInfo.panel.w, layoutInfo.panel.h);
            if (keyboardOverlayTexture_ == nullptr) {
                return;
            }

            SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer_);
            SDL_SetRenderTarget(renderer_, keyboardOverlayTexture_);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, 18, 20, 24, 255);
            SDL_RenderClear(renderer_);

            SDL_Color textColor{226, 230, 236, 255};
            SDL_Color accent{110, 192, 255, 255};
            const std::string header =
                (state_.inputMode == TubeState::InputMode::SearchText ? "URL INPUT " : "TEXT INPUT ") +
                std::string("[") + keyboardModeLabel() + "]";
            drawTextShadow(12, 12, header, 2, accent);
            drawTextShadow(12, 34, keyboardPreviewText(), 2, textColor);

            SDL_SetRenderDrawColor(renderer_, 28, 32, 38, 255);
            SDL_RenderDrawLine(renderer_, 12, 60, layoutInfo.panel.w - 12, 60);

            for (const auto& key : layoutInfo.keys) {
                SDL_Rect keyRect{
                    key.bounds.x - layoutInfo.panel.x,
                    key.bounds.y - layoutInfo.panel.y,
                    key.bounds.w,
                    key.bounds.h
                };
                const bool selected = key.index == state_.keyboardSelectedIndex;
                SDL_SetRenderDrawColor(renderer_,
                                       selected ? 72 : 28,
                                       selected ? 138 : 32,
                                       selected ? 190 : 38,
                                       255);
                SDL_RenderFillRect(renderer_, &keyRect);
                SDL_SetRenderDrawColor(renderer_, selected ? 178 : 46, selected ? 216 : 52, selected ? 240 : 58, 255);
                SDL_RenderDrawRect(renderer_, &keyRect);
                drawTextShadow(keyRect.x + 8, keyRect.y + 8, key.label, 2, selected ? SDL_Color{12, 16, 22, 255} : textColor);
            }

            SDL_SetRenderTarget(renderer_, previousTarget);
            uiDirty_ = false;
        }

        SDL_Rect overlay = layoutInfo.panel;
        SDL_RenderCopy(renderer_, keyboardOverlayTexture_, nullptr, &overlay);
    }
    
    void renderFrame() {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);

        // UI rendering
        if (state_.showUi || state_.inputMode != TubeState::InputMode::None) {
            if (state_.currentScreen == TubeState::Screen::Home) {
                drawTextShadow(20, 20, "tubelite - Home", 3, {255, 255, 255, 255});
                drawText(20, 60, "Press Y to search", 2, {200, 200, 200, 255});
            } else if (state_.currentScreen == TubeState::Screen::Search) {
                drawTextShadow(20, 20, "Search Results", 3, {255, 255, 255, 255});
                int y = 60;
                int idx = 0;
                for (const auto& video : search_results_) {
                    SDL_Color color = (idx == selected_result_idx_) ? SDL_Color{255, 200, 100, 255} : SDL_Color{200, 200, 200, 255};
                    drawText(20, y, video.title.substr(0, 40), 2, color);
                    drawText(20, y + 16, video.author + " - " + video.duration_string + " - " + video.view_count_string, 1, {150, 150, 150, 255});
                    y += 40;
                    idx++;
                }
            } else if (state_.currentScreen == TubeState::Screen::Playback) {
                if (state_.showUi) {
                    drawTextShadow(20, 20, "Playing: " + current_video_.title, 2, {255, 255, 255, 255});
                }
            }
        }
        
        renderKeyboardOverlay(width, height);
        SDL_RenderPresent(renderer_);
    }

    void adjustSystemVolume(int deltaPercent) {
#ifdef _WIN32
        (void)deltaPercent;
#else
        if (deltaPercent == 0) return;
        const int step = std::clamp(std::abs(deltaPercent), 1, 20);
        const int card = std::max(0, envInt("ALSA_CARD", 0));
        const char sign = deltaPercent > 0 ? '+' : '-';

        static bool debugAudio = (std::getenv("FIRE4ARKOS_DEBUG_AUDIO") != nullptr && std::string(std::getenv("FIRE4ARKOS_DEBUG_AUDIO")) == "1");
        
        // Try common handheld control names in order of likelihood
        const char* controls[] = {"Playback", "Master", "Speaker", "PCM", "Headphone"};
        bool success = false;
        
        for (const char* ctrl : controls) {
            std::ostringstream amixerCmd;
            amixerCmd << "amixer -q -c " << card << " sset " << ctrl << " " << step << "%" << sign
                      << " unmute >/dev/null 2>&1";
            if (debugAudio) logInfo("Audio Command: " + amixerCmd.str());
            
            if (std::system(amixerCmd.str().c_str()) == 0) {
                std::ostringstream ss;
                ss << "Volume " << (deltaPercent > 0 ? "up" : "down") << " " << step << "% (ALSA card " << card << " control " << ctrl << ")";
                logInfo(ss.str());
                success = true;
                break;
            }
        }

        if (success) return;

        std::ostringstream pactlCmd;
        pactlCmd << "pactl set-sink-volume @DEFAULT_SINK@ " << sign << step << "% >/dev/null 2>&1";
        if (debugAudio) logInfo("Audio Command: " + pactlCmd.str());
        
        if (std::system(pactlCmd.str().c_str()) == 0) {
            std::ostringstream ss;
            ss << "Volume " << (deltaPercent > 0 ? "up" : "down") << " " << step << "% (Pulse/PipeWire)";
            logInfo(ss.str());
        } else {
            logError("Failed to adjust volume via amixer or pactl");
        }
#endif
    }

    void toggleSystemMute() {
#ifdef _WIN32
        return;
#else
        static bool debugAudio = (std::getenv("FIRE4ARKOS_DEBUG_AUDIO") != nullptr && std::string(std::getenv("FIRE4ARKOS_DEBUG_AUDIO")) == "1");
        const int card = std::max(0, envInt("ALSA_CARD", 0));
        
        std::ostringstream amixerCmd;
        amixerCmd << "amixer -q -c " << card << " sset Master toggle >/dev/null 2>&1";
        if (debugAudio) logInfo("Audio Command: " + amixerCmd.str());
        
        if (std::system(amixerCmd.str().c_str()) == 0) {
            logInfo("Toggled mute (ALSA)");
            return;
        }
        
        if (debugAudio) logInfo("Audio Command: pactl set-sink-mute @DEFAULT_SINK@ toggle");
        if (std::system("pactl set-sink-mute @DEFAULT_SINK@ toggle >/dev/null 2>&1") == 0) {
            logInfo("Toggled mute (Pulse/PipeWire)");
        } else {
            logError("Failed to toggle mute via amixer or pactl");
        }
#endif
    }

    TubeState state_;
    Framebuffer framebuffer_;
    SDL_Texture* framebufferTexture_{nullptr};
    int lastFramebufferWidth_{0};  // Track previous framebuffer size to detect when texture needs recreation
    int lastFramebufferHeight_{0};
    std::vector<uint8_t> lastFrameData_; // For visual frame skip detection
    bool frameChanged_{true};
    SDL_Texture* keyboardOverlayTexture_{nullptr};
    SDL_Texture* statusOverlayTexture_{nullptr};
    SDL_Texture* loadingOverlayTexture_{nullptr};
    int keyboardOverlayWidth_{0};
    int keyboardOverlayHeight_{0};
    int statusOverlayWidth_{0};
    int statusOverlayHeight_{0};
    int loadingOverlayWidth_{0};
    int loadingOverlayHeight_{0};
    int loadingOverlayCurrentSeconds_{-1};
    int loadingOverlayCachedSeconds_{-1};
    bool uiDirty_{true};
    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    SDL_GameController* controller_{nullptr};
    SDL_Joystick* joystick_{nullptr};
    Uint32 preferredTextureFormat_{SDL_PIXELFORMAT_ARGB8888};
    int framesReceived_{0};
    std::chrono::steady_clock::time_point startTime_{std::chrono::steady_clock::now()};
    FirefoxProcessBackend backend_;
    bool maxPerformance_{true};
    bool forceVsync_{false};
    bool noSleep_{false};
    int frameSkip_{1};
    int volumeStepPercent_{5};
    bool fnPressed_{false};  // Track if FN button is held
    std::chrono::steady_clock::time_point volumeOverlayTime_{std::chrono::steady_clock::now()};  // When to hide volume overlay
    
    MpvPlayer mpv_player_;
    YouTubeAPI youtube_api_;
    std::vector<YouTubeVideo> search_results_;
    int selected_result_idx_ = 0;
    YouTubeVideo current_video_;
    
    int keyboardNavDirectionX_{0};
    int keyboardNavDirectionY_{0};
    int keyboardDpadDirectionX_{0};
    int keyboardDpadDirectionY_{0};
    int triggerCursorDirection_{0};
    std::chrono::steady_clock::time_point keyboardNavStartedAt_{};
    std::chrono::steady_clock::time_point keyboardNavNextAt_{};
    std::chrono::steady_clock::time_point keyboardDpadStartedAt_{};
    std::chrono::steady_clock::time_point keyboardDpadNextAt_{};
    std::chrono::steady_clock::time_point triggerCursorStartedAt_{};
    std::chrono::steady_clock::time_point triggerCursorNextAt_{};
    bool lastKeyboardCursorVisible_{true};

};

} // namespace

int main(int argc, char** argv) {
    // Initialize logging system (can be overridden with FIRE4ARKOS_LOG)
    initLogging();
    std::cout << "Fire4ArkOS Browser v1.1\n";
    logInfo("Fire4ArkOS Browser v1.1 started");

    LaunchOptions options;
    if (argc > 1 && argv[1] != nullptr && std::strlen(argv[1]) > 0) {
        options.initialUrl = argv[1];
    }
    if (argc > 0 && argv[0] != nullptr) {
        options.executablePath = argv[0];
    }

    App app(options);
    if (!app.initialize()) {
        app.shutdown();
        return 1;
    }

    app.run();
    app.shutdown();
    return 0;
}

import sys

def patch_main():
    with open('src/main.cpp', 'r') as f:
        content = f.read()

    # 1. Add handleJoy* methods
    joy_methods = """
    void handleJoyHat(Uint8 value) {
        if (value & SDL_HAT_UP) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(0, -1); return; }
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ > 0) { selected_result_idx_--; uiDirty_ = true; }
        }
        if (value & SDL_HAT_DOWN) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(0, 1); return; }
            if (state_.currentScreen == TubeState::Screen::Search && selected_result_idx_ < (int)search_results_.size() - 1) { selected_result_idx_++; uiDirty_ = true; }
        }
        if (value & SDL_HAT_LEFT) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(-1, 0); return; }
        }
        if (value & SDL_HAT_RIGHT) {
            if (hasActiveKeyboard()) { moveKeyboardSelection(1, 0); return; }
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

    void handleJoyButton(Uint8 button, SDL_JoystickID instanceId, bool down) {
        switch (button) {
        case 0: // B (South)
            handleControllerButton(SDL_CONTROLLER_BUTTON_A, down); break;
        case 1: // A (East)
            if (hasActiveKeyboard()) handleControllerButton(SDL_CONTROLLER_BUTTON_B, down);
            break;
        case 2: // X
            handleControllerButton(SDL_CONTROLLER_BUTTON_X, down); break;
        case 3: // Y
            handleControllerButton(SDL_CONTROLLER_BUTTON_Y, down); break;
        case 4: // L1
            handleControllerButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, down); break;
        case 5: // R1
            handleControllerButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, down); break;
        case 8: // D-Pad Up
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_UP, down); break;
        case 9: // D-Pad Down
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, down); break;
        case 10: // D-Pad Left
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, down); break;
        case 11: // D-Pad Right
            handleControllerButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, down); break;
        case 12: // Select
        case 13: // Start
            if (down) {
                SDL_Joystick* joy = SDL_JoystickFromInstanceID(instanceId);
                if (joy && SDL_JoystickGetButton(joy, 12) && SDL_JoystickGetButton(joy, 13)) {
                    state_.running = false;
                    break;
                }
            }
            if (button == 12) handleControllerButton(SDL_CONTROLLER_BUTTON_BACK, down);
            else handleControllerButton(SDL_CONTROLLER_BUTTON_START, down);
            break;
        default: break;
        }
    }
"""
    if "void handleJoyButton" not in content:
        pos = content.find("void handleControllerAxis")
        content = content[:pos] + joy_methods + "\n    " + content[pos:]

    # 2. Add to handleEvent
    event_handling = """
        case SDL_JOYHATMOTION:
            handleJoyHat(event.jhat.value); break;
        case SDL_JOYAXISMOTION:
            handleJoyAxis(event.jaxis); break;
        case SDL_JOYBUTTONDOWN:
            handleJoyButton(event.jbutton.button, event.jbutton.which, true); break;
        case SDL_JOYBUTTONUP:
            handleJoyButton(event.jbutton.button, event.jbutton.which, false); break;
    """
    if "case SDL_JOYBUTTONDOWN" not in content:
        pos = content.find("case SDL_CONTROLLERAXISMOTION:")
        content = content[:pos] + event_handling + "\n        " + content[pos:]

    # 3. Add status overlay rendering and fix renderFrame
    status_overlay = """
    SDL_Texture* statusOverlayTexture_{nullptr};
    int statusOverlayWidth_{0};
    int statusOverlayHeight_{0};

    void renderStatusOverlay(int width, int height) {
        int statusBarHeight = 48;
        if (statusOverlayTexture_ == nullptr || statusOverlayWidth_ != width || statusOverlayHeight_ != statusBarHeight || uiDirty_) {
            if (statusOverlayTexture_) SDL_DestroyTexture(statusOverlayTexture_);
            statusOverlayWidth_ = width;
            statusOverlayHeight_ = statusBarHeight;
            statusOverlayTexture_ = createTargetTexture(width, statusBarHeight);
            
            if (statusOverlayTexture_) {
                SDL_Texture* prev = SDL_GetRenderTarget(renderer_);
                SDL_SetRenderTarget(renderer_, statusOverlayTexture_);
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer_, 12, 14, 18, 255);
                SDL_RenderClear(renderer_);
                
                SDL_Color textColor{150, 160, 170, 255};
                SDL_Color accent{255, 100, 100, 255};
                
                std::string shortcuts = "A/ENTER: Select | B/ESC: Back/Close | Y: Search | START+SELECT: Quit";
                if (hasActiveKeyboard()) {
                    shortcuts = "A: Type | B: Close KB | Y: Space | X: Backspace | L1: Mode | START: Go";
                }
                
                drawTextShadow(20, 16, shortcuts, 1, textColor);
                
                SDL_SetRenderTarget(renderer_, prev);
            }
        }
        
        if (statusOverlayTexture_) {
            SDL_Rect dst{0, height - statusBarHeight, width, statusBarHeight};
            SDL_RenderCopy(renderer_, statusOverlayTexture_, nullptr, &dst);
        }
    }
"""
    if "void renderStatusOverlay" not in content:
        pos = content.find("void renderKeyboardOverlay")
        content = content[:pos] + status_overlay + "\n    " + content[pos:]

    # 4. Modify renderFrame
    new_render_frame = """
    void renderFrame() {
        SDL_SetRenderDrawColor(renderer_, 20, 22, 26, 255); // Dark background
        SDL_RenderClear(renderer_);
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);

        if (state_.showUi || state_.inputMode != TubeState::InputMode::None) {
            if (state_.currentScreen == TubeState::Screen::Home) {
                // Draw cool header
                SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 255);
                SDL_Rect headerRect{0, 0, width, 60};
                SDL_RenderFillRect(renderer_, &headerRect);
                drawTextShadow(20, 20, "tubelite - YouTube Client", 3, {255, 80, 80, 255});
                
                drawText(40, 120, "Welcome to Tubelite!", 2, {220, 220, 220, 255});
                drawText(40, 160, "Press Y to search for a video.", 2, {150, 150, 150, 255});
                drawText(40, 200, "Press START + SELECT to exit.", 2, {150, 150, 150, 255});
            } else if (state_.currentScreen == TubeState::Screen::Search) {
                SDL_SetRenderDrawColor(renderer_, 30, 34, 40, 255);
                SDL_Rect headerRect{0, 0, width, 60};
                SDL_RenderFillRect(renderer_, &headerRect);
                drawTextShadow(20, 20, "Search Results", 3, {255, 80, 80, 255});
                
                int y = 70;
                int idx = 0;
                for (const auto& video : search_results_) {
                    if (y > height - 100) break; // Don't draw past screen
                    
                    bool selected = (idx == selected_result_idx_);
                    if (selected) {
                        SDL_SetRenderDrawColor(renderer_, 60, 68, 80, 255);
                        SDL_Rect bgRect{10, y - 5, width - 20, 48};
                        SDL_RenderFillRect(renderer_, &bgRect);
                        SDL_SetRenderDrawColor(renderer_, 100, 150, 255, 255);
                        SDL_RenderDrawRect(renderer_, &bgRect);
                    }
                    
                    SDL_Color titleColor = selected ? SDL_Color{255, 255, 255, 255} : SDL_Color{200, 200, 200, 255};
                    std::string title = video.title;
                    if (title.length() > 50) title = title.substr(0, 47) + "...";
                    drawText(20, y, title, 2, titleColor);
                    drawText(20, y + 20, video.author + " | " + video.duration_string, 1, {120, 130, 140, 255});
                    y += 50;
                    idx++;
                }
            } else if (state_.currentScreen == TubeState::Screen::Playback) {
                if (state_.showUi) {
                    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
                    SDL_Rect headerRect{0, 0, width, 50};
                    SDL_RenderFillRect(renderer_, &headerRect);
                    drawTextShadow(20, 16, "Playing: " + current_video_.title, 2, {255, 255, 255, 255});
                }
            }
        }
        
        if (state_.showUi || state_.inputMode != TubeState::InputMode::None) {
            renderStatusOverlay(width, height);
        }
        
        renderKeyboardOverlay(width, height);
        SDL_RenderPresent(renderer_);
    }
"""
    pos1 = content.find("void renderFrame() {")
    pos2 = content.find("void handleEvent(", pos1)
    if pos1 != -1 and pos2 != -1:
        content = content[:pos1] + new_render_frame + "\n    " + content[pos2:]

    # 5. Add destroy logic for statusOverlayTexture
    destroy_logic = """
        if (statusOverlayTexture_) {
            SDL_DestroyTexture(statusOverlayTexture_);
            statusOverlayTexture_ = nullptr;
        }
"""
    if "statusOverlayTexture_" not in content[content.find("void destroyUiTextures()"):content.find("}", content.find("void destroyUiTextures()"))]:
        pos = content.find("void destroyUiTextures() {") + 26
        content = content[:pos] + destroy_logic + content[pos:]

    with open('src/main.cpp', 'w') as f:
        f.write(content)

if __name__ == '__main__':
    patch_main()

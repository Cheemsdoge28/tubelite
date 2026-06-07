import sys

def fix_overlay():
    # Read the target renderKeyboardOverlay from fire4arkos
    with open('fire4arkos/src/main.cpp', 'r') as f:
        fire4arkos_content = f.read()
    
    start_str = "    void renderKeyboardOverlay(int width, int height) {"
    end_str = "    void renderFrame() {"
    
    start_idx = fire4arkos_content.find(start_str)
    end_idx = fire4arkos_content.find(end_str, start_idx)
    
    if start_idx == -1 or end_idx == -1:
        print("Could not find renderKeyboardOverlay in fire4arkos")
        return
        
    fire_keyboard_overlay = fire4arkos_content[start_idx:end_idx]

    # Now read tubelite main.cpp
    with open('src/main.cpp', 'r') as f:
        tubelite_content = f.read()
        
    # Replace renderStatusOverlay and renderKeyboardOverlay with just fire's renderKeyboardOverlay
    tubelite_start_status = tubelite_content.find("    SDL_Texture* statusOverlayTexture_{nullptr};")
    tubelite_end_keyboard = tubelite_content.find("    // Main frame rendering")
    
    if tubelite_start_status == -1 or tubelite_end_keyboard == -1:
        print("Could not find overlay section in tubelite main.cpp")
        return
        
    # Remove the rendering call in renderFrame
    tubelite_content = tubelite_content.replace("            renderStatusOverlay(width, height);\n", "")
    
    # Replace the blocks
    new_tubelite_content = tubelite_content[:tubelite_start_status] + fire_keyboard_overlay + "    // -----------------------------------------------------------------------\n" + tubelite_content[tubelite_end_keyboard:]
    
    with open('src/main.cpp', 'w') as f:
        f.write(new_tubelite_content)

if __name__ == '__main__':
    fix_overlay()

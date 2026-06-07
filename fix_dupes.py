import sys

def fix_duplicates():
    with open('src/main.cpp', 'r') as f:
        lines = f.readlines()

    # Find the old one-line declarations of keyCenterX and keyCenterY
    # and the old resolveWrappedKeyboardSelection block
    
    new_lines = []
    skip_resolve = False
    
    for i, line in enumerate(lines):
        # Remove the first occurrence of keyCenterX / keyCenterY which are single-line
        if "static int keyCenterX(const KeyboardKeyGeometry& key) { return key.bounds.x + key.bounds.w / 2; }" in line:
            continue
        if "static int keyCenterY(const KeyboardKeyGeometry& key) { return key.bounds.y + key.bounds.h / 2; }" in line:
            continue
            
        # Remove the unused variable 'accent'
        if "SDL_Color accent{255, 100, 100, 255};" in line:
            continue

        # Skip the old resolveWrappedKeyboardSelection
        if line.strip() == "int resolveWrappedKeyboardSelection(const KeyboardOverlayLayout& layoutInfo, int directionX, int directionY) const {":
            # Check if this is the first (compact) one by looking at the next line
            if i + 1 < len(lines) and "if (!kKeyboardWrapAround || layoutInfo.keys.empty()) return -1;" in lines[i+1]:
                skip_resolve = True
                continue
                
        if skip_resolve:
            if line.strip() == "}":
                skip_resolve = False
            continue
            
        new_lines.append(line)

    with open('src/main.cpp', 'w') as f:
        f.writelines(new_lines)

if __name__ == '__main__':
    fix_duplicates()

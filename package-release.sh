#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASE_ROOT="${1:-$SCRIPT_DIR/dist/release}"
APP_DIR="$RELEASE_ROOT/TubeLite"

RELEASE_VERSION="1.6.1"
RELEASE_TAG="Stable-2026-06-26"

copy_file() {
    local source_path="$1"
    local target_path="$2"
    mkdir -p "$(dirname "$target_path")"
    cp -f "$source_path" "$target_path"
    chmod +x "$target_path" 2>/dev/null || true
}

pick_binary() {
    for candidate in \
        "$SCRIPT_DIR/build/tubelite.arm64" \
        "$SCRIPT_DIR/build/tubelite" \
        "$SCRIPT_DIR/bin/tubelite.arm64" \
        "$SCRIPT_DIR/bin/tubelite" \
        "$SCRIPT_DIR/tubelite.arm64"; do
        if [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/bin"

BINARY_PATH="$(pick_binary || true)"

copy_file "$SCRIPT_DIR/Install-TubeLite.sh" "$APP_DIR/Install-TubeLite.sh"
copy_file "$SCRIPT_DIR/TubeLite.tbl" "$APP_DIR/TubeLite.tbl"
copy_file "$SCRIPT_DIR/Update-TubeLite.sh" "$APP_DIR/Update-TubeLite.sh"
copy_file "$SCRIPT_DIR/Update-TubeLite.tbl" "$APP_DIR/Update-TubeLite.tbl"
if [ -n "$BINARY_PATH" ]; then
    # Stage the app binary under build/ (not bin/) — that's where the launcher
    # and the on-device helper scripts expect it.  yt-dlp stays under bin/.
    copy_file "$BINARY_PATH" "$APP_DIR/build/tubelite.arm64"
else
    echo "WARNING: No pre-built binary found. Staging will proceed without a pre-built binary (the target device can compile natively)."
fi

if [ -f "$SCRIPT_DIR/bin/yt-dlp" ]; then
    copy_file "$SCRIPT_DIR/bin/yt-dlp" "$APP_DIR/bin/yt-dlp"
fi

if [ -d "$SCRIPT_DIR/vendor" ]; then
    mkdir -p "$APP_DIR/vendor"
    cp -r "$SCRIPT_DIR/vendor/"* "$APP_DIR/vendor/" 2>/dev/null || true
    chmod +x "$APP_DIR/vendor/"* 2>/dev/null || true
    # Prune redundant build/download artifacts: the runtimes ship as their
    # extracted binaries, so the original archives (e.g. vendor/deno.zip) are
    # dead weight that would otherwise inflate every release by tens of MB.
    find "$APP_DIR/vendor" -type f \
        \( -name '*.zip' -o -name '*.tar.gz' -o -name '*.tgz' -o -name '*.tar.xz' \) \
        -delete 2>/dev/null || true
    # The JS runtime used for DASH unlock is deno (the installer looks for
    # vendor/deno and adds it to PATH). The vendored node distribution is not
    # used at runtime, so keep it out of the release (~150 MB saved).
    rm -rf "$APP_DIR/vendor/node" 2>/dev/null || true
fi


# Include controller menu scripts
mkdir -p "$APP_DIR/scripts"
cp -r "$SCRIPT_DIR/scripts/"* "$APP_DIR/scripts/"
chmod +x "$APP_DIR/scripts/"* 2>/dev/null || true

# Include source for on-device rebuilding
mkdir -p "$APP_DIR/src"
cp -r "$SCRIPT_DIR/src/"* "$APP_DIR/src/"
copy_file "$SCRIPT_DIR/Makefile" "$APP_DIR/Makefile"

# Include the tubed backend service (persistent YouTube resolver). The C++
# client launches it from <install>/tubed/tubed.py on first request.
if [ -d "$SCRIPT_DIR/tools/tubed" ]; then
    mkdir -p "$APP_DIR/tubed"
    cp -r "$SCRIPT_DIR/tools/tubed/"* "$APP_DIR/tubed/"
    chmod +x "$APP_DIR/tubed/tubed.py" 2>/dev/null || true
fi

# Include backend architecture doc
if [ -f "$SCRIPT_DIR/docs/BACKEND.md" ]; then
    mkdir -p "$APP_DIR/docs"
    copy_file "$SCRIPT_DIR/docs/BACKEND.md" "$APP_DIR/docs/BACKEND.md"
fi

# Include theme folder
if [ -d "$SCRIPT_DIR/theme" ]; then
    mkdir -p "$APP_DIR/theme"
    cp -r "$SCRIPT_DIR/theme/"* "$APP_DIR/theme/"
fi

# Include res folder (fonts, logo, etc)
if [ -d "$SCRIPT_DIR/res" ]; then
    mkdir -p "$APP_DIR/res"
    cp -r "$SCRIPT_DIR/res/"* "$APP_DIR/res/"
fi

if [ -f "$SCRIPT_DIR/README.md" ]; then
    copy_file "$SCRIPT_DIR/README.md" "$APP_DIR/README.md"
fi

# Write version file and generate version.json manifest
echo "Generating version manifest..."
COMMIT_HASH=$(git rev-parse HEAD 2>/dev/null || echo "")
cat > "$RELEASE_ROOT/version.json" <<EOF
{
  "version": "${RELEASE_VERSION}",
  "tag": "${RELEASE_TAG}",
  "commit": "${COMMIT_HASH}",
  "date": "$(date +%Y-%m-%d)",
  "force_install": false
}
EOF
copy_file "$RELEASE_ROOT/version.json" "$APP_DIR/version.json"
echo "${RELEASE_VERSION}" > "$APP_DIR/VERSION"

cat > "$APP_DIR/RELEASE_NOTES.txt" <<EOF
TubeLite YouTube Client (v${RELEASE_VERSION} - $(date +%Y-%m-%d))

Major Features & Improvements:
- Native audio and video playback using hardware acceleration (rk3326-optimized GLESv2/EGL/MPV layers).
- Background audio player daemon supporting controller-based controls (Play/Pause, Next/Prev, Speed, Exit).
- Seamless continuation of playback as a background audio daemon when exiting the main app.
- Background playback speed control (FN+SELECT) and screen-sleep power saving (X / FN+X).
- ALSA dmix audio mixing so TubeLite, MPV and RetroArch share the audio device.
- Unified installer and controller-friendly setup menu.

Fixes in this build:
- Self-Preserving OTA Updater: Added a robust staged self-updating system (Update-TubeLite.sh/Update-TubeLite.tbl) that parses GitHub release API and prompts on reinstallation.
- Input & Controller Modifier Separation: Split joystick Select (button 12) and Fn (button 16) modifier paths to prevent accidental miniplayer toggling during button chords.
- Settings Overlay Chord Hint: Swapped search/home hints from SEL+Y to FN+Y (purple).
- Storyboard Scaling & Masking: Aligned scrub storyboard preview card with the miniplayer aspect-ratio letterboxing, and implemented a cached mask texture to ensure clean rounded corners without subpixel leaks or 1px gaps.
- Search Bar Text Centering: Increased search bar text entry height to 24px and dynamically centered text based on real font height.
- dArkOS, dArkOSRE & OS Distribution Compatibility: Rewrites EOL Ubuntu apt source lists to use old-releases.ubuntu.com and adds a header auto-repair system to restore stripped build headers for native compiling on darkOS and darkOSRE.
- Keyboard Status Overlay Hints: Added missing L1 (MODE) and X (DELETE) status hints.
- Chipped Rounded Corners: Fixed rendering artifact where rounded-rect corners appeared chipped.

Contents:
- Install-TubeLite.sh
- TubeLite.tbl
- bin/tubelite.arm64 (Updated Binary)
- src/ (Source for on-device rebuilds)

Installation:
1. Copy TubeLite folder to EASYROMS/tools/
2. Run Install-TubeLite.sh from the terminal (or ES Tools menu) and follow the prompts.
3. Reboot.
EOF

# Create archive using python (cross-platform fallback for zip)
echo "Creating archive..."
RELEASE_ROOT_PY="$RELEASE_ROOT"
if command -v cygpath >/dev/null 2>&1; then
    RELEASE_ROOT_PY="$(cygpath -w "$RELEASE_ROOT")"
fi
python3 -c "import shutil, os; \
archive_base = os.path.join(r'$RELEASE_ROOT_PY', 'TubeLite-$(date +%G%m%d)'); \
shutil.make_archive(archive_base, 'zip', r'$RELEASE_ROOT_PY', 'TubeLite')"

echo "Release staged at: $APP_DIR"
echo "Archive created at: $RELEASE_ROOT/TubeLite-$(date +%G%m%d).zip"
echo "Version manifest:  $RELEASE_ROOT/version.json"
echo "Binary used: $BINARY_PATH"

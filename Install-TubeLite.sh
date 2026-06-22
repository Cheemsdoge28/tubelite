#!/bin/bash
# ============================================================================
# TubeLite Unified Installer — Self-Elevating
# ============================================================================
# This script handles installation, uninstallation, and updates for TubeLite.
# It automatically elevates to root and presents a controller-friendly menu
# if run on a handheld.
#
# Usage:
#   sudo bash Install-TubeLite.sh [options]
#
# Options:
#   --uninstall         Remove TubeLite from the system
#   --uninstall-app     Remove application/launcher only
#   --uninstall-theme   Remove theme only
#   --rebuild           Force native compile
#   --reinstall-deps    Refresh system dependencies
# ============================================================================

# ---------- Self-Elevation ----------
REAL_SCRIPT_PATH="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
if [ "$(id -u)" -ne 0 ]; then
    if [ ! -t 0 ]; then
        export TUBELITE_FROM_ES=1
    fi
    echo "TubeLite Installer needs root privileges. Elevating..."
    sudo -E bash "$REAL_SCRIPT_PATH" "$@"
    exit $?
fi

set -e

# ---------- Constants ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="TubeLite"
INSTALL_DIR="$SCRIPT_DIR"          # Self-contained: use actual directory
ES_CFG="/etc/emulationstation/es_systems.cfg"
ES_CFG_DUAL="/etc/emulationstation/es_systems.cfg.dual"
LAUNCHER_SCRIPT="$INSTALL_DIR/TubeLite.tbl"
SYSTEM_NAME="tubelite"
PLATFORM_TAG="${PLATFORM_TAG:-$SYSTEM_NAME}"
THEME_NAME="${THEME_NAME:-$SYSTEM_NAME}"

# ============================================================================
# Logging helpers
# ============================================================================
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

log_step() { echo -e "\n${CYAN}${BOLD}[$1]${NC} $2"; }
log_ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
log_warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
log_err()  { echo -e "  ${RED}✗${NC} $1"; }
log_info() { echo -e "  $1"; }

LOG_FILE="$SCRIPT_DIR/install.log"

# ============================================================================
# Guard: purge any display manager packages apt may have installed as side
# effects and restore ArkOS's native KMSDRM / EmulationStation autostart.
# Call this after EVERY apt-get install block.
# ============================================================================
purge_display_managers() {
    local DM_PKGS="gdm3 gdm gdm-core lightdm sddm xdm wdm nodm"
    local TO_PURGE=""
    for pkg in $DM_PKGS; do
        if dpkg -l "$pkg" 2>/dev/null | grep -q '^ii'; then
            TO_PURGE="$TO_PURGE $pkg"
        fi
    done

    if [ -n "$TO_PURGE" ]; then
        log_warn "Display manager side-effects detected:$TO_PURGE — purging..."
        # Also purge common GNOME deps that only exist because of the DM
        GNOME_DRAG="gnome-shell gnome-shell-common gnome-session gnome-session-bin"
        GNOME_DRAG="$GNOME_DRAG mutter mutter-common gir1.2-mutter-3 gnome-settings-daemon"
        GNOME_DRAG="$GNOME_DRAG ubuntu-session gnome-desktop3-data"
        DEBIAN_FRONTEND=noninteractive apt-get purge -y --no-install-recommends \
            $TO_PURGE $GNOME_DRAG 2>/dev/null || true
        DEBIAN_FRONTEND=noninteractive apt-get autoremove -y \
            --no-install-recommends 2>/dev/null || true
        log_ok "Display manager packages purged"
    fi

    # Remove the display-manager.service symlink — ArkOS does not use one.
    # gdm3 install creates this symlink and it prevents ES from getting DRM master.
    if [ -L "/etc/systemd/system/display-manager.service" ]; then
        rm -f "/etc/systemd/system/display-manager.service"
        log_ok "Removed stale display-manager.service symlink"
    fi

    # Stop and mask gdm/lightdm units in case they're still running.
    for svc in gdm gdm3 lightdm display-manager; do
        systemctl stop   "$svc" 2>/dev/null || true
        systemctl disable "$svc" 2>/dev/null || true
        systemctl mask   "$svc" 2>/dev/null || true
    done

    # Re-enable ArkOS EmulationStation autostart if the unit file exists.
    for es_unit in emulationstation emulationstation.service; do
        if systemctl list-unit-files 2>/dev/null | grep -q "^${es_unit}"; then
            systemctl enable  "$es_unit" 2>/dev/null || true
            log_ok "Re-enabled ArkOS ES unit: $es_unit"
        fi
    done

    # ArkOS uses getty autologin for the ark user to reach ES.
    # Make sure it is still enabled on tty1.
    if [ -f "/lib/systemd/system/getty@.service" ]; then
        systemctl enable "getty@tty1.service" 2>/dev/null || true
    fi

    systemctl daemon-reload 2>/dev/null || true
}

DO_DEPS=1
DO_BINARY=1
DO_FILES=1
DO_LAUNCHER=1
DO_THEME=1
DO_ES=1
DO_VERIFY=1

FORCE_REBUILD=0
REINSTALL_DEPS=0
for arg in "$@"; do
    case "$arg" in
        --rebuild) FORCE_REBUILD=1 ;;
        --reinstall-deps) REINSTALL_DEPS=1 ;;
        --skip-theme) DO_THEME=0 ;;
        --skip-es) DO_ES=0 ;;
        --app-es) DO_DEPS=1; DO_BINARY=1; DO_FILES=1; DO_LAUNCHER=1; DO_ES=1; DO_THEME=0; DO_VERIFY=1 ;;
        --theme-es) DO_DEPS=0; DO_BINARY=0; DO_FILES=0; DO_LAUNCHER=0; DO_ES=1; DO_THEME=1; DO_VERIFY=1 ;;
        --app-only) DO_THEME=0; DO_ES=0 ;;
        --theme-only) DO_DEPS=0; DO_BINARY=0; DO_FILES=0; DO_LAUNCHER=0; DO_ES=0; DO_VERIFY=0; DO_THEME=1 ;;
    esac
done

if [ "${TUBELITE_FROM_ES:-0}" = "1" ]; then
    unset DISPLAY XAUTHORITY
    export DISPLAY=""
    export XAUTHORITY=""
fi

# ---------- Uninstall ----------
# ---------- Emergency display-manager repair ----------
# Use this if a previous bad install broke EmulationStation's ability to start:
#   sudo bash Install-TubeLite.sh --fix-display
if [ "$1" = "--fix-display" ]; then
    echo -e "${BOLD}${APP_NAME} — Emergency Display Repair${NC}"
    echo ""
    log_step "1/3" "Purging display manager packages..."
    purge_display_managers
    log_step "2/3" "Checking ArkOS ES autostart..."
    # ArkOS typically auto-starts ES via a getty autologin or custom service.
    # Show what's currently enabled so the user can confirm.
    systemctl list-unit-files --type=service 2>/dev/null | grep -E 'emulation|getty@tty1' || true
    # If /etc/systemd/system/display-manager.service still exists, nuke it.
    rm -f /etc/systemd/system/display-manager.service 2>/dev/null || true
    systemctl daemon-reload 2>/dev/null || true
    log_step "3/3" "Done. Rebooting in 5 seconds..."
    log_ok "After reboot EmulationStation should start normally."
    sleep 5
    reboot
    exit 0
fi

if [ "$1" = "--uninstall" ] || [ "$1" = "--uninstall-app" ] || [ "$1" = "--uninstall-theme" ]; then
    echo -e "${BOLD}${APP_NAME} Uninstaller${NC}"
    echo ""

    REMOVE_APP=0
    REMOVE_THEME=0
    if [ "$1" = "--uninstall" ] || [ "$1" = "--uninstall-app" ]; then
        REMOVE_APP=1
    fi
    if [ "$1" = "--uninstall" ] || [ "$1" = "--uninstall-theme" ]; then
        REMOVE_THEME=1
    fi

    if [ "$REMOVE_APP" -eq 1 ]; then
        rm -f "$INSTALL_DIR/TubeLite.tbl"
        rm -f "/usr/local/bin/tubelite"
        log_ok "Removed launcher script and wrapper symlink"
    fi

    # Remove ES system entries
    for cfg in "$ES_CFG" "$ES_CFG_DUAL"; do
        if [ -f "$cfg" ] && grep -q "tubelite" "$cfg"; then
            sed -i "/<!-- TubeLite YouTube Client/,/<\/system>/d" "$cfg"
            # Also clean up legacy entries if any
            sed -i "/<system>\s*<name>tubelite<\/name>.*<\/system>/d" "$cfg" 2>/dev/null || true
            sed -i "/<!-- Fire4ArkOS Browser/,/<\/system>/d" "$cfg" 2>/dev/null || true
            log_ok "Removed $SYSTEM_NAME entry from $cfg"
        fi
    done

    if [ "$REMOVE_THEME" -eq 1 ]; then
        # Remove theme entries
        BASE_THEME_ROOT="/etc/emulationstation/themes"
        if [ -d "$BASE_THEME_ROOT" ]; then
            for theme_dir in "$BASE_THEME_ROOT"/*/; do
                if [ -d "${theme_dir}${SYSTEM_NAME}" ]; then
                    rm -rf "${theme_dir}${SYSTEM_NAME}"
                fi
                if [ -d "${theme_dir}fire4arkos" ]; then
                    rm -rf "${theme_dir}fire4arkos"
                fi
            done
            rm -rf "$BASE_THEME_ROOT/$SYSTEM_NAME"
            log_ok "Removed theme assets from all theme directories"
        fi
    fi

    echo ""
    echo -e "${GREEN}Uninstall complete.${NC} Restart EmulationStation to apply."
    echo "Your files in $INSTALL_DIR are preserved."
    exit 0
fi

# ---------- Banner ----------
echo ""
echo -e "${BOLD}============================================${NC}"
echo -e "${BOLD}  ${APP_NAME} Unified Installer${NC}"
echo -e "${BOLD}============================================${NC}"
echo ""

# ---------- Interactive Menu ----------
if [ "$#" -eq 0 ]; then
    USE_CONTROLLER_MENU=0
    if [ -c "/dev/input/js0" ] || [ "${TUBELITE_FROM_ES:-0}" = "1" ]; then
        if [ -f "$SCRIPT_DIR/scripts/controller_menu.py" ]; then
            USE_CONTROLLER_MENU=1
        else
            log_warn "Controller menu script missing at $SCRIPT_DIR/scripts/controller_menu.py"
        fi
    fi

    if [ "$USE_CONTROLLER_MENU" -eq 1 ]; then
        CHOICE_FILE=$(mktemp)
        /usr/bin/python3 "$SCRIPT_DIR/scripts/controller_menu.py" "$CHOICE_FILE" || true
        if [ -s "$CHOICE_FILE" ]; then
            choice=$(cat "$CHOICE_FILE")
        else
            log_info "No controller input detected, falling back to CLI menu..."
            USE_CONTROLLER_MENU=0
        fi
        rm -f "$CHOICE_FILE"
    fi

    if [ "$USE_CONTROLLER_MENU" -eq 0 ]; then
        echo -e "${BOLD}Select an option:${NC}"
        echo "1) Full Install (Recommended)"
        echo "2) App Only (No Theme)"
        echo "3) Theme Only"
        echo "4) Uninstall Everything"
        echo "5) Uninstall App Only"
        echo "6) Uninstall Theme Only"
        echo "7) Exit"
        read -p "Enter choice [1-7]: " choice </dev/tty 2>/dev/null || choice="1"
    fi

    case "$choice" in
        1) log_info "Proceeding with Full Install..." ;;
        2) log_info "Proceeding with App Only Install..."; DO_THEME=0; DO_ES=0 ;;
        3) log_info "Proceeding with Theme Only Install..."; DO_DEPS=0; DO_BINARY=0; DO_FILES=0; DO_LAUNCHER=0; DO_ES=1; DO_THEME=1; DO_VERIFY=1 ;;
        4) exec bash "$0" "--uninstall" ;;
        5) exec bash "$0" "--uninstall-app" ;;
        6) exec bash "$0" "--uninstall-theme" ;;
        7) exit 0 ;;
        *) log_err "Invalid choice. Exiting."; exit 1 ;;
    esac
fi

# ---------- Pre-flight checks ----------
for required in scripts/install-es-system.py; do
    if [ ! -f "$SCRIPT_DIR/$required" ]; then
        log_err "$required not found in $SCRIPT_DIR (check scripts/ folder)"
        exit 1
    fi
done

ARCH="$(uname -m)"

if [ "$DO_DEPS" -eq 1 ]; then
    log_step "1/7" "Installing dependencies..."
    
    # APT sources fix for EOL Ubuntu
    if grep -q "archive.ubuntu.com\|security.ubuntu.com\|ports.ubuntu.com" "/etc/apt/sources.list" 2>/dev/null; then
        if ! grep -q "old-releases.ubuntu.com" "/etc/apt/sources.list" 2>/dev/null; then
            log_info "Fixing APT sources for EOL Ubuntu..."
            sed -i 's|http://archive.ubuntu.com|http://old-releases.ubuntu.com|g' /etc/apt/sources.list
            sed -i 's|http://security.ubuntu.com|http://old-releases.ubuntu.com|g' /etc/apt/sources.list
            sed -i 's|http://ports.ubuntu.com|http://old-releases.ubuntu.com|g' /etc/apt/sources.list
        fi
    fi

    dpkg --configure -a || true
    apt-get update -qq || true
    
    # NOTE: ffmpeg binary intentionally omitted — mpv handles all decoding internally.
    # Including ffmpeg pulls in ghostscript → gnome-shell → gdm3 via recommended deps,
    # which hijacks the KMSDRM display and breaks EmulationStation on ArkOS.
    # To protect ArkOS custom/optimized held libraries (like libsdl2-2.0-0, libasound2, libmpv1),
    # we check each dependency and only invoke apt-get install for those that are missing.
    CRITICAL_DEPS="python3 libsdl2-2.0-0 libasound2 libmpv1 libsdl2-ttf-2.0-0 libharfbuzz0b libfreetype6"
    RUNTIME_DEPS=""
    for pkg in $CRITICAL_DEPS; do
        if dpkg -l "$pkg" 2>/dev/null | grep -q '^ii'; then
            log_info "Dependency $pkg is already installed. Keeping stock version."
        else
            RUNTIME_DEPS="$RUNTIME_DEPS $pkg"
        fi
    done

    # --no-install-recommends is CRITICAL on ArkOS: recommended deps often drag in
    # entire GNOME stacks (gdm3, gnome-shell) which steal DRM master from ES.
    APT_FLAGS="-y --no-install-recommends"
    if [ "$REINSTALL_DEPS" = "1" ]; then
        APT_FLAGS="$APT_FLAGS --reinstall"
        log_info "Forcing full system header & developer tools restore ritual..."
        # libssl-dev pulls the OpenSSL runtime + headers — on ArkOS that's libssl1.1.
        # Without it the curl/wget toolchain and any native code that links against
        # OpenSSL goes missing after a partial OS image strip.  Note this does NOT
        # solve the libssl.so.3 mismatch for yt-dlp's PyInstaller binary; that
        # requires side-loading libssl3 + libcrypto3 into /usr/local/lib from a
        # Debian Bookworm .deb (see docs / memory:yt-dlp-libssl).
        DEV_HEADERS="gdb libc6-dev libsdl2-dev linux-libc-dev g++ libstdc++-9-dev libsdl2-ttf-dev git python3 ninja-build cmake make i2c-tools usbutils fbcat fbset mmc-utils libglew-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libglu1-mesa-dev libdrm-dev libssl-dev"
        apt-get install -y --no-install-recommends --reinstall $DEV_HEADERS || true
    fi

    if [ -n "$RUNTIME_DEPS" ]; then
        log_info "Running apt-get install for missing dependencies:$RUNTIME_DEPS..."
        apt-get install $APT_FLAGS $RUNTIME_DEPS || true
    else
        log_info "All runtime dependencies are already installed."
    fi

    # Guard: purge any display manager that snuck in as a side effect.
    purge_display_managers
    
    log_info "Verifying dependencies..."
    MISSING_DEPS=""
    
    # Check python3
    if ! command -v python3 &>/dev/null; then
        MISSING_DEPS="python3"
    fi
    
    # Check yt-dlp installation:
    # First, try to download the latest standalone version from GitHub to ensure extraction compatibility.
    # Standalone binaries include their own Python interpreter and work on EOL systems (like ArkOS on Python 3.7).
    # If the device is offline or download fails, fall back to the pre-included local version.
    YT_DLP_UPDATED=0
    YTDLP_URL="https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp"
    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
        YTDLP_URL="https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_aarch64"
    elif [ "$ARCH" = "x86_64" ]; then
        YTDLP_URL="https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux"
    fi

    log_info "Attempting to download latest standalone yt-dlp from GitHub..."
    if wget -q --timeout=10 "$YTDLP_URL" -O /usr/local/bin/yt-dlp || \
       curl -fsL --connect-timeout 10 "$YTDLP_URL" -o /usr/local/bin/yt-dlp; then
        chmod a+rx /usr/local/bin/yt-dlp
        log_ok "Successfully updated to latest standalone yt-dlp from GitHub"
        YT_DLP_UPDATED=1
    else
        log_warn "Failed to download from GitHub (offline?). Checking for local/existing copy..."
    fi

    if [ "$YT_DLP_UPDATED" -eq 0 ]; then
        if [ -f "$SCRIPT_DIR/bin/yt-dlp" ]; then
            log_info "Installing pre-included local yt-dlp..."
            cp -f "$SCRIPT_DIR/bin/yt-dlp" /usr/local/bin/yt-dlp
            chmod a+rx /usr/local/bin/yt-dlp
        fi
    fi
    
    # Verify installation
    if [ -x "/usr/local/bin/yt-dlp" ]; then
        log_ok "yt-dlp verified at /usr/local/bin/yt-dlp"
    elif command -v yt-dlp &>/dev/null; then
        log_ok "yt-dlp verified in PATH"
    elif [ -x "/usr/bin/yt-dlp" ]; then
        log_ok "yt-dlp verified at /usr/bin/yt-dlp"
    else
        MISSING_DEPS="$MISSING_DEPS yt-dlp"
    fi
    
    if [ -n "$MISSING_DEPS" ]; then
        log_err "Failed to verify some critical runtime dependencies:$MISSING_DEPS"
        log_err "Please ensure python3 and yt-dlp are installed and available in the PATH."
        exit 1
    fi
    if [ -x "$SCRIPT_DIR/vendor/deno" ]; then
        log_ok "Bundled deno detected at $SCRIPT_DIR/vendor/deno"
    else
        log_warn "No bundled deno found at $SCRIPT_DIR/vendor/deno"
    fi
    log_ok "Dependencies verified"
fi

if [ "$DO_BINARY" -eq 1 ]; then
    log_step "2/7" "Setting up TubeLite binary..."
    APP_BIN=""
    if [ "$FORCE_REBUILD" -eq 0 ]; then
        for candidate in "$SCRIPT_DIR/build/tubelite.arm64" "$SCRIPT_DIR/build/tubelite" "$SCRIPT_DIR/bin/tubelite.arm64" "$SCRIPT_DIR/bin/tubelite" "$SCRIPT_DIR/tubelite"; do
            if [ -f "$candidate" ]; then APP_BIN="$candidate"; break; fi
        done
    fi

    if [ -z "$APP_BIN" ]; then
        log_info "No pre-built binary found. Compiling natively..."
        BUILD_DEPS="build-essential g++ make pkg-config libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev libgl1-mesa-dev libfreetype6-dev libharfbuzz-dev libmpv-dev libdrm-dev"
        # --no-install-recommends is CRITICAL: build tools have recommended deps that
        # can pull in full GNOME stacks (via ghostscript, libgs-dev chains).
        BUILD_APT_FLAGS="-y --no-install-recommends"
        if [ "${REINSTALL_DEPS:-0}" = "1" ]; then BUILD_APT_FLAGS="$BUILD_APT_FLAGS --reinstall"; fi
        log_info "Installing build dependencies..."
        apt-get install $BUILD_APT_FLAGS $BUILD_DEPS || true

        # Guard: purge any display manager that snuck in as a build dep side effect.
        purge_display_managers

        # Check for missing FreeType headers on filesystem (often deleted on handheld OS images)
        if [ ! -f "/usr/include/freetype2/ft2build.h" ]; then
            log_warn "FreeType headers (ft2build.h) missing from filesystem. Restoring libfreetype6-dev..."
            apt-get install -y --no-install-recommends --reinstall libfreetype6-dev || true
        fi

        # Check for missing DRM headers on filesystem (often deleted on handheld OS images)
        if [ ! -f "/usr/include/libdrm/drm.h" ] || [ ! -f "/usr/include/xf86drm.h" ]; then
            log_warn "DRM headers (drm.h/xf86drm.h) missing from filesystem. Restoring libdrm-dev..."
            apt-get install -y --no-install-recommends --reinstall libdrm-dev || true
        fi

        # Check for missing core C/C++ or SDL2 headers on filesystem
        if [ ! -f "/usr/include/features.h" ] || [ ! -f "/usr/include/SDL2/SDL.h" ]; then
            log_warn "Core C/C++ or SDL2 development headers are missing from filesystem."
            log_warn "Running header file restore ritual to repair the compilation environment..."
            DEV_HEADERS="gdb libc6-dev libsdl2-dev linux-libc-dev g++ libstdc++-9-dev libsdl2-ttf-dev git python3 ninja-build cmake make i2c-tools usbutils fbcat fbset mmc-utils libglew-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libglu1-mesa-dev libdrm-dev libssl-dev"
            apt-get install -y --no-install-recommends --reinstall $DEV_HEADERS || true
        fi
        
        cd "$SCRIPT_DIR"
        log_info "Running make native..."
        make native || true
        
        if [ -f "$SCRIPT_DIR/build/tubelite" ]; then
            APP_BIN="$SCRIPT_DIR/build/tubelite"
        elif [ -f "$SCRIPT_DIR/build/tubelite.arm64" ]; then
            APP_BIN="$SCRIPT_DIR/build/tubelite.arm64"
        else
            log_err "Compilation failed. Native binary could not be built."
            exit 1
        fi
    fi
    chmod +x "$APP_BIN" 2>/dev/null || true
    strip "$APP_BIN" 2>/dev/null || true
    log_ok "Binary ready"
fi

if [ "$DO_FILES" -eq 1 ]; then
    log_step "3/7" "Preparing files..."
    chmod +x "$INSTALL_DIR"/*.sh 2>/dev/null || true
    chmod +x "$INSTALL_DIR"/*.tbl 2>/dev/null || true
    chmod +x "$INSTALL_DIR"/scripts/*.sh 2>/dev/null || true
    chmod +x "$INSTALL_DIR"/scripts/*.py 2>/dev/null || true
    log_ok "Files prepared"
fi

if [ "$DO_LAUNCHER" -eq 1 ]; then
    log_step "4/7" "Creating ES launch script..."
    cat > "$LAUNCHER_SCRIPT" << LAUNCH_EOF
#!/bin/bash
SCRIPT_DIR="\$(cd "\$(dirname "\$0")" && pwd)"
cd "\$SCRIPT_DIR" || exit 1

# Performance CPU governor for smooth rendering
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    if [ -w "\$gov" ]; then
        echo performance > "\$gov" 2>/dev/null || true
    fi
done

# Surface side-loaded OpenSSL 3 to yt-dlp's PyInstaller bundle.  ArkOS / RG351MP
# ships only libssl.so.1.1, but recent yt-dlp standalone builds bundle a Python
# whose _ssl C ext links against libssl.so.3.  If libssl3+libcrypto3 have been
# side-loaded into /usr/local/lib or /roms/tools/tubelite/lib (or similar
# non-default locations), make sure tubed's forked yt-dlp subprocess can find
# them even when EmulationStation launched us with a stripped environment.
for d in /usr/local/lib /usr/local/lib64 "\$SCRIPT_DIR/lib"; do
    if [ -f "\$d/libssl.so.3" ]; then
        export LD_LIBRARY_PATH="\$d:\${LD_LIBRARY_PATH}"
    fi
done

if [ -x "\$SCRIPT_DIR/vendor/deno" ]; then
    export PATH="\$SCRIPT_DIR/vendor:\${PATH}"
fi

# Launch the app
LOGFILE="\$SCRIPT_DIR/tubelite.log"
BINARIES=(
    "\$SCRIPT_DIR/build/tubelite.arm64"
    "\$SCRIPT_DIR/build/tubelite"
    "\$SCRIPT_DIR/bin/tubelite.arm64"
    "\$SCRIPT_DIR/bin/tubelite"
    "\$SCRIPT_DIR/tubelite"
)

for bin in "\${BINARIES[@]}"; do
    if [ -x "\$bin" ]; then
        echo "[INFO] Launching \$bin..."
        echo "[TubeLite] Launching \$bin" >> "\$LOGFILE"
        exec "\$bin" "\$@" >> "\$LOGFILE" 2>&1
    fi
done

echo "[ERROR] TubeLite binary not found" >> "\$LOGFILE"
exit 1
LAUNCH_EOF
    chmod +x "$LAUNCHER_SCRIPT"
    if [ -d "/usr/local/bin" ]; then
        ln -sf "$LAUNCHER_SCRIPT" "/usr/local/bin/tubelite"
    fi
    log_ok "Launcher created"
fi

if [ "$DO_THEME" -eq 1 ]; then
    log_step "5/7" "Installing theme..."
    BASE_THEME_ROOT="/etc/emulationstation/themes"
    if [ -d "$BASE_THEME_ROOT" ] && [ -d "$SCRIPT_DIR/theme" ]; then
        for theme_dir in "$BASE_THEME_ROOT"/*/; do
            target_dir="${theme_dir}${SYSTEM_NAME}"
            mkdir -p "$target_dir"
            cp -r "$SCRIPT_DIR/theme"/* "$target_dir/" 2>/dev/null || true
        done
        log_ok "Theme installed"
    fi
fi

if [ "$DO_ES" -eq 1 ]; then
    log_step "6/7" "Registering with EmulationStation..."
    if [ -f "$ES_CFG" ]; then
        python3 "$SCRIPT_DIR/scripts/install-es-system.py" --cfg-file "$ES_CFG" --install-dir "$INSTALL_DIR" --platform-tag "$PLATFORM_TAG" --theme-name "$THEME_NAME" || log_warn "Failed to register in $ES_CFG"
    fi
    if [ -f "$ES_CFG_DUAL" ]; then
        python3 "$SCRIPT_DIR/scripts/install-es-system.py" --cfg-file "$ES_CFG_DUAL" --install-dir "$INSTALL_DIR" --platform-tag "$PLATFORM_TAG" --theme-name "$THEME_NAME" || log_warn "Failed to register in $ES_CFG_DUAL"
    fi
    log_ok "ES registration check complete"
fi

if [ "$DO_ES" -eq 1 ]; then
    log_step "6.5/7" "Patching RetroArch for audio mixing + UDP notifications..."
    # Why both edits:
    #   audio_device = "plug:dmix"     → makes RA share the codec via
    #     ALSA's dmix (same path TubeLite uses), so background music
    #     from the daemon coexists with game audio.  Without this, RA
    #     opens the codec exclusively and TubeLite goes silent (or
    #     vice-versa).
    #   network_cmd_enable = "true"    → exposes RA's UDP command
    #     socket on 55355.  The daemon sends `SHOW_MSG <title>` to
    #     this socket on track changes, so the user sees a now-playing
    #     toast inside the game even though the DRM overlay can't
    #     draw on top of an active emulator.
    #
    # Patch idempotently in every retroarch.cfg we can find — ArkOS
    # has both system and per-user copies, sometimes layered.  sed
    # replaces existing keys if present, appends otherwise.
    patch_retroarch_cfg() {
        local cfg="$1"
        [ -f "$cfg" ] || return 0

        # Audio driver
        if grep -q '^audio_driver' "$cfg"; then
            sed -i 's|^audio_driver = .*|audio_driver = "alsa"|' "$cfg"
        else
            echo 'audio_driver = "alsa"' >> "$cfg"
        fi

        # Audio device → plug:dmix (the ONLY ALSA name we've verified
        # mixes reliably with TubeLite + speaker-test on this hardware)
        if grep -q '^audio_device' "$cfg"; then
            sed -i 's|^audio_device = .*|audio_device = "plug:dmix"|' "$cfg"
        else
            echo 'audio_device = "plug:dmix"' >> "$cfg"
        fi

        # OSD message duration: bump from RA's default (~2 frames-as-
        # seconds = quick blip) to 4 s so multi-line SHOW_MSG toasts
        # ("♪ Now Playing\n<title>\n<author> · <duration>\n2 / 5")
        # from the TubeLite daemon stay on screen long enough to read.
        if grep -q '^osd_message_duration' "$cfg"; then
            sed -i 's|^osd_message_duration = .*|osd_message_duration = "4"|' "$cfg"
        else
            echo 'osd_message_duration = "4"' >> "$cfg"
        fi

        # UDP command socket for SHOW_MSG
        if grep -q '^network_cmd_enable' "$cfg"; then
            sed -i 's|^network_cmd_enable = .*|network_cmd_enable = "true"|' "$cfg"
        else
            echo 'network_cmd_enable = "true"' >> "$cfg"
        fi
        if grep -q '^network_cmd_port' "$cfg"; then
            sed -i 's|^network_cmd_port = .*|network_cmd_port = "55355"|' "$cfg"
        else
            echo 'network_cmd_port = "55355"' >> "$cfg"
        fi

        log_ok "Patched $cfg"
    }

    # Candidates: ArkOS user-level configs (ark + root + any other
    # home), plus the system fallback.  The list is generous on
    # purpose — patching a non-existent file is a fast no-op.
    RA_CFG_CANDIDATES=(
        "/home/ark/.config/retroarch/retroarch.cfg"
        "/root/.config/retroarch/retroarch.cfg"
        "/etc/retroarch.cfg"
        "/opt/retroarch/retroarch.cfg"
    )
    for cfg in "${RA_CFG_CANDIDATES[@]}"; do
        patch_retroarch_cfg "$cfg"
    done

    # Also catch any other user homes (e.g. multi-user images).
    for home in /home/*; do
        [ -d "$home" ] || continue
        patch_retroarch_cfg "$home/.config/retroarch/retroarch.cfg"
    done
fi

if [ "$DO_VERIFY" -eq 1 ]; then
    log_step "7/7" "Final display-manager safety check..."
    # One last sweep — ensures nothing snuck in during ES/theme steps.
    purge_display_managers
    echo -e "${GREEN}${BOLD}Installation Finished!${NC}"
    echo "Restart EmulationStation to see TubeLite."
    echo ""
    echo -e "  ${YELLOW}NOTE:${NC} If EmulationStation does not start after reboot,"
    echo -e "  run: ${BOLD}sudo bash Install-TubeLite.sh --fix-display${NC}"
fi

echo ""
echo "Returning to EmulationStation in 10 seconds..."
sleep 10

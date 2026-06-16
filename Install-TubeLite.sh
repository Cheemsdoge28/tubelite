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
    
    apt-mark unhold libsdl2-2.0-0 libasound2 libmpv1 2>/dev/null || true
    
    RUNTIME_DEPS="python3 libsdl2-2.0-0 ffmpeg libasound2 libmpv1 libsdl2-ttf-2.0-0 libharfbuzz0b libfreetype6"
    APT_FLAGS="-y --allow-change-held-packages"
    if [ "$REINSTALL_DEPS" = "1" ]; then APT_FLAGS="$APT_FLAGS --reinstall"; fi
    
    log_info "Running apt-get install..."
    apt-get install $APT_FLAGS $RUNTIME_DEPS || true
    
    log_info "Protecting SDL and Audio libraries..."
    apt-mark hold libsdl2-2.0-0 libasound2 2>/dev/null || true
    
    log_info "Verifying dependencies..."
    MISSING_DEPS=""
    
    # Check python3
    if ! command -v python3 &>/dev/null; then
        MISSING_DEPS="python3"
    fi
    
    # Check yt-dlp installation: prioritize the pre-included local version
    YT_DLP_FOUND=0
    if [ -f "$SCRIPT_DIR/bin/yt-dlp" ]; then
        log_info "Installing pre-included stable yt-dlp (2026.03.13)..."
        cp -f "$SCRIPT_DIR/bin/yt-dlp" /usr/local/bin/yt-dlp
        chmod a+rx /usr/local/bin/yt-dlp
        YT_DLP_FOUND=1
    elif command -v yt-dlp &>/dev/null; then
        YT_DLP_FOUND=1
    elif [ -x "/usr/local/bin/yt-dlp" ]; then
        YT_DLP_FOUND=1
    elif [ -x "/usr/bin/yt-dlp" ]; then
        YT_DLP_FOUND=1
    fi
    
    if [ "$YT_DLP_FOUND" -eq 0 ]; then
        # Try to install it automatically to /usr/local/bin/yt-dlp from the web if local copy is missing
        log_info "yt-dlp not found. Downloading the latest version..."
        wget https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp -O /usr/local/bin/yt-dlp || \
        curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp -o /usr/local/bin/yt-dlp
        chmod a+rx /usr/local/bin/yt-dlp
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
        BUILD_DEPS="build-essential g++ make pkg-config libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev libgl1-mesa-dev libfreetype6-dev libharfbuzz-dev libmpv-dev"
        BUILD_APT_FLAGS="-y"
        if [ "${REINSTALL_DEPS:-0}" = "1" ]; then BUILD_APT_FLAGS="$BUILD_APT_FLAGS --reinstall"; fi
        log_info "Installing build dependencies..."
        apt-get install $BUILD_APT_FLAGS $BUILD_DEPS || true
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

if [ "$DO_VERIFY" -eq 1 ]; then
    log_step "7/7" "Verification complete."
    echo -e "${GREEN}${BOLD}Installation Finished!${NC}"
    echo "Restart EmulationStation to see TubeLite."
fi

echo ""
echo "Returning to EmulationStation in 10 seconds..."
sleep 10

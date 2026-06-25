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

# ============================================================================
# Compatibility Wizard — fix missing/broken libmpv (and its codec chain).
#
# Strategy (cheapest, cleanest first):
#   1. Diagnose whether the system already provides libmpv.
#   2. Try `apt-get install libmpv1` — the proper, version-matched fix.
#   3. Fall back to the bundled compat pack (vendor/lib): either an
#      already-extracted folder or a tubelite-compat-libs.zip placed next to
#      the installer.  The ES launcher already adds vendor/lib to
#      LD_LIBRARY_PATH, so dropping the libs there is enough.
#
# Edge cases handled:
#   - vendor/lib folder absent on entry  → look for a compat zip and extract it
#   - compat zip absent too              → clear, actionable message, no crash
#   - zip nested under vendor-lib/        → flattened into vendor/lib
#   - wrong-arch / still-unresolved libs → ldd sanity check + warning
# ============================================================================
COMPAT_LIB_DIR="$INSTALL_DIR/vendor/lib"

compat_have_system_libmpv() {
    if ldconfig -p 2>/dev/null | grep -q 'libmpv\.so'; then return 0; fi
    for d in /usr/lib /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu \
             /usr/local/lib /usr/local/lib64; do
        if ls "$d"/libmpv.so* >/dev/null 2>&1; then return 0; fi
    done
    return 1
}

compat_bundle_present() {
    [ -d "$COMPAT_LIB_DIR" ] && ls "$COMPAT_LIB_DIR"/*.so* >/dev/null 2>&1
}

run_compat_wizard() {
    echo ""
    echo -e "${BOLD}============================================${NC}"
    echo -e "${BOLD}  ${APP_NAME} Compatibility Wizard (libmpv)${NC}"
    echo -e "${BOLD}============================================${NC}"
    echo ""

    # ── 1. Diagnose ──────────────────────────────────────────────────────
    log_step "1/3" "Checking for libmpv..."
    if compat_have_system_libmpv; then
        log_ok "System libmpv is present — playback should already work."
        log_info "Continuing anyway to refresh/repair the fallback libs."
    else
        log_warn "System libmpv is MISSING — video playback will fail without a fix."
    fi

    # ── 2. Preferred fix: apt ────────────────────────────────────────────
    log_step "2/3" "Restoring libmpv via apt (force --reinstall — see below)..."
    apt-get update -qq 2>/dev/null || true
    # CRITICAL: dpkg often reports libmpv1 as installed ("ii") while the actual
    # libmpv.so.1 file was stripped from the image — so a plain `apt-get install`
    # is a NO-OP and the file never comes back.  Force --reinstall first to
    # re-extract the real .so; fall back to a fresh install only if the package
    # genuinely isn't present.  Try libmpv2 (trixie+) then libmpv1.
    for pkg in libmpv2 libmpv1; do
        if apt-get install -y --no-install-recommends --reinstall "$pkg" 2>/dev/null; then
            log_ok "Reinstalled $pkg"; break
        elif apt-get install -y --no-install-recommends "$pkg" 2>/dev/null; then
            log_ok "Installed $pkg"; break
        fi
    done
    purge_display_managers
    if compat_have_system_libmpv; then
        log_ok "libmpv installed via apt. Playback should work now."
        return 0
    fi
    log_warn "apt could not provide libmpv (offline / EOL repos). Using bundled compat pack."

    # ── 3. Fallback: bundled compat libs ─────────────────────────────────
    log_step "3/3" "Setting up bundled compatibility libraries..."

    # If the folder isn't there (or is empty), try to extract a compat zip
    # that the user dropped next to the installer.
    if ! compat_bundle_present; then
        local zip=""
        for z in "$SCRIPT_DIR/tubelite-compat-libs.zip" \
                 "$SCRIPT_DIR/compat-libs.zip" \
                 "$SCRIPT_DIR/vendor-lib.zip" \
                 "$SCRIPT_DIR/dist/release/tubelite-compat-libs.zip"; do
            if [ -f "$z" ]; then zip="$z"; break; fi
        done

        if [ -n "$zip" ]; then
            log_info "Extracting compat pack: $(basename "$zip")"
            mkdir -p "$COMPAT_LIB_DIR"
            if command -v unzip >/dev/null 2>&1; then
                unzip -o -q "$zip" -d "$COMPAT_LIB_DIR" || true
            elif command -v python3 >/dev/null 2>&1; then
                python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
                    "$zip" "$COMPAT_LIB_DIR" || true
            else
                log_err "Neither 'unzip' nor 'python3' is available to extract the pack."
            fi
            # Flatten if the archive nested everything under vendor-lib/.
            if [ -d "$COMPAT_LIB_DIR/vendor-lib" ]; then
                mv "$COMPAT_LIB_DIR/vendor-lib"/* "$COMPAT_LIB_DIR/" 2>/dev/null || true
                rmdir "$COMPAT_LIB_DIR/vendor-lib" 2>/dev/null || true
            fi
        fi
    fi

    # Edge case: still nothing to work with.
    if ! compat_bundle_present; then
        log_err "No compatibility libraries are available."
        echo ""
        log_info "apt could not install libmpv, and no compat pack was found at:"
        log_info "  $COMPAT_LIB_DIR/  (extracted libs), or"
        log_info "  $SCRIPT_DIR/tubelite-compat-libs.zip  (the pack)"
        echo ""
        log_info "To fix playback, do ONE of the following:"
        log_info "  • Connect to the internet and re-run this wizard (apt path), or"
        log_info "  • Download 'tubelite-compat-libs.zip', copy it into:"
        log_info "      $SCRIPT_DIR/"
        log_info "    then re-run:  sudo bash Install-TubeLite.sh --compat"
        return 1
    fi

    chmod +x "$COMPAT_LIB_DIR"/*.so* 2>/dev/null || true
    local n
    n="$(ls -1 "$COMPAT_LIB_DIR"/*.so* 2>/dev/null | wc -l)"
    log_ok "Bundled compat libraries ready ($n libraries in vendor/lib)."
    log_info "The launcher adds this folder to LD_LIBRARY_PATH, so TubeLite uses"
    log_info "these whenever the system libmpv is missing."

    # Sanity check: does the app binary actually resolve libmpv now?
    local bin=""
    for c in "$INSTALL_DIR/build/tubelite.arm64" "$INSTALL_DIR/build/tubelite" \
             "$INSTALL_DIR/bin/tubelite.arm64" "$INSTALL_DIR/bin/tubelite"; do
        if [ -x "$c" ]; then bin="$c"; break; fi
    done
    if [ -n "$bin" ] && command -v ldd >/dev/null 2>&1; then
        if LD_LIBRARY_PATH="$COMPAT_LIB_DIR:${LD_LIBRARY_PATH:-}" ldd "$bin" 2>/dev/null \
                | grep -q 'libmpv.*not found'; then
            log_warn "libmpv is still unresolved even with the bundle."
            log_warn "The pack may be for the wrong architecture (need aarch64)."
            return 1
        fi
        log_ok "Verified: TubeLite resolves libmpv with the bundled libraries."
    fi
    return 0
}

# ============================================================================
# Fix on-device build headers.
#
# ArkOS / RG351MP images routinely SHIP the -dev packages in dpkg's database
# (so `dpkg -l` says "ii") but DELETE the actual header files to save space.
# Plain `apt-get install libsdl2-dev` is then a no-op — apt sees it as already
# installed — and `make native` dies on "SDL2/SDL.h: No such file".
#
# This routine checks each header FILE the native build needs and force
# `--reinstall`s the owning package (per-package, so one unavailable .deb can't
# abort the rest).  Reusable from the menu, the --fix-headers flag, and the
# native-compile path.
# ============================================================================
fix_dev_headers() {
    # header_file:owning_package — the representative file we test for presence.
    local pairs=(
        "/usr/include/features.h:libc6-dev"
        "/usr/include/SDL2/SDL.h:libsdl2-dev"
        "/usr/include/SDL2/SDL_ttf.h:libsdl2-ttf-dev"
        "/usr/include/freetype2/ft2build.h:libfreetype6-dev"
        "/usr/include/harfbuzz/hb.h:libharfbuzz-dev"
        "/usr/include/libdrm/drm.h:libdrm-dev"
        "/usr/include/xf86drm.h:libdrm-dev"
        "/usr/include/GLES2/gl2.h:libgles2-mesa-dev"
        "/usr/include/EGL/egl.h:libegl1-mesa-dev"
        "/usr/include/GL/gl.h:libgl1-mesa-dev"
        "/usr/include/mpv/client.h:libmpv-dev"
    )

    log_step "Headers" "Checking on-device build headers..."

    # EOL Ubuntu mirror rewrite so --reinstall can actually fetch the .debs.
    if grep -q "archive.ubuntu.com\|security.ubuntu.com\|ports.ubuntu.com" /etc/apt/sources.list 2>/dev/null \
       && ! grep -q "old-releases.ubuntu.com" /etc/apt/sources.list 2>/dev/null; then
        log_info "Rewriting EOL Ubuntu apt sources to old-releases..."
        sed -i -e 's|http://archive.ubuntu.com|http://old-releases.ubuntu.com|g' \
               -e 's|http://security.ubuntu.com|http://old-releases.ubuntu.com|g' \
               -e 's|http://ports.ubuntu.com|http://old-releases.ubuntu.com|g' \
               /etc/apt/sources.list || true
    fi
    apt-get update -qq 2>/dev/null || true

    # Build the (deduplicated) set of packages whose header file is missing.
    local need=""
    local pair hdr pkg
    for pair in "${pairs[@]}"; do
        hdr="${pair%%:*}"; pkg="${pair##*:}"
        if [ ! -f "$hdr" ]; then
            log_warn "Missing: $hdr  →  $pkg"
            case " $need " in *" $pkg "*) ;; *) need="$need $pkg" ;; esac
        fi
    done

    # Compiler / toolchain programs (not header files).
    command -v g++  >/dev/null 2>&1 || need="$need build-essential g++"
    command -v make >/dev/null 2>&1 || need="$need make"
    command -v pkg-config >/dev/null 2>&1 || need="$need pkg-config"

    if [ -z "$need" ]; then
        log_ok "All build headers and the toolchain are present."
        return 0
    fi

    # Force re-extraction PER PACKAGE: `--reinstall` aborts the whole
    # transaction if any one package isn't downloadable, so isolate them.
    log_info "Restoring:${need}"
    local p
    for p in $need; do
        if DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends --reinstall "$p" 2>/dev/null; then
            log_ok "  reinstalled $p"
        elif DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$p" 2>/dev/null; then
            log_ok "  installed $p"
        else
            log_warn "  could not (re)install $p (unavailable on this mirror?)"
        fi
    done

    # Build tools can drag in a display manager via recommends — sweep it.
    purge_display_managers

    # Re-verify the header files actually landed this time.
    local still=""
    for pair in "${pairs[@]}"; do
        hdr="${pair%%:*}"
        [ -f "$hdr" ] || still="$still $hdr"
    done
    if [ -n "$still" ]; then
        log_err "Headers still missing after reinstall:$still"
        log_info "Their .debs may be unavailable on this image's apt mirror, or the"
        log_info "device is offline. Connect to the internet and re-run:"
        log_info "  sudo bash Install-TubeLite.sh --fix-headers"
        return 1
    fi
    log_ok "Build headers restored — 'make native' should compile now."
    return 0
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

# ---------- Compatibility wizard ----------
# Fix missing/broken libmpv (the #1 cause of "playback does nothing" on a
# stripped OS image):  sudo bash Install-TubeLite.sh --compat
if [ "$1" = "--compat" ] || [ "$1" = "--fix-libs" ]; then
    run_compat_wizard
    rc=$?
    echo ""
    if [ "$rc" -eq 0 ]; then
        echo -e "${GREEN}Compatibility check complete.${NC} Launch TubeLite to test playback."
    else
        echo -e "${YELLOW}Compatibility wizard could not fully fix playback.${NC} See the notes above."
    fi
    exit "$rc"
fi

# ---------- Build-header repair ----------
# Restore -dev headers that the OS image deleted while leaving dpkg's records
# intact, so on-device `make native` can compile:
#   sudo bash Install-TubeLite.sh --fix-headers
if [ "$1" = "--fix-headers" ] || [ "$1" = "--fix-build" ]; then
    fix_dev_headers
    rc=$?
    echo ""
    if [ "$rc" -eq 0 ]; then
        echo -e "${GREEN}Build headers OK.${NC} You can now run:  make native"
    else
        echo -e "${YELLOW}Some headers could not be restored.${NC} See the notes above."
    fi
    exit "$rc"
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
        THEME_CLEANED=0
        if [ -d "$BASE_THEME_ROOT" ]; then
            for theme_dir in "$BASE_THEME_ROOT"/*/; do
                [ -d "$theme_dir" ] || continue
                theme_name="$(basename "$theme_dir")"
                for sys_name in "$SYSTEM_NAME" "fire4arkos"; do
                    if [ -d "${theme_dir}${sys_name}" ]; then
                        rm -rf "${theme_dir}${sys_name}"
                        log_ok "  Removed ${sys_name} from $theme_name"
                        THEME_CLEANED=$((THEME_CLEANED + 1))
                    fi
                done
            done
            # Also remove any top-level system name directory
            if [ -d "$BASE_THEME_ROOT/$SYSTEM_NAME" ]; then
                rm -rf "$BASE_THEME_ROOT/$SYSTEM_NAME"
                THEME_CLEANED=$((THEME_CLEANED + 1))
            fi
            if [ "$THEME_CLEANED" -gt 0 ]; then
                log_ok "Removed theme assets from $THEME_CLEANED locations"
            else
                log_info "No theme assets found to remove"
            fi
        else
            log_info "Theme root $BASE_THEME_ROOT does not exist — nothing to remove"
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
        echo "8) Fix Playback / Compatibility (libmpv)"
        echo "9) Fix Build Headers (for on-device compile)"
        read -p "Enter choice [1-9]: " choice </dev/tty 2>/dev/null || choice="1"
    fi

    case "$choice" in
        1) log_info "Proceeding with Full Install..." ;;
        2) log_info "Proceeding with App Only Install..."; DO_THEME=0; DO_ES=0 ;;
        3) log_info "Proceeding with Theme Only Install..."; DO_DEPS=0; DO_BINARY=0; DO_FILES=0; DO_LAUNCHER=0; DO_ES=1; DO_THEME=1; DO_VERIFY=1 ;;
        4) exec bash "$0" "--uninstall" ;;
        5) exec bash "$0" "--uninstall-app" ;;
        6) exec bash "$0" "--uninstall-theme" ;;
        7) exit 0 ;;
        8) run_compat_wizard; exit $? ;;
        9) fix_dev_headers; exit $? ;;
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
    # To protect ArkOS custom/optimized held libraries (like libsdl2-2.0-0, libasound2, libmpv1, libmpv2, and display libraries),
    # we check each dependency and only invoke apt-get install for those that are missing.
    CRITICAL_DEPS="python3 libsdl2-2.0-0 libasound2 libsdl2-ttf-2.0-0 libharfbuzz0b libfreetype6 libegl1 libgles2 libdrm2 libgbm1"
    RUNTIME_DEPS=""
    for pkg in $CRITICAL_DEPS; do
        if dpkg -l "$pkg" 2>/dev/null | grep -q '^ii'; then
            log_info "Dependency $pkg is already installed. Keeping stock version."
        else
            RUNTIME_DEPS="$RUNTIME_DEPS $pkg"
        fi
    done

    # Dynamic check for libmpv1 / libmpv2 to avoid installation failure if one isn't in repositories
    HAS_MPV=0
    if dpkg -l "libmpv1" 2>/dev/null | grep -q '^ii'; then
        log_info "Dependency libmpv1 is already installed. Keeping stock version."
        HAS_MPV=1
    elif dpkg -l "libmpv2" 2>/dev/null | grep -q '^ii'; then
        log_info "Dependency libmpv2 is already installed. Keeping stock version."
        HAS_MPV=1
    fi

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

    # Install libmpv if missing
    if [ "$HAS_MPV" -eq 0 ]; then
        log_info "libmpv is missing. Trying to install libmpv2 or libmpv1..."
        apt-get install $APT_FLAGS libmpv2 2>/dev/null || apt-get install $APT_FLAGS libmpv1 2>/dev/null || true
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
    # Bundled portable libs (libmpv + codec chain) used as a runtime fallback
    # via LD_LIBRARY_PATH when the OS image is missing them.  See TubeLite.tbl.
    if [ -d "$SCRIPT_DIR/vendor/lib" ] && ls "$SCRIPT_DIR/vendor/lib"/*.so* >/dev/null 2>&1; then
        log_ok "Bundled fallback libs detected ($(ls -1 "$SCRIPT_DIR/vendor/lib"/*.so* 2>/dev/null | wc -l) in vendor/lib)"
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
        # Restore every header the native build needs (file-checked + force
        # --reinstall), since the image leaves dpkg records but deletes the
        # actual headers.  This is the comprehensive replacement for the old
        # piecemeal freetype/drm/sdl checks — it also covers harfbuzz, GLES2,
        # EGL and the mpv headers the build requires.
        fix_dev_headers || log_warn "Some build headers could not be restored; compile may fail."

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

    # ── Fallback fonts for the daemon's now-playing overlay ──────────────
    # The overlay's confirm / volume / mute toasts use symbol + media-
    # control glyphs (▲ ▼ ■ ▶ × ♪ ⏸) that Atkinson Hyperlegible lacks —
    # without a covering font they render as tofu boxes.  The daemon
    # loads a per-glyph fallback chain (Noto Sans / Noto Sans Symbols2 /
    # Noto Emoji / DejaVu); make sure at least one is available in
    # res/fonts so coverage doesn't depend on what the OS image ships.
    FONTS_DIR="$INSTALL_DIR/res/fonts"
    mkdir -p "$FONTS_DIR"
    # 1) Copy any matching system fonts we can find (cheap, offline).
    for fname in NotoSans-Regular NotoSansSymbols2-Regular NotoEmoji-Regular DejaVuSans; do
        if [ ! -f "$FONTS_DIR/$fname.ttf" ]; then
            found="$(find /usr/share/fonts -name "$fname.ttf" 2>/dev/null | head -1)"
            if [ -n "$found" ]; then
                cp -f "$found" "$FONTS_DIR/$fname.ttf" && \
                    log_ok "Provisioned font: $fname.ttf (from system)"
            fi
        fi
    done
    # 2) If the symbol-bearing Noto faces are still missing, fetch the
    #    specific TTFs from the notofonts CDN (best-effort; the daemon
    #    falls back to DejaVu for the common glyphs if this fails).
    declare -A NOTO_URLS=(
        [NotoSans-Regular]="https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSans/hinted/ttf/NotoSans-Regular.ttf"
        [NotoSansSymbols2-Regular]="https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSansSymbols2/hinted/ttf/NotoSansSymbols2-Regular.ttf"
        [NotoEmoji-Regular]="https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoEmoji/unhinted/ttf/NotoEmoji-Regular.ttf"
    )
    for fname in "${!NOTO_URLS[@]}"; do
        if [ ! -f "$FONTS_DIR/$fname.ttf" ]; then
            url="${NOTO_URLS[$fname]}"
            if wget -q --timeout=15 "$url" -O "$FONTS_DIR/$fname.ttf" 2>/dev/null || \
               curl -fsL --connect-timeout 15 "$url" -o "$FONTS_DIR/$fname.ttf" 2>/dev/null; then
                # Guard against a 0-byte / HTML error page being saved.
                if [ -s "$FONTS_DIR/$fname.ttf" ] && \
                   [ "$(wc -c < "$FONTS_DIR/$fname.ttf")" -gt 4096 ]; then
                    log_ok "Provisioned font: $fname.ttf (downloaded)"
                else
                    rm -f "$FONTS_DIR/$fname.ttf"
                    log_warn "Font download incomplete: $fname.ttf (overlay will use DejaVu fallback)"
                fi
            else
                rm -f "$FONTS_DIR/$fname.ttf" 2>/dev/null || true
                log_warn "Could not fetch $fname.ttf (offline?); overlay uses DejaVu fallback"
            fi
        fi
    done

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

# Bundled portable userland libs (libmpv + its codec chain, harvested from a
# known-good device).  These are NON-device libs only — GPU/ALSA/SDL/glibc are
# intentionally excluded so we never shadow the hardware-tuned copies ArkOS
# ships.  Append (not prepend) so a working system library still wins and the
# bundle only fills gaps on OS images that are missing libmpv.
for d in "\$SCRIPT_DIR/vendor/lib" "\$SCRIPT_DIR/lib"; do
    if [ -d "\$d" ]; then
        export LD_LIBRARY_PATH="\${LD_LIBRARY_PATH:+\$LD_LIBRARY_PATH:}\$d"
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
    THEME_SRC="$SCRIPT_DIR/theme"

    # ── Validate source assets before touching the target ────────────
    THEME_REQUIRED_FILES="theme.xml logo.png system.png blank.png"
    THEME_SRC_OK=1
    for f in $THEME_REQUIRED_FILES; do
        if [ ! -f "$THEME_SRC/$f" ]; then
            log_err "Required theme asset missing: $THEME_SRC/$f"
            THEME_SRC_OK=0
        fi
    done
    if [ "$THEME_SRC_OK" -eq 0 ]; then
        log_err "Theme source directory is incomplete — skipping theme install"
    elif [ ! -d "$BASE_THEME_ROOT" ]; then
        log_warn "Theme root $BASE_THEME_ROOT does not exist — skipping theme install"
    else
        THEME_OK=0
        THEME_FAIL=0
        THEME_TOTAL=0
        for theme_dir in "$BASE_THEME_ROOT"/*/; do
            [ -d "$theme_dir" ] || continue
            THEME_TOTAL=$((THEME_TOTAL + 1))
            theme_name="$(basename "$theme_dir")"
            target_dir="${theme_dir}${SYSTEM_NAME}"

            # Clean up legacy fire4arkos directory if present
            if [ -d "${theme_dir}fire4arkos" ]; then
                rm -rf "${theme_dir}fire4arkos" 2>/dev/null || true
                log_info "  Cleaned legacy fire4arkos dir from $theme_name"
            fi

            # Attempt to create the target directory
            if ! mkdir -p "$target_dir" 2>/dev/null; then
                log_warn "  $theme_name: cannot create $target_dir (read-only filesystem?) — skipped"
                THEME_FAIL=$((THEME_FAIL + 1))
                continue
            fi

            # Copy theme assets
            if cp -r "$THEME_SRC"/* "$target_dir/" 2>&1; then
                # Validate that the critical file landed
                if [ -f "$target_dir/theme.xml" ]; then
                    log_ok "  $theme_name: theme assets installed"
                    THEME_OK=$((THEME_OK + 1))
                else
                    log_warn "  $theme_name: copy appeared to succeed but theme.xml is missing"
                    THEME_FAIL=$((THEME_FAIL + 1))
                fi
            else
                log_warn "  $theme_name: copy failed"
                THEME_FAIL=$((THEME_FAIL + 1))
            fi
        done

        if [ "$THEME_TOTAL" -eq 0 ]; then
            log_warn "No theme directories found under $BASE_THEME_ROOT"
        elif [ "$THEME_FAIL" -eq 0 ]; then
            log_ok "Theme installed into all $THEME_OK theme directories"
        else
            log_warn "Theme installed into $THEME_OK of $THEME_TOTAL directories ($THEME_FAIL failed)"
        fi
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

if [ "$DO_ES" -eq 1 ]; then
    log_step "6.7/7" "Running ALSA audio compatibility checks..."
    if [ -f "$SCRIPT_DIR/scripts/alsa_compat.py" ]; then
        python3 "$SCRIPT_DIR/scripts/alsa_compat.py" || log_warn "ALSA audio compatibility checks failed or were interrupted."
    else
        log_warn "ALSA audio compatibility helper scripts/alsa_compat.py not found, skipping."
    fi
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

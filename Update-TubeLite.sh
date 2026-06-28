#!/bin/bash
# ============================================================================
# TubeLite Self-Preserving OTA Updater
# ============================================================================

# ---------- Self-Elevation ----------
if [ "$(id -u)" -ne 0 ]; then
    echo "TubeLite Updater needs root privileges. Elevating..."
    sudo -E bash "$0" "$@"
    exit $?
fi

# ---------- Self-Staging (Self-Preserving) ----------
# To avoid file lock/overwrite issues during update (especially for this script itself),
# we copy ourselves to /tmp/ and run the actual update logic from there.
if [ "${1:-}" != "--stage2" ]; then
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    echo "[updater] Copying updater to temp staging..."
    cp -f "$0" /tmp/Update-TubeLite.sh
    chmod +x /tmp/Update-TubeLite.sh
    exec bash /tmp/Update-TubeLite.sh --stage2 "$SCRIPT_DIR" "$@"
fi

shift # Remove --stage2
TARGET_DIR="$1"
shift # Remove original SCRIPT_DIR

if [ -z "$TARGET_DIR" ] || [ ! -d "$TARGET_DIR" ]; then
    echo "ERROR: Invalid target directory: $TARGET_DIR"
    exit 1
fi

set -euo pipefail

# Logging helpers
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

log_step() { echo -e "\n${CYAN}${BOLD}[updater]${NC} $1"; }
log_ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
log_warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
log_err()  { echo -e "  ${RED}✗${NC} $1"; }
log_info() { echo -e "  $1"; }

normalize_version() {
    echo "$1" | tr '[:upper:]' '[:lower:]' | sed -E 's/^tubelite[[:space:]]+//g' | sed -E 's/^v//g' | tr -d ' \t\r\n'
}

is_semver() {
    local v=$(echo "$1" | sed 's/-.*//')
    [[ "$v" =~ ^[0-9]+(\.[0-9]+)*$ ]]
}

compare_versions() {
    local v1=$1
    local v2=$2

    # Split into arrays
    local IFS='.'
    local arr1
    local arr2
    read -r -a arr1 <<< "$v1"
    read -r -a arr2 <<< "$v2"

    # Pad arrays to have at least 3 elements
    for ((i=${#arr1[@]}; i<3; i++)); do arr1[i]=0; done
    for ((i=${#arr2[@]}; i<3; i++)); do arr2[i]=0; done

    for i in 0 1 2; do
        # Strip any non-numeric suffixes (like -beta, -stable)
        local val1=$(echo "${arr1[i]}" | sed 's/[^0-9].*//')
        local val2=$(echo "${arr2[i]}" | sed 's/[^0-9].*//')

        # If empty, treat as 0
        val1=${val1:-0}
        val2=${val2:-0}

        if (( val1 > val2 )); then
            return 1 # local is newer (downgrade)
        elif (( val1 < val2 )); then
            return 2 # local is older (upgrade)
        fi
    done
    return 0 # equal (reinstall)
}

log_step "Fetching latest release URL from GitHub..."

# Inline Python to query GitHub API and extract download URL, tag name, and version manifest
RESPONSE=$(python3 - <<'EOF'
import urllib.request
import json
import sys

try:
    req = urllib.request.Request(
        'https://api.github.com/repos/Cheemsdoge28/r36tube/releases/latest',
        headers={'User-Agent': 'TubeLite-Updater'}
    )
    with urllib.request.urlopen(req) as response:
        data = json.loads(response.read().decode())
        tag_name = data.get('tag_name', '')
        download_url = None
        manifest_url = ""
        for asset in data.get('assets', []):
            name = asset.get('name', '').lower()
            if name.endswith('.zip') and 'compat' not in name:
                download_url = asset.get('browser_download_url')
            elif name == 'version.json':
                manifest_url = asset.get('browser_download_url', '')
        if not download_url:
            download_url = data.get('zipball_url')
        if download_url:
            print(f"{tag_name}|{download_url}|{manifest_url}")
            sys.exit(0)
        else:
            sys.stderr.write("No download URL found\n")
            sys.exit(1)
except Exception as e:
    sys.stderr.write(f"Error: {e}\n")
    sys.exit(1)
EOF
)

if [ -z "$RESPONSE" ] || [[ "$RESPONSE" != *"|"* ]]; then
    log_err "Failed to retrieve the latest release information from GitHub."
    exit 1
fi

TAG_NAME=$(echo "$RESPONSE" | cut -d'|' -f1)
DOWNLOAD_URL=$(echo "$RESPONSE" | cut -d'|' -f2)
MANIFEST_URL=$(echo "$RESPONSE" | cut -d'|' -f3)

if [ -z "$DOWNLOAD_URL" ]; then
    log_err "Failed to retrieve the latest download URL from GitHub."
    exit 1
fi

log_ok "Latest release tag: $TAG_NAME"

# ---------- Version Check ----------
LOCAL_VERSION=""
if [ -f "$TARGET_DIR/VERSION" ]; then
    LOCAL_VERSION=$(cat "$TARGET_DIR/VERSION" | tr -d '\r\n')
fi

NORM_LOCAL=$(normalize_version "$LOCAL_VERSION")
if [ -z "$NORM_LOCAL" ]; then
    NORM_LOCAL="0.0.0"
fi

REMOTE_VERSION=""
REMOTE_TAG=""
REMOTE_COMMIT=""

if [ -n "$MANIFEST_URL" ]; then
    log_info "Fetching version manifest..."
    MANIFEST_PATH="/tmp/tubelite_version.json"
    rm -f "$MANIFEST_PATH"
    
    FETCHED=0
    if command -v wget >/dev/null 2>&1; then
        wget -q --timeout=10 "$MANIFEST_URL" -O "$MANIFEST_PATH" && FETCHED=1 || true
    fi
    if [ "$FETCHED" -eq 0 ]; then
        curl -fsL --connect-timeout 10 "$MANIFEST_URL" -o "$MANIFEST_PATH" && FETCHED=1 || true
    fi
    
    if [ -f "$MANIFEST_PATH" ] && [ -s "$MANIFEST_PATH" ]; then
        PARSED=$(python3 - "$MANIFEST_PATH" <<'PYEOF' 2>/dev/null || true
import json, sys
try:
    with open(sys.argv[1]) as f:
        d = json.load(f)
    force_inst = "true" if d.get("force_install") in (True, 1, "true", "1") else "false"
    # Plain %-formatting, NOT an f-string.  An f-string with quoted dict keys
    # needs backslash-escaped quotes inside `python3 -c '...'`, which is a
    # SyntaxError on Python < 3.12 (the device's eoan Python is far older) and
    # silently broke manifest parsing — making the updater fall back to the tag
    # name on every run.  A quoted heredoc keeps the quotes literal and the
    # %-format avoids backslashes entirely.
    print("%s|%s|%s|%s" % (
        d.get("version", ""), d.get("tag", ""), d.get("commit", ""), force_inst))
except Exception:
    pass
PYEOF
)
        
        if [ -n "$PARSED" ] && [[ "$PARSED" == *"|"* ]]; then
            REMOTE_VERSION=$(echo "$PARSED" | cut -d'|' -f1)
            REMOTE_TAG=$(echo "$PARSED" | cut -d'|' -f2)
            REMOTE_COMMIT=$(echo "$PARSED" | cut -d'|' -f3)
            REMOTE_FORCE_INSTALL=$(echo "$PARSED" | cut -d'|' -f4)
        fi
    fi
    rm -f "$MANIFEST_PATH"
fi

if [ -n "$REMOTE_VERSION" ]; then
    log_ok "Retrieved remote version: $REMOTE_VERSION (tag: $REMOTE_TAG)"
else
    log_warn "Could not retrieve or parse version manifest. Falling back to tag name."
    REMOTE_VERSION="$TAG_NAME"
    REMOTE_TAG="$TAG_NAME"
fi

NORM_REMOTE=$(normalize_version "$REMOTE_VERSION")

# Read local commit hash if version.json exists locally
LOCAL_COMMIT=""
if [ -f "$TARGET_DIR/version.json" ]; then
    LOCAL_COMMIT=$(python3 -c '
import json, sys
try:
    with open(sys.argv[1]) as f:
        print(json.load(f).get("commit", ""))
except Exception:
    pass
' "$TARGET_DIR/version.json" 2>/dev/null || true)
fi

# Perform comparison
COMP_RESULT=0
if is_semver "$NORM_LOCAL" && is_semver "$NORM_REMOTE"; then
    set +e
    compare_versions "$NORM_LOCAL" "$NORM_REMOTE"
    COMP_RESULT=$?
    set -e
else
    # Fallback to string check
    if [ "$NORM_LOCAL" = "$NORM_REMOTE" ]; then
        COMP_RESULT=0
    else
        COMP_RESULT=2 # treat as upgrade / different version
    fi
fi

VERSION_TYPE=""
if [ "$COMP_RESULT" -eq 2 ]; then
    VERSION_TYPE="upgrade"
elif [ "$COMP_RESULT" -eq 1 ]; then
    VERSION_TYPE="downgrade"
else
    VERSION_TYPE="reinstall"
fi

# ---------- Confirmation prompt ----------
# Headless / scripted override: `--yes` (also -y / --non-interactive) skips the
# interactive menu and proceeds with the recommended forward action.  This is
# the supported recovery path on devices whose shipped updater had a broken
# menu, and for any SSH/automation use where there's no controller to press.
ASSUME_YES=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y|--non-interactive|--assume-yes) ASSUME_YES=1 ;;
    esac
done

CHOICE=""
if [ "$ASSUME_YES" -eq 1 ]; then
    case "$VERSION_TYPE" in
        upgrade)   CHOICE=0 ;;   # option 0 = Upgrade
        downgrade) CHOICE=1 ;;   # option 1 = Downgrade
        *)         CHOICE=1 ;;   # reinstall: option 1 = Reinstall
    esac
    log_info "Non-interactive (--yes): proceeding with $VERSION_TYPE."
else
# Interactive controller/keyboard menu.
#
# The program is passed via `python3 -c "$MENU_SRC"`, NOT `python3 - <<EOF`.
# With the heredoc form Python reads its SCRIPT from stdin, which leaves stdin
# at EOF — so the menu's input poll saw an instant EOF, took the non-tty
# branch, and auto-cancelled on the very first loop (the js0 controller never
# got a turn).  That's why the prompt cancelled itself with no keypress on
# EVERY device.  Passing the source as a -c argument keeps stdin connected to
# the real terminal (/dev/tty1 on-device, the SSH pty over ssh) so the D-pad
# (js0) and arrow/Enter keys both work.
MENU_SRC=$(cat <<'EOF'
import struct, os, sys, time, select

def main():
    action_type = sys.argv[1]
    local_ver = sys.argv[2]
    remote_ver = sys.argv[3]
    
    if action_type == "upgrade":
        title = "New version available!"
        desc = f"Local: {local_ver} -> Remote: {remote_ver}"
        options = [
            "Upgrade to remote version (Recommended)",
            "Back to EmulationStation / Exit"
        ]
        selected = 0
    elif action_type == "downgrade":
        title = "Local version is newer than remote!"
        desc = f"Local: {local_ver} -> Remote: {remote_ver}"
        options = [
            "Back to EmulationStation / Exit (Recommended)",
            "Downgrade to remote version"
        ]
        selected = 0
    else: # reinstall
        title = "Current version is already up to date."
        desc = f"Version: {local_ver}"
        options = [
            "Back to EmulationStation / Exit (Recommended)",
            "Reinstall version"
        ]
        selected = 0

    js = None
    try:
        js_fd = os.open('/dev/input/js0', os.O_RDONLY | os.O_NONBLOCK)
        js = os.fdopen(js_fd, 'rb')
    except Exception:
        pass

    old_settings = None
    fd_stdin = sys.stdin.fileno()
    try:
        import tty, termios
        if os.isatty(fd_stdin):
            old_settings = termios.tcgetattr(fd_stdin)
            tty.setraw(fd_stdin)
    except Exception:
        pass

    def print_menu():
        sys.stderr.write("\033[H\033[J")
        sys.stderr.write(f"\033[1;36m=== {title} ===\033[0m\n")
        sys.stderr.write(f"{desc}\n\n")
        sys.stderr.write("Use DPAD or Arrow Keys to move, A or Enter to select.\n\n")
        for i, opt in enumerate(options):
            if i == selected:
                sys.stderr.write(f" \033[1;32m-> [{opt}]\033[0m\n")
            else:
                sys.stderr.write(f"    {opt}\n")
        sys.stderr.flush()

    try:
        print_menu()
        while True:
            inputs = []
            if js:
                inputs.append(js)
            inputs.append(sys.stdin)
            
            r, _, _ = select.select(inputs, [], [], 0.1)
            
            if js in r:
                data = js.read(8)
                if data and len(data) == 8:
                    t, val, type_evt, num = struct.unpack('IhBB', data)
                    if type_evt == 1 and val == 1: # Button Down
                        if num == 1: # A button
                            return selected
                        if num == 0: # B button
                            return 1 if action_type == "upgrade" else 0
                        if num == 8: # UP
                            selected = (selected - 1) % len(options)
                            print_menu()
                        if num == 9: # DOWN
                            selected = (selected + 1) % len(options)
                            print_menu()
            
            if sys.stdin in r:
                if old_settings:
                    char = sys.stdin.read(1)
                    if char == '\x1b':
                        r_seq, _, _ = select.select([sys.stdin], [], [], 0.05)
                        if r_seq:
                            seq2 = sys.stdin.read(1)
                            if seq2 == '[':
                                seq3 = sys.stdin.read(1)
                                if seq3 == 'A':
                                    selected = (selected - 1) % len(options)
                                    print_menu()
                                elif seq3 == 'B':
                                    selected = (selected + 1) % len(options)
                                    print_menu()
                    elif char in ('\r', '\n'):
                        return selected
                    elif char.isdigit():
                        val = int(char)
                        if 1 <= val <= len(options):
                            return val - 1
                    elif char in ('q', 'Q'):
                        return 1 if action_type == "upgrade" else 0
                else:
                    line = sys.stdin.readline().strip()
                    if line.isdigit():
                        val = int(line)
                        if 1 <= val <= len(options):
                            return val - 1
                    elif line in ('y', 'Y'):
                        return 0 if action_type == "upgrade" else 1
                    else:
                        return 1 if action_type == "upgrade" else 0
            time.sleep(0.01)
    finally:
        if old_settings:
            termios.tcsetattr(fd_stdin, termios.TCSADRAIN, old_settings)

if __name__ == '__main__':
    print(main())
EOF
)
    set +e
    CHOICE=$(python3 -c "$MENU_SRC" "$VERSION_TYPE" "$LOCAL_VERSION" "$REMOTE_VERSION")
    set -e
fi

if [ -z "$CHOICE" ]; then
    if [ "$VERSION_TYPE" = "upgrade" ]; then CHOICE="1"; else CHOICE="0"; fi
fi

PROMPT_PROCEED=0
if [ "$VERSION_TYPE" = "upgrade" ]; then
    if [ "$CHOICE" -eq 0 ]; then
        PROMPT_PROCEED=1
    else
        log_step "Upgrade cancelled. Backing out of updater..."
    fi
elif [ "$VERSION_TYPE" = "downgrade" ]; then
    if [ "$CHOICE" -eq 1 ]; then
        PROMPT_PROCEED=1
    else
        log_step "Downgrade cancelled. Backing out of updater..."
    fi
else
    if [ "$CHOICE" -eq 1 ]; then
        PROMPT_PROCEED=1
    else
        log_step "Reinstallation cancelled. Backing out of updater..."
    fi
fi

if [ "$PROMPT_PROCEED" -ne 1 ]; then
    echo "Returning to EmulationStation in 3 seconds..."
    sleep 3
    exit 0
fi

log_info "Proceeding with update/reinstallation..."

USE_DELTA=0
if [ "$COMP_RESULT" -ne 0 ] && [ -n "$LOCAL_COMMIT" ] && [ -n "$REMOTE_COMMIT" ]; then
    # We do delta updates for upgrades/downgrades but not for reinstalls (force reinstall to do full recovery)
    USE_DELTA=1
fi

ZIP_PATH=""
EXTRACT_DIR=""
CHANGED_FILES=""
DELETED_FILES=""

if [ "$USE_DELTA" -eq 1 ]; then
    log_step "Computing delta update from commit $LOCAL_COMMIT to $REMOTE_COMMIT..."
    
    COMPARISON_OUTPUT=$(python3 - <<'EOF'
import urllib.request
import json
import sys

try:
    local_commit = sys.argv[1]
    remote_commit = sys.argv[2]
    url = f"https://api.github.com/repos/Cheemsdoge28/r36tube/compare/{local_commit}...{remote_commit}"
    req = urllib.request.Request(url, headers={'User-Agent': 'TubeLite-Updater'})
    with urllib.request.urlopen(req) as resp:
        data = json.load(resp)
        files = data.get("files", [])
        prefix_release = "dist/release/TubeLite/"
        prefix_releases = "dist/releases/TubeLite/"
        changed = []
        deleted = []
        for f in files:
            fname = f.get("filename", "")
            status = f.get("status", "")
            ref_prefix = None
            if fname.startswith(prefix_release):
                ref_prefix = prefix_release
            elif fname.startswith(prefix_releases):
                ref_prefix = prefix_releases
                
            if ref_prefix:
                rel_path = fname[len(ref_prefix):]
                if status in ("modified", "added"):
                    changed.append(f"{rel_path}:{fname}")
                elif status == "removed":
                    deleted.append(rel_path)
        print(",".join(changed))
        print(",".join(deleted))
except Exception as e:
    sys.stderr.write(f"Compare error: {e}\n")
    sys.exit(1)
EOF
"$LOCAL_COMMIT" "$REMOTE_COMMIT" 2>/dev/null || echo "")

    if [ -n "$COMPARISON_OUTPUT" ]; then
        CHANGED_FILES=$(echo "$COMPARISON_OUTPUT" | head -n 1)
        DELETED_FILES=$(echo "$COMPARISON_OUTPUT" | tail -n +2 | head -n 1)
        
        log_info "Delta check complete."
        IFS=',' read -ra ADDR <<< "$CHANGED_FILES"
        CHANGED_COUNT=0
        for item in "${ADDR[@]}"; do
            if [ -n "$item" ]; then
                CHANGED_COUNT=$((CHANGED_COUNT + 1))
            fi
        done
        log_ok "$CHANGED_COUNT file(s) changed/added."
    else
        log_warn "Failed to compare commits. Falling back to full package update."
        USE_DELTA=0
    fi
fi

if [ "$USE_DELTA" -eq 0 ]; then
    log_step "Downloading update package..."
    ZIP_PATH="/tmp/TubeLite_latest.zip"
    rm -f "$ZIP_PATH"
    
    if command -v wget >/dev/null 2>&1; then
        wget -q --show-progress --timeout=20 "$DOWNLOAD_URL" -O "$ZIP_PATH" || \
        curl -fsL --connect-timeout 20 "$DOWNLOAD_URL" -o "$ZIP_PATH"
    else
        curl -fsL --connect-timeout 20 "$DOWNLOAD_URL" -o "$ZIP_PATH"
    fi
    
    if [ ! -f "$ZIP_PATH" ] || [ ! -s "$ZIP_PATH" ]; then
        log_err "Download failed or package file is empty."
        exit 1
    fi
    log_ok "Download complete."
    
    log_step "Validating update package..."
    if ! python3 -c "import zipfile, sys; zipfile.ZipFile(sys.argv[1]).testzip()" "$ZIP_PATH" 2>/dev/null; then
        log_err "Downloaded zip file is corrupt or invalid."
        exit 1
    fi
    log_ok "Package validation passed."
    
    log_step "Extracting update package..."
    EXTRACT_DIR="/tmp/tubelite_extracted"
    rm -rf "$EXTRACT_DIR"
    mkdir -p "$EXTRACT_DIR"
    
    if command -v unzip >/dev/null 2>&1; then
        unzip -o -q "$ZIP_PATH" -d "$EXTRACT_DIR" || true
    else
        python3 -c "import zipfile, sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$ZIP_PATH" "$EXTRACT_DIR"
    fi
    
    SRC_DIR=""
    if [ -d "$EXTRACT_DIR/TubeLite" ] && [ -f "$EXTRACT_DIR/TubeLite/Install-TubeLite.sh" ]; then
        SRC_DIR="$EXTRACT_DIR/TubeLite"
    else
        FOUND_DIR=$(find "$EXTRACT_DIR" -type f -name "Install-TubeLite.sh" -print -quit | xargs dirname 2>/dev/null || true)
        if [ -n "$FOUND_DIR" ] && [ -d "$FOUND_DIR" ]; then
            SRC_DIR="$FOUND_DIR"
        fi
    fi
    
    if [ -z "$SRC_DIR" ] || [ ! -d "$SRC_DIR" ]; then
        log_err "Invalid package structure: Install-TubeLite.sh not found."
        rm -rf "$EXTRACT_DIR"
        exit 1
    fi
    log_ok "Layout verified: $SRC_DIR"
fi

log_step "Preserving user configurations..."
PRESERVE_DIR="/tmp/tubelite_preserve"
rm -rf "$PRESERVE_DIR"
mkdir -p "$PRESERVE_DIR"

PRESERVE_ITEMS=(
    "settings.json"
    "cookies.txt"
    "cache"
)

PRESERVED_COUNT=0
for item in "${PRESERVE_ITEMS[@]}"; do
    if [ -e "$TARGET_DIR/$item" ]; then
        cp -r "$TARGET_DIR/$item" "$PRESERVE_DIR/"
        log_info "Preserved $item"
        PRESERVED_COUNT=$((PRESERVED_COUNT + 1))
    fi
done
log_ok "Preserved $PRESERVED_COUNT item(s)."

log_step "Stopping background services..."
if [ -f "/dev/shm/tubed.pid" ]; then
    PID=$(cat /dev/shm/tubed.pid)
    log_info "Stopping tubed PID $PID..."
    kill -15 "$PID" 2>/dev/null || true
    sleep 1
fi
pkill -9 -f "tubed.py" 2>/dev/null || true
log_ok "Services stopped."

log_step "Applying update..."
BACKUP_DIR="${TARGET_DIR}.old"
rm -rf "$BACKUP_DIR"

if [ "$USE_DELTA" -eq 1 ]; then
    log_info "Backing up current installation..."
    cp -r "$TARGET_DIR" "$BACKUP_DIR"
    
    log_info "Downloading changed files..."
    set +e
    IFS=',' read -ra ADDR <<< "$CHANGED_FILES"
    DOWNLOAD_FAILED=0
    for item in "${ADDR[@]}"; do
        if [ -n "$item" ]; then
            rel_path="${item%%:*}"
            repo_path="${item#*:}"
            log_info "  Updating $rel_path"
            
            target_file="$TARGET_DIR/$rel_path"
            mkdir -p "$(dirname "$target_file")"
            
            raw_url="https://raw.githubusercontent.com/Cheemsdoge28/r36tube/${REMOTE_COMMIT}/${repo_path}"
            
            FETCHED=0
            if command -v wget >/dev/null 2>&1; then
                wget -q --timeout=15 "$raw_url" -O "$target_file" && FETCHED=1 || true
            fi
            if [ "$FETCHED" -eq 0 ]; then
                curl -fsL --connect-timeout 15 "$raw_url" -o "$target_file" && FETCHED=1 || true
            fi
            
            if [ "$FETCHED" -eq 0 ] || [ ! -f "$target_file" ]; then
                log_err "Failed to download $rel_path"
                DOWNLOAD_FAILED=1
                break
            fi
            
            if [[ "$rel_path" == *.sh ]] || [[ "$rel_path" == *.tbl ]] || [[ "$rel_path" == build/* ]] || [[ "$rel_path" == bin/* ]]; then
                chmod +x "$target_file"
            fi
        fi
    done
    
    if [ "$DOWNLOAD_FAILED" -eq 0 ]; then
        log_info "Removing obsolete files..."
        IFS=',' read -ra DEL_ADDR <<< "$DELETED_FILES"
        for file in "${DEL_ADDR[@]}"; do
            if [ -n "$file" ]; then
                target_file="$TARGET_DIR/$file"
                if [ -f "$target_file" ]; then
                    log_info "  Deleting $file"
                    rm -f "$target_file"
                fi
            fi
        done
        log_ok "Delta update applied successfully."
        set -e
    else
        log_err "Delta update failed during download. Rolling back..."
        rm -rf "$TARGET_DIR"
        mv "$BACKUP_DIR" "$TARGET_DIR"
        set -e
        exit 1
    fi
else
    # Move old folder aside as backup
    mv "$TARGET_DIR" "$BACKUP_DIR"
    if cp -r "$SRC_DIR" "$TARGET_DIR"; then
        log_ok "Files replaced successfully."
    else
        log_err "Failed to replace files. Rolling back..."
        rm -rf "$TARGET_DIR"
        mv "$BACKUP_DIR" "$TARGET_DIR"
        exit 1
    fi
fi

log_step "Restoring preserved configurations..."
for item in "${PRESERVE_ITEMS[@]}"; do
    if [ -e "$PRESERVE_DIR/$item" ]; then
        rm -rf "$TARGET_DIR/$item"
        cp -r "$PRESERVE_DIR/$item" "$TARGET_DIR/"
        log_info "Restored $item"
    fi
done
log_ok "Configurations restored."

log_step "Finalizing installation..."
RUN_INSTALLER=0

if [ "$USE_DELTA" -eq 1 ]; then
    if [[ ",$CHANGED_FILES," == *",Install-TubeLite.sh:"* ]] || [[ "$CHANGED_FILES" == "Install-TubeLite.sh:"* ]]; then
        log_info "Installer script changed. Will execute unified installer."
        RUN_INSTALLER=1
    fi
else
    RUN_INSTALLER=1
fi

if [ "${REMOTE_FORCE_INSTALL:-}" = "true" ]; then
    log_info "Remote manifest requested forced installation."
    RUN_INSTALLER=1
fi

if [ "$RUN_INSTALLER" -eq 1 ]; then
    if bash "$TARGET_DIR/Install-TubeLite.sh" --non-interactive; then
        log_ok "Installation script executed successfully."
    else
        log_warn "Installation script completed with errors."
    fi
else
    log_info "No installer or dependency changes. Skipping unified installer checks."
fi

# Cleanup
rm -rf "$BACKUP_DIR"
if [ "$USE_DELTA" -eq 0 ]; then
    rm -rf "$EXTRACT_DIR"
    rm -f "$ZIP_PATH"
fi
rm -rf "$PRESERVE_DIR"

echo -e "\n${GREEN}${BOLD}Update Finished successfully!${NC}"
echo "Restart EmulationStation to apply any launcher changes."
echo "Returning to EmulationStation in 5 seconds..."
sleep 5

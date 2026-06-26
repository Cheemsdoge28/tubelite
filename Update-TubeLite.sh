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
        PARSED=$(python3 -c '
import json, sys
try:
    with open(sys.argv[1]) as f:
        d = json.load(f)
        print(f"{d.get(\"version\",\"\")}|{d.get(\"tag\",\"\")}")
except Exception:
    pass
' "$MANIFEST_PATH" 2>/dev/null || true)
        
        if [ -n "$PARSED" ] && [[ "$PARSED" == *"|"* ]]; then
            REMOTE_VERSION="${PARSED%%|*}"
            REMOTE_TAG="${PARSED#*|}"
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

PROMPT_PROCEED=0
if [ "$COMP_RESULT" -eq 2 ]; then
    log_info "New version available (Local: $LOCAL_VERSION, Remote: $REMOTE_VERSION)"
    echo -n -e "${CYAN}${BOLD}Would you like to upgrade to $REMOTE_VERSION? (Y/n): ${NC}"
    read -r -n 1 -t 30 ANSWER || ANSWER="y"
    echo ""
    if [ -z "$ANSWER" ] || [[ "$ANSWER" =~ ^[yY\ ]$ ]]; then
        PROMPT_PROCEED=1
    else
        log_step "Upgrade cancelled. Backing out of updater..."
    fi
elif [ "$COMP_RESULT" -eq 1 ]; then
    log_warn "Local version ($LOCAL_VERSION) is newer than the remote version ($REMOTE_VERSION)."
    echo -n -e "${YELLOW}${BOLD}Would you like to downgrade to $REMOTE_VERSION? (y/N): ${NC}"
    read -r -n 1 -t 30 ANSWER || ANSWER="n"
    echo ""
    if [[ "$ANSWER" =~ ^[yY]$ ]]; then
        PROMPT_PROCEED=1
    else
        log_step "Downgrade cancelled. Backing out of updater..."
    fi
else
    log_warn "Current version ($LOCAL_VERSION) is already up to date with the latest release ($REMOTE_VERSION)."
    echo -n -e "${YELLOW}${BOLD}Would you like to reinstall anyway? (y/N): ${NC}"
    read -r -n 1 -t 30 ANSWER || ANSWER="n"
    echo ""
    if [[ "$ANSWER" =~ ^[yY]$ ]]; then
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

# Detect staging layout (whether zip has nested TubeLite folder or files at root)
SRC_DIR=""
if [ -d "$EXTRACT_DIR/TubeLite" ] && [ -f "$EXTRACT_DIR/TubeLite/Install-TubeLite.sh" ]; then
    SRC_DIR="$EXTRACT_DIR/TubeLite"
else
    # Find directory containing Install-TubeLite.sh
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

# Move old folder aside as backup
mv "$TARGET_DIR" "$BACKUP_DIR"

# Copy new folder into place
if cp -r "$SRC_DIR" "$TARGET_DIR"; then
    log_ok "Files replaced successfully."
else
    log_err "Failed to replace files. Rolling back..."
    rm -rf "$TARGET_DIR"
    mv "$BACKUP_DIR" "$TARGET_DIR"
    exit 1
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
# Run the installer non-interactively to finish registration, compile if needed, patch RA etc.
if bash "$TARGET_DIR/Install-TubeLite.sh" --non-interactive; then
    log_ok "Installation script executed successfully."
else
    log_warn "Installation script completed with errors."
fi

# Cleanup
rm -rf "$BACKUP_DIR"
rm -rf "$EXTRACT_DIR"
rm -rf "$PRESERVE_DIR"
rm -f "$ZIP_PATH"

echo -e "\n${GREEN}${BOLD}Update Finished successfully!${NC}"
echo "Restart EmulationStation to apply any launcher changes."
echo "Returning to EmulationStation in 5 seconds..."
sleep 5

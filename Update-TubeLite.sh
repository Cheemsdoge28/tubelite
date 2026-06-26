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

log_step "Fetching latest release URL from GitHub..."

# Inline Python to query GitHub API and extract download URL
DOWNLOAD_URL=$(python3 - <<'EOF'
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
        download_url = None
        for asset in data.get('assets', []):
            if asset.get('name', '').lower().endswith('.zip'):
                download_url = asset.get('browser_download_url')
                break
        if not download_url:
            download_url = data.get('zipball_url')
        if download_url:
            print(download_url)
            sys.exit(0)
        else:
            sys.stderr.write("No download URL found\n")
            sys.exit(1)
except Exception as e:
    sys.stderr.write(f"Error: {e}\n")
    sys.exit(1)
EOF
)

if [ -z "$DOWNLOAD_URL" ]; then
    log_err "Failed to retrieve the latest download URL from GitHub."
    exit 1
fi

log_ok "Download URL: $DOWNLOAD_URL"

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
        cp -rp "$TARGET_DIR/$item" "$PRESERVE_DIR/"
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
if cp -rp "$SRC_DIR" "$TARGET_DIR"; then
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
        cp -rp "$PRESERVE_DIR/$item" "$TARGET_DIR/"
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

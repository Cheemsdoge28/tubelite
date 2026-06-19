#!/usr/bin/env bash
# Cross-build TubeLite for the R36S (aarch64) from any host with Docker.
#
# First run: builds the image (~5-10 min, ~1.5 GB).
# Subsequent runs: reuses the cached image, build itself takes ~10-30s on a
# decent host CPU vs. several minutes on the device.
#
# Usage:
#   tools/cross/build.sh                    # full release build
#   tools/cross/build.sh native-dev         # fast iteration (no LTO, -O1)
#   tools/cross/build.sh clean              # clean
#   tools/cross/build.sh deploy <user@host> # build + scp to device
set -euo pipefail

IMAGE=tubelite-cross
DOCKERFILE=tools/cross/Dockerfile.aarch64
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Build the image if it doesn't exist locally.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building cross-build image (first run only, ~5-10 min)..."
    docker build -f "$PROJECT_ROOT/$DOCKERFILE" -t "$IMAGE" "$PROJECT_ROOT"
fi

cmd="${1:-arm64}"

case "$cmd" in
    deploy)
        target="${2:-}"
        if [[ -z "$target" ]]; then
            echo "usage: $0 deploy <user@host>"
            exit 1
        fi
        docker run --rm -v "$PROJECT_ROOT":/src "$IMAGE" make arm64
        echo "Deploying to $target:/roms/tools/tubelite/tubelite ..."
        scp "$PROJECT_ROOT/build/tubelite.arm64" "$target:/roms/tools/tubelite/tubelite"
        ;;
    *)
        docker run --rm -v "$PROJECT_ROOT":/src "$IMAGE" make "$cmd"
        ;;
esac

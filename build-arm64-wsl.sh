#!/usr/bin/env bash
set -euo pipefail
#
#  build-arm64-wsl.sh — cross-compile TubeLite for the R36S (RK3326, Cortex-A35)
#  ---------------------------------------------------------------------------
#  Maximum-optimization build. Compiles every translation unit in src/ (except
#  the standalone probe_planes.cpp tool) in a single g++ invocation so the
#  optimizer and LTO can work across the whole program, then strips.
#
#  Targeting: RK3326 is quad Cortex-A35 (ARMv8-A). -mcpu/-mtune=cortex-a35 gives
#  the compiler the exact pipeline model. -O3 + -flto + dead-section GC squeeze
#  out the most throughput and the smallest binary. We deliberately do NOT use
#  -ffast-math: mpv/AV sync and timestamp math must stay IEEE-correct.
#

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$REPO_ROOT/build"
OUTPUT="$BUILD_DIR/browser.arm64"

mkdir -p "$BUILD_DIR"

# 🔥 FULL SYSROOT
SYSROOT="${SYSROOT:-/mnt/d/C/sysroot-debian10}"

if [[ ! -d "$SYSROOT/usr" ]]; then
    echo "Invalid SYSROOT: $SYSROOT" >&2
    exit 1
fi

if ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
    echo "Missing aarch64-linux-gnu-g++" >&2
    exit 1
fi

echo "Using sysroot: $SYSROOT"

# Force the toolchain to use the sysroot for both headers and libs.
GCC_FLAGS=(
    --sysroot="$SYSROOT"
    -B"$SYSROOT/usr/lib/aarch64-linux-gnu"
    -B"$SYSROOT/lib/aarch64-linux-gnu"
)

INCLUDE_FLAGS=(
    -I"$SYSROOT/usr/include"
    -I"$SYSROOT/usr/include/aarch64-linux-gnu"
    -I"$SYSROOT/usr/include/freetype2"   # ft2build.h / FT_FREETYPE_H
    -I"$SYSROOT/usr/include/libdrm"      # xf86drm.h, drm/drm_fourcc.h
)

LIB_FLAGS=(
    -L"$SYSROOT/usr/lib/aarch64-linux-gnu"
    -L"$SYSROOT/lib/aarch64-linux-gnu"
    -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu"
    -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu"
    -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1
)

# Optimization flags — global, aggressive, but numerically safe.
OPT_FLAGS=(
    -O3
    -flto
    -mcpu=cortex-a35 -mtune=cortex-a35
    -fno-plt
    -fomit-frame-pointer
    -ffunction-sections -fdata-sections
    -pthread
)

LINK_OPT_FLAGS=(
    -Wl,--gc-sections          # drop unreferenced sections (pairs with -ffunction/-fdata-sections)
    -Wl,--as-needed
)

# Every app source except the standalone plane-probe tool (it has its own main).
mapfile -t SOURCES < <(ls "$REPO_ROOT"/src/*.cpp | grep -v '/probe_planes\.cpp$')

echo "Sources:"
printf '  %s\n' "${SOURCES[@]}"
echo ""

aarch64-linux-gnu-g++ \
    "${GCC_FLAGS[@]}" \
    -std=c++17 "${OPT_FLAGS[@]}" -Wall -Wextra \
    "${INCLUDE_FLAGS[@]}" \
    "${SOURCES[@]}" \
    -o "$OUTPUT" \
    "${LIB_FLAGS[@]}" "${LINK_OPT_FLAGS[@]}" \
    -lmpv \
    -lSDL2 -lSDL2_ttf \
    -lGLESv2 -lEGL \
    -lfreetype \
    -ldrm \
    -lpthread -lm -ldl \
    -static-libstdc++ -static-libgcc

# Strip
aarch64-linux-gnu-strip "$OUTPUT"

echo ""
echo "=== Build Complete ==="
file "$OUTPUT"
ls -lh "$OUTPUT"

echo ""
echo "=== GLIBC Check ==="
readelf -s "$OUTPUT" | grep GLIBC || true

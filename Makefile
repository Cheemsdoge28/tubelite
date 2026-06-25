# TubeLite YouTube Client - Cross-Platform Makefile
# Supports: Windows (MinGW), ARM64 (aarch64-linux-gnu), Linux native

TARGET ?= tubelite
SRC := $(filter-out src/probe_planes.cpp, $(wildcard src/*.cpp))
BUILD_DIR ?= build
OBJ := $(SRC:src/%.cpp=$(BUILD_DIR)/%.o)
DEP := $(OBJ:.o=.d)
INSTALL_DIR ?= /usr/local/bin

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Target platform (can be overridden: make PLATFORM=arm64)
PLATFORM ?= native
ifeq ($(UNAME_S),MINGW64_NT-10.0)
    PLATFORM ?= windows
endif

# Default flags — release optimization profile.
# NOTE: no -ffast-math. mpv A/V sync and timestamp math must stay IEEE-correct;
# fast-math reorders/contracts float ops and can desync audio. Section flags +
# --gc-sections drop unreferenced code/data for a smaller, tighter binary.
CXXFLAGS ?= -std=c++17 -O3 -fno-plt -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic -pthread -Isrc
# -rdynamic exports the executable's symbols into .dynsym so the dynamically
# loaded libmpv can resolve our GBM compatibility stubs (see src/gbm_compat.cpp).
LDFLAGS ?= -ldl -pthread -rdynamic -Wl,--gc-sections -Wl,--as-needed

# LTO flags check (skip on Windows/macOS if causing issues, default on for native optimization)
ifeq ($(LTO),1)
    CXXFLAGS += -flto
    LDFLAGS += -flto
endif

SDL_CFLAGS ?=
SDL_LIBS ?=

# Platform-specific configuration
ifeq ($(PLATFORM),windows)
    # Windows (MinGW)
    CXX ?= g++
    SDL2DIR ?= /mingw64
    SDL_CFLAGS ?= -I$(SDL2DIR)/include/SDL2 -I$(SDL2DIR)/include/freetype2 -I$(SDL2DIR)/include/harfbuzz -Dmain=SDL_main
    SDL_LIBS ?= -L$(SDL2DIR)/lib -lmingw32 -lSDL2main -lSDL2 -lSDL2_ttf -lharfbuzz -lfreetype
    TARGET_SUFFIX := .exe
    STRIP ?= strip

else ifeq ($(PLATFORM),arm64)
    # ARM64 Cross-compilation (aarch64-linux-gnu)
    # R36S uses Rockchip RK3326 with 4x Cortex-A35 @ 1.3GHz (ARMv8-A).
    # Tune precisely for the A35; +crc enables the CRC32 instructions it has.
    # LTO is on for this release path (whole-program optimization across all TUs).
    CXX ?= aarch64-linux-gnu-g++
    # Two valid layouts:
    #   1. Debian multiarch (cross-libs installed as :arm64 via dpkg --add-architecture)
    #      Headers: /usr/include/   Libs: /usr/lib/aarch64-linux-gnu/   pkgconfig there.
    #   2. Legacy cross-toolchain staging at /usr/aarch64-linux-gnu/
    # We search both so the same Makefile works in either env.
    SDL2DIR ?= /usr/aarch64-linux-gnu
    PKG_CONFIG_PATH := /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/aarch64-linux-gnu/lib/pkgconfig
    PKG_CONFIG_LIBDIR := /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/aarch64-linux-gnu/lib/pkgconfig
    SDL_CFLAGS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) PKG_CONFIG_LIBDIR=$(PKG_CONFIG_LIBDIR) pkg-config --cflags sdl2 SDL2_ttf harfbuzz freetype2 libdrm 2>/dev/null || echo "-I$(SDL2DIR)/include/SDL2 -I$(SDL2DIR)/include/freetype2 -I$(SDL2DIR)/include/harfbuzz -I$(SDL2DIR)/include/libdrm")
    SDL_LIBS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) PKG_CONFIG_LIBDIR=$(PKG_CONFIG_LIBDIR) pkg-config --libs sdl2 SDL2_ttf harfbuzz freetype2 libdrm 2>/dev/null || echo "-L$(SDL2DIR)/lib -lSDL2 -lSDL2_ttf -lharfbuzz -lfreetype -ldrm") -lrt -lGLESv2
    CXXFLAGS += -march=armv8-a+crc -mcpu=cortex-a35 -mtune=cortex-a35 -flto -fomit-frame-pointer
    LDFLAGS  += -flto
    TARGET_SUFFIX := .arm64
    STRIP ?= aarch64-linux-gnu-strip

else
    # Linux native (builds directly on the R36S or any Linux host)
    CXX ?= g++
    PKG_CONFIG ?= pkg-config
    SDL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sdl2 SDL2_ttf harfbuzz freetype2 libdrm 2>/dev/null)
    SDL_LIBS   ?= $(shell $(PKG_CONFIG) --libs   sdl2 SDL2_ttf harfbuzz freetype2 libdrm 2>/dev/null)

    ifeq ($(strip $(SDL_CFLAGS)),)
        SDL_CFLAGS := -I/usr/include/SDL2 -I/usr/include/freetype2 -I/usr/include/harfbuzz -I/usr/include/libdrm
    endif
    ifeq ($(strip $(SDL_LIBS)),)
        SDL_LIBS := -lSDL2 -lSDL2_ttf -lharfbuzz -lfreetype -ldrm
    endif

    # GLESv2 for glViewport/glScissor; dl for dlopen/dlsym in the GL proc-address resolver.
    SDL_CFLAGS += -Isrc
    SDL_LIBS   += -lGLESv2 -ldl -lrt

    # When building natively on ARM, tune for the actual host CPU.
    UNAME_M_NATIVE := $(shell uname -m)
    ifeq ($(UNAME_M_NATIVE),aarch64)
        CXXFLAGS += -march=armv8-a+crc -mcpu=cortex-a35 -mtune=cortex-a35
    endif

    STRIP ?= strip
endif

# Build target with suffix
BUILD_TARGET := $(BUILD_DIR)/$(TARGET)$(TARGET_SUFFIX)

# Default target
all: check_compiler $(BUILD_TARGET)

# Check if compiler exists
check_compiler:
	@if ! command -v $(CXX) >/dev/null 2>&1; then \
		echo "ERROR: Compiler '$(CXX)' not found."; \
		echo "Please run 'sudo bash Install-TubeLite.sh' to install build dependencies (build-essential, g++, etc)."; \
		exit 1; \
	fi

# Compile object files
$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "  CXX $<"
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

# Link target
$(BUILD_TARGET): $(OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "[$(PLATFORM)] Linking $(BUILD_TARGET)"
	@echo "  CXX: $(CXX)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  SDL_LIBS: $(SDL_LIBS)"
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(SDL_LIBS)
	@echo "Build complete: $@"
	@ls -lh $@

# Strip debug symbols for deployment
strip: $(BUILD_TARGET)
	$(STRIP) $(BUILD_TARGET)

# Install to system (Linux only)
install: $(BUILD_TARGET)
	@if [ "$(PLATFORM)" = "windows" ]; then \
		echo "Install not supported on Windows. Copy $(BUILD_TARGET) manually."; \
	else \
		install -D $(BUILD_TARGET) $(INSTALL_DIR)/$(TARGET); \
		echo "Installed to $(INSTALL_DIR)/$(TARGET)"; \
	fi

# Clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f tubelite tubelite.exe tubelite.arm64 fire4arkos.log
	@echo "Cleaned build artifacts and stale logs."

# Cross-compile for ARM64.
# Re-invoke make with PLATFORM set on the command line so the aarch64 toolchain
# and A35/LTO release block are chosen at parse time (a target-specific
# `PLATFORM=arm64` is applied too late for the ifeq that selects the block).
arm64:
	$(MAKE) PLATFORM=arm64 all
	@echo "ARM64 build complete"

# Windows MinGW build
windows: PLATFORM=windows
windows: check_compiler $(BUILD_TARGET)
	@echo "Windows build complete: $<"

# Native build (release: LTO + O3)
native: PLATFORM=native
native: check_compiler $(BUILD_TARGET)
	@echo "Native build complete: $<"

# Native dev build — fast iteration: no LTO, O1, parallel-safe
# Usage: make native-dev [-j4]
native-dev: PLATFORM=native
native-dev: CXXFLAGS=-std=c++17 -O1 -Wall -Wextra -pthread -Isrc -march=armv8-a+crc -mcpu=cortex-a35 -mtune=cortex-a35
native-dev: LDFLAGS=-ldl -pthread -rdynamic
native-dev: check_compiler $(BUILD_TARGET)
	@echo "Native dev build complete: $<"


# Show current configuration
config:
	@echo "=== Fire4ArkOS Browser Build Configuration ==="
	@echo "Platform: $(PLATFORM)"
	@echo "Compiler: $(CXX)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "SDL_CFLAGS: $(SDL_CFLAGS)"
	@echo "SDL_LIBS: $(SDL_LIBS)"
	@echo "Target: $(BUILD_TARGET)"
	@echo ""

probe-planes: check_compiler
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -O2 -Wall src/probe_planes.cpp $$(pkg-config --cflags --libs libdrm 2>/dev/null || echo "-I/usr/include/libdrm -ldrm") -o $(BUILD_DIR)/probe_planes
	@echo "Build complete: $(BUILD_DIR)/probe_planes"

.PHONY: all strip install clean arm64 windows native config probe-planes

-include $(DEP)

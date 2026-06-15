# Fire4ArkOS Installation Guide

This guide covers the recommended way to install Fire4ArkOS on your ArkOS handheld (R36S, RG351MP, etc.).

## 📋 Prerequisites
- A handheld running **ArkOS**.
- A computer with an SD card reader.
- The latest **Fire4ArkOS Release** (`.zip` file).

---

## 🛠️ Installation Steps

### 1. Download and Extract
Download the latest release zip from the GitHub releases page. Extract it on your computer; you should see a folder named `Fire4ArkOS`.

### 2. Copy to the 'tools' Directory
Connect your handheld's **EASYROMS** SD card to your computer.
- Navigate to the `tools` folder.
- Copy the entire `Fire4ArkOS` folder into `tools`.
- **Final Path Check**: You should have a file at `EASYROMS/tools/Fire4ArkOS/install-from-es.sh`.

### 3. Run the Installer from EmulationStation
1. Insert the SD card back into your device and boot into ArkOS.
2. Navigate to the **Options** (or **Tools**) system in the main menu.
3. Select **install-browser.sh** or **install-theme.sh** and press **A**.
4. The screen will turn black for a few moments as it installs and registers Fire4ArkOS.
5. Once it finishes, it will return to the menu.

Optional named scripts:
- `install-browser.sh` installs the browser + ES entry (no theme).
- `install-theme.sh` installs the ES entry + theme assets.
- `uninstall-browser.sh` removes the ES entry + launcher (keeps theme).
- `uninstall-theme.sh` removes the ES entry + theme assets.

### 4. Restart EmulationStation
- Press **Start** → **Quit** → **Restart EmulationStation**.
- A new system called **"Fire4ArkOS Browser"** will now appear in your main carousel.

---

## 🖥️ Advanced: Manual Installation (SSH)
If you prefer using the terminal, you can install Fire4ArkOS via SSH:

1. Connect to your device via SSH.
2. Navigate to the folder:
   ```bash
   cd /roms/tools/Fire4ArkOS
   ```
3. Run the installer script with root privileges:
   ```bash
   sudo bash install.sh
   ```
4. Restart EmulationStation to see the changes.

---

## 🧹 Uninstallation
To completely remove Fire4ArkOS:
1. Run the installer again and select the Uninstall option (if available), or run via SSH:
   ```bash
   cd /roms/tools/Fire4ArkOS
   sudo bash install.sh --uninstall
   ```
2. You can then safely delete the `Fire4ArkOS` folder from your `tools` directory.

You can also run:
```bash
cd /roms/tools/Fire4ArkOS
sudo bash uninstall-browser.sh
sudo bash uninstall-theme.sh
```

---

## ❓ Troubleshooting

### Browser doesn't launch
- Ensure you are running at `FIRE4ARKOS_INTERNAL_SCALE=1` if you have display issues.
- Check the log files at `/roms/tools/Fire4ArkOS/install.log` and `/roms/tools/Fire4ArkOS/firefox.log` for errors.

### No Audio
- Audio is handled by `apulse`. If you hear nothing, ensure your system volume is not muted in the ArkOS main settings.
- If issues persist, try a rebuild: `sudo bash install.sh --rebuild`.

### Stick Drift
- R36S analog sticks can vary in quality. If the cursor drifts, we have set a generous deadzone of 10000. If you still experience drift, you may need to increase the `DEADZONE` value in `src/main.cpp` and run a rebuild.

---

## 🏗️ Development & Native Compilation Details

If you are modifying the C++ browser engine (e.g., adding features, adjusting overlays, or testing layouts) and compiling natively on the RK3326 device, use the following guide.

### 1. Build Dependencies
To compile natively on the ArkOS target, you must install the compilation packages and development headers:
- **Core build tools**: `build-essential`, `g++`, `make`, `pkg-config`, `cmake`, `ninja-build`.
- **System development headers**:
  - `libsdl2-dev` (SDL2 library)
  - `libstdc++-dev` (C++ Standard library)
  - `libc6-dev`, `linux-libc-dev` (C and Linux system headers)
- **Graphics & Rendering**:
  - `libgles2-mesa-dev`, `libegl1-mesa-dev`, `libgl1-mesa-dev` (GLES2/EGL Mesa libraries for offscreen compositing)
  - `libglu1-mesa-dev`, `libglew-dev` (OpenGL utilities)
- **Text & Font Pipelines**:
  - `libfreetype6-dev` (FreeType2 header files)
  - `libharfbuzz-dev` (HarfBuzz text-shaping libraries)

Install them all via:
```bash
sudo apt-get update
sudo apt-get install -y build-essential g++ make pkg-config libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev libgl1-mesa-dev libfreetype6-dev libharfbuzz-dev
```

### 2. Runtime Dependencies
To run the video player application after compilation, the following runtime packages must be available on the device:
- `python3`, `yt-dlp` (required to resolve and stream video URLs; must be manually installed/updated to ensure YouTube signature deciphering compatibility)
- `libmpv1` (required to decode and render video streams)
- `libsdl2-2.0-0` (required for application windowing and input routing)
- `ffmpeg`, `libasound2` (required for audio and stream rendering)

> [!IMPORTANT]
> **Outdated Package Warning**: Do NOT install `yt-dlp` via `apt-get` as package manager repositories contain extremely outdated versions that fail to decipher newer YouTube stream signatures. Always download the standalone binary (tested and recommended version: `2026.03.17` or newer):
> ```bash
> sudo wget https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp -O /usr/local/bin/yt-dlp
> sudo chmod a+rx /usr/local/bin/yt-dlp
> ```



### 3. Bundled Platform Headers
- **Khronos platform header**: The source directory bundles `src/KHR/khrplatform.h` to supply cross-platform type declarations (e.g., `khronos_int32_t`, `khronos_float_t`) for EGL and GLES2 rendering across MinGW (Windows), Linux, and ARM64 systems without requiring system-level installation of platform headers.

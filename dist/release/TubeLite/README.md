# 📺 TubeLite

A high-performance, hardware-accelerated native YouTube client for ArkOS handhelds (R36S, RG351MP, Powkiddy RGB10, etc.) powered by the RK3326 SoC.

![Downloads](https://img.shields.io/github/downloads/Cheemsdoge28/r36tube/total?style=for-the-badge&color=green)
![Release](https://img.shields.io/github/v/release/Cheemsdoge28/r36tube?style=for-the-badge&color=blue)
[![Sponsor](https://img.shields.io/badge/Sponsor-Cheemsdoge28-ea4aaa?style=for-the-badge&logo=github-sponsors)](https://github.com/sponsors/Cheemsdoge28)

Unlike web-based wrappers, **TubeLite** is written natively in C++ using SDL2, OpenGL ES 2.0 (via KMSDRM/EGL), and `libmpv` to achieve smooth, hardware-accelerated video/audio rendering directly on the Linux framebuffer without windowing overhead.

---

## ✨ Features
- **rk3326-Optimized GLES2/EGL Compositor**: High-performance, low-overhead native rendering.
- **Seamless Background Audio Player Daemon**: Exit the main application while playing a track, and the playback will seamlessly handover to an audio-only background daemon with controller hooks.
- **On-Screen Controller Overlay**: The background daemon draws a beautiful, premium, semi-transparent card overlay over the screen to show track status, seek position, and buttons.
- **Integrated Controller Keyboard**: Fully navigated virtual keyboard overlay to easily search for videos.
- **Micro-animations & Vibrant Dark Themes**: Sleek UI designed for 4:3 handheld screens.

---

## 📱 Supported Devices
TubeLite is optimized specifically for **RK3326** based handhelds running **ArkOS**, **darkOS**, or **darkOSRE**:
- **R36S** (Highly Recommended)
- **RG351MP / RG351P / RG351M**
- **Powkiddy RGB10 / RGB10S**
- **RK2020**
- **Gameforce Chi**
- Any other RK3326 device on ArkOS, darkOS, darkOSRE, or similar OS distributions

---

## 🛠️ Installation

### 1. Download & Extract
- Download the latest `TubeLite.zip` from the [Releases](https://github.com/Cheemsdoge28/r36tube/releases) page.
- Extract the zip file on your computer. You will see a `TubeLite` folder.

### 2. Copy to Handheld
- Connect your ArkOS SD card (EASYROMS partition) to your computer.
- Copy the entire `TubeLite` folder into the `tools` directory on your SD card.
- Path: `EASYROMS/tools/TubeLite/`

### 3. Setup via EmulationStation
- Navigate to the **Options** (or Tools) section in EmulationStation.
- Select and run **Install-TubeLite.sh**.
- Choose **Full Install** in the controller-friendly menu.
  - *Note*: The installer includes an **Audio Compatibility** check which scans your ALSA configuration (`~/.asoundrc` or `/etc/asound.conf`). If a `dmix` mixer running at `44100 Hz` is detected, it will safely offer to update it to `48000 Hz` (making a backup first) to ensure TubeLite, MPV, and RetroArch co-exist and play audio perfectly without locking the audio device.
- Once finished, **Restart EmulationStation** (Start Menu → Quit → Restart EmulationStation).

A new system named **"TubeLite"** will now appear in your main frontend carousel!

---

## 🎮 Controls

### Main App Controls

| Button | Action |
|--------|--------|
| **D-Pad / Left Stick** | Navigate home grid / search results / virtual keyboard |
| **A** | Click/Play video / Play & Pause during playback |
| **B** | Back / Close Virtual Keyboard / Exit Playback screen |
| **X** | Open Sign-in Help (in grid) / **Screen Sleep** light toggle (during playback, double-tap to confirm) |
| **Y** | Open Virtual Keyboard (in grid) / Toggle Subtitles (during playback) |
| **L2 / R2** | Volume Decrease / Increase (during playback) |
| **D-Pad Left / Right** | Seek/Scrub backward/forward (during playback) |
| **L3** | Toggle debug/stats overlay (anywhere, including playback) |
| **R3** | Reload current feed / re-run search |
| **Select** (tap) | Toggle miniplayer mode |
| **Select + Y** | Open Settings |
| **Select + A** | Toggle description drawer (playback) / Play & Pause (miniplayer) |
| **Select + L1 / R1** | Previous / Next track in queue (playback or miniplayer) |
| **Select + B** | Leave playback (miniplayer) |
| **Start + Select** | Exit Main Application (Handover to Background Daemon) |

> **Note on input:** TubeLite reads the gamepad through SDL's raw joystick layer with a fixed button map (`B A X Y L1 R1 L2 R2 … Select Start … FN`), and never opens it as an SDL *game controller*. This guarantees the **exact same mapping on every SD card / OS image** — some ArkOS images ship a `gamecontrollerdb` entry that would otherwise swap A/B and break Select/Start.

### Background Daemon Controls (FN Combination)

Hold the **FN** button (Select on some configurations) and press one of the following:

| Combination | Action |
|-------------|--------|
| **FN + A** | Play / Pause audio |
| **FN + B** | Exit background daemon completely (requires double-tap confirmation) |
| **FN + X** | Screen Sleep / Power-save mode (requires double-tap confirmation; wakes on any key) |
| **FN + SELECT** | Cycle playback speed (1.0x -> 1.25x -> 1.5x -> 1.75x -> 2.0x -> 0.25x -> 0.5x -> 0.75x -> 1.0x) |
| **FN + R1** | Skip to next track in queue |
| **FN + L1** | Go back to previous track |
| **FN + L2 / R2** | Volume Decrease / Increase |
| **FN + D-Pad Up / START** | Show on-screen daemon overlay card (shows track status & position) |

---

## 🎮 RetroArch Integration & Audio Mixing

To guarantee smooth co-existence between gameplay and background music, the TubeLite installer includes an automated **RetroArch Config Editor** step that dynamically patches all `retroarch.cfg` configurations on the system:

1. **Audio Sharing via ALSA dmix**:
   - Sets `audio_driver = "alsa"`
   - Sets `audio_device = "plug:dmix"`
   - **Why**: This instructs RetroArch to use the ALSA `dmix` mixer plugin instead of opening the audio hardware exclusively. This allows gameplay sounds to mix seamlessly with background audio from the TubeLite daemon, rather than one silencing the other.
2. **In-Game Now-Playing Notifications**:
   - Sets `network_cmd_enable = "true"`
   - **Why**: This opens RetroArch's UDP network socket (port 55355). When you change tracks in the background daemon (e.g. via `FN + D-Pad Right`), the daemon pushes a text payload to this socket, prompting RetroArch to render a native "Now Playing" OSD notification overlay inside the game.

---

## 🏗️ Development & Manual Compilation
To compile TubeLite natively on your device, connect via SSH and run:
```bash
cd /roms/tools/TubeLite
sudo bash Install-TubeLite.sh --rebuild
```

For release packaging:
```bash
./package-release.sh
```

## 🤝 Support & Contribution

If you find TubeLite useful, please consider [sponsoring the project on GitHub](https://github.com/sponsors/Cheemsdoge28) to support its development!

Feel free to open an issue or pull request if you find bugs or want to improve performance!

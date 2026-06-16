# 📺 TubeLite

A high-performance, hardware-accelerated native YouTube client for ArkOS handhelds (R36S, RG351MP, Powkiddy RGB10, etc.) powered by the RK3326 SoC.

![Downloads](https://img.shields.io/github/downloads/Cheemsdoge28/r36tube/total?style=for-the-badge&color=green)
![Release](https://img.shields.io/github/v/release/Cheemsdoge28/r36tube?style=for-the-badge&color=blue)

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
TubeLite is optimized specifically for **RK3326** based handhelds running **ArkOS**:
- **R36S** (Highly Recommended)
- **RG351MP / RG351P / RG351M**
- **Powkiddy RGB10 / RGB10S**
- **RK2020**
- **Gameforce Chi**
- Any other RK3326 device on ArkOS or similar OS

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
- Insert the SD card back into your device and boot ArkOS.
- Navigate to the **Options** (or Tools) section in EmulationStation.
- Select and run **Install-TubeLite.sh**.
- Choose **Full Install** in the controller-friendly menu.
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
| **Y** | Open Virtual Keyboard (in grid) / Toggle Subtitles (during playback) |
| **L2 / R2** | Volume Decrease / Increase (during playback) |
| **D-Pad Left / Right** | Seek/Scrub backward/forward (during playback) |
| **Select** | Toggle miniplayer mode |
| **Start + Select** | Exit Main Application (Handover to Background Daemon) |

### Background Daemon Controls (Select Combination)

Hold the **Select** button and press one of the following:

| Combination | Action |
|-------------|--------|
| **Select + A** | Play / Pause audio |
| **Select + B** | Exit background daemon completely |
| **Select + R1 / D-Pad Right** | Skip to next track in queue |
| **Select + L1 / D-Pad Left** | Go back to previous track |
| **Select + D-Pad Up** | Show on-screen daemon overlay card (shows track status & position) |

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
Feel free to open an issue or pull request if you find bugs or want to improve performance!

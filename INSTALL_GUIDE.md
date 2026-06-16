# TubeLite Installation Guide

This guide covers the recommended way to install TubeLite on your ArkOS handheld (R36S, RG351MP, etc.).

## 📋 Prerequisites
- A handheld running **ArkOS**.
- A computer with an SD card reader.
- The latest **TubeLite Release** (`.zip` file).

---

## 🛠️ Installation Steps

### 1. Download and Extract
Download the latest release zip from the GitHub releases page. Extract it on your computer; you should see a folder named `TubeLite`.

### 2. Copy to the 'tools' Directory
Connect your handheld's **EASYROMS** SD card to your computer.
- Navigate to the `tools` folder.
- Copy the entire `TubeLite` folder into `tools`.
- **Final Path Check**: You should have a file at `EASYROMS/tools/TubeLite/Install-TubeLite.sh`.

### 3. Run the Installer from EmulationStation
1. Insert the SD card back into your device and boot into ArkOS.
2. Navigate to the **Options** (or **Tools**) system in the main menu.
3. Select **Install-TubeLite.sh** and press **A**.
4. The installer will present a controller-friendly menu. Select **Full Install** to install dependencies, copy the system theme, and register TubeLite.
5. Once it finishes, it will return to the menu.

### 4. Restart EmulationStation
- Press **Start** → **Quit** → **Restart EmulationStation**.
- A new system called **"TubeLite"** will now appear in your main carousel.

---

## 🖥️ Advanced: Manual Installation (SSH)
If you prefer using the terminal, you can install TubeLite via SSH:

1. Connect to your device via SSH.
2. Navigate to the folder:
   ```bash
   cd /roms/tools/TubeLite
   ```
3. Run the installer script with root privileges:
   ```bash
   sudo bash Install-TubeLite.sh
   ```
4. Restart EmulationStation to see the changes.

---

## 🧹 Uninstallation
To completely remove TubeLite:
1. Run the installer again and select the Uninstall option, or run via SSH:
   ```bash
   cd /roms/tools/TubeLite
   ```
   - To uninstall everything:
     ```bash
     sudo bash Install-TubeLite.sh --uninstall
     ```
   - To uninstall only the application launcher:
     ```bash
     sudo bash Install-TubeLite.sh --uninstall-app
     ```
   - To uninstall only the theme assets:
     ```bash
     sudo bash Install-TubeLite.sh --uninstall-theme
     ```
2. You can then safely delete the `TubeLite` folder from your `tools` directory.

---

## ❓ Troubleshooting

### Application doesn't launch
- Check the log files at `/roms/tools/TubeLite/tubelite.log` for execution errors.
- If dependencies are missing, run a dependencies reinstall: `sudo bash Install-TubeLite.sh --reinstall-deps`.

### No Audio or Video
- If audio or video decoding fails, ensure you have `libmpv1` and `ffmpeg` installed. You can reinstall them via the installer script.

---

## 🏗️ Development & Native Compilation Details

If you are compiling natively on the RK3326 target device, install compile-time dependencies:
```bash
sudo apt-get update
sudo apt-get install -y build-essential g++ make pkg-config libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev libgl1-mesa-dev libfreetype6-dev libharfbuzz-dev libmpv-dev
```

You can trigger a rebuild using the installer script:
```bash
sudo bash Install-TubeLite.sh --rebuild
```

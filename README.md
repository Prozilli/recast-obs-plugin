# Recast OBS Plugin

Multi-destination RTMP streaming from a single OBS instance with per-scene rendering support.

## What It Does

- **Multiple RTMP outputs** — Stream to Twitch, YouTube, TikTok, and more simultaneously from one OBS instance
- **Per-scene rendering** — Send different scenes to different platforms (e.g., 16:9 "Gaming Layout" to Twitch, 9:16 "Shorts Layout" to TikTok)
- **Shared encoders** — Same-resolution outputs share the main encoder for zero extra CPU usage
- **Independent control** — Start/stop each output independently without affecting others
- **Recast server sync** — Optionally pull platform configs from the Recast dashboard API

## Requirements

- OBS Studio 30.0+
- Windows 10/11 (64-bit), Linux, or macOS

### Build Requirements

- [CMake 3.28+](https://cmake.org/download/)
- [Visual Studio 2022](https://visualstudio.microsoft.com/) (Windows) with C++ desktop workload
- [Qt6](https://www.qt.io/download-qt-installer) (Widgets + Network modules)
- [OBS Studio source/SDK](https://github.com/obsproject/obs-studio) (for headers and link libraries)

## Building

### Windows

1. Install the build requirements above

2. Set environment variables pointing to your OBS build and Qt6:
   ```powershell
   $env:OBS_BUILD_DIR = "C:\path\to\obs-studio\build"
   $env:Qt6_DIR = "C:\Qt\6.x.x\msvc2022_64\lib\cmake\Qt6"
   ```

3. Configure and build:
   ```bash
   cmake --preset windows-x64
   cmake --build build_windows --config Release
   ```

4. The plugin DLL will be at `build_windows/Release/recast-obs-plugin.dll`

### Linux

```bash
sudo apt install cmake ninja-build qt6-base-dev libobs-dev obs-studio
cmake --preset linux-x64
cmake --build build_linux
```

### macOS

```bash
brew install cmake qt@6 obs-studio
cmake --preset macos-universal
cmake --build build_macos --config Release
```

## Installing

### Manual Install (Windows)

Copy the built files to your OBS installation:

```powershell
copy build_windows\Release\recast-obs-plugin.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
xcopy /E data "C:\Program Files\obs-studio\data\obs-plugins\recast-obs-plugin\data\"
```

Then restart OBS.

### NSIS Installer (Windows)

If you have [NSIS](https://nsis.sourceforge.io/) installed:

```bash
cd installer
makensis installer.nsi
```

This produces `recast-obs-plugin-installer.exe` which handles installation and uninstallation.

### Linux

```bash
sudo cmake --install build_linux
```

## Usage

1. Open OBS Studio — the **"Recast Output"** dock panel appears automatically
2. Click **"+ Add Output"** to add a streaming target
3. Enter a name, RTMP URL, stream key, and optionally select a scene and resolution override
4. Click **Start** on any output card to begin streaming to that target
5. Each output can be started/stopped independently

### Per-Scene Streaming

To stream different layouts to different platforms:

1. Create your scenes in OBS (e.g., "Gaming Layout" at 1920x1080 and "Shorts Layout" at 1080x1920)
2. Add an output for each platform
3. Select the desired scene in each output's configuration
4. Outputs using the same scene/resolution as the main stream share the encoder (no extra CPU)
5. Outputs with a different scene or resolution get a dedicated encoder automatically

### Recast Server Sync

Click **"Sync with Recast Server"** to pull your platform configurations from the Recast dashboard. You'll be prompted for your API token on first use.

## Project Structure

```
recast-obs-plugin/
├── CMakeLists.txt          — Build configuration
├── CMakePresets.json        — Platform build presets
├── src/
│   ├── plugin-main.c       — Entry point, module load/unload
│   ├── recast-output.c/h   — Output target lifecycle management
│   ├── recast-service.c/h  — Custom RTMP service (URL + key storage)
│   ├── recast-dock.cpp/h   — Qt dock widget UI
│   └── recast-config.c/h   — JSON config persistence
├── data/
│   └── locale/
│       └── en-US.ini       — Localization strings
└── installer/
    └── installer.nsi       — NSIS Windows installer script
```

## License

Proprietary — Recast

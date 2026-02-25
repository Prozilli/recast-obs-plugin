# Recast OBS Plugin

**The ultimate multi-platform streaming toolkit for OBS Studio** — built by [Prozilli](https://prozilli.com).

Stream to Twitch, YouTube, Kick, TikTok, Facebook, and more simultaneously from a single OBS instance. Manage a dedicated 9:16 vertical canvas, view unified chat from all platforms, and monitor real-time events like follows, subscriptions, bits, Super Chats, and raids — all without leaving OBS.

---

## Features

### Multi-Destination Streaming
- **Stream everywhere at once** — Send your stream to unlimited platforms simultaneously (Twitch, YouTube, Kick, TikTok, Facebook, X/Twitter, Trovo, Instagram, and any custom RTMP/SRT/WHIP endpoint)
- **Independent controls** — Start and stop each destination independently
- **Auto start/stop** — Optionally sync destinations with your main OBS stream
- **Start All / Stop All** — One-click global controls for all destinations
- **Live health monitoring** — Real-time bitrate and dropped frames per destination
- **Protocol support** — RTMP, RTMPS, SRT, RIST, and WHIP
- **Shared encoders** — Same-canvas outputs share the main encoder for zero extra CPU

### Vertical Canvas (9:16)
- **Dedicated vertical pipeline** — Private 1080x1920 canvas running alongside your main stream
- **Independent scenes & sources** — Build vertical layouts without affecting your main scenes
- **Live preview** — Real-time vertical canvas preview with interactive source manipulation
- **Scene linking** — Link vertical scenes to main OBS scenes for automatic mirroring
- **Drag-and-drop editing** — Move, scale, crop, and rotate sources directly in the preview

### Unified Chat
- **All chats in one place** — See Twitch, YouTube, and Kick chat messages in a single dock
- **Platform badges** — Color-coded platform indicators for every message
- **Send to all** — Type once, send to all connected platforms
- **Auto-connect** — Automatically connects to chat for your streaming destinations
- **Mod/sub indicators** — See moderator and subscriber badges at a glance

### Platform Events Feed
- **Real-time event notifications** — Follows, subscriptions, bits/cheers, Super Chats, raids, channel points, hype trains, gifted subs, membership milestones, and more
- **Multi-platform** — Events from Twitch (EventSub), YouTube (Live Chat API), and Kick (Pusher WebSocket)
- **Formatted event cards** — Beautiful color-coded cards with monetary values, tier info, and timestamps
- **Event types supported**:
  - **Twitch**: Follows, Subscriptions, Gift Subs, Resubs, Bits/Cheers, Raids, Channel Point Redemptions, Hype Trains
  - **YouTube**: Super Chats, Super Stickers, New Members, Member Milestones, Gifted Memberships
  - **Kick**: Follows, Subscriptions, Gifted Subscriptions

### Platform Authentication
- **Twitch** — Device Code Grant flow (no browser redirect needed, just enter a code)
- **YouTube** — OAuth2 with PKCE (opens browser, redirects to local server)
- **Kick** — No authentication needed (public chat and events)
- **Secure token storage** — Tokens stored locally in your OBS profile, auto-refreshed before expiry

### Recast Server Sync
- **Cloud configuration** — Optionally pull streaming destinations from the Recast dashboard API
- **API token authentication** — Securely sync your platform configs with one click

---

## Requirements

- **OBS Studio 30.0+**
- **Windows 10/11** (64-bit), Linux, or macOS

### Build Requirements

- [CMake 3.28+](https://cmake.org/download/)
- [Visual Studio 2022](https://visualstudio.microsoft.com/) (Windows) with C++ desktop workload
- [Qt6](https://www.qt.io/download-qt-installer) (Widgets, Network, and WebSockets modules)
- [OBS Studio source/SDK](https://github.com/obsproject/obs-studio) (for headers and link libraries)

---

## Building

### Windows

1. Install the build requirements above

2. Set environment variables:
   ```powershell
   $env:OBS_SDK_DIR = "C:\path\to\obs-studio-source"
   $env:OBS_LIB_DIR = "C:\path\to\obs-libs"  # contains obs.lib and obs-frontend-api.lib
   $env:Qt6_DIR = "C:\Qt\6.x.x\msvc2022_64\lib\cmake\Qt6"
   ```

3. Configure and build:
   ```bash
   cmake -B build -DOBS_SDK_DIR="$env:OBS_SDK_DIR" -DOBS_LIB_DIR="$env:OBS_LIB_DIR"
   cmake --build build --config Release
   ```

4. The plugin DLL will be at `build/Release/recast-obs-plugin.dll`

### Linux

```bash
sudo apt install cmake ninja-build qt6-base-dev libqt6websockets6-dev libobs-dev obs-studio
cmake -B build
cmake --build build
```

### macOS

```bash
brew install cmake qt@6 obs-studio
cmake -B build
cmake --build build --config Release
```

---

## Installing

### Manual Install (Windows)

Copy the built files to your OBS installation:

```powershell
# Plugin DLL
copy build\Release\recast-obs-plugin.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"

# Data files (locale, etc.)
xcopy /E /I data "C:\Program Files\obs-studio\data\obs-plugins\recast-obs-plugin\data\"
```

Then restart OBS Studio.

### Linux

```bash
sudo cmake --install build
```

---

## Quick Start

1. **Launch OBS Studio** — Six Recast docks appear automatically:
   - **Recast Multistream** — Add and manage streaming destinations
   - **Recast Vertical Preview** — Live 9:16 canvas preview
   - **Recast Vertical Scenes** — Manage vertical canvas scenes
   - **Recast Vertical Sources** — Manage vertical canvas sources
   - **Recast Chat** — Unified multi-platform chat
   - **Recast Events** — Real-time platform event feed

2. **Add destinations** — Click "+ Add Destination" in the Multistream dock. Enter a name, RTMP URL, and stream key for each platform.

3. **Connect accounts** — Open Recast Accounts (from the Chat or Events dock) to connect your Twitch and YouTube accounts for chat and events.

4. **Start streaming** — Click "Start" on individual destinations or "Start All" to go live everywhere.

5. **Set up vertical canvas** — Add scenes in the Vertical Scenes dock, add sources in Vertical Sources, and assign vertical-canvas destinations to stream your 9:16 layout.

---

## Platform Setup

### Twitch Chat & Events

1. Register an application at [dev.twitch.tv/console](https://dev.twitch.tv/console)
2. Copy your **Client ID**
3. In OBS, open Recast Accounts and paste the Client ID
4. Click **Connect** — a code will appear. Enter it at the Twitch URL shown
5. Once authorized, chat and events connect automatically

### YouTube Chat & Events

1. Create a project in [Google Cloud Console](https://console.cloud.google.com/)
2. Enable the **YouTube Data API v3**
3. Create OAuth 2.0 credentials (Desktop application type)
4. Copy your **Client ID** and **Client Secret**
5. In OBS, open Recast Accounts, paste both values
6. Click **Connect** — your browser will open for Google authorization
7. Once authorized, chat and events connect automatically when you're live

### Kick Chat & Events

No setup needed! Kick chat and events are publicly accessible. Just add a Kick streaming destination and chat/events connect automatically.

---

## Project Structure

```
recast-obs-plugin/
├── CMakeLists.txt              — Build configuration
├── src/
│   ├── plugin-main.c           — Entry point, module load/unload
│   ├── recast-ui.cpp/h         — Top-level UI orchestrator, creates all 6 docks
│   ├── recast-multistream.cpp/h — Multi-destination streaming dock
│   ├── recast-auth.cpp/h       — Platform OAuth authentication
│   ├── recast-chat.cpp/h       — Unified multi-platform chat dock
│   ├── recast-events.cpp/h     — Platform events feed dock
│   ├── recast-vertical.cpp/h   — Vertical canvas singleton (9:16 pipeline)
│   ├── recast-vertical-preview.cpp/h — Vertical canvas live preview
│   ├── recast-vertical-scenes.cpp/h  — Vertical scene management
│   ├── recast-vertical-sources.cpp/h — Vertical source management
│   ├── recast-output.c/h       — OBS output lifecycle management
│   ├── recast-protocol.c/h     — URL-based protocol detection
│   ├── recast-config.c/h       — JSON config persistence
│   ├── recast-scene-model.c/h  — Private scene model for vertical canvas
│   ├── recast-platform-icons.cpp/h — Platform detection and icon generation
│   ├── recast-source-tree.cpp/h    — Source list tree widget
│   └── recast-preview-widget.cpp/h — Interactive source preview
├── data/
│   └── locale/
│       └── en-US.ini           — English localization strings
└── installer/
    └── installer.nsi           — NSIS Windows installer script
```

---

## How It Works

### Multi-Destination Architecture

Recast creates independent OBS outputs for each streaming destination. Destinations using the **main canvas** share the primary OBS encoder, adding zero CPU overhead. Destinations using the **vertical canvas** share a separate dedicated encoder for the 9:16 pipeline.

Each output has its own:
- OBS service (URL + stream key)
- Connection state and health monitoring
- Auto start/stop configuration

### Chat Integration

- **Twitch**: Connects via IRC over WebSocket (`wss://irc-ws.chat.twitch.tv`) with full tag parsing for badges, colors, and emotes
- **YouTube**: Polls the Live Chat Messages API with adaptive polling intervals
- **Kick**: Connects via Pusher WebSocket (public, no auth required)

### Events Integration

- **Twitch**: Real-time via EventSub WebSocket — follows, subs, bits, raids, channel points, hype trains
- **YouTube**: Extracted from Live Chat API polling — Super Chats, Super Stickers, new members, milestones, gifted memberships
- **Kick**: Via Pusher WebSocket — subscriptions, gifted subs, follows

---

## Supported Platforms

| Platform | Streaming | Chat | Events |
|----------|-----------|------|--------|
| Twitch | RTMP/RTMPS | IRC WebSocket | EventSub WebSocket |
| YouTube | RTMP/RTMPS | REST Polling | REST Polling |
| Kick | RTMP/RTMPS | Pusher WebSocket | Pusher WebSocket |
| TikTok | RTMP | — | — |
| Facebook | RTMP/RTMPS | — | — |
| X/Twitter | RTMP | — | — |
| Trovo | RTMP | — | — |
| Instagram | RTMP | — | — |
| Custom | RTMP/RTMPS/SRT/RIST/WHIP | — | — |

---

## Configuration

All settings are stored in `<OBS Profile>/recast-outputs.json` and persist across OBS restarts. This includes:

- Streaming destinations (URLs, keys, canvas assignment, auto start/stop)
- Vertical canvas scenes and source transforms
- Platform authentication tokens
- Dock positions and sizes

---

## Troubleshooting

### Chat not connecting
- Verify your account is connected in Recast Accounts
- Check that your Client ID is valid
- For YouTube, ensure you have an active live broadcast

### Destinations failing to start
- Verify the RTMP URL and stream key are correct
- For Main canvas destinations, ensure OBS is streaming
- For Vertical canvas destinations, ensure at least one vertical scene exists

### Events not appearing
- Events require authentication (Twitch and YouTube)
- Kick events work without auth but require an active destination
- Check the OBS log for `[Recast Events]` messages

---

## About Prozilli

**Recast OBS Plugin** is developed by [Prozilli](https://prozilli.com) — building tools for content creators who stream across multiple platforms. We believe multistreaming should be simple, powerful, and free.

Visit [prozilli.com](https://prozilli.com) for more creator tools and updates.

---

## License

Proprietary — Recast / Prozilli

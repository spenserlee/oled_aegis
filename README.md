# OLED Aegis

A lightweight, reliable screen saver for Windows 10 and 11 designed specifically for OLED monitors.

> **Note**: This is a Windows-only application. The source code uses Windows-specific APIs and will show compilation errors on non-Windows systems. Build instructions below require Visual Studio on Windows.

## Features

* **Per-Monitor Control**: Enable screen saver on specific monitors only (perfect for multi-monitor setups)
* **Media Awareness**: Automatically pauses when audio is playing (Bluetooth pause/play works correctly)
* **Reliable Activation**: Consistently activates after system sleep/wake cycles
* **Minimal Resource Usage**: Written in pure C with no external dependencies
* **Simple Configuration**: Edit a plain text INI file or use the system tray menu
* **System Tray Integration**: Taskbar icon for easy control
* **Startup Support**: Automatically run when Windows starts

## Building

Requires Visual Studio (2015 or later) with the C++ build tools installed.

### Windows (Developer Command Prompt)
```batch
build.bat
```

### Windows (PowerShell)
```powershell
.\build.ps1
```

### WSL
```bash
./build.sh
```

### Manual Build
```batch
cl.exe src\oled_aegis.c /Fe:oled_aegis.exe /O2 /MD /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib
```

See [BUILD.md](BUILD.md) for detailed build instructions and troubleshooting.

## Configuration

Configuration is stored in `%APPDATA%\oled_aegis.ini`. This file is created automatically on first run. Edit it to customize behavior:

```ini
idleTimeout=300
audioDetectionEnabled=1
startupEnabled=0
monitor0=1
monitor1=1
monitor2=1
```

### Settings

* **idleTimeout**: Seconds of inactivity before screen saver activates (default: 300 seconds = 5 minutes)
* **audioDetectionEnabled**: Set to `1` to check for audio playback, `0` to ignore (default: 1)
* **startupEnabled**: Set to `1` to run at Windows startup, `0` to disable (default: 0)
* **monitorN**: Set to `1` to enable screen saver on monitor N, `0` to disable (default: 1 for all)

## Usage

### System Tray

* **Right-click** the tray icon to access:
  * Settings (shows config file location)
  * Enable/Disable Startup
  * Exit

* **Left-click** to toggle screen saver manually

### Behavior

The screen saver will automatically activate when:
1. No user input (keyboard/mouse) for `idleTimeout` seconds
2. No audio is playing (if `audioDetectionEnabled=1`)

The screen saver will automatically deactivate when:
1. Any user input is detected
2. Audio starts playing (if `audioDetectionEnabled=1`)

## How It Works

OLED Aegis solves the problems with Windows' built-in screen saver by:

1. **Direct Window Management**: Creates full-screen black windows directly using Windows API, bypassing the unreliable screen saver subsystem
2. **Media Session Awareness**: Uses Windows Audio Session API to detect audio playback, ensuring Bluetooth media controls work correctly
3. **Per-Monitor Enumeration**: Uses `EnumDisplayMonitors()` to discover and control each monitor independently
4. **Timer-Based Activation**: Uses a simple timer loop that remains consistent across system sleep/wake cycles

## Troubleshooting

### Screen saver not activating

* Check that `idleTimeout` is set correctly in the INI file
* Ensure `audioDetectionEnabled` isn't blocking activation (audio is playing)
* Verify monitor settings (`monitorN=1` for each monitor you want to protect)

### Bluetooth pause/play not working

* Ensure `audioDetectionEnabled=1` in the configuration
* This feature requires that Windows Audio Session API can detect audio output

### Running at startup

* Use the tray menu to "Enable Startup" OR set `startupEnabled=1` in the INI file
* Requires adding a registry key to `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`

## Technical Details

**Language**: C (C99 compatible)
**Dependencies**: Windows API only (user32.lib, shell32.lib, ole32.lib, uuid.lib, gdi32.lib, advapi32.lib)
**Build System**: Single-file unity build with Visual Studio's `cl.exe`
**Resource Usage**: ~2-5 MB RAM when idle, minimal CPU usage (1-second timer loop)

## License

MIT License - Feel free to modify and distribute as needed for your OLED protection needs!

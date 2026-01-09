# OLED Aegis

A lightweight, reliable screen saver for Windows.

## Why I made this

Recently, I purchased my first OLED monitor and discovered that Windows 11
built-in screen saver has some issues:

* Randomly not activating after putting the computer to sleep.
* Breaks Bluetooth pause/play interactions when the screen saver is active.
* Does not provide a way to only turn on screen saver on one monitor in a
  multi-monitor setup. (I only want to enable the screen saver on my OLED
  monitor)

Solution: make my own screen saver app and give it a bad name.

OLED Aegis solves these problems by implementing a screen saver app in the
simplest way possible: draw a black fullscreen window after a period of user
inactivity on the specified monitors.

> **Note**: It should work just fine on Windows 10, but I only tested it on Windows 11.

## Features

* **Per-Monitor Control**: Enable screen saver on specific monitors only
* **Per-Monitor Input Detection**: Optionally track input separately for each monitor, allowing unused monitors to activate screen saver while you continue using others
* **Media Awareness**: Doesn't activate the screen saver if a video is playing
* **Reliable Activation**: Consistently activates after system sleep/wake cycles
* **Minimal Resource Usage**: Written in pure C with no external dependencies
* **Simple Configuration**: Edit a plain text INI file or use the system tray menu
* **System Tray Integration**: Taskbar icon for easy control
* **Startup Support**: Automatically run when Windows starts

## Download & Installation

Download the latest `oled_aegis.exe` from the [Releases page](https://github.com/spenserlee/oled_aegis/releases). No installation required - simply run the executable.

For convenient access, you can place it in any folder and create a shortcut, or add it to your Windows startup folder.

## Building

Requires Visual Studio (2015 or later) with the C++ build tools installed.

### Windows (Command Prompt)
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
cl.exe src\oled_aegis.c /Fe:oled_aegis.exe /O2 /MD /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib powrprof.lib
```

See [BUILD.md](BUILD.md) for more information.

## Configuration

Configuration is stored in `%APPDATA%\OLED_Aegis\oled_aegis.ini`. This file is created automatically on first run. Edit it to customize behavior:

```ini
idleTimeout=300
checkInterval=1000
mediaDetectionEnabled=1
startupEnabled=0
debugMode=0
perMonitorInputDetection=0
monitorEnabled_\\.\DISPLAY1=1
monitorEnabled_\\.\DISPLAY2=1
monitorEnabled_\\.\DISPLAY3=0
```

### Settings

* **idleTimeout**: Seconds of inactivity before screen saver activates (default: 300 seconds = 5 minutes)
* **checkInterval**: Milliseconds between idle time checks (default: 1000ms, min: 250ms, max: 10000ms)
* **mediaDetectionEnabled**: Set to `1` to prevent screen saver during media playback, `0` to disable (default: 1)
* **startupEnabled**: Set to `1` to run at Windows startup, `0` to disable (default: 0)
* **debugMode**: Set to `1` to enable debug logging to `%APPDATA%\OLED_Aegis\oled_aegis_debug.log`, `0` to disable (default: 0). **Note:** Enable only if troubleshooting issues, as continuous file I/O every `checkInterval` milliseconds may impact performance of other applications.
* **perMonitorInputDetection**: Set to `1` to track input separately for each monitor (default: 0). When enabled, each monitor has its own idle timer based on mouse cursor position and focused window location. This allows the screen saver to activate on unused monitors while you continue working on others.
* **monitorEnabled_\<device\>**: Set to `1` to enable screen saver on the specified monitor, `0` to disable (default: 1 for all). The device name (e.g., `\\.\DISPLAY1`) uniquely identifies each monitor, so settings persist correctly even when monitors are powered off/on or display configuration changes.

## Usage

### System Tray

* **Right-click** the tray icon to access:
  * Settings (shows config file location)
  * Enable/Disable Startup
  * Exit

* **Left-click** to toggle screen saver manually

### Behavior

#### Global Mode (default)
The screen saver will automatically activate on all enabled monitors when:
1. No user input (keyboard/mouse) for `idleTimeout` seconds
2. No media is playing (if `mediaDetectionEnabled=1`)

The screen saver will automatically deactivate from all monitors when:
1. Any user input is detected
2. Media starts playing (if `mediaDetectionEnabled=1`)

#### Per-Monitor Mode (`perMonitorInputDetection=1`)
Each enabled monitor has its own independent idle timer. Input is attributed to monitors based on:
- **Mouse movement**: Updates the idle timer for the monitor where the cursor is located
- **Keyboard input**: Updates the idle timer for the monitor containing the focused window, and also the monitor where the cursor is located

This allows you to:
- Continue using one monitor while others activate their screen savers
- Have different monitors timeout independently based on where you're actively working
- Keep your OLED monitor protected while watching content on a secondary display

### Known Issues

**Media Detection Limitation**: The media detection feature uses system-wide Windows Power Management APIs, which cannot distinguish which specific monitor has media playing. If media is playing on ANY monitor (including those with screen saver disabled), it will prevent screen saver activation on ALL configured monitors.

However, with per-monitor input detection, there is a workaround for viewing
content on secondary monitors. For example, you can initiate media playback and
then left-click the tray icon to activate the screen saver. Alternatively, if
the screen saver is already active and you use Bluetooth or keyboard controls to
start media playback and the screen saver will not be disabled (provided the
focused media window isn't on the screen saver configured display).

## Why didn't you just make a custom Screen Saver (`.scr`)?

From my testing, it is not possible to have a real Windows screensaver (`.scr`
launched by the OS) affect only one monitor while leaving the others showing
their normal desktop / video playback.

Even if a `.scr` program specifically draws on only one monitor, when Windows
activates a screensaver due to timeout, Explorer switches into a screensaver
mode and the desktop window manager creates an internal blank backdrop surface
which is applied to *all* monitors.

Also, one of the primary issues I had with the built-in screen saver was it's
apparent disabling of Bluetooth media controls, so using the built-in screen
saver wouldn't resolve this particular issue.


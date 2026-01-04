# OLED Aegis

A lightweight, reliable screen saver for Windows.

## Why I made this

Recently, I purchased my first OLED monitor and discovered that Windows 11
built-in screen saver had some issues:

* Randomly not activating after putting the computer to sleep.
* Breaks Bluetooth pause/play interactions when the screen saver is active.
* Does not provide a way to only turn on screen saver on one monitor in a
  multi-monitor setup. (I only want to enable the screen saver on my OLED
  monitor)

Solution: make my own screen saver app and give it a bad name.

OLED Aegis solves these problems by implementing a screen saver app in the
simplest way possible - draw a black window after a period of user inactivity on
the specified monitors.

> **Note**: It should work just fine on Windows 10, but I only tested it on Windows 11.

## Features

* **Per-Monitor Control**: Enable screen saver on specific monitors only
* **Media Awareness**: Doesn't activate the screen saver on the target monitor if a video is playing on it
* **Reliable Activation**: Consistently activates after system sleep/wake cycles
* **Minimal Resource Usage**: Written in pure C with no external dependencies
* **Simple Configuration**: Edit a plain text INI file or use the system tray menu
* **System Tray Integration**: Taskbar icon for easy control
* **Startup Support**: Automatically run when Windows starts

## Known Issues

Since this is just a regular Windows application, it cannot draw over special
system windows such as the Start Menu, Task View, or Action center. Workaround
tbd.

As a failsafe, I recommend still enabling the the built-in screen saver with a
longer timeout.

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
cl.exe src\oled_aegis.c /Fe:oled_aegis.exe /O2 /MD /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib
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

## Why didn't you just make a custom Screen Saver (`.scr`)?

Installing a custom `.scr` program indeed would resolve issues like not drawing
over the Start Menu / Task View.

However, from my testing, it is not possible to have a real Windows screensaver
(`.scr` launched by the OS) affect only one monitor while leaving the others
showing their normal desktop / video playback.

Even if a `.scr` program specifically draws on only one monitor, when Windows
activates a screensaver due to timeout, Explorer switches into a screensaver
mode and the desktop window manager creates an internal blank backdrop surface
which is applied to *all* monitors.

Also, one of the primary issues I had with the built-in screen saver was it's
apparent disabling of Bluetooth media controls, so using the built-in screen
saver wouldn't resolve this particular issue.


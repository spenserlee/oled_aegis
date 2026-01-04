# Build Instructions for OLED Aegis

## Prerequisites

You need Visual Studio installed with the C++ build tools. You can use:
- Visual Studio 2022/2019/2017/2015 (Community edition is free)
- Or just the "Build Tools for Visual Studio" from Microsoft

## Quick Start (Windows)

### Option 1: Using build.bat (Easiest)

1. Open **Developer Command Prompt for VS 2022** (or your version)
   - Find it in the Start Menu: "Developer Command Prompt for VS 2022"

2. Navigate to the project directory:
   ```batch
   cd C:\path\to\oled_aegis
   ```

3. Run the build script:
   ```batch
   build.bat
   ```

4. Run the application:
   ```batch
   oled_aegis.exe
   ```

### Option 2: Using build.ps1 (Recommended for WSL users)

1. Open PowerShell (can be run from WSL or Windows)

2. Navigate to the project directory:
   ```powershell
   cd C:\path\to\oled_aegis
   ```

3. Run the build script:
   ```powershell
   .\build.ps1
   ```

4. For a debug build:
   ```powershell
   .\build.ps1 debug
   ```

### Option 3: Using build.sh from WSL

1. Open WSL terminal

2. Navigate to the project directory (mounted Windows drive):
   ```bash
   cd /mnt/c/path/to/oled_aegis
   ```

3. Run the build script:
   ```bash
   ./build.sh
   ```

4. For a debug build:
   ```bash
   ./build.sh debug
   ```

#### Note on WSL development

WSL is nice but obviously not an ideal Windows application development
environment. To workaround LSP errors (e.g. missing "windows.h"), one can
install `cargo-xwin` to obtain the necessary Windows SDK components:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
cargo install xwin --locked
xwin --accept-license splat --output ~/.xwin
# Update .clangd paths!
# Unfortunately clangd configuration doesn't support env vars like $USER.
```

## Manual Build

If you prefer to compile manually from a Developer Command Prompt:

```batch
cl.exe src\oled_aegis.c /Fe:oled_aegis.exe /O2 /MD /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib
```

## Build Options

### PowerShell/build.ps1 Features

The PowerShell build script includes:

* **Environment Variable Caching**: MSVC environment variables are cached in `build/vc_env.txt` to speed up subsequent builds
* **Build Type Selection**: Choose between `release` (optimized) or `debug` builds
* **Automatic Directory Creation**: Creates `build/` directory if it doesn't exist
* **Error Handling**: Proper error messages if Visual Studio is not found

### Compiler Flags

* **/O2** - Maximum optimization (fastest code, smallest size) - release builds only
* **/FC** - Display full path in diagnostics
* **/Zi** - Generate debugging information - debug builds only
* **/MD** - Link with multi-threaded DLL runtime (release)
* **/MDd** - Link with debug multi-threaded DLL runtime (debug)

### Linked Libraries

* **user32.lib** - Windows user interface functions
* **shell32.lib** - Shell functions (for system tray)
* **ole32.lib** - COM functions (for audio detection)
* **uuid.lib** - GUID definitions for audio APIs
* **gdi32.lib** - Graphics Device Interface (for creating solid black brushes)
* **advapi32.lib** - Advanced Windows APIs (for registry functions)
* **comctl32.lib** - Common Controls library (for NumericUpDown control)

## Troubleshooting

### "cl.exe is not recognized"

This means you're not using a Developer Command Prompt. You need to:
1. Open Start Menu
2. Search for "Developer Command Prompt for VS 2022"
3. Run that instead of regular cmd.exe

### Build succeeds but application crashes on startup

Make sure you have:
- Windows 10 or Windows 11
- Audio drivers installed (for audio detection feature)

## Clean Build

### From Windows (build.bat)
```batch
del oled_aegis.exe
del oled_aegis.obj
build.bat
```

### From PowerShell (build.ps1)
```powershell
Remove-Item build\oled_aegis.exe -ErrorAction SilentlyContinue
.\build.ps1
```

### From WSL (build.sh)
```bash
rm -f build/oled_aegis.exe
./build.sh
```

## Clearing Environment Cache (PowerShell/WSL only)

If you change your Visual Studio installation or environment settings, delete the cached environment variables:

```bash
rm build/vc_env.txt
```

Then rebuild to regenerate the cache.

## Running without Installing

The compiled `oled_aegis.exe` is a standalone executable. You can:
- Copy it anywhere on your computer
- Run it from a USB drive
- No installation required

The application will automatically create its config file in `%APPDATA%\OLED_Aegis\oled_aegis.ini` on first run.

#!/bin/bash

# Wrapper script to build oled_aegis from WSL using PowerShell
# This runs the build.ps1 script via PowerShell on Windows

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check if PowerShell is available
if ! command -v powershell.exe &> /dev/null; then
    echo "Error: PowerShell not found. Please install PowerShell on Windows."
    exit 1
fi

# Run the PowerShell build script
# Pass any arguments (e.g., "debug" or "release")
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$SCRIPT_DIR/build.ps1" "$@"

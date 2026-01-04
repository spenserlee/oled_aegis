$ErrorActionPreference = "Stop"

$buildDir = Join-Path $PSScriptRoot "build"
if (!(Test-Path -Path $buildDir -PathType Container)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}
Push-Location $buildDir

$sourceFile = Join-Path $PSScriptRoot "src\oled_aegis.c"
$outputExe = "oled_aegis.exe"

# Get environment variables from vcvarsall.bat
# Cache them to a file vc_env.txt.
# NOTE: delete cache file if MSVC setup changes.
$envCache = Join-Path $buildDir "vc_env.txt"
if (Test-Path $envCache) {
    Get-Content $envCache | ForEach-Object {
        $key, $value = $_ -split '=', 2
        Set-Item -Path "env:$key" -Value $value
    }
} else {
    $vsPath = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    if (!(Test-Path $vsPath)) {
        Write-Error "Visual Studio not found at expected path: $vsPath"
        Pop-Location
        exit 1
    }

    $envVars = & "cmd.exe" /c "`"$vsPath`" x64 > nul && set"
    $envVars | Where-Object { $_ -notmatch '^PROMPT=' } > $envCache
    foreach ($line in $envVars) {
        $key, $value = $line -split '=', 2
        if ($key -ne "PROMPT") {
            Set-Item -Path "env:$key" -Value $value
        }
    }
}

# Check if source file exists
if (!(Test-Path $sourceFile)) {
    Write-Error "Source file not found: $sourceFile"
    Pop-Location
    exit 1
}

# Determine build type from arguments
$buildType = if ($args.Count -gt 0) { $args[0] } else { "release" }

Write-Host "Building OLED Aegis ($buildType)..." -ForegroundColor Green

if ($buildType -eq "debug") {
    # Debug Compile
    Write-Host "Configuration: Debug" -ForegroundColor Yellow
    cl.exe -FC -Zi -MDd /D "INITGUID" "$sourceFile" /Fe:"$outputExe" /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib
} else {
    # Optimized Compile (Release)
    Write-Host "Configuration: Release (Optimized)" -ForegroundColor Yellow
    cl.exe -O2 -FC -MD /D "INITGUID" "$sourceFile" /Fe:"$outputExe" /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib
}

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBuild successful!" -ForegroundColor Green
    Write-Host "Output: $outputExe" -ForegroundColor Cyan
    Write-Host "Location: $(Get-Location)" -ForegroundColor Cyan
} else {
    Write-Host "`nBuild failed with exit code: $LASTEXITCODE" -ForegroundColor Red
    Pop-Location
    exit $LASTEXITCODE
}

Pop-Location

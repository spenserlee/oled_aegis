@echo off
echo Building OLED Aegis...

if not exist build mkdir build
cd build

echo Compiling resources...
rc.exe /nologo /fo oled_aegis.res ..\src\oled_aegis.rc
if %ERRORLEVEL% NEQ 0 (
    echo Resource compilation failed!
    pause
    exit /b %ERRORLEVEL%
)

echo Compiling executable...
cl.exe ..\src\oled_aegis.c /Fe:oled_aegis.exe /O2 /nologo /MD /D "INITGUID" /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib powrprof.lib psapi.lib oled_aegis.res
if %ERRORLEVEL% EQU 0 (
    echo Build successful!
    echo Output: build\oled_aegis.exe
) else (
    echo Build failed!
    pause
)

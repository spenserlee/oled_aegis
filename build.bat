@echo off
echo Building OLED Aegis...
cl.exe src\oled_aegis.c /Fe:oled_aegis.exe /O2 /nologo /MD /D "INITGUID" /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib powrprof.lib
if %ERRORLEVEL% EQU 0 (
    echo Build successful!
    echo.
    echo Run oled_aegis.exe to start the application.
) else (
    echo Build failed!
    pause
)

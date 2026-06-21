#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <commctrl.h>
#include <powerbase.h>
#include <psapi.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

// The MMDevice / audio-session GUIDs are only extern-declared in the SDK
// headers, not DEFINE_GUID'd, so they don't resolve at link time. INITGUID is
// defined via the build command (/D "INITGUID"), so these DEFINE_GUID lines
// emit the actual definitions with SELECTANY storage.
DEFINE_GUID(CLSID_MMDeviceEnumerator,     0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator,      0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioSessionManager2,    0x77AA99A0, 0x1BD6, 0x484F, 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F);
DEFINE_GUID(IID_IAudioSessionControl2,    0xBFB7FF88, 0x7239, 0x4FC9, 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D);
DEFINE_GUID(IID_IAudioMeterInformation,   0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64);

#define APP_NAME L"OLED Aegis"
#define WM_TRAYICON (WM_USER + 1)
#define TIMER_IDLE_CHECK 1
#define DEFAULT_IDLE_TIMEOUT 300
#define MAX_LOG_SIZE_BYTES (1 * 1024 * 1024)  // 1 MB log file size limit
#define MANUAL_ACTIVATION_COOLDOWN_MS 2500
#define MAX_MONITOR_COUNT 16

// Resource IDs (must match oled_aegis.rc)
#define IDI_ICON_ACTIVE   101
#define IDI_ICON_INACTIVE 102

// Settings dialog control IDs
#define IDC_TIMEOUT_EDIT            1001
#define IDC_MEDIA_CHECK             1002
#define IDC_DEBUG_CHECK             1003
#define IDC_STARTUP_CHECK           1004
#define IDC_APPLY_BTN               1005
#define IDC_CONFIG_BTN              1006
#define IDC_CLOSE_BTN               1007
#define IDC_INTERVAL_EDIT           1008
#define IDC_PERMONITOR_CHECK        1009
#define IDC_PERMONITOR_MEDIA_CHECK  1011
#define IDC_MUTED_MEDIA_CHECK       1012
#define IDC_PIXELSHIFT_EDIT         1010
#define IDC_MONITOR_BASE            2000  // Monitor checkboxes: IDC_MONITOR_BASE + index

// Tray context menu command IDs
#define IDM_SETTINGS            1
#define IDM_EXIT                2

// Timing constants
#define INPUT_IGNORE_DELAY_MS           500     // Delay after screen saver window creation to ignore input
#define IDLE_ACTIVITY_THRESHOLD_MS      1000    // Time threshold to consider user active (1 second)
#define IDLE_DEACTIVATE_THRESHOLD_MS    2000    // Time threshold to deactivate screen saver after input
#define IDLE_DEACTIVATE_THRESHOLD_SEC   2       // Time threshold in seconds (for per-monitor mode)
#define SHELL_CLOSE_DELAY_MS            250     // Delay after sending Escape to close shell windows
#define SHELL_CLOSE_MAX_ATTEMPTS        2       // Maximum attempts to close shell windows
#define MIN_MEDIA_WINDOW_AREA           10000   // Ignore tiny windows when mapping media to monitors
#define MIN_MEDIA_WINDOW_OVERLAP_RATIO  0.10    // Ignore thin window-border overlap onto adjacent monitors
#define MEDIA_DETECTION_CACHE_MS        2000    // Cache media-window scans to keep timer work light
#define AUDIO_ACTIVE_PEAK_THRESHOLD     0.0001f // Ignore paused/silent sessions that remain "active"
#define CURSOR_COUNTER_MAX_ATTEMPTS     16      // Safety bound when normalizing ShowCursor's counter
#define TOPMOST_REFRESH_INTERVAL_MS     5000    // Reassert topmost occasionally, not every timer tick
#define MAX_ACTIVE_AUDIO_PIDS           64      // Upper bound on concurrently active audio sessions we track
#define MAX_BROWSER_WINDOW_INFO         32      // Max browser windows to collect for diagnostic logging

// Check interval bounds (milliseconds)
#define MIN_CHECK_INTERVAL_MS   250
#define MAX_CHECK_INTERVAL_MS   10000

// Idle timeout bounds (seconds)
#define MIN_IDLE_TIMEOUT_SEC    5
#define MAX_IDLE_TIMEOUT_SEC    3600

// Pixel shift compensation bounds (pixels)
#define MIN_PIXEL_SHIFT_COMPENSATION    0
#define MAX_PIXEL_SHIFT_COMPENSATION    1024

// Device name prefix for display devices (e.g., "\\.\DISPLAY1")
#define DEVICE_NAME_PREFIX      "\\\\.\\"
#define DEVICE_NAME_PREFIX_LEN  4

static char g_logFilePath[MAX_PATH];
static FILE* g_logFile = NULL;
static char g_appDataPath[MAX_PATH];
static int g_appDataPathInitialized = 0;

void ApplySettings(HWND hWnd);
LRESULT CALLBACK SettingsDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void ShowScreenSaver(int isManual);
void ShowScreenSaverOnMonitor(int monitorIndex, int isManual);
void HideScreenSaver();
void HideScreenSaverOnMonitor(int monitorIndex);
int IsAnyMonitorActive();
void UpdateTrayIcon(int active);
void LogMessage(const char* format, ...);
int FindMonitorByDeviceName(const char* deviceName);
int FindMonitorByDevicePath(const char* devicePath);
int FindPrimaryMonitorIndex();
int IsAnyMonitorEnabled();
int GetProcessNameFromHwnd(HWND hWnd, char* buffer, int bufferSize);
int UpdateMediaMonitorStates(int mediaOnMonitor[MAX_MONITOR_COUNT]);
void ResetMediaDetectionCache();

typedef struct {
    HMONITOR hMonitor;
    RECT rect;
    int monitorIndex;
    char deviceName[CCHDEVICENAME];     // GDI device name (e.g., \\.\DISPLAY1)
    char displayName[128];              // UI display name (friendly name + resolution)
    char friendlyName[64];              // EDID friendly name (e.g., "LG OLED48C1")
    char monitorDevicePath[256];        // Persistent device path for config matching
    int isPrimary;
    int width;
    int height;
} MonitorInfo;

typedef struct {
    time_t lastInputTime;
    int screenSaverActive;
    int enabled;
    HWND hScreenSaverWnd;
} MonitorState;

typedef struct {
    int idleTimeout;
    int checkInterval;
    int mediaDetectionEnabled;
    int monitorsEnabled[MAX_MONITOR_COUNT];
    int monitorCount;
    int startupEnabled;
    int debugMode;
    int perMonitorInputDetection;
    int perMonitorMediaDetection;
    int blockOnMutedMedia;
    int pixelShiftCompensation;
} Config;

typedef struct {
    HWND hWnd;
    Config config;
    NOTIFYICONDATAA nid;
    int screenSaverActive;
    int isShuttingDown;
    int cursorHidden;
    int trayMenuActive;
    int trayIconActive;
    DWORD manualActivationTime;
    int isManualActivation;
} AppState;

static AppState g_app;
static HANDLE g_hInstanceMutex = NULL;  // Single-instance mutex (kept for app lifetime)

static UINT g_settingsDpi = 96;
static HBRUSH g_blackBrush = NULL;
static HWND g_hSettingsDialog = NULL;
static HFONT g_hSettingsFont = NULL;
static HICON g_hIconActive = NULL;
static HICON g_hIconInactive = NULL;
static HWND g_hTooltipControl = NULL;
static int g_monitorCount = 0;
static int g_currentMonitorIndex = 0;
static MonitorInfo g_monitors[MAX_MONITOR_COUNT];
static MonitorState g_monitorStates[MAX_MONITOR_COUNT];
static UINT g_uTaskbarRestart = 0;  // Registered "TaskbarCreated" message ID (0 if not registered)
static int g_mediaCacheInvalidated = 0;  // Set by WM_POWERBROADCAST to force media cache refresh

int ClampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

void ClampConfigValues() {
    g_app.config.idleTimeout = ClampInt(g_app.config.idleTimeout, MIN_IDLE_TIMEOUT_SEC, MAX_IDLE_TIMEOUT_SEC);
    g_app.config.checkInterval = ClampInt(g_app.config.checkInterval, MIN_CHECK_INTERVAL_MS, MAX_CHECK_INTERVAL_MS);
    g_app.config.pixelShiftCompensation = ClampInt(
        g_app.config.pixelShiftCompensation,
        MIN_PIXEL_SHIFT_COMPENSATION,
        MAX_PIXEL_SHIFT_COMPENSATION
    );

    g_app.config.mediaDetectionEnabled = g_app.config.mediaDetectionEnabled ? 1 : 0;
    g_app.config.startupEnabled = g_app.config.startupEnabled ? 1 : 0;
    g_app.config.debugMode = g_app.config.debugMode ? 1 : 0;
    g_app.config.perMonitorInputDetection = g_app.config.perMonitorInputDetection ? 1 : 0;
    g_app.config.perMonitorMediaDetection = g_app.config.perMonitorMediaDetection ? 1 : 0;
    g_app.config.blockOnMutedMedia = g_app.config.blockOnMutedMedia ? 1 : 0;
}

int IsAppUiActive() {
    return g_app.trayMenuActive;
}

// Restore the cursor, handling ShowCursor's reference-counted nature. The
// counter can drift if ShowCursor calls are missed (e.g. due to focus changes),
// so we loop until the cursor is actually showing or we hit a safety bound.
// Also checks GetCursorInfo as a fallback when the cursorHidden flag is wrong.
void EnsureCursorVisible(const char* reason) {
    int adjusted = 0;
    int count = 0;

    if (g_app.cursorHidden) {
        do {
            count = ShowCursor(TRUE);
            adjusted++;
        } while (count < 0 && adjusted < CURSOR_COUNTER_MAX_ATTEMPTS);

        g_app.cursorHidden = 0;
    } else {
        CURSORINFO cursorInfo = {0};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) == 0) {
            do {
                count = ShowCursor(TRUE);
                adjusted++;
            } while (count < 0 && adjusted < CURSOR_COUNTER_MAX_ATTEMPTS);
        }
    }

    if (adjusted) {
        LogMessage("Cursor restored (%s, count=%d, adjustments=%d)",
                   reason ? reason : "unknown", count, adjusted);
    }
}

// Hide the cursor for the screen saver, unless app UI (tray menu / settings
// dialog) is active — in that case the cursor should stay visible.
void HideCursorForScreenSaver(const char* reason) {
    if (IsAppUiActive()) {
        EnsureCursorVisible(reason ? reason : "app UI active");
        return;
    }

    if (!g_app.cursorHidden) {
        int count = ShowCursor(FALSE);
        g_app.cursorHidden = 1;
        LogMessage("Cursor hidden (%s, count=%d)", reason ? reason : "screen saver", count);
    }
}

int ScaleDPI(int value) {
    return MulDiv(value, g_settingsDpi, 96);
}

UINT GetDpiForWindowCompat(HWND hWnd) {
    // GetDpiForWindow requires Windows 10 1607+
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pfnGetDpiForWindow = NULL;
    static int checked = 0;

    if (!checked) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            pfnGetDpiForWindow = (PFN_GetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
        }
        checked = 1;
    }

    if (pfnGetDpiForWindow && hWnd) {
        return pfnGetDpiForWindow(hWnd);
    }

    // Fallback for older Windows versions
    HDC hdc = GetDC(hWnd);
    UINT dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hWnd, hdc);
    return dpi ? dpi : 96;
}

void GetAppDataPath(char* buffer, size_t bufferSize) {
    if (!g_appDataPathInitialized) {
        SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, g_appDataPath);
        sprintf_s(g_appDataPath, sizeof(g_appDataPath), "%s\\OLED_Aegis", g_appDataPath);
        CreateDirectoryA(g_appDataPath, NULL);
        g_appDataPathInitialized = 1;
    }

    strncpy_s(buffer, bufferSize, g_appDataPath, _TRUNCATE);
}

void LoadTrayIcons() {
    HINSTANCE hInstance = GetModuleHandle(NULL);

    // Load icons from embedded resources
    g_hIconActive = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_ACTIVE));
    g_hIconInactive = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_INACTIVE));

    // Fall back to system icons if resource icons not found
    if (!g_hIconActive) {
        g_hIconActive = LoadIcon(NULL, IDI_APPLICATION);
    }
    if (!g_hIconInactive) {
        g_hIconInactive = LoadIcon(NULL, IDI_INFORMATION);
    }
}

void RotateLogFileIfNeeded() {
    if (!g_logFile) return;

    // Check current file size
    long pos = ftell(g_logFile);
    if (pos < 0 || pos < MAX_LOG_SIZE_BYTES) return;

    // Close current log file
    fclose(g_logFile);
    g_logFile = NULL;

    // Create path for old log file
    char oldLogPath[MAX_PATH];
    sprintf_s(oldLogPath, MAX_PATH, "%s.old", g_logFilePath);

    // Delete existing .old file and rename current to .old
    DeleteFileA(oldLogPath);
    MoveFileA(g_logFilePath, oldLogPath);

    // Reopen fresh log file
    g_logFile = fopen(g_logFilePath, "a");
    if (g_logFile) {
        time_t now = time(NULL);
        char timeStr[64];
        ctime_s(timeStr, sizeof(timeStr), &now);
        timeStr[24] = '\0';
        fprintf(g_logFile, "\n=== Log rotated at %s (previous log saved as .old) ===\n", timeStr);
        fflush(g_logFile);
    }
}

void LogMessage(const char* format, ...) {
    if (!g_app.config.debugMode) return;

    if (!g_logFile) {
        char appDataPath[MAX_PATH];
        GetAppDataPath(appDataPath, sizeof(appDataPath));
        sprintf_s(g_logFilePath, MAX_PATH, "%s\\oled_aegis_debug.log", appDataPath);

        g_logFile = fopen(g_logFilePath, "a");
        if (g_logFile) {
            time_t now = time(NULL);
            char timeStr[64];
            ctime_s(timeStr, sizeof(timeStr), &now);
            timeStr[24] = '\0';
            fprintf(g_logFile, "\n=== OLED Aegis Started at %s ===\n", timeStr);
            fflush(g_logFile);
        }
    }

    if (g_logFile) {
        // Check if log rotation is needed
        RotateLogFileIfNeeded();

        time_t now = time(NULL);
        char timeStr[64];
        ctime_s(timeStr, sizeof(timeStr), &now);
        timeStr[24] = '\0';

        va_list args;
        va_start(args, format);
        fprintf(g_logFile, "[%s] ", timeStr);
        vfprintf(g_logFile, format, args);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
        va_end(args);
    }
}

// Get monitor friendly name and device path using DisplayConfig API
// Returns 1 on success, 0 on failure
int GetMonitorIdentifiers(const char* gdiDeviceName,
                          char* friendlyName, int friendlyNameLen,
                          char* devicePath, int devicePathLen) {
    UINT32 pathCount = 0, modeCount = 0;
    int result = 0;

    if (friendlyName && friendlyNameLen > 0) friendlyName[0] = '\0';
    if (devicePath && devicePathLen > 0) devicePath[0] = '\0';

    LONG ret = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (ret != ERROR_SUCCESS || pathCount == 0) {
        return 0;
    }

    DISPLAYCONFIG_PATH_INFO* paths = malloc(pathCount * sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO* modes = malloc(modeCount * sizeof(DISPLAYCONFIG_MODE_INFO));
    if (!paths || !modes) {
        free(paths);
        free(modes);
        return 0;
    }

    ret = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths, &modeCount, modes, NULL);
    if (ret != ERROR_SUCCESS) {
        free(paths);
        free(modes);
        return 0;
    }

    // Convert GDI device name to wide string for comparison
    WCHAR gdiDeviceNameW[CCHDEVICENAME];
    MultiByteToWideChar(CP_ACP, 0, gdiDeviceName, -1, gdiDeviceNameW, CCHDEVICENAME);

    // Find matching path
    for (UINT32 i = 0; i < pathCount; i++) {
        // Get source device name (GDI device name like \\.\DISPLAY1)
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {0};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
        sourceName.header.id = paths[i].sourceInfo.id;

        ret = DisplayConfigGetDeviceInfo(&sourceName.header);
        if (ret != ERROR_SUCCESS) {
            continue;
        }

        // Check if this source matches our GDI device name
        if (wcscmp(sourceName.viewGdiDeviceName, gdiDeviceNameW) != 0) {
            continue;
        }

        // Found matching source, now get target (monitor) info
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {0};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = paths[i].targetInfo.adapterId;
        targetName.header.id = paths[i].targetInfo.id;

        ret = DisplayConfigGetDeviceInfo(&targetName.header);
        if (ret != ERROR_SUCCESS) {
            continue;
        }

        // Extract friendly name (if available from EDID)
        if (friendlyName && friendlyNameLen > 0) {
            if (targetName.flags.friendlyNameFromEdid) {
                WideCharToMultiByte(CP_UTF8, 0, targetName.monitorFriendlyDeviceName, -1,
                                   friendlyName, friendlyNameLen, NULL, NULL);
            } else {
                strncpy(friendlyName, "Unknown Monitor", friendlyNameLen - 1);
                friendlyName[friendlyNameLen - 1] = '\0';
            }
        }

        // Extract device path (persistent identifier)
        if (devicePath && devicePathLen > 0) {
            WideCharToMultiByte(CP_UTF8, 0, targetName.monitorDevicePath, -1,
                               devicePath, devicePathLen, NULL, NULL);
        }

        result = 1;
        break;
    }

    free(paths);
    free(modes);
    return result;
}

BOOL CALLBACK EnumMonitorCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(MONITORINFOEXA);
    GetMonitorInfoA(hMonitor, (LPMONITORINFO)&mi);

    if (g_monitorCount < MAX_MONITOR_COUNT) {
        g_monitors[g_monitorCount].hMonitor = hMonitor;
        g_monitors[g_monitorCount].rect = *lprcMonitor;
        g_monitors[g_monitorCount].monitorIndex = g_monitorCount;
        g_monitors[g_monitorCount].isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        strncpy(g_monitors[g_monitorCount].deviceName, mi.szDevice, CCHDEVICENAME);
        g_monitors[g_monitorCount].deviceName[31] = '\0';

        DEVMODEA dm = {0};
        dm.dmSize = sizeof(DEVMODEA);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            g_monitors[g_monitorCount].width = dm.dmPelsWidth;
            g_monitors[g_monitorCount].height = dm.dmPelsHeight;
        } else {
            g_monitors[g_monitorCount].width = lprcMonitor->right - lprcMonitor->left;
            g_monitors[g_monitorCount].height = lprcMonitor->bottom - lprcMonitor->top;
        }

        // Get friendly name and device path using DisplayConfig API
        int gotIdentifiers = GetMonitorIdentifiers(
            mi.szDevice,
            g_monitors[g_monitorCount].friendlyName,
            sizeof(g_monitors[g_monitorCount].friendlyName),
            g_monitors[g_monitorCount].monitorDevicePath,
            sizeof(g_monitors[g_monitorCount].monitorDevicePath)
        );

        // Fallback: use GDI device name if DisplayConfig failed
        if (!gotIdentifiers || g_monitors[g_monitorCount].friendlyName[0] == '\0') {
            const char* fallbackName = mi.szDevice;
            if (strncmp(fallbackName, DEVICE_NAME_PREFIX, DEVICE_NAME_PREFIX_LEN) == 0) {
                fallbackName += DEVICE_NAME_PREFIX_LEN;
            }
            strncpy(g_monitors[g_monitorCount].friendlyName, fallbackName,
                    sizeof(g_monitors[g_monitorCount].friendlyName) - 1);
            g_monitors[g_monitorCount].friendlyName[sizeof(g_monitors[g_monitorCount].friendlyName) - 1] = '\0';
        }

        // Fallback: use GDI device name as device path if not available
        if (!gotIdentifiers || g_monitors[g_monitorCount].monitorDevicePath[0] == '\0') {
            strncpy(g_monitors[g_monitorCount].monitorDevicePath, mi.szDevice,
                    sizeof(g_monitors[g_monitorCount].monitorDevicePath) - 1);
            g_monitors[g_monitorCount].monitorDevicePath[sizeof(g_monitors[g_monitorCount].monitorDevicePath) - 1] = '\0';
        }

        // Format display name with friendly name, resolution, and primary indicator
        snprintf(g_monitors[g_monitorCount].displayName,
                sizeof(g_monitors[g_monitorCount].displayName),
                "%s (%dx%d)%s",
                g_monitors[g_monitorCount].friendlyName,
                g_monitors[g_monitorCount].width,
                g_monitors[g_monitorCount].height,
                g_monitors[g_monitorCount].isPrimary ? " [Primary]" : "");

        g_monitorCount++;
    }

    return TRUE;
}

LRESULT CALLBACK MonitorWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static DWORD ignoreInputUntil = 0;

    switch (message) {
        case WM_CREATE:
            ignoreInputUntil = GetTickCount() + INPUT_IGNORE_DELAY_MS;
            break;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rect;
            GetClientRect(hWnd, &rect);
            FillRect(hdc, &rect, g_blackBrush);
            EndPaint(hWnd, &ps);
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (GetTickCount() < ignoreInputUntil) {
                break;
            }
            LogMessage("Input detected on monitor window (msg: %u)", message);
            if (g_app.config.perMonitorInputDetection) {
                for (int i = 0; i < g_monitorCount; i++) {
                    if (g_monitorStates[i].hScreenSaverWnd == hWnd) {
                        HideScreenSaverOnMonitor(i);
                        break;
                    }
                }
                if (!IsAnyMonitorActive()) {
                    g_app.screenSaverActive = 0;
                }
                UpdateTrayIcon(IsAnyMonitorActive() ? 1 : 0);
            } else {
                HideScreenSaver();
                UpdateTrayIcon(0);
            }
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int ConfigFileExists() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    FILE* f = fopen(configPath, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

void LoadConfig() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    g_app.config.monitorCount = g_monitorCount;

    int hadMonitorConfig = 0;  // Track if we found any monitor config entries
    int anyMonitorMatched = 0; // Track if any monitor config matched current monitors

    FILE* f = fopen(configPath, "r");
    if (f) {
        char line[512];  // Increased buffer size for longer device paths
        while (fgets(line, sizeof(line), f)) {
            // Strip inline comments (everything after ';')
            char* comment = strchr(line, ';');
            if (comment) *comment = '\0';

            // Trim trailing whitespace
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' || line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }

            char key[300], value[64];  // Increased key size for device paths
            if (sscanf(line, "%299[^=]=%63s", key, value) == 2) {
                if (strcmp(key, "idleTimeout") == 0) {
                    g_app.config.idleTimeout = atoi(value);
                } else if (strcmp(key, "checkInterval") == 0) {
                    g_app.config.checkInterval = atoi(value);
                } else if (strcmp(key, "audioDetectionEnabled") == 0) {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                } else if (strcmp(key, "mediaDetectionEnabled") == 0) {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                } else if (strcmp(key, "startupEnabled") == 0) {
                    g_app.config.startupEnabled = atoi(value);
                } else if (strcmp(key, "debugMode") == 0) {
                    g_app.config.debugMode = atoi(value);
                } else if (strcmp(key, "perMonitorInputDetection") == 0) {
                    g_app.config.perMonitorInputDetection = atoi(value);
                } else if (strcmp(key, "perMonitorMediaDetection") == 0) {
                    g_app.config.perMonitorMediaDetection = atoi(value);
                } else if (strcmp(key, "blockOnMutedMedia") == 0) {
                    g_app.config.blockOnMutedMedia = atoi(value);
                } else if (strcmp(key, "pixelShiftCompensation") == 0) {
                    g_app.config.pixelShiftCompensation = atoi(value);
                } else if (strncmp(key, "monitorEnabled_", 15) == 0) {
                    const char* identifier = key + 15;
                    hadMonitorConfig = 1;

                    // Try matching by device path first (new format)
                    int idx = FindMonitorByDevicePath(identifier);
                    if (idx < 0) {
                        // Fall back to device name match (legacy format: \\.\DISPLAY1)
                        idx = FindMonitorByDeviceName(identifier);
                    }

                    if (idx >= 0 && idx < MAX_MONITOR_COUNT) {
                        g_app.config.monitorsEnabled[idx] = atoi(value);
                        if (atoi(value)) {
                            anyMonitorMatched = 1;
                        }
                        LogMessage("Config: matched monitor %d (%s) from identifier: %s",
                                  idx, g_monitors[idx].friendlyName, identifier);
                    } else {
                        LogMessage("Config: no match for monitor identifier: %s", identifier);
                    }
                } else if (strncmp(key, "monitor", 7) == 0 && key[7] >= '0' && key[7] <= '9') {
                    // Legacy format: monitor0=1, monitor1=0, etc.
                    int idx = atoi(key + 7);
                    hadMonitorConfig = 1;
                    if (idx >= 0 && idx < MAX_MONITOR_COUNT && idx < g_monitorCount) {
                        g_app.config.monitorsEnabled[idx] = atoi(value);
                        if (atoi(value)) {
                            anyMonitorMatched = 1;
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    // Fallback: if we had monitor config but none matched, enable the primary monitor
    // This handles the case where display configuration changed (monitors unplugged/replugged)
    if (hadMonitorConfig && !anyMonitorMatched) {
        int primaryIdx = FindPrimaryMonitorIndex();
        if (primaryIdx >= 0) {
            g_app.config.monitorsEnabled[primaryIdx] = 1;
            LogMessage("Config fallback: no monitors matched saved config, enabled primary monitor %d (%s)",
                      primaryIdx, g_monitors[primaryIdx].friendlyName);
        }
    }

    ClampConfigValues();
}

void SaveConfig() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    FILE* f = fopen(configPath, "w");
    if (f) {
        fprintf(f, "idleTimeout=%d\n", g_app.config.idleTimeout);
        fprintf(f, "checkInterval=%d\n", g_app.config.checkInterval);
        fprintf(f, "mediaDetectionEnabled=%d\n", g_app.config.mediaDetectionEnabled);
        fprintf(f, "startupEnabled=%d\n", g_app.config.startupEnabled);
        fprintf(f, "debugMode=%d\n", g_app.config.debugMode);
        fprintf(f, "perMonitorInputDetection=%d\n", g_app.config.perMonitorInputDetection);
        fprintf(f, "perMonitorMediaDetection=%d\n", g_app.config.perMonitorMediaDetection);
        fprintf(f, "blockOnMutedMedia=%d\n", g_app.config.blockOnMutedMedia);
        fprintf(f, "pixelShiftCompensation=%d\n", g_app.config.pixelShiftCompensation);
        // Save monitor settings using persistent device path as key, with comment showing friendly name
        for (int i = 0; i < g_monitorCount; i++) {
            fprintf(f, "monitorEnabled_%s=%d ; %s\n",
                    g_monitors[i].monitorDevicePath,
                    g_app.config.monitorsEnabled[i],
                    g_monitors[i].displayName);
        }
        fclose(f);
    }
}

void UpdateStartupRegistry() {
    HKEY hKey;
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (g_app.config.startupEnabled) {
            DWORD valueSize = (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR));
            LONG result = RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE*)exePath, valueSize);
            if (result == ERROR_SUCCESS) {
                RegFlushKey(hKey);
            }
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

DWORD GetIdleTime() {
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(LASTINPUTINFO);
    GetLastInputInfo(&lii);
    return GetTickCount() - lii.dwTime;
}

int GetMonitorIndexFromPoint(POINT pt) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (PtInRect(&g_monitors[i].rect, pt)) {
            return i;
        }
    }
    return -1;
}

int GetMonitorIndexFromRect(RECT rect) {
    POINT center = {
        (rect.left + rect.right) / 2,
        (rect.top + rect.bottom) / 2
    };
    return GetMonitorIndexFromPoint(center);
}

int IsAnyMonitorActive() {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].screenSaverActive) {
            return 1;
        }
    }
    return 0;
}

int IsAnyMonitorEnabled() {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].enabled) {
            return 1;
        }
    }
    return 0;
}

int FindMonitorByDeviceName(const char* deviceName) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (strcmp(g_monitors[i].deviceName, deviceName) == 0) {
            return i;
        }
    }
    return -1;
}

int FindMonitorByDevicePath(const char* devicePath) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (strcmp(g_monitors[i].monitorDevicePath, devicePath) == 0) {
            return i;
        }
    }
    return -1;
}

int FindPrimaryMonitorIndex() {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].isPrimary) {
            return i;
        }
    }
    return -1;
}

void HideScreenSaverOnMonitor(int monitorIndex) {
    if (monitorIndex < 0 || monitorIndex >= g_monitorCount) return;

    if (g_monitorStates[monitorIndex].hScreenSaverWnd) {
        ShowWindow(g_monitorStates[monitorIndex].hScreenSaverWnd, SW_HIDE);
        LogMessage("Screen saver window hidden on monitor %d", monitorIndex);
    }

    g_monitorStates[monitorIndex].screenSaverActive = 0;
    g_monitorStates[monitorIndex].lastInputTime = time(NULL);
}

void EnumerateMonitors() {
    g_monitorCount = 0;
    EnumDisplayMonitors(NULL, NULL, EnumMonitorCallback, 0);
    LogMessage("Enumerated %d monitors", g_monitorCount);
}

int IsMediaPlaying() {
    static int lastMediaState = -1;

    if (!g_app.config.mediaDetectionEnabled) {
        return 0;
    }

    ULONG executionState = 0;
    NTSTATUS status = CallNtPowerInformation(
        SystemExecutionState,
        NULL, 0,
        &executionState, sizeof(executionState)
    );

    if (status == 0) {
        int isPlaying = (executionState & ES_DISPLAY_REQUIRED) != 0;
        // Only log when state changes to reduce noise
        if (isPlaying != lastMediaState) {
            LogMessage("Media detection: state changed to %s (executionState=0x%08X)",
                     isPlaying ? "PLAYING" : "NOT_PLAYING", executionState);
            lastMediaState = isPlaying;
        }
        return isPlaying;
    }

    LogMessage("Media detection: CallNtPowerInformation failed with status=%d", status);
    return 0;
}

// Get the process name (e.g., "explorer.exe") from a window handle
// Returns 1 on success, 0 on failure
int GetProcessNameFromHwnd(HWND hWnd, char* buffer, int bufferSize) {
    if (!hWnd || !buffer || bufferSize <= 0) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == 0) return 0;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) {
        // Try with fewer permissions
        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) return 0;
    }

    DWORD result = GetModuleBaseNameA(hProcess, NULL, buffer, bufferSize);
    CloseHandle(hProcess);

    return result > 0 ? 1 : 0;
}

// Case-insensitive substring search (MSVC _strnicmp-based)
int ContainsIgnoreCase(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;

    size_t needleLen = strlen(needle);
    if (needleLen == 0) return 1;

    for (const char* p = haystack; *p; p++) {
        if (_strnicmp(p, needle, needleLen) == 0) {
            return 1;
        }
    }

    return 0;
}

int ProcessNameMatchesAny(const char* processName, const char* const* names, int count) {
    if (!processName) return 0;

    for (int i = 0; i < count; i++) {
        if (_stricmp(processName, names[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

int IsKnownBrowserProcess(const char* processName) {
    static const char* const browserProcesses[] = {
        "chrome.exe",
        "msedge.exe",
        "firefox.exe",
        "brave.exe",
        "opera.exe",
        "opera_gx.exe",
        "vivaldi.exe",
        "arc.exe",
        "thorium.exe",
        "zen.exe"
    };

    return ProcessNameMatchesAny(
        processName,
        browserProcesses,
        (int)(sizeof(browserProcesses) / sizeof(browserProcesses[0]))
    );
}

// Known VIDEO player processes. Audio-only apps (Spotify, iTunes, etc.) are
// intentionally excluded: music playback does not keep the display on, so an
// open music player should not block the screen saver on its monitor. Video
// players are still gated by an active-audio check before they count as media.
int IsKnownMediaProcess(const char* processName) {
    static const char* const mediaProcesses[] = {
        "vlc.exe",
        "mpv.exe",
        "mpvnet.exe",
        "potplayer.exe",
        "potplayermini.exe",
        "potplayermini64.exe",
        "wmplayer.exe",
        "mpc-hc.exe",
        "mpc-hc64.exe",
        "mpc-be.exe",
        "mpc-be64.exe",
        "kodi.exe",
        "plex.exe",
        "jellyfinmediaplayer.exe",
        "embytheater.exe",
        "video.ui.exe"
    };

    return ProcessNameMatchesAny(
        processName,
        mediaProcesses,
        (int)(sizeof(mediaProcesses) / sizeof(mediaProcesses[0]))
    );
}

// Title hints for VIDEO playback sites. Audio-only services (Spotify,
// SoundCloud, Bandcamp, Apple Music) are excluded since music does not keep
// the display on. "YouTube Music" is covered by the "YouTube" hint.
int WindowTitleHasMediaHint(const char* title) {
    static const char* const mediaTitleHints[] = {
        "YouTube",
        "Twitch",
        "Netflix",
        "Hulu",
        "Disney+",
        "Prime Video",
        "Amazon Prime",
        "HBO Max",
        "Paramount+",
        "Peacock",
        "Crunchyroll",
        "Vimeo",
        "Dailymotion",
        "Plex",
        "Jellyfin",
        "Emby",
        "Media Player",
        "VLC media player",
        "Picture in picture",
        "TikTok",
        "/ X"           // For x.com titles are "Home / X", "@user / X", "user on X: ... / X"
    };

    if (!title || title[0] == '\0') return 0;

    for (int i = 0; i < (int)(sizeof(mediaTitleHints) / sizeof(mediaTitleHints[0])); i++) {
        if (ContainsIgnoreCase(title, mediaTitleHints[i])) {
            return 1;
        }
    }

    return 0;
}

// Returns 1 if the (processName, title) pair looks like a media-playing window,
// 0 otherwise. Known media players always count; browsers count only with a
// video-site title hint; any other process counts only with a title hint.
int IsMediaCandidateWindow(const char* processName, const char* title) {
    if (IsKnownMediaProcess(processName)) {
        return 1;
    }

    if (IsKnownBrowserProcess(processName)) {
        return WindowTitleHasMediaHint(title) ? 1 : 0;
    }

    return WindowTitleHasMediaHint(title) ? 1 : 0;
}

int IsWindowCloakedCompat(HWND hWnd) {
    DWORD cloaked = 0;
    HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

// Get the process name (e.g., "brave.exe") from a process ID.
// Uses QueryFullProcessImageNameW (needs only PROCESS_QUERY_LIMITED_INFORMATION)
// instead of GetModuleBaseNameA (needs PROCESS_VM_READ, which sandboxed
// Chromium renderer processes deny). Returns 1 on success, 0 on failure.
int GetProcessNameFromPid(DWORD pid, char* buffer, int bufferSize) {
    if (!buffer || bufferSize <= 0 || pid == 0) return 0;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return 0;

    WCHAR wpath[MAX_PATH] = {0};
    DWORD wpathLen = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProcess, 0, wpath, &wpathLen);
    CloseHandle(hProcess);

    if (!ok || wpathLen == 0) return 0;

    // Extract filename from full path (e.g. "C:\...\brave.exe" -> "brave.exe")
    WCHAR* wexe = wpath;
    for (WCHAR* p = wpath; *p; p++) {
        if (*p == L'\\') wexe = p + 1;
    }

    // Convert to narrow char (exe names are ASCII)
    int i = 0;
    for (; wexe[i] && i < bufferSize - 1; i++) {
        buffer[i] = (char)wexe[i];
    }
    buffer[i] = '\0';

    return buffer[0] != '\0' ? 1 : 0;
}

// Context passed to EnumMediaWindowCallback. Carries the per-monitor media
// flags being built plus the set of process names currently emitting audio.
// We match by name (not PID) because Chromium browsers (Chrome/Brave/Edge/etc.)
// run multi-process: the audio session's PID is the renderer process, while the
// browser window is owned by the main process. Both share the same exe name, so
// name matching bridges the gap. Single-process players (VLC, mpv) match too.
typedef struct {
    int mediaOnMonitor[MAX_MONITOR_COUNT];
    char audioActiveProcessNames[MAX_ACTIVE_AUDIO_PIDS][MAX_PATH];
    int audioActiveProcessNameCount;
    // Diagnostic info: all browser windows with active audio, collected during
    // enumeration and logged once in UpdateMediaMonitorStates when the mask changes.
    // This avoids per-tick log spam and shows both matching and non-matching windows.
    char browserTitles[MAX_BROWSER_WINDOW_INFO][256];
    int browserMatched[MAX_BROWSER_WINDOW_INFO];  // 1 = matched a hint, 0 = no hint
    int browserWindowCount;
} MediaEnumContext;

int IsAudioActiveProcessName(const MediaEnumContext* ctx, const char* processName) {
    if (!processName || !ctx) return 0;
    for (int i = 0; i < ctx->audioActiveProcessNameCount; i++) {
        if (_stricmp(ctx->audioActiveProcessNames[i], processName) == 0) {
            return 1;
        }
    }
    return 0;
}

// Returns 1 if every audio-active process is a known browser. Used to decide
// whether to skip the block-all fallback: when a browser has active audio but
// no window title matches a video hint (e.g. video in a background tab), we
// can't determine which monitor is playing. For browsers, not blocking is
// preferable to blocking everything, since the user's use case is to let the
// OLED sleep when video plays elsewhere. For non-browser apps (unknown apps,
// media players with minimized windows), we keep the safe block-all fallback.
int AllAudioActiveAreBrowsers(const MediaEnumContext* ctx) {
    if (ctx->audioActiveProcessNameCount == 0) return 0;
    for (int i = 0; i < ctx->audioActiveProcessNameCount; i++) {
        if (!IsKnownBrowserProcess(ctx->audioActiveProcessNames[i])) {
            return 0;
        }
    }
    return 1;
}

// Collect exe names of processes with an ACTIVE audio session on the default
// render endpoint. Returns the count filled into names[] (0 on any failure,
// which causes the caller's safe fallback to block all enabled monitors).
int CollectActiveAudioProcessNames(char names[][MAX_PATH], int maxNames) {
    if (!names || maxNames <= 0) return 0;

    IMMDeviceEnumerator* pEnum = NULL;
    IMMDevice* pDevice = NULL;
    IAudioSessionManager2* pSessionManager = NULL;
    IAudioSessionEnumerator* pSessionEnum = NULL;
    int count = 0;

    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void**)&pEnum);
    if (FAILED(hr) || !pEnum) {
        LogMessage("Audio: CoCreateInstance failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pEnum->lpVtbl->GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDevice);
    if (FAILED(hr) || !pDevice) {
        LogMessage("Audio: GetDefaultAudioEndpoint(eRender,eConsole) failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pDevice->lpVtbl->Activate(pDevice, &IID_IAudioSessionManager2,
                                   CLSCTX_ALL, NULL, (void**)&pSessionManager);
    if (FAILED(hr) || !pSessionManager) {
        LogMessage("Audio: Activate(IAudioSessionManager2) failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pSessionManager->lpVtbl->GetSessionEnumerator(pSessionManager, &pSessionEnum);
    if (FAILED(hr) || !pSessionEnum) {
        LogMessage("Audio: GetSessionEnumerator failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    int sessionCount = 0;
    pSessionEnum->lpVtbl->GetCount(pSessionEnum, &sessionCount);

    for (int i = 0; i < sessionCount && count < maxNames; i++) {
        IAudioSessionControl* pControl = NULL;
        if (FAILED(pSessionEnum->lpVtbl->GetSession(pSessionEnum, i, &pControl)) || !pControl) {
            continue;
        }

        AudioSessionState state = AudioSessionStateInactive;
        pControl->lpVtbl->GetState(pControl, &state);

        if (state == AudioSessionStateActive) {
            // AudioSessionStateActive can be true even when a video is paused
            // (the session stays "active" but produces no sound). Use the peak
            // meter to filter out silent sessions so paused video doesn't block
            // the screen saver.
            IAudioMeterInformation* pMeter = NULL;
            if (SUCCEEDED(pControl->lpVtbl->QueryInterface(pControl, &IID_IAudioMeterInformation, (void**)&pMeter)) && pMeter) {
                float peak = 0.0f;
                if (SUCCEEDED(pMeter->lpVtbl->GetPeakValue(pMeter, &peak)) && peak > AUDIO_ACTIVE_PEAK_THRESHOLD) {
                    IAudioSessionControl2* pControl2 = NULL;
                    if (SUCCEEDED(pControl->lpVtbl->QueryInterface(pControl, &IID_IAudioSessionControl2, (void**)&pControl2)) && pControl2) {
                        DWORD pid = 0;
                        if (SUCCEEDED(pControl2->lpVtbl->GetProcessId(pControl2, &pid)) && pid != 0) {
                            char procName[MAX_PATH] = {0};
                            if (GetProcessNameFromPid(pid, procName, sizeof(procName))) {
                                int found = 0;
                                for (int j = 0; j < count; j++) {
                                    if (_stricmp(names[j], procName) == 0) { found = 1; break; }
                                }
                                if (!found) {
                                    strncpy(names[count], procName, MAX_PATH - 1);
                                    names[count][MAX_PATH - 1] = '\0';
                                    count++;
                                }
                            }
                        }
                        pControl2->lpVtbl->Release(pControl2);
                    }
                }
                pMeter->lpVtbl->Release(pMeter);
            }
        }
        pControl->lpVtbl->Release(pControl);
    }

done:
    if (pSessionEnum) pSessionEnum->lpVtbl->Release(pSessionEnum);
    if (pSessionManager) pSessionManager->lpVtbl->Release(pSessionManager);
    if (pDevice) pDevice->lpVtbl->Release(pDevice);
    if (pEnum) pEnum->lpVtbl->Release(pEnum);
    return count;
}

LONGLONG RectArea(const RECT* rect) {
    LONG width = rect->right - rect->left;
    LONG height = rect->bottom - rect->top;

    if (width <= 0 || height <= 0) {
        return 0;
    }

    return (LONGLONG)width * (LONGLONG)height;
}

LONGLONG RectIntersectionArea(const RECT* a, const RECT* b) {
    LONG left = a->left > b->left ? a->left : b->left;
    LONG top = a->top > b->top ? a->top : b->top;
    LONG right = a->right < b->right ? a->right : b->right;
    LONG bottom = a->bottom < b->bottom ? a->bottom : b->bottom;

    if (right <= left || bottom <= top) {
        return 0;
    }

    return (LONGLONG)(right - left) * (LONGLONG)(bottom - top);
}

// Prefer DWM's extended frame bounds (accounts for invisible drop-shadow borders);
// fall back to GetWindowRect if DWM query fails.
int GetVisibleWindowRect(HWND hWnd, RECT* rect) {
    RECT frameRect;
    HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
    if (SUCCEEDED(hr) && RectArea(&frameRect) > 0) {
        *rect = frameRect;
        return 1;
    }

    return GetWindowRect(hWnd, rect) != 0;
}

void MarkMediaWindowMonitors(MediaEnumContext* ctx, const RECT* windowRect) {
    LONGLONG windowArea = RectArea(windowRect);

    if (windowArea < MIN_MEDIA_WINDOW_AREA) {
        return;
    }

    int marked = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        LONGLONG intersectionArea = RectIntersectionArea(windowRect, &g_monitors[i].rect);
        double overlapRatio = (double)intersectionArea / (double)windowArea;
        if (intersectionArea >= MIN_MEDIA_WINDOW_AREA && overlapRatio >= MIN_MEDIA_WINDOW_OVERLAP_RATIO) {
            ctx->mediaOnMonitor[i] = 1;
            marked = 1;
        }
    }

    if (!marked) {
        int monitorIndex = GetMonitorIndexFromRect(*windowRect);
        if (monitorIndex >= 0 && monitorIndex < g_monitorCount) {
            ctx->mediaOnMonitor[monitorIndex] = 1;
        }
    }
}

BOOL CALLBACK EnumMediaWindowCallback(HWND hWnd, LPARAM lParam) {
    MediaEnumContext* ctx = (MediaEnumContext*)lParam;

    if (!IsWindowVisible(hWnd) || IsIconic(hWnd) || IsWindowCloakedCompat(hWnd)) {
        return TRUE;
    }

    LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }

    RECT rect;
    if (!GetVisibleWindowRect(hWnd, &rect)) {
        return TRUE;
    }

    if (RectArea(&rect) <= 0) {
        return TRUE;
    }

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hWnd, processName, sizeof(processName))) {
        return TRUE;
    }

    // A window only counts as media if its process is actually emitting audio.
    // We match by exe name (not PID) because Chromium browsers run multi-process:
    // the audio session belongs to the renderer process, while the window belongs
    // to the main process both share the same exe name. This also handles
    // single-process players (VLC, mpv) where name match == PID match.
    if (!IsAudioActiveProcessName(ctx, processName)) {
        return TRUE;
    }

    char title[512] = {0};
    GetWindowTextA(hWnd, title, sizeof(title));

    int matched = IsMediaCandidateWindow(processName, title);

    // Collect diagnostic info for all browser windows with active audio,
    // regardless of whether they matched a hint. This is logged once in
    // UpdateMediaMonitorStates when the mask changes, so the user can see
    // ALL browser windows (including the one playing video) without per-tick spam.
    if (IsKnownBrowserProcess(processName) && title[0] && ctx->browserWindowCount < MAX_BROWSER_WINDOW_INFO) {
        int idx = ctx->browserWindowCount++;
        strncpy(ctx->browserTitles[idx], title, 255);
        ctx->browserTitles[idx][255] = '\0';
        ctx->browserMatched[idx] = matched;
    }

    if (!matched) {
        return TRUE;
    }

    MarkMediaWindowMonitors(ctx, &rect);
    return TRUE;
}

// Reset media detection cache. Called after system sleep/wake to force a fresh
// scan, since WASAPI sessions and ES_DISPLAY_REQUIRED state may be stale.
void ResetMediaDetectionCache() {
    g_mediaCacheInvalidated = 1;
}

// Fills mediaOnMonitor[] with 1 for each monitor hosting a visible media window.
// Uses the cheap ES_DISPLAY_REQUIRED gate to skip enumeration when nothing is
// playing, and caches the scan for MEDIA_DETECTION_CACHE_MS to keep the timer
// light. Returns 1 if any monitor has media. If media is playing globally but
// no candidate window maps to a monitor, falls back to blocking all enabled
// monitors (safe default so unknown apps are never covered).
int UpdateMediaMonitorStates(int mediaOnMonitor[MAX_MONITOR_COUNT]) {
    static DWORD lastLoggedMask = (DWORD)-1;
    static DWORD lastScanTick = 0;
    static int hasCachedState = 0;
    static int cachedAnyMedia = 0;
    static int cachedMediaOnMonitor[MAX_MONITOR_COUNT] = {0};

    for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
        mediaOnMonitor[i] = 0;
    }

    if (g_mediaCacheInvalidated) {
        hasCachedState = 0;
        lastLoggedMask = (DWORD)-1;
        g_mediaCacheInvalidated = 0;
        LogMessage("Media detection cache invalidated (sleep/wake)");
    }

    if (!g_app.config.mediaDetectionEnabled) {
        hasCachedState = 0;
        if (lastLoggedMask != 0) {
            LogMessage("Media monitor detection: disabled");
            lastLoggedMask = 0;
        }
        return 0;
    }

    DWORD nowTick = GetTickCount();
    if (hasCachedState && (DWORD)(nowTick - lastScanTick) < MEDIA_DETECTION_CACHE_MS) {
        for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
            mediaOnMonitor[i] = cachedMediaOnMonitor[i];
        }
        return cachedAnyMedia;
    }

    lastScanTick = nowTick;

    int globalMediaPlaying = IsMediaPlaying();

    if (!globalMediaPlaying) {
        for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
            cachedMediaOnMonitor[i] = 0;
        }
        hasCachedState = 1;
        cachedAnyMedia = 0;

        if (lastLoggedMask != 0) {
            LogMessage("Media monitor detection: no active media monitors");
            lastLoggedMask = 0;
        }
        return 0;
    }

    int localMediaOnMonitor[MAX_MONITOR_COUNT] = {0};
    MediaEnumContext ctx = {0};
    ctx.audioActiveProcessNameCount = CollectActiveAudioProcessNames(
        ctx.audioActiveProcessNames, MAX_ACTIVE_AUDIO_PIDS);
    EnumWindows(EnumMediaWindowCallback, (LPARAM)&ctx);

    int mappedMonitorCount = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        if (ctx.mediaOnMonitor[i]) {
            localMediaOnMonitor[i] = 1;
            mediaOnMonitor[i] = 1;
            mappedMonitorCount++;
        }
    }

    int usedGlobalFallback = 0;
    int skippedFallbackForBrowser = 0;
    int skippedFallbackForNoAudio = 0;
    if (mappedMonitorCount == 0) {
        if (ctx.audioActiveProcessNameCount == 0) {
            // ES_DISPLAY_REQUIRED is set but no audible audio is detected on the
            // default render endpoint. This happens with muted video, OBS replay
            // buffer, or other apps that call SetThreadExecutionState without
            // producing audio.
            if (g_app.config.blockOnMutedMedia) {
                // User opted in to blocking on muted/silent media. Conservatively
                // block all enabled monitors.
                for (int i = 0; i < g_monitorCount; i++) {
                    if (g_monitorStates[i].enabled) {
                        mediaOnMonitor[i] = 1;
                    }
                }
                usedGlobalFallback = 1;
            } else {
                // No audible media is playing, let the screen saver activate.
                skippedFallbackForNoAudio = 1;
                LogMessage("Media detection: ES_DISPLAY_REQUIRED set but no audible audio detected: skipping fallback");
            }
        } else if (AllAudioActiveAreBrowsers(&ctx)) {
            // All audio-active processes are known browsers, but no window title
            // matched a video hint. This typically means video is playing in a
            // background tab - the window title shows the active tab, not the
            // playing one. We can't determine which monitor is playing, so
            // rather than blocking all monitors (which would defeat per-monitor
            // detection), we skip the fallback and let the screen saver activate.
            // The user can always move the mouse to dismiss it if needed.
            skippedFallbackForBrowser = 1;
            LogMessage("Media detection: no title hint match, all audio-active processes are browsers: skipping fallback");
        } else {
            // Non-browser audio (unknown app, media player with minimized window,
            // audio on non-default device). Conservatively block all enabled
            // monitors to avoid covering playback.
            for (int i = 0; i < g_monitorCount; i++) {
                if (g_monitorStates[i].enabled) {
                    mediaOnMonitor[i] = 1;
                }
            }
            usedGlobalFallback = 1;
        }
    }

    DWORD mask = 0;
    for (int i = 0; i < g_monitorCount && i < 32; i++) {
        if (mediaOnMonitor[i]) {
            mask |= (1u << i);
        }
    }

    hasCachedState = 1;
    cachedAnyMedia = mask != 0;
    for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
        cachedMediaOnMonitor[i] = mediaOnMonitor[i];
    }

    if (mask != lastLoggedMask) {
        for (int i = 0; i < ctx.browserWindowCount; i++) {
            LogMessage("Media detection: browser window %s: '%.120s'",
                       ctx.browserMatched[i] ? "MATCHED  " : "no hint  ",
                       ctx.browserTitles[i]);
        }
        LogMessage("Media monitor detection: mask=0x%08X (activeAudioNames=%d, fallback=%d, browserSkip=%d, noAudioSkip=%d, browserWindows=%d)",
                   mask, ctx.audioActiveProcessNameCount, usedGlobalFallback, skippedFallbackForBrowser, skippedFallbackForNoAudio, ctx.browserWindowCount);
        lastLoggedMask = mask;
    }

    return cachedAnyMedia;
}

// Check if a Windows shell overlay window (Start Menu, Task View, Action Center) is open
// Returns the number of shell windows detected (0, 1, or 2 if both Start Menu and Action Center)
int IsShellWindowOpen() {
    HWND hFg = GetForegroundWindow();
    if (!hFg) {
        LogMessage("Shell detection: No foreground window");
        return 0;
    }

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hFg, processName, sizeof(processName))) {
        LogMessage("Shell detection: Could not get process name for foreground window");
        return 0;
    }

    char className[256] = {0};
    GetClassNameA(hFg, className, sizeof(className));

    LogMessage("Shell detection: Foreground window - process='%s', class='%s'", processName, className);

    int shellWindowCount = 0;

    // Check for known shell host processes
    // ShellExperienceHost.exe - Start Menu, Action Center on Windows 11
    // SearchHost.exe - Windows Search/Start Menu
    // StartMenuExperienceHost.exe - Start Menu on Windows 10
    // ShellHost.exe - Action Center / Control Center on Windows 11
    if (_stricmp(processName, "ShellExperienceHost.exe") == 0 ||
        _stricmp(processName, "SearchHost.exe") == 0 ||
        _stricmp(processName, "StartMenuExperienceHost.exe") == 0 ||
        _stricmp(processName, "ShellHost.exe") == 0) {

        LogMessage("Shell detection: Shell host process detected: %s", processName);
        shellWindowCount = 1;
    }

    // Additional check: Task View is hosted by explorer.exe with specific window classes
    if (_stricmp(processName, "explorer.exe") == 0) {
        // Task View uses Windows.UI.Core.CoreWindow or XamlExplorerHostIslandWindow
        if (strstr(className, "Windows.UI.Core.CoreWindow") != NULL ||
            strstr(className, "XamlExplorerHostIslandWindow") != NULL) {

            LogMessage("Shell detection: Explorer shell window detected: class=%s", className);
            shellWindowCount = 1;
        }
    }

    if (shellWindowCount == 0) {
        LogMessage("Shell detection: No shell windows detected");
    }

    return shellWindowCount;
}

// Send Escape key(s) to close shell windows (Start Menu, Task View, Action Center)
void CloseShellWindows(int escapeCount) {
    LogMessage("Sending %d Escape key(s) to close shell windows", escapeCount);

    for (int i = 0; i < escapeCount; i++) {
        INPUT input[2] = {0};

        // Key down
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wVk = VK_ESCAPE;
        input[0].ki.dwFlags = 0;

        // Key up
        input[1].type = INPUT_KEYBOARD;
        input[1].ki.wVk = VK_ESCAPE;
        input[1].ki.dwFlags = KEYEVENTF_KEYUP;

        UINT sent = SendInput(2, input, sizeof(INPUT));
        LogMessage("SendInput returned %u (expected 2)", sent);

        // Small delay between escape presses if sending multiple
        if (i < escapeCount - 1) {
            Sleep(50);
        }
    }
}

void ShowScreenSaverOnMonitor(int monitorIndex, int isManual) {
    if (monitorIndex < 0 || monitorIndex >= g_monitorCount) return;
    if (!g_monitorStates[monitorIndex].enabled) return;
    if (g_monitorStates[monitorIndex].screenSaverActive) return;

    if (g_app.config.perMonitorInputDetection) {
        int wasInactiveCount = 0;
        for (int i = 0; i < g_monitorCount; i++) {
            if (g_monitorStates[i].enabled && !g_monitorStates[i].screenSaverActive) {
                wasInactiveCount++;
            }
        }

        if (wasInactiveCount == 1) {
            int sentEscapeKeys = 0;
            for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++) {
                int shellWindowCount = IsShellWindowOpen();
                if (shellWindowCount > 0) {
                    LogMessage("Shell window(s) detected before last monitor activation (attempt %d), closing them", attempt + 1);
                    CloseShellWindows(1);
                    Sleep(SHELL_CLOSE_DELAY_MS);
                    sentEscapeKeys = 1;
                } else {
                    break;
                }
            }

            if (sentEscapeKeys) {
                // The Escape keys sent via SendInput update GetLastInputInfo,
                // which would make the next timer tick think the user is active
                // and deactivate the screen saver. Use the manual-activation
                // cooldown to suppress deactivation for a short period, instead
                // of resetting lastInputTime (which would also deactivate the
                // monitor we just activated).
                g_app.isManualActivation = 1;
                g_app.manualActivationTime = GetTickCount();
            }
        }
    } else {
        int sentEscapeKeys = 0;
        for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++) {
            int shellWindowCount = IsShellWindowOpen();
            if (shellWindowCount > 0) {
                LogMessage("Shell window(s) detected before screen saver activation (attempt %d), closing them", attempt + 1);
                CloseShellWindows(1);
                Sleep(SHELL_CLOSE_DELAY_MS);
                sentEscapeKeys = 1;
            } else {
                break;
            }
        }

        if (sentEscapeKeys) {
            g_app.isManualActivation = 1;
            g_app.manualActivationTime = GetTickCount();
        }
    }

    if (g_monitorStates[monitorIndex].hScreenSaverWnd) {
        // Reposition and resize in case the pixel shift compensation setting changed since the
        // window was last created.
        LONG_PTR exStyle = GetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_NOACTIVATE) == 0) {
            SetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
        }
        int pad = g_app.config.pixelShiftCompensation;
        SetWindowPos(g_monitorStates[monitorIndex].hScreenSaverWnd, HWND_TOPMOST,
                     g_monitors[monitorIndex].rect.left   - pad,
                     g_monitors[monitorIndex].rect.top    - pad,
                     g_monitors[monitorIndex].rect.right  - g_monitors[monitorIndex].rect.left + pad * 2,
                     g_monitors[monitorIndex].rect.bottom - g_monitors[monitorIndex].rect.top  + pad * 2,
                     SWP_NOACTIVATE);
        ShowWindow(g_monitorStates[monitorIndex].hScreenSaverWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_monitorStates[monitorIndex].hScreenSaverWnd);
        g_monitorStates[monitorIndex].screenSaverActive = 1;
        LogMessage("Screen saver window shown on monitor %d (reused)", monitorIndex);
    } else {
        // Expand the window beyond the monitor's reported bounds by the pixel shift compensation
        // amount on all four sides. This ensures hardware pixel shift (used by some OLED panels
        // to reduce burn-in) cannot expose the desktop behind the screen saver window.
        int pad = g_app.config.pixelShiftCompensation;
        HWND hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    L"OLEDAegisScreen", L"",
                                   WS_POPUP,
                                   g_monitors[monitorIndex].rect.left   - pad,
                                   g_monitors[monitorIndex].rect.top    - pad,
                                   g_monitors[monitorIndex].rect.right  - g_monitors[monitorIndex].rect.left + pad * 2,
                                   g_monitors[monitorIndex].rect.bottom - g_monitors[monitorIndex].rect.top  + pad * 2,
                                   NULL, NULL, GetModuleHandle(NULL), NULL);

        if (hWnd) {
            ShowWindow(hWnd, SW_SHOWNOACTIVATE);
            UpdateWindow(hWnd);
            g_monitorStates[monitorIndex].hScreenSaverWnd = hWnd;
            g_monitorStates[monitorIndex].screenSaverActive = 1;
            LogMessage("Screen saver window created on monitor %d", monitorIndex);
        }
    }

    if (!g_app.config.perMonitorInputDetection) {
        HideCursorForScreenSaver("screen saver activation");
    }
}

void ShowScreenSaver(int isManual) {
    if (g_app.screenSaverActive && !g_app.config.perMonitorInputDetection) return;

    if (g_app.config.perMonitorInputDetection && !isManual) {
        return;
    }

    if (isManual) {
        g_app.isManualActivation = 1;
        g_app.manualActivationTime = GetTickCount();
        LogMessage("Showing screen saver (manual activation)");
    }

    if (!g_app.config.perMonitorInputDetection) {
        int sentEscapeKeys = 0;
        for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++) {
            int shellWindowCount = IsShellWindowOpen();
            if (shellWindowCount > 0) {
                LogMessage("Shell window(s) detected before screen saver activation (attempt %d), closing them", attempt + 1);
                CloseShellWindows(1);
                Sleep(SHELL_CLOSE_DELAY_MS);
                sentEscapeKeys = 1;
            } else {
                break;
            }
        }

        if (sentEscapeKeys && !isManual) {
            g_app.isManualActivation = 1;
            g_app.manualActivationTime = GetTickCount();
            LogMessage("Showing screen saver (automatic activation, with input cooldown due to shell window closure)");
        } else if (!isManual) {
            g_app.isManualActivation = 0;
            g_app.manualActivationTime = 0;
            LogMessage("Showing screen saver (automatic activation)");
        }
    }

    LogMessage("%d monitors detected", g_monitorCount);

    int windowsCreated = 0;
    time_t now = time(NULL);

    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].enabled && !g_monitorStates[i].screenSaverActive) {
            if (g_app.config.perMonitorInputDetection) {
                g_monitorStates[i].lastInputTime = now;
            }
            ShowScreenSaverOnMonitor(i, isManual);
            windowsCreated++;
        }
    }

    LogMessage("Activated screen saver on %d monitors", windowsCreated);

    g_app.screenSaverActive = 1;
}

void HideScreenSaver() {
    if (!g_app.screenSaverActive && !IsAnyMonitorActive()) return;

    LogMessage("Hiding screen saver");

    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].screenSaverActive) {
            HideScreenSaverOnMonitor(i);
        }
    }

    g_app.screenSaverActive = 0;
    g_app.manualActivationTime = 0;
    g_app.isManualActivation = 0;

    EnsureCursorVisible("screen saver hidden");
}

void EnsureScreenSaverTopmost() {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].screenSaverActive && g_monitorStates[i].hScreenSaverWnd) {
            SetWindowPos(g_monitorStates[i].hScreenSaverWnd, HWND_TOPMOST,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

void OpenConfigFileLocation() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    char selectCmd[MAX_PATH + 20];
    sprintf_s(selectCmd, sizeof(selectCmd), "/select,\"%s\"", configPath);
    ShellExecuteA(NULL, "open", "explorer.exe", selectCmd, NULL, SW_SHOW);
}

LRESULT CALLBACK SettingsDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDC_APPLY_BTN:
                    ApplySettings(hWnd);
                    break;
                case IDC_CONFIG_BTN:
                    LogMessage("Settings: Opening config file location");
                    OpenConfigFileLocation();
                    break;
                case IDC_CLOSE_BTN:
                    LogMessage("Settings: Dialog closed via 'Close' button");
                    DestroyWindow(hWnd);
                    g_hSettingsDialog = NULL;
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            LogMessage("Settings: Dialog closed via WM_CLOSE");
            DestroyWindow(hWnd);
            g_hSettingsDialog = NULL;
            return 0;
        case WM_DESTROY:
            if (g_hSettingsFont) {
                DeleteObject(g_hSettingsFont);
                g_hSettingsFont = NULL;
            }
            return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void AddTooltip(HWND hParent, HWND hControl, const char* text) {
    TOOLINFOA ti = {0};
    ti.cbSize = sizeof(TOOLINFOA);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = hParent;
    ti.uId = (UINT_PTR)hControl;
    ti.hinst = GetModuleHandle(NULL);
    ti.lpszText = (LPSTR)text;

    SendMessageA(g_hTooltipControl, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

void ShowSettingsDialog() {
    EnsureCursorVisible("settings dialog opened");

    if (g_hSettingsDialog) {
        SetForegroundWindow(g_hSettingsDialog);
        return;
    }

    // Get DPI before creating the window (use primary monitor DPI)
    HDC hdc = GetDC(NULL);
    g_settingsDpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);
    if (g_settingsDpi == 0) g_settingsDpi = 96;

    // Create DPI-scaled font
    NONCLIENTMETRICSA ncm = {0};
    ncm.cbSize = sizeof(NONCLIENTMETRICSA);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSA), &ncm, 0);
    // Scale the font height for DPI
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, g_settingsDpi, 96);
    g_hSettingsFont = CreateFontIndirectA(&ncm.lfMessageFont);

    HWND hTooltip = CreateWindowExA(0, TOOLTIPS_CLASSA, NULL,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               g_hSettingsDialog, NULL, GetModuleHandle(NULL), NULL);
    g_hTooltipControl = hTooltip;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = g_hIconActive;
    wc.lpszClassName = "OLED Aegis Settings Dialog";
    RegisterClassA(&wc);

    HMODULE hMod = GetModuleHandle(NULL);
    g_hSettingsDialog = CreateWindowExA(0, "OLED Aegis Settings Dialog", "OLED Aegis Settings",
                                      WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      ScaleDPI(410), ScaleDPI(430),
                                      NULL, NULL, hMod, NULL);

    if (g_hSettingsDialog) {
        SetWindowLongPtr(g_hSettingsDialog, GWLP_WNDPROC, (LONG_PTR)SettingsDialogProc);

        // Update DPI now that we have a window
        g_settingsDpi = GetDpiForWindowCompat(g_hSettingsDialog);

        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_UPDOWN_CLASS;
        InitCommonControlsEx(&icex);

        // Layout constants (base values at 96 DPI)
        int margin = ScaleDPI(20);
        int rowHeight = ScaleDPI(25);
        int controlHeight = ScaleDPI(20);
        int labelWidth = ScaleDPI(180);
        int editWidth = ScaleDPI(100);
        int checkboxWidth = ScaleDPI(340);
        int buttonWidth = ScaleDPI(100);
        int configBtnWidth = ScaleDPI(130);
        int buttonHeight = ScaleDPI(30);
        int buttonSpacing = ScaleDPI(10);

        int y = margin;

        HWND hTimeoutLabel = CreateWindowA("STATIC", "Idle Timeout (seconds):",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, labelWidth, controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hTimeoutEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     margin + labelWidth, y, editWidth, controlHeight,
                     g_hSettingsDialog, (HMENU)IDC_TIMEOUT_EDIT, hMod, NULL);
        HWND hTimeoutUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hTimeoutEdit);
        SendMessage(hTimeoutUpDown, UDM_SETRANGE, 0, MAKELPARAM(MAX_IDLE_TIMEOUT_SEC, MIN_IDLE_TIMEOUT_SEC));
        y += rowHeight + ScaleDPI(5);

        HWND hIntervalLabel = CreateWindowA("STATIC", "Check Interval (ms):",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, labelWidth, controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hIntervalEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     margin + labelWidth, y, editWidth, controlHeight,
                     g_hSettingsDialog, (HMENU)IDC_INTERVAL_EDIT, hMod, NULL);
        HWND hIntervalUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hIntervalEdit);
        SendMessage(hIntervalUpDown, UDM_SETRANGE, 0, MAKELPARAM(MAX_CHECK_INTERVAL_MS, MIN_CHECK_INTERVAL_MS));
        y += rowHeight + ScaleDPI(5);

        HWND hPixelShiftLabel = CreateWindowA("STATIC", "Pixel Shift Compensation (px):",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, labelWidth, controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hPixelShiftEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     margin + labelWidth, y, editWidth, controlHeight,
                     g_hSettingsDialog, (HMENU)IDC_PIXELSHIFT_EDIT, hMod, NULL);
        HWND hPixelShiftUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hPixelShiftEdit);
        SendMessage(hPixelShiftUpDown, UDM_SETRANGE, 0, MAKELPARAM(MAX_PIXEL_SHIFT_COMPENSATION, MIN_PIXEL_SHIFT_COMPENSATION));
        y += rowHeight + ScaleDPI(5);

        HWND hVideoCheck = CreateWindowA("BUTTON", "Prevent Screen Saver During Media Playback",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_MEDIA_CHECK, hMod, NULL);
        y += rowHeight;

        HWND hDebugCheck = CreateWindowA("BUTTON", "Debug Mode",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_DEBUG_CHECK, hMod, NULL);
        y += rowHeight;

        HWND hStartupCheck = CreateWindowA("BUTTON", "Run at Startup",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_STARTUP_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hPerMonitorCheck = CreateWindowA("BUTTON", "Per-Monitor Input Detection",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_PERMONITOR_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hPerMonitorMediaCheck = CreateWindowA("BUTTON", "Per-Monitor Media Detection",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_PERMONITOR_MEDIA_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hMutedMediaCheck = CreateWindowA("BUTTON", "Block During Muted Media",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_MUTED_MEDIA_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hMonitorsLabel = CreateWindowA("STATIC", "Monitors:",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, ScaleDPI(100), controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        y += rowHeight;

        for (int i = 0; i < g_monitorCount; i++) {
            HWND hMonitorCheck = CreateWindowA("BUTTON", g_monitors[i].displayName,
                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                         margin, y, checkboxWidth, controlHeight,
                         g_hSettingsDialog, (HMENU)(INT_PTR)(IDC_MONITOR_BASE + i),
                         hMod, NULL);
            if (g_hSettingsFont) SendMessageA(hMonitorCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            y += rowHeight;
        }

        y += margin;
        int btnX = margin;
        HWND hApplyBtn = CreateWindowA("BUTTON", "Apply",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     btnX, y, buttonWidth, buttonHeight, g_hSettingsDialog, (HMENU)IDC_APPLY_BTN, hMod, NULL);
        btnX += buttonWidth + buttonSpacing;

        HWND hConfigBtn = CreateWindowA("BUTTON", "Open Config File",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     btnX, y, configBtnWidth, buttonHeight, g_hSettingsDialog, (HMENU)IDC_CONFIG_BTN, hMod, NULL);
        btnX += configBtnWidth + buttonSpacing;

        HWND hCloseBtn = CreateWindowA("BUTTON", "Close",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     btnX, y, buttonWidth, buttonHeight, g_hSettingsDialog, (HMENU)IDC_CLOSE_BTN, hMod, NULL);

        // Calculate dialog size based on content
        int dialogWidth = margin + checkboxWidth + margin + ScaleDPI(20);  // Add extra for window borders
        int dialogHeight = y + buttonHeight + margin + ScaleDPI(40);  // Add extra for title bar and new control
        SetWindowPos(g_hSettingsDialog, NULL, 0, 0, dialogWidth, dialogHeight,
                     SWP_NOMOVE | SWP_NOZORDER);

        // Center on primary monitor
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(g_hSettingsDialog, NULL,
                     (screenWidth - dialogWidth) / 2, (screenHeight - dialogHeight) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);

        // Apply font to all controls
        if (g_hSettingsFont) {
            SendMessageA(hTimeoutLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hTimeoutEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hTimeoutUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hIntervalLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hIntervalEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hIntervalUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hVideoCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hDebugCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hStartupCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPerMonitorCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPerMonitorMediaCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hMutedMediaCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPixelShiftLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPixelShiftEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPixelShiftUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hMonitorsLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hApplyBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hConfigBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hCloseBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
        }

        // Add tooltips
        AddTooltip(g_hSettingsDialog, hTimeoutEdit,
                   "Idle timeout in seconds before the screen saver activates.");
        AddTooltip(g_hSettingsDialog, hIntervalEdit,
                   "How often to poll for user activity. (250-10000ms).");
        AddTooltip(g_hSettingsDialog, hVideoCheck,
                   "Prevent screen saver activation during video playback (on any monitor).");
        AddTooltip(g_hSettingsDialog, hDebugCheck,
                   "Enable debug logging to %APPDATA%\\OLED_Aegis\\oled_aegis_debug.log");
        AddTooltip(g_hSettingsDialog, hStartupCheck,
                   "Automatically start OLED Aegis when you log into Windows.");
        AddTooltip(g_hSettingsDialog, hPerMonitorCheck,
                   "Track input separately for each monitor. Allows screen saver to activate on unused monitors while you continue using others.");
        AddTooltip(g_hSettingsDialog, hPerMonitorMediaCheck,
                   "Detect media playback per monitor instead of globally. Only blocks the screen saver on the monitor where media is actually playing, so playback on a non-OLED display won't keep the OLED awake.");
        AddTooltip(g_hSettingsDialog, hMutedMediaCheck,
                   "Block the screen saver even when media is muted or inaudible (e.g. muted video, OBS replay buffer). When off, only audible media prevents the screen saver.");
        AddTooltip(g_hSettingsDialog, hPixelShiftEdit,
                   "Expand the screen saver window beyond the monitor bounds by this many pixels on each side. "
                   "Use 4-8 on QD-OLED panels (e.g. Alienware) to prevent hardware pixel shift from exposing the desktop edge. (0 = disabled)");

        // Set initial values
        char buffer[32];
        sprintf_s(buffer, 32, "%d", g_app.config.idleTimeout);
        SetDlgItemTextA(g_hSettingsDialog, IDC_TIMEOUT_EDIT, buffer);

        sprintf_s(buffer, 32, "%d", g_app.config.checkInterval);
        SetDlgItemTextA(g_hSettingsDialog, IDC_INTERVAL_EDIT, buffer);

        CheckDlgButton(g_hSettingsDialog, IDC_MEDIA_CHECK, g_app.config.mediaDetectionEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_DEBUG_CHECK, g_app.config.debugMode ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_STARTUP_CHECK, g_app.config.startupEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_PERMONITOR_CHECK, g_app.config.perMonitorInputDetection ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_PERMONITOR_MEDIA_CHECK, g_app.config.perMonitorMediaDetection ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_MUTED_MEDIA_CHECK, g_app.config.blockOnMutedMedia ? BST_CHECKED : BST_UNCHECKED);

        sprintf_s(buffer, 32, "%d", g_app.config.pixelShiftCompensation);
        SetDlgItemTextA(g_hSettingsDialog, IDC_PIXELSHIFT_EDIT, buffer);

        for (int i = 0; i < g_monitorCount; i++) {
            CheckDlgButton(g_hSettingsDialog, IDC_MONITOR_BASE + i, g_app.config.monitorsEnabled[i] ? BST_CHECKED : BST_UNCHECKED);
        }

        ShowWindow(g_hSettingsDialog, SW_SHOW);
        UpdateWindow(g_hSettingsDialog);
    }
}

void ApplySettings(HWND hWnd) {
    char buffer[32];
    GetDlgItemTextA(hWnd, IDC_TIMEOUT_EDIT, buffer, 32);
    int oldTimeout = g_app.config.idleTimeout;
    g_app.config.idleTimeout = atoi(buffer);

    GetDlgItemTextA(hWnd, IDC_INTERVAL_EDIT, buffer, 32);
    int oldInterval = g_app.config.checkInterval;
    g_app.config.checkInterval = atoi(buffer);

    int oldMedia = g_app.config.mediaDetectionEnabled;
    int oldDebug = g_app.config.debugMode;
    int oldStartup = g_app.config.startupEnabled;
    int oldPerMonitor = g_app.config.perMonitorInputDetection;
    int oldPerMonitorMedia = g_app.config.perMonitorMediaDetection;
    int oldBlockOnMutedMedia = g_app.config.blockOnMutedMedia;

    g_app.config.mediaDetectionEnabled = IsDlgButtonChecked(hWnd, IDC_MEDIA_CHECK) == BST_CHECKED;
    g_app.config.debugMode = IsDlgButtonChecked(hWnd, IDC_DEBUG_CHECK) == BST_CHECKED;
    g_app.config.startupEnabled = IsDlgButtonChecked(hWnd, IDC_STARTUP_CHECK) == BST_CHECKED;
    g_app.config.perMonitorInputDetection = IsDlgButtonChecked(hWnd, IDC_PERMONITOR_CHECK) == BST_CHECKED;
    g_app.config.perMonitorMediaDetection = IsDlgButtonChecked(hWnd, IDC_PERMONITOR_MEDIA_CHECK) == BST_CHECKED;
    g_app.config.blockOnMutedMedia = IsDlgButtonChecked(hWnd, IDC_MUTED_MEDIA_CHECK) == BST_CHECKED;

    GetDlgItemTextA(hWnd, IDC_PIXELSHIFT_EDIT, buffer, 32);
    g_app.config.pixelShiftCompensation = atoi(buffer);
    ClampConfigValues();

    for (int i = 0; i < g_monitorCount; i++) {
        int wasEnabled = g_app.config.monitorsEnabled[i];
        g_app.config.monitorsEnabled[i] = IsDlgButtonChecked(hWnd, IDC_MONITOR_BASE + i) == BST_CHECKED;
        g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];

        if (!g_app.config.monitorsEnabled[i] && g_monitorStates[i].screenSaverActive) {
            LogMessage("Disabling monitor %d which has active screen saver, hiding it", i);
            HideScreenSaverOnMonitor(i);
        }

        if (!g_app.config.monitorsEnabled[i] && wasEnabled && g_monitorStates[i].hScreenSaverWnd) {
            DestroyWindow(g_monitorStates[i].hScreenSaverWnd);
            g_monitorStates[i].hScreenSaverWnd = NULL;
            LogMessage("Destroyed screen saver window for disabled monitor %d", i);
        }
    }

    if (!IsAnyMonitorActive()) {
        g_app.screenSaverActive = 0;
        EnsureCursorVisible("no active monitors after settings");
    }

    if (!oldPerMonitor && g_app.config.perMonitorInputDetection) {
        time_t now = time(NULL);
        for (int i = 0; i < g_monitorCount; i++) {
            g_monitorStates[i].lastInputTime = now;
        }
        LogMessage("Per-monitor mode enabled: reset all monitor idle times");
    }

    SaveConfig();
    UpdateStartupRegistry();

    LogMessage("Settings applied: timeout %ds->%ds, interval %dms->%dms, media %d->%d, debug %d->%d, startup %d->%d, perMonitor %d->%d, perMonitorMedia %d->%d, mutedMedia %d->%d, pixelShift %dpx",
             oldTimeout, g_app.config.idleTimeout,
             oldInterval, g_app.config.checkInterval,
             oldMedia, g_app.config.mediaDetectionEnabled,
             oldDebug, g_app.config.debugMode,
             oldStartup, g_app.config.startupEnabled,
             oldPerMonitor, g_app.config.perMonitorInputDetection,
             oldPerMonitorMedia, g_app.config.perMonitorMediaDetection,
             oldBlockOnMutedMedia, g_app.config.blockOnMutedMedia,
             g_app.config.pixelShiftCompensation);

    if (oldInterval != g_app.config.checkInterval) {
        KillTimer(g_app.hWnd, TIMER_IDLE_CHECK);
        SetTimer(g_app.hWnd, TIMER_IDLE_CHECK, g_app.config.checkInterval, NULL);
        LogMessage("Timer recreated with new interval: %dms", g_app.config.checkInterval);
    }

    sprintf_s(buffer, 32, "%d", g_app.config.checkInterval);
    SetDlgItemTextA(hWnd, IDC_INTERVAL_EDIT, buffer);
}

void UpdateTrayIcon(int active) {
    active = active ? 1 : 0;
    if (g_app.trayIconActive == active) {
        return;
    }

    g_app.nid.hIcon = active ? g_hIconActive : g_hIconInactive;
    lstrcpyA(g_app.nid.szTip, active ? "OLED Aegis - Active" : "OLED Aegis - Idle");
    Shell_NotifyIconA(NIM_MODIFY, (PNOTIFYICONDATAA)&g_app.nid);
    g_app.trayIconActive = active;
}

// Handle WM_CREATE: initialize application state, tray icon, config, monitors,
// and the idle-check timer. Returns 0 on success, -1 to abort window creation
// (used when another instance is already running).
int HandleCreation(HWND hWnd) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.hWnd = hWnd;
    g_app.isShuttingDown = 0;
    g_app.trayIconActive = -1;  // Force first UpdateTrayIcon call to fire

    g_hInstanceMutex = CreateMutexW(NULL, TRUE, L"OLEDAegis_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"OLED Aegis is already running", L"OLED Aegis", MB_OK | MB_ICONINFORMATION);
        if (g_hInstanceMutex) {
            CloseHandle(g_hInstanceMutex);
            g_hInstanceMutex = NULL;
        }
        PostQuitMessage(0);
        return -1;
    }
    // Keep mutex handle open for app lifetime to maintain single-instance lock

    LoadTrayIcons();

    g_app.nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_app.nid.hWnd = hWnd;
    g_app.nid.uID = 1;
    g_app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.nid.uCallbackMessage = WM_TRAYICON;
    g_app.nid.hIcon = g_hIconInactive;
    lstrcpyA(g_app.nid.szTip, "OLED Aegis - Idle");
    Shell_NotifyIconA(NIM_ADD, &g_app.nid);

    g_app.config.idleTimeout = DEFAULT_IDLE_TIMEOUT;
    g_app.config.checkInterval = 1000;
    g_app.config.mediaDetectionEnabled = 1;
    g_app.config.startupEnabled = 0;
    g_app.config.debugMode = 0;
    g_app.config.perMonitorInputDetection = 0;
            g_app.config.perMonitorMediaDetection = 1;
            g_app.config.blockOnMutedMedia = 0;
            for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
        g_app.config.monitorsEnabled[i] = 1;
    }

    EnumerateMonitors();

    for (int i = 0; i < g_monitorCount; i++) {
        g_monitorStates[i].lastInputTime = time(NULL);
        g_monitorStates[i].screenSaverActive = 0;
        g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];
    }

    if (!ConfigFileExists()) {
        SaveConfig();
    }

    LoadConfig();

    for (int i = 0; i < g_monitorCount; i++) {
        g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];
    }

    UpdateStartupRegistry();

    LogMessage("Application started. Timeout: %ds, Media: %d, Debug: %d",
             g_app.config.idleTimeout, g_app.config.mediaDetectionEnabled, g_app.config.debugMode);

    g_blackBrush = CreateSolidBrush(RGB(0, 0, 0));

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MonitorWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = g_blackBrush;
    wc.lpszClassName = L"OLEDAegisScreen";
    RegisterClassW(&wc);

    SetTimer(hWnd, TIMER_IDLE_CHECK, g_app.config.checkInterval, NULL);

    return 0;
}

void HandleTimeout(WPARAM wParam) {
    if (wParam != TIMER_IDLE_CHECK) {
        return;
    }

    // Skip all processing if no monitors have screen saver enabled
    if (!IsAnyMonitorEnabled()) {
        return;
    }

    // Per-monitor input detection mode:
    //   Each monitor has its own idle timer, updated by tracking cursor position
    //   and focused-window location. This lets the screen saver activate on
    //   unused monitors while the user continues working on others. Media is
    //   checked per-monitor (if perMonitorMediaDetection is on) or globally.
    //   Each monitor's screen saver is activated/deactivated independently.
    if (g_app.config.perMonitorInputDetection) {
        DWORD idleTime = GetIdleTime();
        time_t now = time(NULL);

        int usePerMonitorMedia = (g_app.config.perMonitorMediaDetection && g_app.config.mediaDetectionEnabled);
        int mediaOnMonitor[MAX_MONITOR_COUNT] = {0};
        int mediaPlaying = 0;
        if (usePerMonitorMedia) {
            UpdateMediaMonitorStates(mediaOnMonitor);
        } else {
            mediaPlaying = IsMediaPlaying();
        }

        int inManualCooldown = 0;
        if (g_app.isManualActivation) {
            DWORD timeSinceActivation = GetTickCount() - g_app.manualActivationTime;
            if (timeSinceActivation < MANUAL_ACTIVATION_COOLDOWN_MS) {
                inManualCooldown = 1;
            } else {
                g_app.isManualActivation = 0;
                g_app.manualActivationTime = 0;
            }
        }

        if (idleTime < IDLE_ACTIVITY_THRESHOLD_MS && !inManualCooldown) {
            POINT pt;
            GetCursorPos(&pt);
            int cursorMonitorIndex = GetMonitorIndexFromPoint(pt);

            if (cursorMonitorIndex >= 0 && cursorMonitorIndex < g_monitorCount) {
                g_monitorStates[cursorMonitorIndex].lastInputTime = now;
            }

            HWND hFg = GetForegroundWindow();
            if (hFg) {
                int isOledWindow = 0;
                for (int i = 0; i < g_monitorCount; i++) {
                    if (g_monitorStates[i].hScreenSaverWnd == hFg) {
                        isOledWindow = 1;
                        break;
                    }
                }

                if (!isOledWindow) {
                    RECT rect;
                    GetWindowRect(hFg, &rect);
                    int fgMonitorIndex = GetMonitorIndexFromRect(rect);
                    if (fgMonitorIndex >= 0 && fgMonitorIndex < g_monitorCount && fgMonitorIndex != cursorMonitorIndex) {
                        g_monitorStates[fgMonitorIndex].lastInputTime = now;
                    }
                }
            }
        }

        for (int i = 0; i < g_monitorCount; i++) {
            if (!g_monitorStates[i].enabled) continue;

            int idleSeconds = (int)(now - g_monitorStates[i].lastInputTime);
            int monitorHasMedia = usePerMonitorMedia ? mediaOnMonitor[i] : mediaPlaying;

            if (!monitorHasMedia && idleSeconds >= g_app.config.idleTimeout) {
                if (!g_monitorStates[i].screenSaverActive) {
                    LogMessage("Timer: Activating screen saver on monitor %d (idle: %ds)", i, idleSeconds);
                    ShowScreenSaverOnMonitor(i, 0);
                }
            } else if (g_monitorStates[i].screenSaverActive && !inManualCooldown) {
                if (monitorHasMedia) {
                    LogMessage("Timer: Deactivating screen saver on monitor %d (media detected)", i);
                    HideScreenSaverOnMonitor(i);
                } else if (idleSeconds < IDLE_DEACTIVATE_THRESHOLD_SEC) {
                    LogMessage("Timer: Deactivating screen saver on monitor %d (input detected)", i);
                    HideScreenSaverOnMonitor(i);
                }
            }
        }

        if (!IsAnyMonitorActive()) {
            g_app.screenSaverActive = 0;
        }

        POINT cursorPt;
        GetCursorPos(&cursorPt);
        int cursorMonitorIndex = GetMonitorIndexFromPoint(cursorPt);
        int cursorOnActiveMonitor = (cursorMonitorIndex >= 0 && cursorMonitorIndex < g_monitorCount && g_monitorStates[cursorMonitorIndex].screenSaverActive);

        if (cursorOnActiveMonitor) {
            HideCursorForScreenSaver("cursor on active monitor");
        } else {
            if (g_app.cursorHidden) {
                EnsureCursorVisible("cursor left active monitor");
            }
        }

        UpdateTrayIcon(IsAnyMonitorActive() ? 1 : 0);
    } else {
        // Global input detection mode:
        //   A single idle timer (GetIdleTime) covers all monitors. When per-
        //   monitor media detection is on, each monitor is still activated/
        //   deactivated independently based on whether media is playing on it,
        //   but idle time is global. Without per-monitor media, the original
        //   all-on/all-off behavior is used.
        DWORD idleTime = GetIdleTime();

        if (g_app.config.perMonitorMediaDetection && g_app.config.mediaDetectionEnabled) {
            // Per-monitor media with global input:
            //   When idle beyond the timeout, activate the screen saver on
            //   monitors without media and deactivate it on monitors where media
            //   is detected. When the user is active, deactivate everything
            //   (preserving the manual-activation cooldown logic).
            int mediaOnMonitor[MAX_MONITOR_COUNT] = {0};
            UpdateMediaMonitorStates(mediaOnMonitor);

            if (idleTime > (DWORD)(g_app.config.idleTimeout * 1000)) {
                for (int i = 0; i < g_monitorCount; i++) {
                    if (!g_monitorStates[i].enabled) continue;

                    if (mediaOnMonitor[i]) {
                        if (g_monitorStates[i].screenSaverActive) {
                            LogMessage("Timer: Deactivating screen saver on monitor %d (media detected)", i);
                            HideScreenSaverOnMonitor(i);
                        }
                    } else if (!g_monitorStates[i].screenSaverActive) {
                        LogMessage("Timer: Activating screen saver on monitor %d (idle: %lums)", i, idleTime);
                        ShowScreenSaverOnMonitor(i, 0);
                    }
                }

                g_app.screenSaverActive = IsAnyMonitorActive() ? 1 : 0;

                if (!g_app.screenSaverActive && g_app.cursorHidden) {
                    EnsureCursorVisible("no active monitors");
                }

                UpdateTrayIcon(g_app.screenSaverActive);
            } else {
                // User is active: deactivate everything (preserve manual cooldown logic)
                if (g_app.screenSaverActive) {
                    if (g_app.isManualActivation) {
                        DWORD timeSinceActivation = GetTickCount() - g_app.manualActivationTime;
                        if (timeSinceActivation < MANUAL_ACTIVATION_COOLDOWN_MS) {
                            LogMessage("Timer: Skipping deactivation (manual cooldown: %lums/%dms)",
                                     timeSinceActivation, MANUAL_ACTIVATION_COOLDOWN_MS);
                        } else {
                            if (idleTime < IDLE_DEACTIVATE_THRESHOLD_MS) {
                                LogMessage("Timer: Deactivating screen saver (new input detected after cooldown)");
                                HideScreenSaver();
                                UpdateTrayIcon(0);
                            }
                        }
                    } else {
                        LogMessage("Timer: Deactivating screen saver (idle: %lums)", idleTime);
                        HideScreenSaver();
                        UpdateTrayIcon(0);
                    }
                }
            }
        } else {
            // Original global behavior:
            //   When idle beyond the timeout and no media is playing, activate
            //   the screen saver on all enabled monitors at once. When the user
            //   is active or media starts playing, deactivate everything.
            //   Manual-activation cooldown logic is preserved.
            int mediaPlaying = IsMediaPlaying();

            if (!mediaPlaying && idleTime > (DWORD)(g_app.config.idleTimeout * 1000)) {
                if (!g_app.screenSaverActive) {
                    LogMessage("Timer: Activating screen saver (idle: %lums)", idleTime);
                    ShowScreenSaver(0);
                    UpdateTrayIcon(1);
                }
            } else {
                if (g_app.screenSaverActive) {
                    if (g_app.isManualActivation) {
                        DWORD timeSinceActivation = GetTickCount() - g_app.manualActivationTime;
                        if (timeSinceActivation < MANUAL_ACTIVATION_COOLDOWN_MS) {
                            LogMessage("Timer: Skipping deactivation (manual cooldown: %lums/%dms)",
                                     timeSinceActivation, MANUAL_ACTIVATION_COOLDOWN_MS);
                        } else {
                            if (idleTime < IDLE_DEACTIVATE_THRESHOLD_MS) {
                                LogMessage("Timer: Deactivating screen saver (new input detected after cooldown)");
                                HideScreenSaver();
                                UpdateTrayIcon(0);
                            }
                        }
                    } else {
                        LogMessage("Timer: Deactivating screen saver (idle: %lums, media: %d)", idleTime, mediaPlaying);
                        HideScreenSaver();
                        UpdateTrayIcon(0);
                    }
                }
            }
        }
    }

    // Ensure screen saver windows stay on top (handles notifications like MS
    // Teams, Steam friends, etc.), but throttle to avoid a SetWindowPos call
    // every timer tick.
    static DWORD lastTopmostRefresh = 0;
    if (IsAnyMonitorActive()) {
        DWORD nowTick = GetTickCount();
        if ((DWORD)(nowTick - lastTopmostRefresh) >= TOPMOST_REFRESH_INTERVAL_MS) {
            EnsureScreenSaverTopmost();
            lastTopmostRefresh = nowTick;
        }
    } else {
        lastTopmostRefresh = 0;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_uTaskbarRestart && g_uTaskbarRestart != 0 && g_app.nid.cbSize != 0) {
        LogMessage("Taskbar recreated (Explorer restart) - restoring tray icon");
        g_app.nid.hIcon = g_app.screenSaverActive ? g_hIconActive : g_hIconInactive;
        lstrcpyA(g_app.nid.szTip, g_app.screenSaverActive ? "OLED Aegis - Active" : "OLED Aegis - Idle");
        Shell_NotifyIconA(NIM_ADD, &g_app.nid);
        g_app.trayIconActive = g_app.screenSaverActive ? 1 : 0;
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            return HandleCreation(hWnd);

        case WM_TIMER:
            HandleTimeout(wParam);
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMEAUTOMATIC) {
                LogMessage("System resumed from sleep - resetting media detection cache");
                ResetMediaDetectionCache();
            }
            break;

        case WM_DISPLAYCHANGE:
            LogMessage("Display configuration changed - re-enumerating monitors");

            // Destroy all screen saver windows first
            for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
                if (g_monitorStates[i].hScreenSaverWnd) {
                    DestroyWindow(g_monitorStates[i].hScreenSaverWnd);
                    g_monitorStates[i].hScreenSaverWnd = NULL;
                }
                g_monitorStates[i].screenSaverActive = 0;
            }
            g_app.screenSaverActive = 0;
            UpdateTrayIcon(0);

            EnsureCursorVisible("display configuration changed");

            // Re-enumerate monitors to detect added/removed displays
            int oldMonitorCount = g_monitorCount;
            EnumerateMonitors();

            // Load config to get per-device settings
            LoadConfig();

            // Reinitialize monitor states
            time_t now = time(NULL);
            for (int i = 0; i < g_monitorCount; i++) {
                g_monitorStates[i].lastInputTime = now;
                g_monitorStates[i].screenSaverActive = 0;
                g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];
                g_monitorStates[i].hScreenSaverWnd = NULL;
            }

            // If settings dialog is open, close and reopen to refresh monitor list
            if (g_hSettingsDialog) {
                LogMessage("Refreshing settings dialog for new monitor configuration");
                DestroyWindow(g_hSettingsDialog);
                g_hSettingsDialog = NULL;
                ShowSettingsDialog();
            }

            LogMessage("Monitor configuration updated: %d -> %d monitors", oldMonitorCount, g_monitorCount);
            break;

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                LogMessage("User: Right-clicked tray icon - opening context menu");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hWnd);
                g_app.trayMenuActive = 1;
                EnsureCursorVisible("tray menu opened");

                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, IDM_SETTINGS, "Settings...");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, IDM_EXIT, "Exit");

                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                DestroyMenu(hMenu);
                g_app.trayMenuActive = 0;
                EnsureCursorVisible("tray menu closed");
            } else if (lParam == WM_LBUTTONDOWN) {
                EnsureCursorVisible("tray icon clicked");
                if (g_hSettingsDialog) {
                    LogMessage("User: Left-clicked tray icon - settings dialog already open, bringing to foreground");
                    SetForegroundWindow(g_hSettingsDialog);
                } else {
                    if (g_app.screenSaverActive) {
                        LogMessage("User: Left-clicked tray icon - deactivating screen saver");
                        HideScreenSaver();
                        UpdateTrayIcon(0);
                    } else {
                        LogMessage("User: Left-clicked tray icon - activating screen saver (manual)");
                        ShowScreenSaver(1);
                        UpdateTrayIcon(1);
                    }
                }
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_SETTINGS:
                    LogMessage("User: Selected 'Settings' from tray menu");
                    ShowSettingsDialog();
                    break;
                case IDM_EXIT:
                    LogMessage("User: Selected 'Exit' from tray menu - shutting down");
                    HideScreenSaver();
                    Shell_NotifyIconA(NIM_DELETE, &g_app.nid);
                    PostQuitMessage(0);
                    break;
            }
            break;

        case WM_DESTROY:
            LogMessage("Application shutting down");

            EnsureCursorVisible("shutdown");

            for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
                if (g_monitorStates[i].hScreenSaverWnd) {
                    DestroyWindow(g_monitorStates[i].hScreenSaverWnd);
                    g_monitorStates[i].hScreenSaverWnd = NULL;
                }
                g_monitorStates[i].screenSaverActive = 0;
            }

            Shell_NotifyIconA(NIM_DELETE, &g_app.nid);

            if (g_hSettingsDialog) {
                DestroyWindow(g_hSettingsDialog);
                g_hSettingsDialog = NULL;
            }

            if (g_logFile) {
                fclose(g_logFile);
                g_logFile = NULL;
            }

            if (g_blackBrush) {
                DeleteObject(g_blackBrush);
                g_blackBrush = NULL;
            }

            // Note: Icons loaded via LoadIcon from resources don't need DestroyIcon
            g_hIconActive = NULL;
            g_hIconInactive = NULL;

            if (g_hInstanceMutex) {
                CloseHandle(g_hInstanceMutex);
                g_hInstanceMutex = NULL;
            }

            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDPIAware();

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    // hrCom may be S_OK or S_FALSE (already initialized); either is fine to proceed.

    g_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OLEDAegisWindow";

    RegisterClassW(&wc);

    CreateWindowExW(0, L"OLEDAegisWindow", APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }

    return (int)msg.wParam;
}

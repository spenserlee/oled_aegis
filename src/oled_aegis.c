#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <commctrl.h>
#include <powerbase.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "powrprof.lib")

#define APP_NAME L"OLED Aegis"
#define WM_TRAYICON (WM_USER + 1)
#define TIMER_IDLE_CHECK 1
#define DEFAULT_IDLE_TIMEOUT 300
#define MAX_LOG_FILES 10
#define MANUAL_ACTIVATION_COOLDOWN_MS 2500

typedef NTSTATUS (WINAPI *PFN_CallNtPowerInformation)(
    POWER_INFORMATION_LEVEL InformationLevel,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength
);

static char g_logFilePath[MAX_PATH];
static FILE* g_logFile = NULL;

void ApplySettings(HWND hWnd);
LRESULT CALLBACK SettingsDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void ShowScreenSaver(int isManual);
void HideScreenSaver();
void UpdateTrayIcon(int active);

typedef struct {
    HMONITOR hMonitor;
    RECT rect;
    int monitorIndex;
    char deviceName[32];
    char displayName[64];
    int isPrimary;
    int width;
    int height;
} MonitorInfo;

typedef struct {
    int idleTimeout;
    int mediaDetectionEnabled;
    int monitorsEnabled[16];
    int monitorCount;
    int startupEnabled;
    int debugMode;
} Config;

typedef struct {
    HWND hWnd;
    HWND monitorWindows[16];
    int monitorWindowCount;
    Config config;
    NOTIFYICONDATA nid;
    int screenSaverActive;
    time_t lastInputTime;
    int isMediaPlaying;
    int isShuttingDown;
    int cursorHidden;
    DWORD manualActivationTime;
    int isManualActivation;
} AppState;

static AppState g_app;

static int g_currentMonitorIndex = 0;
static HBRUSH g_blackBrush = NULL;
static MonitorInfo g_monitors[16];
static int g_monitorCount = 0;
static HWND g_hSettingsDialog = NULL;
static HFONT g_hSettingsFont = NULL;

void GetAppDataPath(char* buffer, size_t bufferSize) {
    char appDataPath[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appDataPath);
    sprintf_s(buffer, bufferSize, "%s\\OLED_Aegis", appDataPath);

    CreateDirectoryA(buffer, NULL);
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

BOOL CALLBACK EnumMonitorCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MONITORINFOEX mi;
    mi.cbSize = sizeof(MONITORINFOEX);
    GetMonitorInfo(hMonitor, (LPMONITORINFO)&mi);

    if (g_monitorCount < 16) {
        g_monitors[g_monitorCount].hMonitor = hMonitor;
        g_monitors[g_monitorCount].rect = *lprcMonitor;
        g_monitors[g_monitorCount].monitorIndex = g_monitorCount;
        g_monitors[g_monitorCount].isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        WideCharToMultiByte(CP_ACP, 0, mi.szDevice, -1,
                           g_monitors[g_monitorCount].deviceName, 32, NULL, NULL);

        DEVMODEA dm = {0};
        dm.dmSize = sizeof(DEVMODEA);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            g_monitors[g_monitorCount].width = dm.dmPelsWidth;
            g_monitors[g_monitorCount].height = dm.dmPelsHeight;
        } else {
            g_monitors[g_monitorCount].width = lprcMonitor->right - lprcMonitor->left;
            g_monitors[g_monitorCount].height = lprcMonitor->bottom - lprcMonitor->top;
        }

        snprintf(g_monitors[g_monitorCount].displayName, 64,
                "Display %d (%dx%d)%s",
                g_monitorCount,
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
            ignoreInputUntil = GetTickCount() + 500;
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
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (GetTickCount() < ignoreInputUntil) {
                break;
            }
            LogMessage("Input detected on monitor window (msg: %u)", message);
            HideScreenSaver();
            UpdateTrayIcon(0);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

BOOL CALLBACK CreateMonitorWindowsCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    AppState* app = (AppState*)dwData;

    if (g_currentMonitorIndex >= 16) return TRUE;

    if (app->config.monitorsEnabled[g_currentMonitorIndex]) {
        HWND hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                   L"OLEDAegisScreen", L"",
                                   WS_POPUP,
                                   lprcMonitor->left, lprcMonitor->top,
                                   lprcMonitor->right - lprcMonitor->left,
                                   lprcMonitor->bottom - lprcMonitor->top,
                                   NULL, NULL, GetModuleHandle(NULL), NULL);

        if (hWnd) {
            ShowWindow(hWnd, SW_SHOW);
            UpdateWindow(hWnd);
            app->monitorWindows[app->monitorWindowCount++] = hWnd;
        }
    }

    g_currentMonitorIndex++;
    return TRUE;
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

    FILE* f = fopen(configPath, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64], value[64];
            if (sscanf(line, "%63[^=]=%63s", key, value) == 2) {
                if (strcmp(key, "idleTimeout") == 0) {
                    g_app.config.idleTimeout = atoi(value);
                } else if (strcmp(key, "audioDetectionEnabled") == 0) {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                } else if (strcmp(key, "mediaDetectionEnabled") == 0) {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                } else if (strcmp(key, "startupEnabled") == 0) {
                    g_app.config.startupEnabled = atoi(value);
                } else if (strcmp(key, "debugMode") == 0) {
                    g_app.config.debugMode = atoi(value);
                } else if (strncmp(key, "monitor", 7) == 0) {
                    int idx = atoi(key + 7);
                    if (idx >= 0 && idx < 16) {
                        g_app.config.monitorsEnabled[idx] = atoi(value);
                    }
                }
            }
        }
        fclose(f);
    }
}

void SaveConfig() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    FILE* f = fopen(configPath, "w");
    if (f) {
        fprintf(f, "idleTimeout=%d\n", g_app.config.idleTimeout);
        fprintf(f, "mediaDetectionEnabled=%d\n", g_app.config.mediaDetectionEnabled);
        fprintf(f, "startupEnabled=%d\n", g_app.config.startupEnabled);
        fprintf(f, "debugMode=%d\n", g_app.config.debugMode);
        for (int i = 0; i < g_monitorCount; i++) {
            fprintf(f, "monitor%d=%d\n", i, g_app.config.monitorsEnabled[i]);
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

void EnumerateMonitors() {
    g_monitorCount = 0;
    EnumDisplayMonitors(NULL, NULL, EnumMonitorCallback, 0);
    LogMessage("Enumerated %d monitors", g_monitorCount);
}

int IsMediaPlaying() {
    if (!g_app.config.mediaDetectionEnabled) {
        return 0;
    }

    static HMODULE hPowrProf = NULL;
    static PFN_CallNtPowerInformation pfnCallNtPowerInformation = NULL;

    if (!hPowrProf) {
        hPowrProf = LoadLibraryW(L"powrprof.dll");
    }

    if (hPowrProf && !pfnCallNtPowerInformation) {
        pfnCallNtPowerInformation = (PFN_CallNtPowerInformation)GetProcAddress(hPowrProf, "CallNtPowerInformation");
    }

    if (!pfnCallNtPowerInformation) {
        return 0;
    }

    ULONG executionState = 0;
    NTSTATUS status = pfnCallNtPowerInformation(
        SystemExecutionState,
        NULL, 0,
        &executionState, sizeof(executionState)
    );

    if (status == 0) {
        int isPlaying = (executionState & ES_DISPLAY_REQUIRED) != 0;
        LogMessage("Media detection: executionState=0x%08X, ES_DISPLAY_REQUIRED=%d, mediaPlaying=%d",
                 executionState, (executionState & ES_DISPLAY_REQUIRED) != 0, isPlaying);
        return isPlaying;
    }

    LogMessage("Media detection: CallNtPowerInformation failed with status=%d", status);
    return 0;
}

void ShowScreenSaver(int isManual) {
    if (g_app.screenSaverActive) return;

    g_app.isManualActivation = isManual;
    if (isManual) {
        g_app.manualActivationTime = GetTickCount();
        LogMessage("Showing screen saver (manual activation)");
    } else {
        g_app.manualActivationTime = 0;
        LogMessage("Showing screen saver (automatic activation)");
    }

    LogMessage("Showing screen saver on %d monitors", g_monitorCount);

    g_currentMonitorIndex = 0;
    g_app.monitorWindowCount = 0;

    EnumDisplayMonitors(NULL, NULL, CreateMonitorWindowsCallback, (LPARAM)&g_app);

    LogMessage("Created %d screen saver windows", g_app.monitorWindowCount);

    g_app.screenSaverActive = 1;

    if (!g_app.cursorHidden) {
        ShowCursor(FALSE);
        g_app.cursorHidden = 1;
        LogMessage("Cursor hidden");
    }
}

void HideScreenSaver() {
    if (!g_app.screenSaverActive) return;

    LogMessage("Hiding screen saver (%d windows)", g_app.monitorWindowCount);

    for (int i = 0; i < g_app.monitorWindowCount; i++) {
        if (g_app.monitorWindows[i]) {
            DestroyWindow(g_app.monitorWindows[i]);
            g_app.monitorWindows[i] = NULL;
        }
    }
    g_app.monitorWindowCount = 0;
    g_app.screenSaverActive = 0;
    g_app.manualActivationTime = 0;

    if (g_app.cursorHidden) {
        ShowCursor(TRUE);
        g_app.cursorHidden = 0;
        LogMessage("Cursor restored");
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
                case 1005:
                    ApplySettings(hWnd);
                    break;
                case 1006:
                    LogMessage("Settings: Opening config file location");
                    OpenConfigFileLocation();
                    break;
                case 1007:
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



void ShowSettingsDialog() {
    if (g_hSettingsDialog) {
        SetForegroundWindow(g_hSettingsDialog);
        return;
    }

    NONCLIENTMETRICS ncm = {0};
    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &ncm, 0);
    g_hSettingsFont = CreateFontIndirectA(&ncm.lfMessageFont);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "OLED Aegis Settings Dialog";
    RegisterClassA(&wc);

    HMODULE hMod = GetModuleHandle(NULL);
    g_hSettingsDialog = CreateWindowExA(0, "OLED Aegis Settings Dialog", "OLED Aegis Settings",
                                      // WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                      WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 410, 400,
                                      NULL, NULL, hMod, NULL);

    if (g_hSettingsDialog) {
        SetWindowLongPtr(g_hSettingsDialog, GWLP_WNDPROC, (LONG_PTR)SettingsDialogProc);

        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_UPDOWN_CLASS;
        InitCommonControlsEx(&icex);

        HWND hTimeoutLabel = CreateWindowA("STATIC", "Idle Timeout (seconds):",
                     WS_CHILD | WS_VISIBLE,
                     20, 20, 180, 20, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hTimeoutEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     200, 20, 100, 20, g_hSettingsDialog, (HMENU)1001, hMod, NULL);
        HWND hTimeoutUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hTimeoutEdit);
        SendMessage(hTimeoutUpDown, UDM_SETRANGE, 0, MAKELPARAM(3600, 5));

        HWND hAudioCheck = CreateWindowA("BUTTON", "Prevent Screen Saver During Media Playback",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, 50, 260, 20, g_hSettingsDialog, (HMENU)1002, hMod, NULL);
        HWND hDebugCheck = CreateWindowA("BUTTON", "Debug Mode",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, 75, 220, 20, g_hSettingsDialog, (HMENU)1003, hMod, NULL);
        HWND hStartupCheck = CreateWindowA("BUTTON", "Run at Startup",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     20, 100, 220, 20, g_hSettingsDialog, (HMENU)1004, hMod, NULL);

        HWND hMonitorsLabel = CreateWindowA("STATIC", "Monitors:",
                     WS_CHILD | WS_VISIBLE,
                     20, 130, 100, 20, g_hSettingsDialog, NULL, hMod, NULL);

        int y = 155;
        for (int i = 0; i < g_monitorCount; i++) {
            HWND hMonitorCheck = CreateWindowA("BUTTON", g_monitors[i].displayName,
                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                         20, y, 380, 20, g_hSettingsDialog, (HMENU)(2000 + i),
                         hMod, NULL);
            if (g_hSettingsFont) SendMessageA(hMonitorCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            y += 25;
        }

        int btnY = y + 25;
        HWND hApplyBtn = CreateWindowA("BUTTON", "Apply",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     20, btnY, 100, 30, g_hSettingsDialog, (HMENU)1005, hMod, NULL);
        HWND hConfigBtn = CreateWindowA("BUTTON", "Open Config File",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     130, btnY, 130, 30, g_hSettingsDialog, (HMENU)1006, hMod, NULL);
        HWND hCloseBtn = CreateWindowA("BUTTON", "Close",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     270, btnY, 100, 30, g_hSettingsDialog, (HMENU)1007, hMod, NULL);

        int dialogWidth = 410;
        int dialogHeight = btnY + 90;
        SetWindowPos(g_hSettingsDialog, NULL, 0, 0, dialogWidth, dialogHeight,
                     SWP_NOMOVE | SWP_NOZORDER);

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(g_hSettingsDialog, NULL, (screenWidth - dialogWidth) / 2, (screenHeight - dialogHeight) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);

        if (g_hSettingsFont) {
            SendMessageA(hTimeoutLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hTimeoutEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hTimeoutUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hAudioCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hDebugCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hStartupCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hMonitorsLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hApplyBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hConfigBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hCloseBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
        }

        char buffer[32];
        sprintf_s(buffer, 32, "%d", g_app.config.idleTimeout);
        SetDlgItemTextA(g_hSettingsDialog, 1001, buffer);

        CheckDlgButton(g_hSettingsDialog, 1002, g_app.config.mediaDetectionEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, 1003, g_app.config.debugMode ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, 1004, g_app.config.startupEnabled ? BST_CHECKED : BST_UNCHECKED);

        for (int i = 0; i < g_monitorCount; i++) {
            CheckDlgButton(g_hSettingsDialog, 2000 + i, g_app.config.monitorsEnabled[i] ? BST_CHECKED : BST_UNCHECKED);
        }

        ShowWindow(g_hSettingsDialog, SW_SHOW);
        UpdateWindow(g_hSettingsDialog);
    }
}

void ApplySettings(HWND hWnd) {
    char buffer[32];
    GetDlgItemTextA(hWnd, 1001, buffer, 32);
    int oldTimeout = g_app.config.idleTimeout;
    g_app.config.idleTimeout = atoi(buffer);

    int oldMedia = g_app.config.mediaDetectionEnabled;
    int oldDebug = g_app.config.debugMode;
    int oldStartup = g_app.config.startupEnabled;

    g_app.config.mediaDetectionEnabled = IsDlgButtonChecked(hWnd, 1002) == BST_CHECKED;
    g_app.config.debugMode = IsDlgButtonChecked(hWnd, 1003) == BST_CHECKED;
    g_app.config.startupEnabled = IsDlgButtonChecked(hWnd, 1004) == BST_CHECKED;

    for (int i = 0; i < g_monitorCount; i++) {
        g_app.config.monitorsEnabled[i] = IsDlgButtonChecked(hWnd, 2000 + i) == BST_CHECKED;
    }

    SaveConfig();
    UpdateStartupRegistry();

    LogMessage("Settings applied: timeout %ds->%ds, media %d->%d, debug %d->%d, startup %d->%d",
             oldTimeout, g_app.config.idleTimeout,
             oldMedia, g_app.config.mediaDetectionEnabled,
             oldDebug, g_app.config.debugMode,
             oldStartup, g_app.config.startupEnabled);
}

void UpdateTrayIcon(int active) {
    HICON hIcon = LoadIcon(NULL, active ? IDI_APPLICATION : IDI_INFORMATION);
    g_app.nid.hIcon = hIcon;
    lstrcpyW(g_app.nid.szTip, active ? L"OLED Aegis - Active" : L"OLED Aegis - Idle");
    Shell_NotifyIconW(NIM_MODIFY, &g_app.nid);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            memset(&g_app, 0, sizeof(g_app));
            g_app.hWnd = hWnd;
            g_app.isShuttingDown = 0;

            HANDLE hMutex = CreateMutexW(NULL, TRUE, L"OLEDAegis_SingleInstance");
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                MessageBoxW(NULL, L"OLED Aegis is already running", L"OLED Aegis", MB_OK | MB_ICONINFORMATION);
                PostQuitMessage(0);
                return -1;
            }
            if (hMutex) {
                CloseHandle(hMutex);
            }

            g_app.nid.cbSize = sizeof(NOTIFYICONDATAW);
            g_app.nid.hWnd = hWnd;
            g_app.nid.uID = 1;
            g_app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_app.nid.uCallbackMessage = WM_TRAYICON;
            g_app.nid.hIcon = LoadIcon(NULL, IDI_INFORMATION);
            lstrcpyW(g_app.nid.szTip, L"OLED Aegis - Idle");
            Shell_NotifyIconW(NIM_ADD, &g_app.nid);

            g_app.config.idleTimeout = DEFAULT_IDLE_TIMEOUT;
            g_app.config.mediaDetectionEnabled = 1;
            g_app.config.startupEnabled = 0;
            g_app.config.debugMode = 0;
            for (int i = 0; i < 16; i++) {
                g_app.config.monitorsEnabled[i] = 1;
            }

            EnumerateMonitors();

            if (!ConfigFileExists()) {
                SaveConfig();
            }

            LoadConfig();
            UpdateStartupRegistry();

            LogMessage("Application started. Timeout: %ds, Media: %d, Debug: %d",
                     g_app.config.idleTimeout, g_app.config.mediaDetectionEnabled, g_app.config.debugMode);

            g_blackBrush = CreateSolidBrush(RGB(0, 0, 0));

            WNDCLASSW wc = {0};
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = MonitorWindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.hbrBackground = g_blackBrush;
            wc.lpszClassName = L"OLEDAegisScreen";
            RegisterClassW(&wc);

            SetTimer(hWnd, TIMER_IDLE_CHECK, 500, NULL);

            break;

        case WM_TIMER:
            if (wParam == TIMER_IDLE_CHECK) {
                DWORD idleTime = GetIdleTime();
                int mediaPlaying = IsMediaPlaying();
                if (mediaPlaying) {
                    LogMessage("Timer: Media playback detected, preventing screen saver");
                }

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
                                if (idleTime < 2000) {
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
            break;

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                LogMessage("User: Right-clicked tray icon - opening context menu");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hWnd);

                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, 1, "Settings...");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, 2, "Exit");

                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                DestroyMenu(hMenu);
            } else if (lParam == WM_LBUTTONDOWN) {
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
                case 1:
                    LogMessage("User: Selected 'Settings' from tray menu");
                    ShowSettingsDialog();
                    break;
                case 2:
                    LogMessage("User: Selected 'Exit' from tray menu - shutting down");
                    HideScreenSaver();
                    Shell_NotifyIcon(NIM_DELETE, &g_app.nid);
                    PostQuitMessage(0);
                    break;
            }
            break;

        case WM_DESTROY:
            LogMessage("Application shutting down");

            if (g_app.cursorHidden) {
                ShowCursor(TRUE);
                g_app.cursorHidden = 0;
                LogMessage("Cursor restored on shutdown");
            }

            HideScreenSaver();
            Shell_NotifyIcon(NIM_DELETE, &g_app.nid);

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

            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDPIAware();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OLEDAegisWindow";

    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(0, L"OLEDAegisWindow", APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <time.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>

static const GUID IID_IMMDeviceEnumerator_impl = {0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6};
static const GUID CLSID_MMDeviceEnumerator_impl = {0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E};
static const GUID IID_IAudioMeterInformation_impl = {0xC02216F6, 0x8C63, 0x4B94, 0x98, 0x5F, 0x13, 0x89, 0xD8, 0x2F, 0x98, 0x5E};

#define IID_IMMDeviceEnumerator (&IID_IMMDeviceEnumerator_impl)
#define CLSID_MMDeviceEnumerator (&CLSID_MMDeviceEnumerator_impl)
#define IID_IAudioMeterInformation (&IID_IAudioMeterInformation_impl)

#define APP_NAME L"OLED Aegis"
#define WM_TRAYICON (WM_USER + 1)
#define TIMER_IDLE_CHECK 1
#define TIMER_AUDIO_CHECK 2
#define DEFAULT_IDLE_TIMEOUT 300
#define DEFAULT_AUDIO_THRESHOLD 0.001f

typedef struct {
    HMONITOR hMonitor;
    RECT rect;
    int monitorIndex;
} MonitorInfo;

typedef struct {
    int idleTimeout;
    int audioDetectionEnabled;
    int monitorsEnabled[16];
    int monitorCount;
    int startupEnabled;
} Config;

typedef struct {
    HWND hWnd;
    HWND monitorWindows[16];
    int monitorWindowCount;
    Config config;
    NOTIFYICONDATA nid;
    int screenSaverActive;
    time_t lastInputTime;
    int isAudioPlaying;
    IMMDeviceEnumerator *pAudioEnumerator;
    IMMDevice *pAudioDevice;
    IAudioMeterInformation *pAudioMeter;
} AppState;

static AppState g_app;

static int g_currentMonitorIndex = 0;
static HBRUSH g_blackBrush = NULL;

BOOL CALLBACK EnumMonitorCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MONITORINFOEX mi;
    mi.cbSize = sizeof(MONITORINFOEX);
    GetMonitorInfo(hMonitor, &mi);

    if (g_app.config.monitorCount < 16) {
        g_app.config.monitorsEnabled[g_app.config.monitorCount] = 1;
        g_app.config.monitorCount++;
    }

    return TRUE;
}

LRESULT CALLBACK MonitorWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
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
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

BOOL CALLBACK CreateMonitorWindowsCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    AppState* app = (AppState*)dwData;

    if (g_currentMonitorIndex >= 16) return TRUE;

    if (app->config.monitorsEnabled[g_currentMonitorIndex]) {
        HWND hWnd = CreateWindowEx(WS_EX_TOPMOST,
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

void LoadConfig() {
    char configPath[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, configPath);
    strcat_s(configPath, sizeof(configPath), "\\oled_aegis.ini");

    FILE* f = fopen(configPath, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64], value[64];
            if (sscanf(line, "%63[^=]=%63s", key, value) == 2) {
                if (strcmp(key, "idleTimeout") == 0) {
                    g_app.config.idleTimeout = atoi(value);
                } else if (strcmp(key, "audioDetectionEnabled") == 0) {
                    g_app.config.audioDetectionEnabled = atoi(value);
                } else if (strcmp(key, "startupEnabled") == 0) {
                    g_app.config.startupEnabled = atoi(value);
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
    char configPath[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, configPath);
    strcat_s(configPath, sizeof(configPath), "\\oled_aegis.ini");

    FILE* f = fopen(configPath, "w");
    if (f) {
        fprintf(f, "idleTimeout=%d\n", g_app.config.idleTimeout);
        fprintf(f, "audioDetectionEnabled=%d\n", g_app.config.audioDetectionEnabled);
        fprintf(f, "startupEnabled=%d\n", g_app.config.startupEnabled);
        for (int i = 0; i < g_app.config.monitorCount; i++) {
            fprintf(f, "monitor%d=%d\n", i, g_app.config.monitorsEnabled[i]);
        }
        fclose(f);
    }
}

void UpdateStartupRegistry() {
    HKEY hKey;
    TCHAR exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (g_app.config.startupEnabled) {
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE*)exePath, (DWORD)(wcslen(exePath) + 1) * sizeof(TCHAR));
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

int IsAudioPlaying() {
    if (!g_app.config.audioDetectionEnabled) {
        return 0;
    }

    if (!g_app.pAudioMeter) {
        return 0;
    }

    float peakValue = 0.0f;
    int result = 0;
    HRESULT hr = g_app.pAudioMeter->lpVtbl->GetPeakValue(g_app.pAudioMeter, &peakValue);
    if (SUCCEEDED(hr)) {
        result = (peakValue > DEFAULT_AUDIO_THRESHOLD);
    }

    return result;
}

void ShowScreenSaver() {
    if (g_app.screenSaverActive) return;

    g_currentMonitorIndex = 0;
    g_app.monitorWindowCount = 0;

    EnumDisplayMonitors(NULL, NULL, CreateMonitorWindowsCallback, (LPARAM)&g_app);

    g_app.screenSaverActive = 1;
}

void HideScreenSaver() {
    if (!g_app.screenSaverActive) return;

    for (int i = 0; i < g_app.monitorWindowCount; i++) {
        if (g_app.monitorWindows[i]) {
            DestroyWindow(g_app.monitorWindows[i]);
            g_app.monitorWindows[i] = NULL;
        }
    }
    g_app.monitorWindowCount = 0;
    g_app.screenSaverActive = 0;
}

void ShowSettingsDialog() {
    char configPath[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, configPath);
    strcat_s(configPath, sizeof(configPath), "\\oled_aegis.ini");

    MessageBoxA(NULL,
                "Configuration file location:\n\n"
                "Edit oled_aegis.ini in your AppData folder to change:\n"
                "- idleTimeout: seconds of inactivity before activation\n"
                "- audioDetectionEnabled: 1 to check for audio, 0 to ignore\n"
                "- monitorN: 1 to enable screen saver on monitor N, 0 to disable\n"
                "- startupEnabled: 1 to run at startup, 0 to disable\n\n"
                "Changes will be loaded automatically.",
                "OLED Aegis Settings",
                MB_OK | MB_ICONINFORMATION);
}

void UpdateTrayIcon(int active) {
    HICON hIcon = LoadIcon(NULL, active ? IDI_APPLICATION : IDI_INFORMATION);
    g_app.nid.hIcon = hIcon;
    wcscpy_s(g_app.nid.szTip, 128, active ? L"OLED Aegis - Active" : L"OLED Aegis - Idle");
    Shell_NotifyIcon(NIM_MODIFY, &g_app.nid);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            memset(&g_app, 0, sizeof(g_app));
            g_app.hWnd = hWnd;

            g_app.nid.cbSize = sizeof(NOTIFYICONDATA);
            g_app.nid.hWnd = hWnd;
            g_app.nid.uID = 1;
            g_app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_app.nid.uCallbackMessage = WM_TRAYICON;
            g_app.nid.hIcon = LoadIcon(NULL, IDI_INFORMATION);
            wcscpy_s(g_app.nid.szTip, 128, L"OLED Aegis - Idle");
            Shell_NotifyIcon(NIM_ADD, &g_app.nid);

            g_app.config.idleTimeout = DEFAULT_IDLE_TIMEOUT;
            g_app.config.audioDetectionEnabled = 1;
            g_app.config.startupEnabled = 0;
            for (int i = 0; i < 16; i++) {
                g_app.config.monitorsEnabled[i] = 1;
            }

            LoadConfig();
            UpdateStartupRegistry();

            g_blackBrush = CreateSolidBrush(RGB(0, 0, 0));

            WNDCLASS wc = {0};
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = MonitorWindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.hbrBackground = g_blackBrush;
            wc.lpszClassName = L"OLEDAegisScreen";
            RegisterClass(&wc);

            CoInitialize(NULL);
            HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&g_app.pAudioEnumerator);
            if (SUCCEEDED(hr) && g_app.pAudioEnumerator) {
                hr = g_app.pAudioEnumerator->lpVtbl->GetDefaultAudioEndpoint(g_app.pAudioEnumerator, eRender, eConsole, &g_app.pAudioDevice);
                if (SUCCEEDED(hr) && g_app.pAudioDevice) {
                    hr = g_app.pAudioDevice->lpVtbl->Activate(g_app.pAudioDevice, IID_IAudioMeterInformation, CLSCTX_ALL, NULL, (void**)&g_app.pAudioMeter);
                }
            }

            SetTimer(hWnd, TIMER_IDLE_CHECK, 1000, NULL);

            break;

        case WM_TIMER:
            if (wParam == TIMER_IDLE_CHECK) {
                DWORD idleTime = GetIdleTime();
                int audioPlaying = IsAudioPlaying();

                if (!audioPlaying && idleTime > (DWORD)(g_app.config.idleTimeout * 1000)) {
                    if (!g_app.screenSaverActive) {
                        ShowScreenSaver();
                        UpdateTrayIcon(1);
                    }
                } else {
                    if (g_app.screenSaverActive) {
                        HideScreenSaver();
                        UpdateTrayIcon(0);
                    }
                }
            }
            break;

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hWnd);

                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, 1, "Settings...");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, 2, "Enable Startup");
                AppendMenuA(hMenu, MF_STRING, 3, "Disable Startup");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, 4, "Exit");

                CheckMenuItem(hMenu, 2, g_app.config.startupEnabled ? MF_CHECKED : MF_UNCHECKED);
                CheckMenuItem(hMenu, 3, g_app.config.startupEnabled ? MF_UNCHECKED : MF_CHECKED);

                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                DestroyMenu(hMenu);
            } else if (lParam == WM_LBUTTONDOWN) {
                if (g_app.screenSaverActive) {
                    HideScreenSaver();
                    UpdateTrayIcon(0);
                } else {
                    ShowScreenSaver();
                    UpdateTrayIcon(1);
                }
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case 1:
                    ShowSettingsDialog();
                    break;
                case 2:
                    g_app.config.startupEnabled = 1;
                    UpdateStartupRegistry();
                    SaveConfig();
                    break;
                case 3:
                    g_app.config.startupEnabled = 0;
                    UpdateStartupRegistry();
                    SaveConfig();
                    break;
                case 4:
                    HideScreenSaver();
                    Shell_NotifyIcon(NIM_DELETE, &g_app.nid);
                    PostQuitMessage(0);
                    break;
            }
            break;

        case WM_DESTROY:
            HideScreenSaver();
            Shell_NotifyIcon(NIM_DELETE, &g_app.nid);

            if (g_app.pAudioMeter) {
                g_app.pAudioMeter->lpVtbl->Release(g_app.pAudioMeter);
                g_app.pAudioMeter = NULL;
            }
            if (g_app.pAudioDevice) {
                g_app.pAudioDevice->lpVtbl->Release(g_app.pAudioDevice);
                g_app.pAudioDevice = NULL;
            }
            if (g_app.pAudioEnumerator) {
                g_app.pAudioEnumerator->lpVtbl->Release(g_app.pAudioEnumerator);
                g_app.pAudioEnumerator = NULL;
            }
            CoUninitialize();

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
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OLEDAegisWindow";

    RegisterClass(&wc);

    HWND hWnd = CreateWindowEx(0, L"OLEDAegisWindow", APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}

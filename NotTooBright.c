/*
 * NotTooBright - NotTooBright.c
 *
 * System tray utility for lowering the brightness of desktop monitors from
 * software, for displays that offer no native Windows brightness control.
 *
 * Current scope:
 *   - System tray icon with a context menu (Configure / Exit)
 *   - WebView2-hosted configuration dialog (React UI embedded as a resource)
 *   - Registry-persisted settings, optional start-at-sign-in registration
 *   - Optional debug log in %LOCALAPPDATA%\NotTooBright\debug.log
 *
 * Cross-compiled with MinGW-w64. The WebView2 COM interfaces are declared
 * inline as minimal vtables so no SDK download is needed to build.
 */

#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "resource.h"
#include "version.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define APP_NAME L"NotTooBright"
#define MUTEX_NAME L"NotTooBright_SingleInstance_Mutex_6D1F2B3A_7C4E_4F89_9A21_5E0B8D7C3F14"

/* Registry settings (per user). */
#define REG_KEY_PATH L"SOFTWARE\\JPIT\\NotTooBright"
#define REG_VALUE_DEBUGLOG L"DebugLog"
/* Windows' per-user startup list; the value name is the entry Windows shows
 * in Task Manager > Startup apps. */
#define REG_RUN_KEY_PATH L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_RUN_VALUE_NAME L"NotTooBright"

#define TRAY_ICON_ID 100
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_MENU_CONFIGURE 1
#define ID_TRAY_MENU_EXIT 2

/* The config dialog is normally shown by its first resize message; the
 * fallback timer keeps waiting while WebView2 is still initializing and
 * gives up (with an error) after this many 350 ms ticks. */
#define ID_TIMER_CFG_SHOW_FALLBACK 1
#define CFG_SHOW_FALLBACK_DELAY_MS 350
#define CFG_SHOW_FALLBACK_MAX_TRIES 20
/* Initial (hidden) dialog size in CSS pixels; the page resizes it. */
#define CFG_INITIAL_WIDTH 480
#define CFG_INITIAL_HEIGHT 320

/* ── WebView2 COM interface definitions (minimal vtable approach) ─────── */

typedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;

typedef struct ICoreWebView2Environment ICoreWebView2Environment;
typedef struct ICoreWebView2Controller ICoreWebView2Controller;
typedef struct ICoreWebView2 ICoreWebView2;
typedef struct ICoreWebView2Settings ICoreWebView2Settings;
typedef struct ICoreWebView2WebMessageReceivedEventArgs ICoreWebView2WebMessageReceivedEventArgs;
typedef struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
typedef struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
typedef struct ICoreWebView2WebMessageReceivedEventHandler ICoreWebView2WebMessageReceivedEventHandler;

/* ICoreWebView2Environment vtable */
typedef struct ICoreWebView2EnvironmentVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Environment*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Environment*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Environment*);
    HRESULT (STDMETHODCALLTYPE *CreateCoreWebView2Controller)(ICoreWebView2Environment*, HWND, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    HRESULT (STDMETHODCALLTYPE *CreateWebResourceResponse)(ICoreWebView2Environment*, void*, int, LPCWSTR, LPCWSTR, void**);
    HRESULT (STDMETHODCALLTYPE *get_BrowserVersionString)(ICoreWebView2Environment*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *add_NewBrowserVersionAvailable)(ICoreWebView2Environment*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NewBrowserVersionAvailable)(ICoreWebView2Environment*, EventRegistrationToken);
} ICoreWebView2EnvironmentVtbl;
struct ICoreWebView2Environment { const ICoreWebView2EnvironmentVtbl *lpVtbl; };

/* ICoreWebView2Controller vtable */
typedef struct ICoreWebView2ControllerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Controller*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Controller*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Controller*);
    HRESULT (STDMETHODCALLTYPE *get_IsVisible)(ICoreWebView2Controller*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsVisible)(ICoreWebView2Controller*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_Bounds)(ICoreWebView2Controller*, RECT*);
    HRESULT (STDMETHODCALLTYPE *put_Bounds)(ICoreWebView2Controller*, RECT);
    HRESULT (STDMETHODCALLTYPE *get_ZoomFactor)(ICoreWebView2Controller*, double*);
    HRESULT (STDMETHODCALLTYPE *put_ZoomFactor)(ICoreWebView2Controller*, double);
    HRESULT (STDMETHODCALLTYPE *add_ZoomFactorChanged)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ZoomFactorChanged)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *SetBoundsAndZoomFactor)(ICoreWebView2Controller*, RECT, double);
    HRESULT (STDMETHODCALLTYPE *MoveFocus)(ICoreWebView2Controller*, int);
    HRESULT (STDMETHODCALLTYPE *add_MoveFocusRequested)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_MoveFocusRequested)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_GotFocus)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_GotFocus)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_LostFocus)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_LostFocus)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_AcceleratorKeyPressed)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_AcceleratorKeyPressed)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_ParentWindow)(ICoreWebView2Controller*, HWND*);
    HRESULT (STDMETHODCALLTYPE *put_ParentWindow)(ICoreWebView2Controller*, HWND);
    HRESULT (STDMETHODCALLTYPE *NotifyParentWindowPositionChanged)(ICoreWebView2Controller*);
    HRESULT (STDMETHODCALLTYPE *Close)(ICoreWebView2Controller*);
    HRESULT (STDMETHODCALLTYPE *get_CoreWebView2)(ICoreWebView2Controller*, ICoreWebView2**);
} ICoreWebView2ControllerVtbl;
struct ICoreWebView2Controller { const ICoreWebView2ControllerVtbl *lpVtbl; };

/* ICoreWebView2 vtable */
typedef struct ICoreWebView2Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *get_Settings)(ICoreWebView2*, ICoreWebView2Settings**);
    HRESULT (STDMETHODCALLTYPE *get_Source)(ICoreWebView2*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *Navigate)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *NavigateToString)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *add_NavigationStarting)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NavigationStarting)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ContentLoading)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ContentLoading)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_SourceChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_SourceChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_HistoryChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_HistoryChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_NavigationCompleted)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NavigationCompleted)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_FrameNavigationStarting)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_FrameNavigationStarting)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_FrameNavigationCompleted)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_FrameNavigationCompleted)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ScriptDialogOpening)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ScriptDialogOpening)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_PermissionRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_PermissionRequested)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ProcessFailed)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ProcessFailed)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *AddScriptToExecuteOnDocumentCreated)(ICoreWebView2*, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *RemoveScriptToExecuteOnDocumentCreated)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *ExecuteScript)(ICoreWebView2*, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *CapturePreview)(ICoreWebView2*, int, void*, void*);
    HRESULT (STDMETHODCALLTYPE *Reload)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *PostWebMessageAsJson)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *PostWebMessageAsString)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *add_WebMessageReceived)(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventHandler*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_WebMessageReceived)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *CallDevToolsProtocolMethod)(ICoreWebView2*, LPCWSTR, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *get_BrowserProcessId)(ICoreWebView2*, UINT32*);
    HRESULT (STDMETHODCALLTYPE *get_CanGoBack)(ICoreWebView2*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *get_CanGoForward)(ICoreWebView2*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *GoBack)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *GoForward)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *GetDevToolsProtocolEventReceiver)(ICoreWebView2*, LPCWSTR, void**);
    HRESULT (STDMETHODCALLTYPE *Stop)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *add_NewWindowRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NewWindowRequested)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_DocumentTitleChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_DocumentTitleChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_DocumentTitle)(ICoreWebView2*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *AddHostObjectToScript)(ICoreWebView2*, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *RemoveHostObjectFromScript)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *OpenDevToolsWindow)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *add_ContainsFullScreenElementChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ContainsFullScreenElementChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_ContainsFullScreenElement)(ICoreWebView2*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *add_WebResourceRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_WebResourceRequested)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *AddWebResourceRequestedFilter)(ICoreWebView2*, LPCWSTR, int);
    HRESULT (STDMETHODCALLTYPE *RemoveWebResourceRequestedFilter)(ICoreWebView2*, LPCWSTR, int);
    HRESULT (STDMETHODCALLTYPE *add_WindowCloseRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_WindowCloseRequested)(ICoreWebView2*, EventRegistrationToken);
} ICoreWebView2Vtbl;
struct ICoreWebView2 { const ICoreWebView2Vtbl *lpVtbl; };

/* ICoreWebView2Settings vtable */
typedef struct ICoreWebView2SettingsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Settings*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Settings*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Settings*);
    HRESULT (STDMETHODCALLTYPE *get_IsScriptEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsScriptEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsWebMessageEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsWebMessageEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDefaultScriptDialogsEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreDefaultScriptDialogsEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsStatusBarEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsStatusBarEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDevToolsEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreDevToolsEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDefaultContextMenusEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreDefaultContextMenusEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreHostObjectsAllowed)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreHostObjectsAllowed)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsZoomControlEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsZoomControlEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsBuiltInErrorPageEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsBuiltInErrorPageEnabled)(ICoreWebView2Settings*, BOOL);
} ICoreWebView2SettingsVtbl;
struct ICoreWebView2Settings { const ICoreWebView2SettingsVtbl *lpVtbl; };

/* ICoreWebView2WebMessageReceivedEventArgs vtable */
typedef struct ICoreWebView2WebMessageReceivedEventArgsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2WebMessageReceivedEventArgs*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2WebMessageReceivedEventArgs*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2WebMessageReceivedEventArgs*);
    HRESULT (STDMETHODCALLTYPE *get_Source)(ICoreWebView2WebMessageReceivedEventArgs*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *get_WebMessageAsJson)(ICoreWebView2WebMessageReceivedEventArgs*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *TryGetWebMessageAsString)(ICoreWebView2WebMessageReceivedEventArgs*, LPWSTR*);
} ICoreWebView2WebMessageReceivedEventArgsVtbl;
struct ICoreWebView2WebMessageReceivedEventArgs { const ICoreWebView2WebMessageReceivedEventArgsVtbl *lpVtbl; };

/* ── COM callback handler types ──────────────────────────────────────────── */

typedef struct EnvironmentCompletedHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*, HRESULT, ICoreWebView2Environment*);
} EnvironmentCompletedHandlerVtbl;

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    const EnvironmentCompletedHandlerVtbl *lpVtbl;
    ULONG refCount;
};

typedef struct ControllerCompletedHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, HRESULT, ICoreWebView2Controller*);
} ControllerCompletedHandlerVtbl;

struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    const ControllerCompletedHandlerVtbl *lpVtbl;
    ULONG refCount;
};

typedef struct WebMessageReceivedHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2WebMessageReceivedEventHandler*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2WebMessageReceivedEventHandler*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2WebMessageReceivedEventHandler*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(ICoreWebView2WebMessageReceivedEventHandler*, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*);
} WebMessageReceivedHandlerVtbl;

struct ICoreWebView2WebMessageReceivedEventHandler {
    const WebMessageReceivedHandlerVtbl *lpVtbl;
    ULONG refCount;
};

typedef HRESULT (STDAPICALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    LPCWSTR browserExecutableFolder, LPCWSTR userDataFolder, void* options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);
typedef HRESULT (STDAPICALLTYPE *PFN_GetAvailableCoreWebView2BrowserVersionString)(
    LPCWSTR browserExecutableFolder, LPWSTR* versionInfo);

/* ── Configuration ───────────────────────────────────────────────────────── */

typedef struct {
    BOOL startWithWindows;
    BOOL debugLogEnabled;
} Configuration;

/* ── Globals ─────────────────────────────────────────────────────────────── */

static HINSTANCE g_hInstance = NULL;
static HWND g_hwnd = NULL;              /* hidden top-level message window */
static HANDLE g_hMutex = NULL;
static NOTIFYICONDATAW g_nid;
static UINT g_WM_TASKBARCREATED = 0;
static Configuration g_config;
static volatile LONG g_debugLogEnabled = FALSE;

/* Config dialog state. */
static HWND g_cfgHwnd = NULL;
static ICoreWebView2Environment *g_cfgEnv = NULL;
static ICoreWebView2Controller *g_cfgController = NULL;
static ICoreWebView2 *g_cfgWebView = NULL;
static BOOL g_cfgWindowShown = FALSE;
static int g_cfgShowFallbackTries = 0;

static PFN_CreateCoreWebView2EnvironmentWithOptions fnCreateEnvironment = NULL;
static PFN_GetAvailableCoreWebView2BrowserVersionString fnGetAvailableBrowserVersion = NULL;
static WCHAR g_extractedDllPath[MAX_PATH] = {0};
static wchar_t g_webView2Version[128] = L"Unknown";

/* ── Forward declarations ────────────────────────────────────────────────── */

static void DebugPrint(const wchar_t* format, ...);
static BOOL LoadConfigFromRegistry(Configuration* config);
static BOOL SaveConfigToRegistry(const Configuration* config);
static BOOL IsStartupRegistered(void);
static BOOL SetStartupRegistration(BOOL enabled);
static BOOL load_webview2_loader(void);
static void ShowConfigDialog(void);
static void CreateTrayIcon(HWND hwnd);
static void RefreshTrayIcon(void);
static void RemoveTrayIcon(void);
static void ShowContextMenu(HWND hwnd);

/* ── Debug logging ───────────────────────────────────────────────────────── */

static void GetDebugLogPath(wchar_t path[MAX_PATH]) {
    path[0] = L'\0';
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        path[0] = L'\0';
        return;
    }
    PathAppendW(path, APP_NAME L"\\debug.log");
}

static void AppendDebugLogLine(const wchar_t* line) {
    wchar_t path[MAX_PATH];
    GetDebugLogPath(path);
    if (!path[0]) return;

    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, path);
    PathRemoveFileSpecW(dir);
    SHCreateDirectoryExW(NULL, dir, NULL);

    /* Cap growth: once per process, if the log has passed ~1 MB shift it to
     * debug.old.log (keeping one previous generation) before appending. */
    static BOOL rotationChecked = FALSE;
    if (!rotationChecked) {
        rotationChecked = TRUE;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) &&
            fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 1024 * 1024) {
            wchar_t oldPath[MAX_PATH];
            wcscpy_s(oldPath, MAX_PATH, dir);
            PathAppendW(oldPath, L"debug.old.log");
            MoveFileExW(path, oldPath, MOVEFILE_REPLACE_EXISTING);
        }
    }

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    size_t len = wcslen(line);
    if (len == 0 || line[len - 1] != L'\n') {
        fputwc(L'\n', f);
    }
    fclose(f);
}

/* Diagnostic output. Debug builds always emit to the debugger; release
 * builds only write when the debug log setting is enabled. */
static void DebugPrint(const wchar_t* format, ...) {
    BOOL logEnabled = InterlockedCompareExchange(&g_debugLogEnabled, TRUE, TRUE) == TRUE;
#ifndef _DEBUG
    if (!logEnabled) return;
#endif
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    vswprintf_s(buffer, sizeof(buffer) / sizeof(wchar_t), format, args);
    va_end(args);
#ifdef _DEBUG
    OutputDebugStringW(buffer);
#endif
    if (logEnabled) {
        AppendDebugLogLine(buffer);
    }
}

/* ── Registry configuration ──────────────────────────────────────────────── */

static BOOL LoadConfigFromRegistry(Configuration* config) {
    ZeroMemory(config, sizeof(*config));
    config->startWithWindows = IsStartupRegistered();

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return FALSE;
    }

    DWORD value = 0;
    DWORD dataSize = sizeof(value);
    DWORD dataType = 0;
    if (RegQueryValueExW(hKey, REG_VALUE_DEBUGLOG, NULL, &dataType,
                         (LPBYTE)&value, &dataSize) == ERROR_SUCCESS &&
        dataType == REG_DWORD) {
        config->debugLogEnabled = (value != 0);
    }

    RegCloseKey(hKey);
    return TRUE;
}

static BOOL SaveConfigToRegistry(const Configuration* config) {
    HKEY hKey;
    DWORD disposition;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
                        &disposition) != ERROR_SUCCESS) {
        return FALSE;
    }

    BOOL success = TRUE;
    DWORD debugLogVal = config->debugLogEnabled ? 1 : 0;
    if (RegSetValueExW(hKey, REG_VALUE_DEBUGLOG, 0, REG_DWORD,
                       (const BYTE*)&debugLogVal, sizeof(debugLogVal)) != ERROR_SUCCESS) {
        success = FALSE;
    }
    RegCloseKey(hKey);

    if (!SetStartupRegistration(config->startWithWindows)) {
        success = FALSE;
    }
    return success;
}

/* Quoted full path of the running executable, as stored in the Run key. */
static BOOL GetStartupCommand(wchar_t* command, size_t commandCount) {
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return FALSE;
    return swprintf_s(command, commandCount, L"\"%s\"", exePath) > 0;
}

static BOOL IsStartupRegistered(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return FALSE;
    }
    DWORD dataType = 0;
    LONG result = RegQueryValueExW(hKey, REG_RUN_VALUE_NAME, NULL, &dataType, NULL, NULL);
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS && dataType == REG_SZ;
}

/* Adds or removes the per-user startup entry. When enabling, the stored
 * command always points at the executable currently running, so a moved or
 * updated executable re-registers itself on the next save. */
static BOOL SetStartupRegistration(BOOL enabled) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey,
                        NULL) != ERROR_SUCCESS) {
        return FALSE;
    }

    LONG result;
    if (enabled) {
        wchar_t command[MAX_PATH + 2];
        if (!GetStartupCommand(command, sizeof(command) / sizeof(wchar_t))) {
            RegCloseKey(hKey);
            return FALSE;
        }
        result = RegSetValueExW(hKey, REG_RUN_VALUE_NAME, 0, REG_SZ,
                                (const BYTE*)command,
                                (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(hKey, REG_RUN_VALUE_NAME);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */

static BOOL json_get_string(const char *json, const char *key, char *out, size_t outLen) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return FALSE;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return FALSE;
    p++;
    size_t i = 0;
    while (*p && i < outLen - 1) {
        if (*p == '"') break;
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return TRUE;
}

static BOOL json_get_bool(const char *json, const char *key, BOOL defVal) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return defVal;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0) return TRUE;
    if (strncmp(p, "false", 5) == 0) return FALSE;
    if (*p == '1') return TRUE;
    if (*p == '0') return FALSE;
    return defVal;
}

/* Accepts both bare numbers and quoted numeric strings. */
static BOOL json_get_int(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return FALSE;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return FALSE;
    *out = atoi(p);
    return TRUE;
}

static void json_escape_wstring(const wchar_t *in, wchar_t *out, size_t outLen) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < outLen - 2; i++) {
        wchar_t c = in[i];
        if (c == L'"' || c == L'\\') {
            if (j + 2 >= outLen) break;
            out[j++] = L'\\';
            out[j++] = c;
        } else if (c == L'\n') {
            if (j + 2 >= outLen) break;
            out[j++] = L'\\';
            out[j++] = L'n';
        } else if (c == L'\r') {
            if (j + 2 >= outLen) break;
            out[j++] = L'\\';
            out[j++] = L'r';
        } else if (c == L'\t') {
            if (j + 2 >= outLen) break;
            out[j++] = L'\\';
            out[j++] = L't';
        } else if (c < 0x20) {
            /* Other control characters are not valid inside a JSON string. */
            continue;
        } else {
            out[j++] = c;
        }
    }
    out[j] = L'\0';
}

/* ── DPI ─────────────────────────────────────────────────────────────────── */

/* The process is per-monitor DPI aware (see NotTooBright.manifest), so
 * window pixels are physical pixels and anything sized from CSS pixels has
 * to be scaled by the window's DPI. GetDpiForWindow is resolved dynamically
 * (Windows 10 1607+); the GDI metric is the fallback and, for an aware
 * process, also returns the real DPI. */
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
static UINT GetWindowDpi(HWND hwnd) {
    static PFN_GetDpiForWindow fnGetDpiForWindow = NULL;
    static BOOL resolved = FALSE;
    if (!resolved) {
        fnGetDpiForWindow = (PFN_GetDpiForWindow)(void*)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        resolved = TRUE;
    }
    if (fnGetDpiForWindow && hwnd) {
        UINT dpi = fnGetDpiForWindow(hwnd);
        if (dpi) return dpi;
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = hdc ? (UINT)GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(hwnd, hdc);
    return dpi ? dpi : 96;
}

/* ── WebView2 loader ─────────────────────────────────────────────────────── */

/* WebView2Loader.dll is embedded as a resource and extracted to a per-app
 * temp folder on demand. If the file is locked because another instance is
 * still running, the existing copy is loaded instead. */
static BOOL load_webview2_loader(void) {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_WEBVIEW2_DLL), (LPCWSTR)RT_RCDATA);
    if (!hRes) return FALSE;
    HGLOBAL hData = LoadResource(NULL, hRes);
    DWORD dllSize = SizeofResource(NULL, hRes);
    const void *dllBytes = hData ? LockResource(hData) : NULL;
    if (!dllBytes || dllSize == 0) return FALSE;

    WCHAR tempDir[MAX_PATH];
    DWORD tempLen = GetTempPathW(MAX_PATH, tempDir);
    if (tempLen == 0 || tempLen >= MAX_PATH - 50) return FALSE;
    swprintf_s(g_extractedDllPath, MAX_PATH, L"%s%s", tempDir, APP_NAME);
    CreateDirectoryW(g_extractedDllPath, NULL);
    swprintf_s(g_extractedDllPath, MAX_PATH, L"%s%s\\WebView2Loader.dll", tempDir, APP_NAME);

    HANDLE hFile = CreateFileW(g_extractedDllPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, dllBytes, dllSize, &written, NULL);
        CloseHandle(hFile);
        if (written != dllSize) {
            DebugPrint(L"[WARNING] Incomplete WebView2Loader.dll write (%lu of %lu bytes)\n",
                       (unsigned long)written, (unsigned long)dllSize);
        }
    } else {
        DebugPrint(L"[INFO] WebView2Loader.dll is in use; loading the existing copy (error %lu)\n",
                   (unsigned long)GetLastError());
    }

    HMODULE hMod = LoadLibraryW(g_extractedDllPath);
    if (!hMod) {
        DebugPrint(L"[ERROR] LoadLibrary(WebView2Loader.dll) failed (error %lu)\n",
                   (unsigned long)GetLastError());
        return FALSE;
    }
    fnCreateEnvironment = (PFN_CreateCoreWebView2EnvironmentWithOptions)(void*)
        GetProcAddress(hMod, "CreateCoreWebView2EnvironmentWithOptions");
    fnGetAvailableBrowserVersion = (PFN_GetAvailableCoreWebView2BrowserVersionString)(void*)
        GetProcAddress(hMod, "GetAvailableCoreWebView2BrowserVersionString");
    return fnCreateEnvironment != NULL;
}

static void RefreshWebView2VersionString(void) {
    if (!fnGetAvailableBrowserVersion) return;

    LPWSTR versionString = NULL;
    if (SUCCEEDED(fnGetAvailableBrowserVersion(NULL, &versionString)) &&
        versionString && versionString[0] != L'\0') {
        wcscpy_s(g_webView2Version,
                 sizeof(g_webView2Version) / sizeof(wchar_t), versionString);
    }
    CoTaskMemFree(versionString);
}

static void ReportWebView2Unavailable(void) {
    MessageBoxW(NULL,
        L"Failed to load WebView2.\n\n"
        L"Please ensure the Microsoft Edge WebView2 Runtime is installed.\n"
        L"Download from: https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
        APP_NAME, MB_ICONERROR | MB_OK);
}

/* ── Config dialog WebView2 helpers ──────────────────────────────────────── */

static void webview_cfg_execute_script(const wchar_t* script) {
    if (g_cfgWebView && script) {
        g_cfgWebView->lpVtbl->ExecuteScript(g_cfgWebView, script, NULL);
    }
}

static void cfg_sync_controller_bounds(void) {
    if (!g_cfgController || !g_cfgHwnd) return;
    RECT bounds;
    GetClientRect(g_cfgHwnd, &bounds);
    g_cfgController->lpVtbl->put_Bounds(g_cfgController, bounds);
    g_cfgController->lpVtbl->put_IsVisible(g_cfgController, TRUE);
}

static void webview_push_init_config(void) {
    wchar_t eWebView2Version[256];
    json_escape_wstring(g_webView2Version, eWebView2Version,
                        sizeof(eWebView2Version) / sizeof(wchar_t));

    wchar_t script[1024];
    int written = swprintf_s(script, sizeof(script) / sizeof(wchar_t),
        L"window.onInit({\"config\":{\"startWithWindows\":%s,\"debugLog\":%s},"
        L"\"webView2Version\":\"%s\"})",
        g_config.startWithWindows ? L"true" : L"false",
        g_config.debugLogEnabled ? L"true" : L"false",
        eWebView2Version);
    if (written > 0) webview_cfg_execute_script(script);
}

/* A silent environment/controller failure would leave the fallback timer to
 * show a window with nothing inside it. Fail loudly and take the window
 * down instead. */
static void CfgReportInitFailureAndClose(HRESULT hr) {
    if (g_cfgHwnd) {
        KillTimer(g_cfgHwnd, ID_TIMER_CFG_SHOW_FALLBACK);
        PostMessageW(g_cfgHwnd, WM_CLOSE, 0, 0);
    }
    wchar_t msg[256];
    swprintf_s(msg, sizeof(msg) / sizeof(wchar_t),
        L"The configuration window could not initialize WebView2 (0x%08X).\n\n"
        L"Please check the Microsoft Edge WebView2 Runtime installation.",
        (unsigned)hr);
    MessageBoxW(NULL, msg, APP_NAME, MB_ICONERROR | MB_OK);
}

/* Content-driven sizing: the page reports its height (CSS pixels) whenever
 * its layout changes; the window is sized to fit, clamped to the work area,
 * re-centered, and shown on the first report. */
static void CfgApplyContentSize(int contentHeight, int contentWidth) {
    if (contentHeight <= 0 || !g_cfgHwnd) return;
    /* Content-driven sizing must not fight a maximized (or minimized)
     * window; WM_SIZE keeps the WebView bounds in sync there. */
    if (IsZoomed(g_cfgHwnd) || IsIconic(g_cfgHwnd)) return;

    int dpi = (int)GetWindowDpi(g_cfgHwnd);
    RECT clientRect = {0}, windowRect = {0};
    GetClientRect(g_cfgHwnd, &clientRect);
    GetWindowRect(g_cfgHwnd, &windowRect);
    int chromeW = (windowRect.right - windowRect.left) - (clientRect.right - clientRect.left);
    int chromeH = (windowRect.bottom - windowRect.top) - (clientRect.bottom - clientRect.top);
    int windowH = MulDiv(contentHeight, dpi, 96) + chromeH;
    int windowW = windowRect.right - windowRect.left;
    if (contentWidth > 0) windowW = MulDiv(contentWidth, dpi, 96) + chromeW;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    RECT work;
    HMONITOR mon = MonitorFromWindow(g_cfgHwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon || !GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    } else {
        work = mi.rcWork;
    }
    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    if (windowH > workH) windowH = workH;
    if (windowW > workW) windowW = workW;

    /* Always center on the measured height. The page's ResizeObserver
     * fires more than once (a short first measurement, then the real
     * height): re-centering every time keeps the dialog centered instead
     * of anchoring its top edge and letting later growth push it down. */
    int posX = work.left + (workW - windowW) / 2;
    int posY = work.top + (workH - windowH) / 2;
    UINT flags = SWP_NOZORDER;
    if (g_cfgWindowShown) {
        flags |= SWP_NOACTIVATE;
    } else {
        flags |= SWP_SHOWWINDOW;
        KillTimer(g_cfgHwnd, ID_TIMER_CFG_SHOW_FALLBACK);
    }
    SetWindowPos(g_cfgHwnd, NULL, posX, posY, windowW, windowH, flags);
    if (!g_cfgWindowShown) SetForegroundWindow(g_cfgHwnd);
    g_cfgWindowShown = TRUE;
    cfg_sync_controller_bounds();
}

/* ── Config dialog COM handlers ──────────────────────────────────────────── */

static HRESULT STDMETHODCALLTYPE CfgCtrlCompleted_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, HRESULT, ICoreWebView2Controller*);
static HRESULT STDMETHODCALLTYPE CfgMsgReceived_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler*, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*);

/* One reference-counted handler shape is shared by all three callbacks; the
 * vtables differ only in Invoke. */
static HRESULT STDMETHODCALLTYPE CfgHandler_QueryInterface(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, REFIID riid, void **ppv) {
    (void)riid;
    *ppv = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE CfgHandler_AddRef(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    return ++This->refCount;
}
static ULONG STDMETHODCALLTYPE CfgHandler_Release(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    ULONG rc = --This->refCount;
    if (rc == 0) free(This);
    return rc;
}

static HRESULT STDMETHODCALLTYPE CfgEnvCompleted_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT result, ICoreWebView2Environment *env) {
    (void)This;
    if (FAILED(result) || !env) {
        CfgReportInitFailureAndClose(FAILED(result) ? result : E_POINTER);
        return S_OK;
    }
    if (!g_cfgHwnd) return S_OK;  /* dialog closed while the environment was starting */
    g_cfgEnv = env;
    env->lpVtbl->AddRef(env);

    static ControllerCompletedHandlerVtbl ctrlVtbl = {0};
    static BOOL ctrlVtblInit = FALSE;
    if (!ctrlVtblInit) {
        ctrlVtbl.QueryInterface = (HRESULT (STDMETHODCALLTYPE *)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, REFIID, void**))CfgHandler_QueryInterface;
        ctrlVtbl.AddRef = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*))CfgHandler_AddRef;
        ctrlVtbl.Release = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*))CfgHandler_Release;
        ctrlVtbl.Invoke = CfgCtrlCompleted_Invoke;
        ctrlVtblInit = TRUE;
    }

    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler = malloc(sizeof(*handler));
    if (!handler) return E_OUTOFMEMORY;
    handler->lpVtbl = &ctrlVtbl;
    handler->refCount = 1;

    env->lpVtbl->CreateCoreWebView2Controller(env, g_cfgHwnd, handler);
    handler->lpVtbl->Release(handler);
    return S_OK;
}

static EnvironmentCompletedHandlerVtbl g_cfgEnvVtbl = {
    CfgHandler_QueryInterface,
    CfgHandler_AddRef,
    CfgHandler_Release,
    CfgEnvCompleted_Invoke
};

static HRESULT STDMETHODCALLTYPE CfgCtrlCompleted_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT result, ICoreWebView2Controller *controller) {
    (void)This;
    if (FAILED(result) || !controller) {
        CfgReportInitFailureAndClose(FAILED(result) ? result : E_POINTER);
        return S_OK;
    }
    if (!g_cfgHwnd) {
        controller->lpVtbl->Close(controller);
        return S_OK;
    }

    g_cfgController = controller;
    controller->lpVtbl->AddRef(controller);

    RECT bounds;
    GetClientRect(g_cfgHwnd, &bounds);
    controller->lpVtbl->put_Bounds(controller, bounds);
    controller->lpVtbl->put_IsVisible(controller, TRUE);

    ICoreWebView2 *webview = NULL;
    controller->lpVtbl->get_CoreWebView2(controller, &webview);
    if (!webview) {
        CfgReportInitFailureAndClose(E_FAIL);
        return E_FAIL;
    }
    g_cfgWebView = webview;

    ICoreWebView2Settings *settings = NULL;
    webview->lpVtbl->get_Settings(webview, &settings);
    if (settings) {
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings, FALSE);
        settings->lpVtbl->put_AreDevToolsEnabled(settings, FALSE);
        settings->lpVtbl->put_IsStatusBarEnabled(settings, FALSE);
        settings->lpVtbl->put_IsZoomControlEnabled(settings, FALSE);
        settings->lpVtbl->Release(settings);
    }

    static WebMessageReceivedHandlerVtbl msgVtbl = {0};
    static BOOL msgVtblInit = FALSE;
    if (!msgVtblInit) {
        msgVtbl.QueryInterface = (HRESULT (STDMETHODCALLTYPE *)(ICoreWebView2WebMessageReceivedEventHandler*, REFIID, void**))CfgHandler_QueryInterface;
        msgVtbl.AddRef = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2WebMessageReceivedEventHandler*))CfgHandler_AddRef;
        msgVtbl.Release = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2WebMessageReceivedEventHandler*))CfgHandler_Release;
        msgVtbl.Invoke = CfgMsgReceived_Invoke;
        msgVtblInit = TRUE;
    }

    ICoreWebView2WebMessageReceivedEventHandler *msgHandler = malloc(sizeof(*msgHandler));
    if (!msgHandler) return E_OUTOFMEMORY;
    msgHandler->lpVtbl = &msgVtbl;
    msgHandler->refCount = 1;

    EventRegistrationToken token;
    webview->lpVtbl->add_WebMessageReceived(webview, msgHandler, &token);
    msgHandler->lpVtbl->Release(msgHandler);

    /* Load the embedded UI. */
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_HTML_UI), (LPCWSTR)RT_RCDATA);
    if (hRes) {
        HGLOBAL hData = LoadResource(NULL, hRes);
        if (hData) {
            DWORD htmlSize = SizeofResource(NULL, hRes);
            const char *htmlUtf8 = (const char *)LockResource(hData);
            if (htmlUtf8 && htmlSize > 0) {
                int wLen = MultiByteToWideChar(CP_UTF8, 0, htmlUtf8, (int)htmlSize, NULL, 0);
                wchar_t *wHtml = malloc(((size_t)wLen + 1) * sizeof(wchar_t));
                if (wHtml) {
                    MultiByteToWideChar(CP_UTF8, 0, htmlUtf8, (int)htmlSize, wHtml, wLen);
                    wHtml[wLen] = L'\0';
                    webview->lpVtbl->NavigateToString(webview, wHtml);
                    free(wHtml);
                }
            }
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CfgMsgReceived_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender,
    ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)This; (void)sender;

    LPWSTR wMsg = NULL;
    args->lpVtbl->TryGetWebMessageAsString(args, &wMsg);
    if (!wMsg) return S_OK;

    int len = WideCharToMultiByte(CP_UTF8, 0, wMsg, -1, NULL, 0, NULL, NULL);
    char *msg = len > 0 ? malloc((size_t)len) : NULL;
    if (msg) WideCharToMultiByte(CP_UTF8, 0, wMsg, -1, msg, len, NULL, NULL);
    CoTaskMemFree(wMsg);
    if (!msg) return S_OK;

    char action[64] = {0};
    json_get_string(msg, "action", action, sizeof(action));

    if (strcmp(action, "getInit") == 0) {
        webview_push_init_config();
    } else if (strcmp(action, "saveSettings") == 0) {
        g_config.startWithWindows = json_get_bool(msg, "startWithWindows", FALSE);
        g_config.debugLogEnabled = json_get_bool(msg, "debugLog", FALSE);
        InterlockedExchange(&g_debugLogEnabled, g_config.debugLogEnabled ? TRUE : FALSE);
        if (!SaveConfigToRegistry(&g_config)) {
            DebugPrint(L"[WARNING] Could not save all settings to the registry\n");
            MessageBoxW(g_cfgHwnd,
                L"Some settings could not be saved. Check that the current user "
                L"can write to HKEY_CURRENT_USER.",
                APP_NAME, MB_ICONWARNING | MB_OK);
        }
        DebugPrint(L"[INFO] Settings saved (startWithWindows=%d, debugLog=%d)\n",
                   g_config.startWithWindows, g_config.debugLogEnabled);
        PostMessageW(g_cfgHwnd, WM_CLOSE, 0, 0);
    } else if (strcmp(action, "close") == 0) {
        PostMessageW(g_cfgHwnd, WM_CLOSE, 0, 0);
    } else if (strcmp(action, "resize") == 0) {
        int contentHeight = 0, contentWidth = 0;
        json_get_int(msg, "height", &contentHeight);
        json_get_int(msg, "width", &contentWidth);
        CfgApplyContentSize(contentHeight, contentWidth);
    }

    free(msg);
    return S_OK;
}

/* ── Config dialog window ────────────────────────────────────────────────── */

static LRESULT CALLBACK CfgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            cfg_sync_controller_bounds();
            return 0;

        case WM_DPICHANGED: {
            const RECT* suggested = (const RECT*)lParam;
            SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_TIMER:
            if (wParam == ID_TIMER_CFG_SHOW_FALLBACK) {
                if (g_cfgWindowShown) {
                    KillTimer(hwnd, ID_TIMER_CFG_SHOW_FALLBACK);
                    return 0;
                }
                if (g_cfgController) {
                    /* Content exists but the resize message never came:
                     * show the window at its default size. */
                    KillTimer(hwnd, ID_TIMER_CFG_SHOW_FALLBACK);
                    ShowWindow(hwnd, SW_SHOW);
                    UpdateWindow(hwnd);
                    g_cfgWindowShown = TRUE;
                    cfg_sync_controller_bounds();
                } else if (++g_cfgShowFallbackTries >= CFG_SHOW_FALLBACK_MAX_TRIES) {
                    /* WebView2 creation neither completed nor reported
                     * failure; never present an empty window. */
                    KillTimer(hwnd, ID_TIMER_CFG_SHOW_FALLBACK);
                    CfgReportInitFailureAndClose(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                }
                /* Otherwise keep waiting: the periodic timer fires again. */
                return 0;
            }
            break;

        case WM_CLOSE:
            g_cfgWindowShown = FALSE;
            KillTimer(hwnd, ID_TIMER_CFG_SHOW_FALLBACK);
            if (g_cfgController) {
                g_cfgController->lpVtbl->Close(g_cfgController);
                g_cfgController->lpVtbl->Release(g_cfgController);
                g_cfgController = NULL;
            }
            if (g_cfgWebView) {
                g_cfgWebView->lpVtbl->Release(g_cfgWebView);
                g_cfgWebView = NULL;
            }
            if (g_cfgEnv) {
                g_cfgEnv->lpVtbl->Release(g_cfgEnv);
                g_cfgEnv = NULL;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_cfgHwnd = NULL;
            g_cfgWindowShown = FALSE;
            KillTimer(hwnd, ID_TIMER_CFG_SHOW_FALLBACK);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowConfigDialog(void) {
    if (g_cfgHwnd != NULL) {
        if (IsIconic(g_cfgHwnd)) ShowWindow(g_cfgHwnd, SW_RESTORE);
        else if (g_cfgWindowShown) ShowWindow(g_cfgHwnd, SW_SHOW);
        SetForegroundWindow(g_cfgHwnd);
        return;
    }

    if (!fnCreateEnvironment && !load_webview2_loader()) {
        ReportWebView2Unavailable();
        return;
    }

    static BOOL classRegistered = FALSE;
    if (!classRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CfgWndProc;
        wc.hInstance = g_hInstance;
        wc.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON));
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"NotTooBrightCfgWnd";
        wc.hIconSm = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR);
        RegisterClassExW(&wc);
        classRegistered = TRUE;
    }

    /* Center the initial (hidden) window in the primary work area; the
     * resize message from the page re-centers it at its real height before
     * it is shown, and clamps it to the work area of whatever monitor it
     * lands on. */
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int dpi = (int)GetWindowDpi(NULL);
    int width = MulDiv(CFG_INITIAL_WIDTH, dpi, 96);
    int height = MulDiv(CFG_INITIAL_HEIGHT, dpi, 96);
    if (height > workArea.bottom - workArea.top) height = workArea.bottom - workArea.top;
    int posX = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    int posY = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;

    g_cfgHwnd = CreateWindowExW(0, L"NotTooBrightCfgWnd", L"Configuration",
        WS_OVERLAPPEDWINDOW,
        posX, posY, width, height,
        NULL, NULL, g_hInstance, NULL);
    if (!g_cfgHwnd) {
        DebugPrint(L"[ERROR] Failed to create the configuration window (error %lu)\n",
                   (unsigned long)GetLastError());
        return;
    }
    g_cfgWindowShown = FALSE;
    g_cfgShowFallbackTries = 0;
    SetTimer(g_cfgHwnd, ID_TIMER_CFG_SHOW_FALLBACK, CFG_SHOW_FALLBACK_DELAY_MS, NULL);

    /* WebView2 needs a writable profile folder; keep it in the user's temp
     * directory so nothing persists in the application folder. */
    WCHAR userDataFolder[MAX_PATH];
    DWORD tempLen = GetTempPathW(MAX_PATH, userDataFolder);
    if (tempLen > 0 && tempLen < MAX_PATH - 30) {
        wcscat_s(userDataFolder, MAX_PATH, APP_NAME L".WebView2");
    } else {
        userDataFolder[0] = L'\0';
    }

    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *envHandler = malloc(sizeof(*envHandler));
    if (!envHandler) {
        DestroyWindow(g_cfgHwnd);
        g_cfgHwnd = NULL;
        return;
    }
    envHandler->lpVtbl = &g_cfgEnvVtbl;
    envHandler->refCount = 1;

    HRESULT hr = fnCreateEnvironment(NULL, userDataFolder[0] ? userDataFolder : NULL,
                                     NULL, envHandler);
    envHandler->lpVtbl->Release(envHandler);

    if (FAILED(hr)) {
        DebugPrint(L"[ERROR] CreateCoreWebView2EnvironmentWithOptions failed (0x%08X)\n",
                   (unsigned)hr);
        DestroyWindow(g_cfgHwnd);
        g_cfgHwnd = NULL;
        ReportWebView2Unavailable();
    }
}

/* ── Tray icon ───────────────────────────────────────────────────────────── */

static void CreateTrayIcon(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_ICON_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    /* SM_CXSMICON already reflects the system DPI, so the shell gets an
     * icon rendered at the size it will actually display. */
    int iconSize = GetSystemMetrics(SM_CXSMICON);
    if (iconSize <= 0) iconSize = 16;
    g_nid.hIcon = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON),
                                    IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    }

    wcscpy_s(g_nid.szTip, sizeof(g_nid.szTip) / sizeof(wchar_t), APP_DISPLAY_NAME_WSTRING);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    DebugPrint(L"[INFO] Tray icon created (%dx%d)\n", iconSize, iconSize);
}

static void RemoveTrayIcon(void) {
    if (!g_nid.hWnd) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = NULL;
    }
}

/* Explorer restarted (TaskbarCreated) or the DPI changed: re-add the icon. */
static void RefreshTrayIcon(void) {
    HWND hwnd = g_nid.hWnd;
    if (!hwnd) return;
    RemoveTrayIcon();
    CreateTrayIcon(hwnd);
    DebugPrint(L"[INFO] Tray icon refreshed\n");
}

static void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MENU_CONFIGURE, L"Configure");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MENU_EXIT, L"Exit");
    /* Bold the item that a tray double-click also triggers. */
    SetMenuDefaultItem(hMenu, ID_TRAY_MENU_CONFIGURE, FALSE);

    /* Documented requirement for tray menus (KB135788): the window must be
     * foreground for the menu to dismiss when the user clicks outside it,
     * and a WM_NULL afterwards forces the task switch. */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

/* ── Main (hidden) window ────────────────────────────────────────────────── */

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TRAYICON:
            switch (lParam) {
                case WM_LBUTTONDBLCLK:
                    ShowConfigDialog();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowContextMenu(hwnd);
                    break;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_MENU_CONFIGURE:
                    ShowConfigDialog();
                    return 0;
                case ID_TRAY_MENU_EXIT:
                    DebugPrint(L"[INFO] Exit selected from the tray menu\n");
                    if (g_cfgHwnd) SendMessageW(g_cfgHwnd, WM_CLOSE, 0, 0);
                    RemoveTrayIcon();
                    PostQuitMessage(0);
                    return 0;
            }
            break;

        case WM_DPICHANGED:
            RefreshTrayIcon();
            return 0;

        case WM_CLOSE:
            /* The hidden window is never closed by the user; keep running. */
            return 0;

        default:
            if (g_WM_TASKBARCREATED != 0 && uMsg == g_WM_TASKBARCREATED) {
                RefreshTrayIcon();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    g_hInstance = hInstance;

    /* Single instance check. A relaunch can arrive while the previous
     * process is still shutting down, so retry briefly before declaring
     * another instance is running. */
    g_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    DWORD mutexStatus = g_hMutex ? GetLastError() : ERROR_SUCCESS;
    for (int attempt = 0;
         g_hMutex && mutexStatus == ERROR_ALREADY_EXISTS && attempt < 10;
         attempt++) {
        CloseHandle(g_hMutex);
        Sleep(250);
        g_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
        mutexStatus = g_hMutex ? GetLastError() : ERROR_SUCCESS;
    }
    if (g_hMutex && mutexStatus == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_hMutex);
        g_hMutex = NULL;
        MessageBoxW(NULL,
            L"NotTooBright is already running.\n\nCheck your system tray for the application icon.",
            L"Already Running", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"COM initialization failed", APP_NAME, MB_OK | MB_ICONERROR);
        if (g_hMutex) { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); }
        return 1;
    }

    LoadConfigFromRegistry(&g_config);
    InterlockedExchange(&g_debugLogEnabled, g_config.debugLogEnabled ? TRUE : FALSE);
    DebugPrint(L"[INFO] " APP_DISPLAY_NAME_WSTRING L" " APP_VERSION_WSTRING L" starting\n");

    /* Keep the startup entry pointing at wherever the executable lives now. */
    if (g_config.startWithWindows && !SetStartupRegistration(TRUE)) {
        DebugPrint(L"[WARNING] Could not refresh the startup registration\n");
    }

    /* The loader is only needed for the configuration dialog; resolving it
     * up front makes the WebView2 version available and surfaces a missing
     * runtime immediately rather than on first use. */
    if (load_webview2_loader()) {
        RefreshWebView2VersionString();
    } else {
        DebugPrint(L"[WARNING] WebView2 loader unavailable; the configuration dialog will not open\n");
    }

    /* A hidden top-level window (not a message-only window) so that
     * broadcasts such as TaskbarCreated and display/power changes arrive. */
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"NotTooBrightMainClass";
    wc.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class", APP_NAME, MB_OK | MB_ICONERROR);
        CoUninitialize();
        if (g_hMutex) { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); }
        return 1;
    }

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"NotTooBrightMainClass", APP_NAME,
                             WS_OVERLAPPED, 0, 0, 0, 0,
                             NULL, NULL, hInstance, NULL);
    if (!g_hwnd) {
        MessageBoxW(NULL, L"Failed to create window", APP_NAME, MB_OK | MB_ICONERROR);
        CoUninitialize();
        if (g_hMutex) { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); }
        return 1;
    }

    /* Register for Explorer restart notifications. If this process happens
     * to run elevated, allow the (unelevated) shell's broadcast through UIPI. */
    g_WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_WM_TASKBARCREATED != 0) {
        typedef BOOL (WINAPI *ChangeWindowMessageFilterExFn)(HWND, UINT, DWORD, PCHANGEFILTERSTRUCT);
        ChangeWindowMessageFilterExFn changeWindowMessageFilterEx =
            (ChangeWindowMessageFilterExFn)(void*)GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx");
        if (changeWindowMessageFilterEx) {
            changeWindowMessageFilterEx(g_hwnd, g_WM_TASKBARCREATED, MSGFLT_ALLOW, NULL);
        }
    }

    CreateTrayIcon(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DebugPrint(L"[INFO] Exiting\n");
    RemoveTrayIcon();
    CoUninitialize();
    if (g_hwnd) DestroyWindow(g_hwnd);
    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
    }
    return (int)msg.wParam;
}

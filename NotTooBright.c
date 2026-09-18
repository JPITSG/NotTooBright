/*
 * NotTooBright - NotTooBright.c
 *
 * System tray utility for lowering the brightness of desktop monitors from
 * software, for displays that offer no native Windows brightness control.
 *
 * How brightness is controlled:
 *   - Hardware first: DDC/CI through the Windows Monitor Configuration API
 *     (dxva2), which drives the monitor's own backlight exactly like its
 *     on-screen menu. All DDC/CI traffic runs on a worker thread because a
 *     single command can block for hundreds of milliseconds.
 *   - Software fallback: a click-through, per-monitor black overlay window
 *     with adjustable alpha, used for monitors that do not answer DDC/CI,
 *     when the user asks for it, or to dim below the backlight's minimum.
 *   - One brightness value per monitor, remembered per monitor (identified
 *     by its EDID) and re-applied after sleep, monitor power cycles, and
 *     display changes.
 *
 * Other parts:
 *   - System tray icon with a context menu (Configure / Exit)
 *   - WebView2-hosted configuration dialog (React UI embedded as a resource)
 *   - Registry-persisted settings
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
#include <dbt.h>
#include <sddl.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <winver.h>
#include <userenv.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "resource.h"
#include "version.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define APP_NAME L"NotTooBright"
#define MUTEX_NAME L"NotTooBright_SingleInstance_Mutex_6D1F2B3A_7C4E_4F89_9A21_5E0B8D7C3F14"

/* Registry settings (per user). */
#define REG_KEY_PATH L"SOFTWARE\\JPIT\\NotTooBright"
#define REG_VALUE_DEBUGLOG L"DebugLog"
#define REG_VALUE_ALLOW_BELOW_MIN L"AllowBelowHardwareMinimum"
/* Per-monitor settings live in a subkey named after the monitor's identity. */
#define REG_MONITORS_SUBKEY L"Monitors"
#define REG_VALUE_MON_BRIGHTNESS L"Brightness"
#define REG_VALUE_MON_SOFTWARE_ONLY L"SoftwareOnly"
#define REG_VALUE_MON_HIDDEN L"Hidden"
/* Set once a monitor has answered DDC/CI; such a monitor is never dimmed in
 * software behind the user's back (its backlight may sit below 100%). */
#define REG_VALUE_MON_HARDWARE L"HardwareControl"
/* The DDC/CI value the monitor reported the very first time it was seen,
 * before anything was written to it; restored when the monitor is hidden. */
#define REG_VALUE_MON_ORIGINAL L"OriginalBrightness"
#define REG_VALUE_MON_ORIGINAL_MAX L"OriginalBrightnessMax"
#define REG_VALUE_MON_SCHEDULED L"Scheduled"
#define REG_VALUE_MON_PAUSED_UNTIL L"SchedulePausedUntil"   /* REG_QWORD, UTC FILETIME */
/* Sun-based automatic brightness. */
#define REG_VALUE_SCHEDULE_ENABLED L"ScheduleEnabled"
#define REG_VALUE_LATITUDE L"Latitude"                        /* REG_SZ, decimal degrees */
#define REG_VALUE_LONGITUDE L"Longitude"
#define REG_VALUE_SCHEDULE_DAY L"ScheduleDayLevel"
#define REG_VALUE_SCHEDULE_NIGHT L"ScheduleNightLevel"
#define REG_VALUE_SCHEDULE_DAWN_START L"ScheduleDawnStartOffset"
#define REG_VALUE_SCHEDULE_DAWN_END L"ScheduleDawnEndOffset"
#define REG_VALUE_SCHEDULE_DUSK_START L"ScheduleDuskStartOffset"
#define REG_VALUE_SCHEDULE_DUSK_END L"ScheduleDuskEndOffset"
#define REG_VALUE_CYCLE_RESET L"CycleResetMinutes"
#define REG_VALUE_MON_NAME L"Name"

/* Self update: the repository's release binary is downloaded, its embedded
 * file version compared with the running one, and a short-lived elevated
 * copy of the executable swaps the file and restarts the application. */
#define UPDATE_URL L"https://github.com/JPITSG/NotTooBright/raw/refs/heads/main/releases/NotTooBright.exe"
#define UPDATE_MAX_BYTES (100ULL * 1024ULL * 1024ULL)
#define UPDATE_PROGRESS_INTERVAL_MS 250
#define UPDATE_HELPER_READY_MS 10000
#define UPDATE_HELPER_WAIT_MS 120000
#define REG_VALUE_AUTO_UPDATE L"AutoCheckForUpdates"
#define REG_VALUE_IGNORED_UPDATE_VERSION L"IgnoredUpdateVersion"
#define ID_TIMER_AUTO_UPDATE 8
#define AUTO_UPDATE_INTERVAL_MS (60u * 60u * 1000u)

#define TRAY_ICON_ID 100
#define WM_TRAYICON (WM_APP + 1)
/* Posted by the DDC/CI worker thread. */
#define WM_APP_DDC_PROBED (WM_APP + 2)      /* lParam: DdcProbeResult* (receiver frees) */
#define WM_APP_DDC_SET_RESULT (WM_APP + 3)  /* wParam: uid, lParam: success */
/* Posted by the update check thread. */
#define WM_APP_UPDATE_RESULT (WM_APP + 4)
#define WM_APP_UPDATE_PROGRESS (WM_APP + 5)
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

/* Monitor re-enumeration is debounced: display changes arrive in bursts,
 * and monitors need a moment after resume/power-on before DDC/CI answers. */
#define ID_TIMER_REFRESH_MONITORS 2
#define REFRESH_MONITORS_DEBOUNCE_MS 1500
#define REFRESH_MONITORS_RESUME_DELAY_MS 3000
/* Other topmost windows can end up above a dimming overlay; this timer
 * periodically checks and re-raises the overlays. */
#define ID_TIMER_OVERLAY_TOPMOST 3
#define OVERLAY_TOPMOST_INTERVAL_MS 1500
/* Brightness changes stream in while a slider is dragged; registry writes
 * are batched behind this delay. */
#define ID_TIMER_PERSIST 4
#define PERSIST_DEBOUNCE_MS 500

#define MAX_MONITORS 16
#define MAX_PHYSICAL_PER_DISPLAY 4
#define VCP_BRIGHTNESS 0x10
/* Software dimming never takes the apparent brightness below this, so a
 * screen can never be dimmed to black. */
#define SOFT_MIN_BRIGHTNESS 10
#define SOFT_MAX_DIM (100 - SOFT_MIN_BRIGHTNESS)
/* After this many consecutive failed DDC/CI writes a monitor is treated as
 * not answering: software-only if it never worked, otherwise left alone and
 * re-probed with a growing delay. */
#define DDC_MAX_CONSECUTIVE_FAILURES 3
#define ID_TIMER_DDC_RETRY 7
#define DDC_RETRY_INITIAL_MS 3000
#define DDC_RETRY_MAX_MS 60000
/* Probe attempts per monitor and the pauses between them: DDC/CI often
 * drops the first request after a handle is opened or a display change. */
#define DDC_PROBE_ATTEMPTS 4
#define DDC_PROBE_ATTEMPTS_UNKNOWN 3
#define DDC_HANDLE_SETTLE_MS 100
#define DDC_REOPEN_PAUSE_MS 1500
/* A monitor that has never answered is re-probed this many times after a
 * failure (a flaky first probe must not lock it into software mode); one
 * that has answered before is retried for as long as it takes. */
#define DDC_UNKNOWN_MONITOR_RETRIES 3

/* The schedule is evaluated on this timer while enabled; values only
 * change by whole percents, so this is plenty for a slow transition. */
#define ID_TIMER_SCHEDULE 5
#define SCHEDULE_INTERVAL_MS 30000
#define SCHEDULE_DEFAULT_DAY 100
#define SCHEDULE_DEFAULT_NIGHT 30
/* Transition anchors are minutes relative to sunrise (dawn) and sunset
 * (dusk); the defaults cover roughly civil twilight. */
#define SCHEDULE_DEFAULT_DAWN_START (-30)
#define SCHEDULE_DEFAULT_DAWN_END 30
#define SCHEDULE_DEFAULT_DUSK_START (-30)
#define SCHEDULE_DEFAULT_DUSK_END 30
#define SCHEDULE_MAX_OFFSET (6 * 60)
#define SCHEDULE_MIN_GAP 5
#define SCHEDULE_DEFAULT_RESET_MINUTES (4 * 60)

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

/* ── Configuration and monitor model ─────────────────────────────────────── */

/* Sun-based automatic brightness: the day/night levels and four transition
 * anchors (minutes relative to sunrise and sunset) that shape the curve. */
typedef struct {
    BOOL enabled;
    BOOL hasLocation;
    double latitude;          /* degrees, north positive */
    double longitude;         /* degrees, east positive */
    int dayLevel;             /* -SOFT_MAX_DIM..100 */
    int nightLevel;
    int dawnStartOffset;      /* night level until here (sunrise + offset) */
    int dawnEndOffset;        /* day level from here (sunrise + offset) */
    int duskStartOffset;      /* day level until here (sunset + offset) */
    int duskEndOffset;        /* night level from here (sunset + offset) */
    int cycleResetMinutes;    /* time of day when manual overrides expire */
} Schedule;

typedef struct {
    BOOL allowBelowMinimum;   /* let hardware monitors dim further in software */
    BOOL debugLogEnabled;
    BOOL autoCheckForUpdates;
    Schedule schedule;
} Configuration;

typedef struct {
    WORD major;
    WORD minor;
    WORD patch;
    WORD build;
} ExecutableVersion;

typedef enum {
    UPDATE_CHECK_SAME = 1,
    UPDATE_CHECK_NEWER,
    UPDATE_CHECK_OLDER,
    UPDATE_CHECK_CANCELLED,
    UPDATE_CHECK_ERROR
} UpdateCheckKind;

typedef struct {
    HWND targetWindow;
    BOOL automatic;
    UpdateCheckKind kind;
    ULONGLONG cacheBuster;
    ExecutableVersion runningVersion;
    ExecutableVersion availableVersion;
    wchar_t message[512];
    wchar_t targetPath[MAX_PATH];
    wchar_t stagedPath[MAX_PATH];
} UpdateCheckTask;

/* Sunrise, sunset, and solar noon for one local date, in local minutes
 * since midnight (may fall outside 0..1439 near the date line). */
typedef struct {
    int polar;                /* 0 normal, 1 sun never sets, -1 sun never rises */
    int sunrise;
    int sunset;
    int noon;
} SolarDay;

typedef enum {
    HW_UNKNOWN = 0,   /* probe in flight */
    HW_AVAILABLE,     /* monitor answers VCP 0x10 */
    HW_UNAVAILABLE    /* no DDC/CI brightness: software dimming only */
} HardwareState;

typedef enum {
    MODE_PROBING = 0,
    MODE_HARDWARE,
    MODE_SOFTWARE,
    MODE_WAITING      /* known DDC/CI monitor not answering: left alone, retried */
} BrightnessMode;

/* One entry per physical monitor. A display (HMONITOR) normally has exactly
 * one; in clone mode it can have several, which then share one overlay. */
typedef struct {
    int uid;                      /* process-unique id used across threads */
    wchar_t key[128];             /* persistent identity (EDID based) */
    wchar_t name[64];             /* friendly name from the EDID */
    wchar_t device[CCHDEVICENAME];/* \\.\DISPLAYn */
    HMONITOR hmon;
    int physicalIndex;
    RECT rect;
    BOOL primary;
    HardwareState hardwareState;
    BOOL knownHardware;           /* has answered DDC/CI at some point (persisted) */
    int probeFailures;            /* consecutive failed probes in this outage */
    DWORD ddcMax;
    DWORD ddcMin;                 /* non-zero only on the high-level route */
    DWORD ddcCurrent;
    BOOL ddcHighLevel;            /* GetMonitorBrightness/SetMonitorBrightness route */
    BOOL forceSoftware;           /* user asked for software dimming only */
    BOOL hidden;                  /* removed from the dialog and left alone until a rescan */
    BOOL hasOriginal;             /* original DDC/CI value recorded */
    DWORD originalRaw;            /* raw VCP value at first sighting */
    DWORD originalMax;            /* its maximum, for display as a percent */
    BOOL scheduled;               /* follows the automatic brightness schedule */
    ULONGLONG pausedUntil;        /* UTC FILETIME; automation resumes after a manual change */
    int value;                    /* desired brightness, -SOFT_MAX_DIM..100 */
    BOOL hasValue;
    int lastHwSent;               /* last percent handed to the worker, -1 = none */
    int failures;                 /* consecutive failed DDC/CI writes */
    HWND overlay;                 /* software dimming window, first entry only */
    int overlayDim;               /* current overlay dim 0..SOFT_MAX_DIM */
    BOOL dirty;                   /* needs persisting */
    wchar_t error[160];           /* user-facing status, empty when fine */
} Monitor;

/* Main-to-worker probe request: which physical monitors to open and query.
 * patient: the monitor has answered before, so it is worth the long retry
 * sequence including a handle reopen. */
typedef struct {
    int uid;
    HMONITOR hmon;
    int physicalIndex;
    BOOL patient;
} DdcProbeEntry;

/* Main-to-worker brightness request; one slot per monitor so a burst of
 * slider moves collapses into the latest value. */
typedef struct {
    int uid;
    int target;     /* percent 0..100, or a raw VCP value when raw is set */
    BOOL raw;
    BOOL pending;
} DdcSetSlot;

/* Worker-to-main probe result. highLevel: the raw VCP read failed but the
 * high-level GetMonitorBrightness/SetMonitorBrightness route works. */
typedef struct {
    int uid;
    BOOL supported;
    BOOL highLevel;
    DWORD min;
    DWORD current;
    DWORD max;
    DWORD error;
    DWORD elapsedMs;
} DdcProbeResult;

/* ── Globals ─────────────────────────────────────────────────────────────── */

static HINSTANCE g_hInstance = NULL;
static HWND g_hwnd = NULL;              /* hidden top-level message window */
static HANDLE g_hMutex = NULL;
static NOTIFYICONDATAW g_nid;
static UINT g_WM_TASKBARCREATED = 0;
static Configuration g_config;
static volatile LONG g_debugLogEnabled = FALSE;
static CRITICAL_SECTION g_logLock;

static Monitor g_monitors[MAX_MONITORS];
static int g_monitorCount = 0;
static int g_nextUid = 1;
static HPOWERNOTIFY g_displayStateNotify = NULL;
static LONG g_lastDisplayState = -1;
static UINT g_ddcRetryDelayMs = DDC_RETRY_INITIAL_MS;
static BOOL g_ddcRetryPending = FALSE;

/* DDC/CI worker thread and its shared request state (guarded by g_ddcLock). */
static CRITICAL_SECTION g_ddcLock;
static HANDLE g_ddcEvent = NULL;
static HANDLE g_ddcThread = NULL;
static volatile LONG g_ddcStop = 0;
static BOOL g_ddcProbeRequested = FALSE;
static DdcProbeEntry g_ddcProbeList[MAX_MONITORS];
static int g_ddcProbeCount = 0;
static DdcSetSlot g_ddcSets[MAX_MONITORS];
static int g_ddcSetCount = 0;

/* Self update state. */
static BOOL g_configViewReady = FALSE;
static BOOL g_updateConfirmationPending = FALSE;
static volatile LONG g_updateCheckPending = FALSE;
static volatile LONG g_updateCheckAutomatic = FALSE;
static BOOL g_updateInstallReady = FALSE;
static volatile LONG g_updateRequestSequence = 0;
static HANDLE g_updateCancelEvent = NULL;
static volatile LONG g_updateSpeedKbps = 0;
static volatile LONG g_updateProgressPosted = FALSE;
static UpdateCheckTask* volatile g_updatePostedResult = NULL;
static UpdateCheckTask* g_updateNoticeTask = NULL;
static UpdateCheckTask* g_updateReadyTask = NULL;
static wchar_t g_ignoredUpdateVersion[32] = L"";

/* Config dialog state. */
static HWND g_cfgHwnd = NULL;
static ICoreWebView2Environment *g_cfgEnv = NULL;
static ICoreWebView2Controller *g_cfgController = NULL;
static ICoreWebView2 *g_cfgWebView = NULL;
static BOOL g_cfgWindowShown = FALSE;
static int g_cfgShowFallbackTries = 0;

static PFN_CreateCoreWebView2EnvironmentWithOptions fnCreateEnvironment = NULL;
static WCHAR g_extractedDllPath[MAX_PATH] = {0};

/* GUID_CONSOLE_DISPLAY_STATE, declared locally so no GUID library is needed. */
static const GUID kGuidConsoleDisplayState =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

/* ── Forward declarations ────────────────────────────────────────────────── */

static void DebugPrint(const wchar_t* format, ...);
static BOOL LoadConfigFromRegistry(Configuration* config);
static BOOL SaveConfigToRegistry(const Configuration* config);
static void LoadMonitorSettings(Monitor* m);
static void SaveMonitorSettings(const Monitor* m);
static Monitor* FindMonitorByUid(int uid);
static BrightnessMode MonitorMode(const Monitor* m);
static const wchar_t* ModeName(BrightnessMode mode);
static const wchar_t* HardwareStateName(HardwareState state);
static int MonitorMinValue(const Monitor* m);
static void ApplyMonitor(Monitor* m);
static void SetMonitorValue(Monitor* m, int value);
static void RefreshMonitors(void);
static void ScheduleMonitorRefresh(UINT delayMs);
static void ScheduleDdcRetry(void);
static void SchedulePersist(void);
static void PersistDirtyMonitors(void);
static int VisibleMonitorCount(void);
static void UnhideAllMonitors(void);
static void DdcRequestSet(int uid, int percent);
static void DdcRequestProbe(const DdcProbeEntry* entries, int count);
static BOOL load_webview2_loader(void);
static void ShowConfigDialog(void);
static void webview_cfg_execute_script(const wchar_t* script);
static void StartUpdateCheck(BOOL automatic);
static void CancelUpdateCheck(void);
static void InstallPreparedUpdate(BOOL reopenSettings);
static void DiscardPreparedUpdate(void);
static void DiscardPendingUpdateNotice(void);
static void DiscardUpdateTask(UpdateCheckTask* task);
static void PresentPendingUpdateNotice(void);
static void HandleCompletedUpdateCheck(UpdateCheckTask* task);
static void IgnorePreparedUpdateVersion(const char* requestedVersion);
static void CfgSendUpdateProgress(DWORD speedKbps);
static int HandleUpdateCommandLine(BOOL* handled, BOOL* updateCompleted, BOOL* reopenSettings);
static void PushMonitorsToDialog(void);
static void CreateTrayIcon(HWND hwnd);
static void RefreshTrayIcon(void);
static void RemoveTrayIcon(void);
static void ShowContextMenu(HWND hwnd);
static BOOL ScheduleTargetNow(int* value);
static void EvaluateSchedule(void);
static void NoteManualChange(Monitor* m);
static void UpdateScheduleTimer(void);
static ULONGLONG NowFileTime(void);
static BOOL IsSchedulePaused(const Monitor* m, ULONGLONG nowFt);
static void FormatLocalTimeOfDay(ULONGLONG ft, wchar_t* out, size_t count);

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

    /* The DDC/CI worker logs too; serialize appends so lines never interleave. */
    EnterCriticalSection(&g_logLock);
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f) {
        LeaveCriticalSection(&g_logLock);
        return;
    }
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
    LeaveCriticalSection(&g_logLock);
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

static BOOL ReadRegistryBool(HKEY hKey, const wchar_t* name, BOOL* out) {
    DWORD value = 0;
    DWORD dataSize = sizeof(value);
    DWORD dataType = 0;
    if (RegQueryValueExW(hKey, name, NULL, &dataType, (LPBYTE)&value, &dataSize) != ERROR_SUCCESS ||
        dataType != REG_DWORD) {
        return FALSE;
    }
    *out = (value != 0);
    return TRUE;
}

static BOOL WriteRegistryDword(HKEY hKey, const wchar_t* name, DWORD value) {
    return RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&value,
                          sizeof(value)) == ERROR_SUCCESS;
}

/* Signed values are stored as two's complement DWORDs. */
static BOOL ReadRegistryInt(HKEY hKey, const wchar_t* name, int* out, int min, int max) {
    DWORD value = 0;
    DWORD dataSize = sizeof(value);
    DWORD dataType = 0;
    if (RegQueryValueExW(hKey, name, NULL, &dataType, (LPBYTE)&value, &dataSize) != ERROR_SUCCESS ||
        dataType != REG_DWORD) {
        return FALSE;
    }
    int v = (int)(LONG)value;
    if (v < min) v = min;
    if (v > max) v = max;
    *out = v;
    return TRUE;
}

static BOOL ReadRegistryQword(HKEY hKey, const wchar_t* name, ULONGLONG* out) {
    ULONGLONG value = 0;
    DWORD dataSize = sizeof(value);
    DWORD dataType = 0;
    if (RegQueryValueExW(hKey, name, NULL, &dataType, (LPBYTE)&value, &dataSize) != ERROR_SUCCESS ||
        dataType != REG_QWORD) {
        return FALSE;
    }
    *out = value;
    return TRUE;
}

static BOOL WriteRegistryQword(HKEY hKey, const wchar_t* name, ULONGLONG value) {
    return RegSetValueExW(hKey, name, 0, REG_QWORD, (const BYTE*)&value,
                          sizeof(value)) == ERROR_SUCCESS;
}

/* Coordinates are stored as text so they survive any rounding rules. */
static BOOL ReadRegistryDouble(HKEY hKey, const wchar_t* name, double* out) {
    wchar_t text[64];
    DWORD dataSize = sizeof(text) - sizeof(wchar_t);
    DWORD dataType = 0;
    if (RegQueryValueExW(hKey, name, NULL, &dataType, (LPBYTE)text, &dataSize) != ERROR_SUCCESS ||
        dataType != REG_SZ) {
        return FALSE;
    }
    text[dataSize / sizeof(wchar_t)] = L'\0';
    wchar_t* end = NULL;
    double value = wcstod(text, &end);
    if (end == text) return FALSE;
    *out = value;
    return TRUE;
}

static BOOL WriteRegistryDouble(HKEY hKey, const wchar_t* name, double value) {
    wchar_t text[64];
    swprintf_s(text, sizeof(text) / sizeof(wchar_t), L"%.6f", value);
    return RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE*)text,
                          (DWORD)((wcslen(text) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static void SetScheduleDefaults(Schedule* schedule) {
    ZeroMemory(schedule, sizeof(*schedule));
    schedule->dayLevel = SCHEDULE_DEFAULT_DAY;
    schedule->nightLevel = SCHEDULE_DEFAULT_NIGHT;
    schedule->dawnStartOffset = SCHEDULE_DEFAULT_DAWN_START;
    schedule->dawnEndOffset = SCHEDULE_DEFAULT_DAWN_END;
    schedule->duskStartOffset = SCHEDULE_DEFAULT_DUSK_START;
    schedule->duskEndOffset = SCHEDULE_DEFAULT_DUSK_END;
    schedule->cycleResetMinutes = SCHEDULE_DEFAULT_RESET_MINUTES;
}

static BOOL IsValidLatitude(double v) { return v >= -90.0 && v <= 90.0; }
static BOOL IsValidLongitude(double v) { return v >= -180.0 && v <= 180.0; }

static BOOL LoadConfigFromRegistry(Configuration* config) {
    ZeroMemory(config, sizeof(*config));
    SetScheduleDefaults(&config->schedule);
    config->autoCheckForUpdates = TRUE;   /* default enabled */

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return FALSE;
    }
    ReadRegistryBool(hKey, REG_VALUE_DEBUGLOG, &config->debugLogEnabled);
    ReadRegistryBool(hKey, REG_VALUE_ALLOW_BELOW_MIN, &config->allowBelowMinimum);
    ReadRegistryBool(hKey, REG_VALUE_AUTO_UPDATE, &config->autoCheckForUpdates);

    DWORD dataType = 0;
    DWORD dataSize = sizeof(g_ignoredUpdateVersion);
    if (RegQueryValueExW(hKey, REG_VALUE_IGNORED_UPDATE_VERSION, NULL,
                         &dataType, (LPBYTE)g_ignoredUpdateVersion,
                         &dataSize) != ERROR_SUCCESS ||
        dataType != REG_SZ || dataSize < sizeof(wchar_t)) {
        g_ignoredUpdateVersion[0] = L'\0';
    }
    g_ignoredUpdateVersion[
        (sizeof(g_ignoredUpdateVersion) / sizeof(wchar_t)) - 1] = L'\0';

    Schedule* sc = &config->schedule;
    ReadRegistryBool(hKey, REG_VALUE_SCHEDULE_ENABLED, &sc->enabled);
    double latitude = 0, longitude = 0;
    if (ReadRegistryDouble(hKey, REG_VALUE_LATITUDE, &latitude) &&
        ReadRegistryDouble(hKey, REG_VALUE_LONGITUDE, &longitude) &&
        IsValidLatitude(latitude) && IsValidLongitude(longitude)) {
        sc->latitude = latitude;
        sc->longitude = longitude;
        sc->hasLocation = TRUE;
    }
    ReadRegistryInt(hKey, REG_VALUE_SCHEDULE_DAY, &sc->dayLevel, -SOFT_MAX_DIM, 100);
    ReadRegistryInt(hKey, REG_VALUE_SCHEDULE_NIGHT, &sc->nightLevel, -SOFT_MAX_DIM, 100);
    ReadRegistryInt(hKey, REG_VALUE_SCHEDULE_DAWN_START, &sc->dawnStartOffset, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    ReadRegistryInt(hKey, REG_VALUE_SCHEDULE_DAWN_END, &sc->dawnEndOffset, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    ReadRegistryInt(hKey, REG_VALUE_SCHEDULE_DUSK_START, &sc->duskStartOffset, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    ReadRegistryInt(hKey, REG_VALUE_SCHEDULE_DUSK_END, &sc->duskEndOffset, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    ReadRegistryInt(hKey, REG_VALUE_CYCLE_RESET, &sc->cycleResetMinutes, 0, 1439);
    if (!sc->hasLocation) sc->enabled = FALSE;
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
    if (!WriteRegistryDword(hKey, REG_VALUE_DEBUGLOG, config->debugLogEnabled ? 1 : 0)) success = FALSE;
    if (!WriteRegistryDword(hKey, REG_VALUE_ALLOW_BELOW_MIN, config->allowBelowMinimum ? 1 : 0)) success = FALSE;
    if (!WriteRegistryDword(hKey, REG_VALUE_AUTO_UPDATE, config->autoCheckForUpdates ? 1 : 0)) success = FALSE;
    RegSetValueExW(hKey, REG_VALUE_IGNORED_UPDATE_VERSION, 0, REG_SZ,
                   (const BYTE*)g_ignoredUpdateVersion,
                   (DWORD)((wcslen(g_ignoredUpdateVersion) + 1) * sizeof(wchar_t)));

    const Schedule* sc = &config->schedule;
    if (!WriteRegistryDword(hKey, REG_VALUE_SCHEDULE_ENABLED, sc->enabled ? 1 : 0)) success = FALSE;
    if (sc->hasLocation) {
        if (!WriteRegistryDouble(hKey, REG_VALUE_LATITUDE, sc->latitude)) success = FALSE;
        if (!WriteRegistryDouble(hKey, REG_VALUE_LONGITUDE, sc->longitude)) success = FALSE;
    }
    WriteRegistryDword(hKey, REG_VALUE_SCHEDULE_DAY, (DWORD)(LONG)sc->dayLevel);
    WriteRegistryDword(hKey, REG_VALUE_SCHEDULE_NIGHT, (DWORD)(LONG)sc->nightLevel);
    WriteRegistryDword(hKey, REG_VALUE_SCHEDULE_DAWN_START, (DWORD)(LONG)sc->dawnStartOffset);
    WriteRegistryDword(hKey, REG_VALUE_SCHEDULE_DAWN_END, (DWORD)(LONG)sc->dawnEndOffset);
    WriteRegistryDword(hKey, REG_VALUE_SCHEDULE_DUSK_START, (DWORD)(LONG)sc->duskStartOffset);
    WriteRegistryDword(hKey, REG_VALUE_SCHEDULE_DUSK_END, (DWORD)(LONG)sc->duskEndOffset);
    WriteRegistryDword(hKey, REG_VALUE_CYCLE_RESET, (DWORD)sc->cycleResetMinutes);
    RegCloseKey(hKey);
    return success;
}

/* Per-monitor settings: HKCU\...\NotTooBright\Monitors\<identity key>. */
static BOOL OpenMonitorKey(const Monitor* m, BOOL forWrite, HKEY* hKey) {
    wchar_t path[320];
    swprintf_s(path, sizeof(path) / sizeof(wchar_t),
               REG_KEY_PATH L"\\" REG_MONITORS_SUBKEY L"\\%s", m->key);
    if (forWrite) {
        return RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, REG_OPTION_NON_VOLATILE,
                               KEY_WRITE, NULL, hKey, NULL) == ERROR_SUCCESS;
    }
    return RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, hKey) == ERROR_SUCCESS;
}

static void LoadMonitorSettings(Monitor* m) {
    HKEY hKey;
    if (!OpenMonitorKey(m, FALSE, &hKey)) return;

    DWORD value = 0;
    DWORD dataSize = sizeof(value);
    DWORD dataType = 0;
    if (RegQueryValueExW(hKey, REG_VALUE_MON_BRIGHTNESS, NULL, &dataType,
                         (LPBYTE)&value, &dataSize) == ERROR_SUCCESS &&
        dataType == REG_DWORD) {
        /* Stored as a two's complement DWORD so negatives (software dimming
         * below the hardware minimum) round-trip. */
        int stored = (int)(LONG)value;
        if (stored < -SOFT_MAX_DIM) stored = -SOFT_MAX_DIM;
        if (stored > 100) stored = 100;
        m->value = stored;
        m->hasValue = TRUE;
    }
    ReadRegistryBool(hKey, REG_VALUE_MON_SOFTWARE_ONLY, &m->forceSoftware);
    ReadRegistryBool(hKey, REG_VALUE_MON_HIDDEN, &m->hidden);
    ReadRegistryBool(hKey, REG_VALUE_MON_HARDWARE, &m->knownHardware);
    ReadRegistryBool(hKey, REG_VALUE_MON_SCHEDULED, &m->scheduled);
    ReadRegistryQword(hKey, REG_VALUE_MON_PAUSED_UNTIL, &m->pausedUntil);
    DWORD original = 0, originalMax = 0;
    DWORD originalSize = sizeof(original), originalMaxSize = sizeof(originalMax);
    if (RegQueryValueExW(hKey, REG_VALUE_MON_ORIGINAL, NULL, &dataType,
                         (LPBYTE)&original, &originalSize) == ERROR_SUCCESS &&
        dataType == REG_DWORD &&
        RegQueryValueExW(hKey, REG_VALUE_MON_ORIGINAL_MAX, NULL, &dataType,
                         (LPBYTE)&originalMax, &originalMaxSize) == ERROR_SUCCESS &&
        dataType == REG_DWORD && originalMax > 0) {
        m->hasOriginal = TRUE;
        m->originalRaw = original;
        m->originalMax = originalMax;
    }
    RegCloseKey(hKey);
}

static void SaveMonitorSettings(const Monitor* m) {
    HKEY hKey;
    if (!OpenMonitorKey(m, TRUE, &hKey)) {
        DebugPrint(L"[WARNING] Could not save settings for monitor %s\n", m->key);
        return;
    }
    if (m->hasValue) {
        WriteRegistryDword(hKey, REG_VALUE_MON_BRIGHTNESS, (DWORD)(LONG)m->value);
    }
    WriteRegistryDword(hKey, REG_VALUE_MON_SOFTWARE_ONLY, m->forceSoftware ? 1 : 0);
    WriteRegistryDword(hKey, REG_VALUE_MON_HIDDEN, m->hidden ? 1 : 0);
    WriteRegistryDword(hKey, REG_VALUE_MON_HARDWARE, m->knownHardware ? 1 : 0);
    WriteRegistryDword(hKey, REG_VALUE_MON_SCHEDULED, m->scheduled ? 1 : 0);
    WriteRegistryQword(hKey, REG_VALUE_MON_PAUSED_UNTIL, m->pausedUntil);
    if (m->hasOriginal) {
        WriteRegistryDword(hKey, REG_VALUE_MON_ORIGINAL, m->originalRaw);
        WriteRegistryDword(hKey, REG_VALUE_MON_ORIGINAL_MAX, m->originalMax);
    }
    /* Informational: lets a user recognise entries when browsing the registry. */
    RegSetValueExW(hKey, REG_VALUE_MON_NAME, 0, REG_SZ, (const BYTE*)m->name,
                   (DWORD)((wcslen(m->name) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
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

/* ── Monitor identity ────────────────────────────────────────────────────── */

/* Registry key names must stay simple; the identity key is built from the
 * PnP id, serial, and device instance path, which can contain odd characters. */
static void SanitizeKeyChars(wchar_t* s) {
    for (; *s; s++) {
        wchar_t c = *s;
        BOOL ok = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'a' && c <= L'z') || c == L'_' || c == L'-' ||
                  c == L'&' || c == L'.';
        if (!ok) *s = L'_';
    }
}

static void CopyRange(wchar_t* dst, size_t dstCount, const wchar_t* begin, size_t n) {
    if (n >= dstCount) n = dstCount - 1;
    memcpy(dst, begin, n * sizeof(wchar_t));
    dst[n] = L'\0';
}

/* EDID text descriptors are 13 bytes of ASCII terminated by 0x0A and padded
 * with spaces. */
static void CopyEdidText(const BYTE* src, wchar_t* out, size_t outCount) {
    size_t n = 0;
    for (int i = 0; i < 13 && n + 1 < outCount; i++) {
        BYTE b = src[i];
        if (b == 0x0A || b == 0) break;
        if (b < 0x20 || b > 0x7E) continue;
        out[n++] = (wchar_t)b;
    }
    out[n] = L'\0';
    while (n > 0 && out[n - 1] == L' ') out[--n] = L'\0';
}

/* Some monitors ship an all-zero or blank serial; such values cannot tell two
 * identical monitors apart and are ignored. */
static BOOL IsMeaningfulSerial(const wchar_t* s) {
    if (!s[0]) return FALSE;
    for (const wchar_t* p = s; *p; p++) {
        if (*p != L'0' && *p != L' ') return TRUE;
    }
    return FALSE;
}

/* The EDID of every monitor Windows has seen is cached under the monitor's
 * device instance key, readable without elevation. */
static DWORD ReadEdidFromRegistry(const wchar_t* pnpId, const wchar_t* instance,
                                  BYTE* edid, DWORD capacity) {
    wchar_t path[512];
    swprintf_s(path, sizeof(path) / sizeof(wchar_t),
               L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s\\%s\\Device Parameters",
               pnpId, instance);
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD dataType = 0;
    DWORD dataSize = capacity;
    LONG result = RegQueryValueExW(hKey, L"EDID", NULL, &dataType, edid, &dataSize);
    RegCloseKey(hKey);
    return (result == ERROR_SUCCESS && dataType == REG_BINARY) ? dataSize : 0;
}

static BOOL ParseEdid(const BYTE* edid, DWORD length,
                      wchar_t* name, size_t nameCount,
                      wchar_t* serial, size_t serialCount,
                      DWORD* numericSerial) {
    static const BYTE header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    name[0] = L'\0';
    serial[0] = L'\0';
    *numericSerial = 0;
    if (length < 128 || memcmp(edid, header, sizeof(header)) != 0) return FALSE;

    *numericSerial = (DWORD)edid[12] | ((DWORD)edid[13] << 8) |
                     ((DWORD)edid[14] << 16) | ((DWORD)edid[15] << 24);
    /* Four 18-byte descriptors at 54, 72, 90, 108; display descriptors
     * start with three zero bytes and a tag: 0xFC = name, 0xFF = serial. */
    for (int d = 54; d + 18 <= 126; d += 18) {
        if (edid[d] != 0 || edid[d + 1] != 0 || edid[d + 2] != 0) continue;
        if (edid[d + 3] == 0xFC && !name[0]) {
            CopyEdidText(edid + d + 5, name, nameCount);
        } else if (edid[d + 3] == 0xFF && !serial[0]) {
            CopyEdidText(edid + d + 5, serial, serialCount);
        }
    }
    return TRUE;
}

/* Fills in the persistent identity key and friendly name of the
 * physicalIndex-th active monitor on the given adapter (\\.\DISPLAYn). */
static void ResolveMonitorIdentity(const wchar_t* adapterDevice, int physicalIndex, Monitor* m) {
    DISPLAY_DEVICEW dd;
    BOOL found = FALSE;
    int activeSeen = 0;
    for (DWORD i = 0; i < 8; i++) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(adapterDevice, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        if (activeSeen++ == physicalIndex) {
            found = TRUE;
            break;
        }
    }
    if (!found) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        found = EnumDisplayDevicesW(adapterDevice, (DWORD)physicalIndex, &dd,
                                    EDD_GET_DEVICE_INTERFACE_NAME);
    }

    /* Interface name: \\?\DISPLAY#<PnP id>#<instance path>#{class guid} */
    wchar_t pnpId[32] = L"";
    wchar_t instance[160] = L"";
    if (found) {
        DebugPrint(L"[ENUM] %s physical %d -> monitor device \"%s\" (%s), flags 0x%08lX, id %s\n",
                   adapterDevice, physicalIndex, dd.DeviceString, dd.DeviceName,
                   (unsigned long)dd.StateFlags, dd.DeviceID);
    } else {
        DebugPrint(L"[ENUM] %s physical %d -> no monitor device found (error %lu)\n",
                   adapterDevice, physicalIndex, (unsigned long)GetLastError());
    }
    if (found) {
        const wchar_t* p = dd.DeviceID;
        if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
        const wchar_t* h1 = wcschr(p, L'#');
        const wchar_t* h2 = h1 ? wcschr(h1 + 1, L'#') : NULL;
        if (h1 && h2) {
            CopyRange(pnpId, sizeof(pnpId) / sizeof(wchar_t), h1 + 1, (size_t)(h2 - h1 - 1));
            const wchar_t* h3 = wcschr(h2 + 1, L'#');
            size_t n = h3 ? (size_t)(h3 - h2 - 1) : wcslen(h2 + 1);
            CopyRange(instance, sizeof(instance) / sizeof(wchar_t), h2 + 1, n);
        }
    }

    BYTE edid[512];
    wchar_t edidName[64] = L"", edidSerial[64] = L"";
    DWORD numericSerial = 0;
    BOOL edidOk = FALSE;
    if (pnpId[0] && instance[0]) {
        DWORD edidLen = ReadEdidFromRegistry(pnpId, instance, edid, sizeof(edid));
        edidOk = edidLen > 0 && ParseEdid(edid, edidLen, edidName, 64, edidSerial, 64, &numericSerial);
        DebugPrint(L"[ENUM] EDID for %s\\%s: %lu bytes, %s, name \"%s\", serial \"%s\", numeric serial %lu\n",
                   pnpId, instance, (unsigned long)edidLen, edidOk ? L"parsed" : L"not usable",
                   edidName, edidSerial, (unsigned long)numericSerial);
    } else {
        DebugPrint(L"[ENUM] no PnP id/instance for %s physical %d; EDID not read\n", adapterDevice, physicalIndex);
    }

    const size_t keyCount = sizeof(m->key) / sizeof(wchar_t);
    if (pnpId[0]) {
        if (edidOk && IsMeaningfulSerial(edidSerial)) {
            swprintf_s(m->key, keyCount, L"%s_%s", pnpId, edidSerial);
        } else if (edidOk && numericSerial != 0) {
            swprintf_s(m->key, keyCount, L"%s_%08X", pnpId, (unsigned)numericSerial);
        } else {
            /* No usable serial: fall back to the connector, which is stable
             * as long as the monitor stays on the same port. */
            swprintf_s(m->key, keyCount, L"%s_%s", pnpId, instance);
        }
    } else {
        const wchar_t* tail = wcsrchr(adapterDevice, L'\\');
        swprintf_s(m->key, keyCount, L"%s_%d", tail ? tail + 1 : adapterDevice, physicalIndex);
    }
    SanitizeKeyChars(m->key);

    const size_t nameCount = sizeof(m->name) / sizeof(wchar_t);
    if (edidName[0]) {
        wcscpy_s(m->name, nameCount, edidName);
    } else if (found && dd.DeviceString[0]) {
        wcscpy_s(m->name, nameCount, dd.DeviceString);
    } else {
        wcscpy_s(m->name, nameCount, L"Display");
    }
}

/* ── Software dimming overlays ───────────────────────────────────────────── */

/* A dimming overlay is a topmost, click-through, never-activated black popup
 * covering one display; its per-window alpha is the amount of dimming. It
 * is excluded from screen capture so screenshots and screen sharing show
 * the undimmed picture, and it disappears with the process, so a crash can
 * never leave a screen dark. */
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
        case WM_CLOSE:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateOverlayWindow(const RECT* rc) {
    static BOOL classRegistered = FALSE;
    if (!classRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = g_hInstance;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"NotTooBrightOverlay";
        if (!RegisterClassExW(&wc)) return NULL;
        classRegistered = TRUE;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        L"NotTooBrightOverlay", APP_DISPLAY_NAME_WSTRING L" dimming overlay", WS_POPUP,
        rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
        NULL, NULL, g_hInstance, NULL);
    if (!hwnd) {
        DebugPrint(L"[ERROR] Could not create a dimming overlay (error %lu)\n",
                   (unsigned long)GetLastError());
        return NULL;
    }
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    /* Windows 10 2004+; older versions simply keep the overlay in captures. */
    BOOL excluded = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    DWORD affinityError = excluded ? 0 : GetLastError();
    if (excluded) {
        DebugPrint(L"[OVERLAY] created hwnd=%p at (%ld,%ld) %ldx%ld, excluded from capture\n",
                   (void*)hwnd, (long)rc->left, (long)rc->top, (long)(rc->right - rc->left),
                   (long)(rc->bottom - rc->top));
    } else {
        DebugPrint(L"[OVERLAY] created hwnd=%p at (%ld,%ld) %ldx%ld, capture exclusion unavailable (error %lu)\n",
                   (void*)hwnd, (long)rc->left, (long)rc->top, (long)(rc->right - rc->left),
                   (long)(rc->bottom - rc->top), (unsigned long)affinityError);
    }
    return hwnd;
}

static void PositionOverlay(Monitor* m) {
    if (!m->overlay) return;
    SetWindowPos(m->overlay, HWND_TOPMOST, m->rect.left, m->rect.top,
                 m->rect.right - m->rect.left, m->rect.bottom - m->rect.top,
                 SWP_NOACTIVATE);
}

/* dim is the percentage of darkening, 0..SOFT_MAX_DIM. */
static void SetOverlayDim(Monitor* m, int dim) {
    if (dim < 0) dim = 0;
    if (dim > SOFT_MAX_DIM) dim = SOFT_MAX_DIM;
    int previous = m->overlayDim;
    m->overlayDim = dim;
    if (!m->overlay) return;
    if (dim == 0) {
        if (IsWindowVisible(m->overlay)) {
            ShowWindow(m->overlay, SW_HIDE);
            DebugPrint(L"[OVERLAY] %s: hidden (was %d%%)\n", m->name, previous);
        }
        return;
    }
    BYTE alpha = (BYTE)((dim * 255 + 50) / 100);
    SetLayeredWindowAttributes(m->overlay, 0, alpha, LWA_ALPHA);
    if (!IsWindowVisible(m->overlay)) {
        ShowWindow(m->overlay, SW_SHOWNOACTIVATE);
        PositionOverlay(m);
    }
    if (previous != dim) DebugPrint(L"[OVERLAY] %s: dim %d%% (alpha %u)\n", m->name, dim, (unsigned)alpha);
}

static BOOL IsOverlayWindow(HWND hwnd) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].overlay == hwnd) return TRUE;
    }
    return FALSE;
}

/* Any topmost window shown later (an always-on-top app, a media player,
 * another utility) lands above the overlays and escapes the dimming; bring
 * the overlays back to the top when that happens. Menus and tooltips are
 * ignored because they are short-lived and re-raising while one is open
 * makes it flicker. */
static void KeepOverlaysOnTop(void) {
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (!m->overlay || m->overlayDim == 0 || !IsWindowVisible(m->overlay)) continue;
        BOOL raise = FALSE;
        HWND above = GetWindow(m->overlay, GW_HWNDPREV);
        for (int guard = 0; above && guard < 64; guard++) {
            if (IsWindowVisible(above) && !IsOverlayWindow(above) &&
                (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
                wchar_t cls[32] = L"";
                GetClassNameW(above, cls, 32);
                if (wcscmp(cls, L"#32768") != 0 && wcscmp(cls, L"tooltips_class32") != 0) {
                    raise = TRUE;
                    break;
                }
            }
            above = GetWindow(above, GW_HWNDPREV);
        }
        if (raise) {
            SetWindowPos(m->overlay, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

/* Drops a monitor's overlay window entirely (used when the monitor is
 * hidden: the application must own nothing on that display). */
static void ReleaseMonitorOverlay(Monitor* m) {
    if (m->overlay) {
        DestroyWindow(m->overlay);
        m->overlay = NULL;
    }
    m->overlayDim = 0;
}

static void DestroyAllOverlays(void) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].overlay) {
            DestroyWindow(g_monitors[i].overlay);
            g_monitors[i].overlay = NULL;
        }
        g_monitors[i].overlayDim = 0;
    }
}

/* ── DDC/CI worker ───────────────────────────────────────────────────────── */

/* Everything that talks to a monitor over DDC/CI happens on this thread: a
 * single VCP read or write can take hundreds of milliseconds, and a
 * misbehaving monitor can stall for seconds, so the UI thread only ever
 * queues requests and receives posted results. */

typedef struct {
    HMONITOR hmon;
    DWORD count;
    PHYSICAL_MONITOR* monitors;
    BOOL inJob;
} WorkerSource;

typedef struct {
    int uid;
    HMONITOR hmon;
    int physicalIndex;
    BOOL patient;
    BOOL supported;
    BOOL highLevel;
    DWORD min;
    DWORD max;
} WorkerMonitor;

static void DdcRequestWrite(int uid, int target, BOOL raw) {
    EnterCriticalSection(&g_ddcLock);
    DdcSetSlot* slot = NULL;
    for (int i = 0; i < g_ddcSetCount; i++) {
        if (g_ddcSets[i].uid == uid) {
            slot = &g_ddcSets[i];
            break;
        }
    }
    if (!slot && g_ddcSetCount < MAX_MONITORS) {
        slot = &g_ddcSets[g_ddcSetCount++];
        slot->uid = uid;
    }
    if (slot) {
        slot->target = target;
        slot->raw = raw;
        slot->pending = TRUE;
    }
    LeaveCriticalSection(&g_ddcLock);
    if (g_ddcEvent) SetEvent(g_ddcEvent);
}

static void DdcRequestSet(int uid, int percent) {
    DdcRequestWrite(uid, percent, FALSE);
}

/* Writes an exact VCP value, used to put a monitor back to the value it
 * reported before the application ever touched it. */
static void DdcRequestSetRaw(int uid, DWORD rawValue) {
    DdcRequestWrite(uid, (int)rawValue, TRUE);
}

/* Replaces the worker's monitor set. Pending writes are dropped: every
 * monitor is re-applied once its probe result arrives. */
static void DdcRequestProbe(const DdcProbeEntry* entries, int count) {
    if (count > MAX_MONITORS) count = MAX_MONITORS;
    EnterCriticalSection(&g_ddcLock);
    memcpy(g_ddcProbeList, entries, sizeof(DdcProbeEntry) * (size_t)count);
    g_ddcProbeCount = count;
    g_ddcProbeRequested = TRUE;
    g_ddcSetCount = 0;
    LeaveCriticalSection(&g_ddcLock);
    if (g_ddcEvent) SetEvent(g_ddcEvent);
}

static void CloseWorkerSource(WorkerSource* s) {
    if (s->monitors) {
        BOOL ok = DestroyPhysicalMonitors(s->count, s->monitors);
        DebugPrint(L"[DDC] hmon=%p DestroyPhysicalMonitors(%lu) -> %s\n", (void*)s->hmon,
                   (unsigned long)s->count, ok ? L"ok" : L"FAILED");
        free(s->monitors);
    }
    s->monitors = NULL;
    s->count = 0;
}

static BOOL OpenWorkerSource(WorkerSource* s) {
    s->monitors = NULL;
    s->count = 0;
    DWORD n = 0;
    ULONGLONG start = GetTickCount64();
    BOOL ok = GetNumberOfPhysicalMonitorsFromHMONITOR(s->hmon, &n);
    DWORD error = ok ? 0 : GetLastError();
    DebugPrint(L"[DDC] hmon=%p GetNumberOfPhysicalMonitorsFromHMONITOR -> %s, count %lu, error %lu, %lu ms\n",
               (void*)s->hmon, ok ? L"ok" : L"FAILED", (unsigned long)n, (unsigned long)error,
               (unsigned long)(GetTickCount64() - start));
    if (!ok || n == 0) return FALSE;
    s->monitors = (PHYSICAL_MONITOR*)calloc(n, sizeof(PHYSICAL_MONITOR));
    if (!s->monitors) return FALSE;
    start = GetTickCount64();
    ok = GetPhysicalMonitorsFromHMONITOR(s->hmon, n, s->monitors);
    error = ok ? 0 : GetLastError();
    DebugPrint(L"[DDC] hmon=%p GetPhysicalMonitorsFromHMONITOR -> %s, error %lu, %lu ms\n",
               (void*)s->hmon, ok ? L"ok" : L"FAILED", (unsigned long)error,
               (unsigned long)(GetTickCount64() - start));
    if (!ok) {
        free(s->monitors);
        s->monitors = NULL;
        return FALSE;
    }
    for (DWORD i = 0; i < n; i++) {
        DebugPrint(L"[DDC] hmon=%p physical[%lu] handle=%p description=\"%s\"\n",
                   (void*)s->hmon, (unsigned long)i, (void*)s->monitors[i].hPhysicalMonitor,
                   s->monitors[i].szPhysicalMonitorDescription);
    }
    s->count = n;
    Sleep(DDC_HANDLE_SETTLE_MS);
    return TRUE;
}

static WorkerSource* FindWorkerSource(WorkerSource* sources, int count, HMONITOR hmon) {
    for (int i = 0; i < count; i++) {
        if (sources[i].hmon == hmon) return &sources[i];
    }
    return NULL;
}

/* A physical monitor handle is an opaque value that can legitimately be 0:
 * some drivers (AMD, for one) hand out small sequential numbers starting
 * at zero. Never treat 0 as "no handle"; use the return value instead. */
static BOOL WorkerMonitorHandle(WorkerSource* sources, int count, const WorkerMonitor* wm, HANDLE* handle) {
    WorkerSource* src = FindWorkerSource(sources, count, wm->hmon);
    if (!src || (DWORD)wm->physicalIndex >= src->count) return FALSE;
    *handle = src->monitors[wm->physicalIndex].hPhysicalMonitor;
    return TRUE;
}

/* Physical monitor handles stay open for as long as their display is
 * present: some GPU drivers refuse DDC/CI for a while after a handle is
 * destroyed and recreated, so tearing everything down on every probe made
 * a lone monitor look unresponsive. New displays are opened before a job
 * and displays that left the job are closed only after it, so a close
 * never immediately precedes a read. */
static void OpenJobSources(WorkerSource* sources, int* count, const DdcProbeEntry* job, int jobCount) {
    for (int i = 0; i < *count; i++) sources[i].inJob = FALSE;
    for (int j = 0; j < jobCount; j++) {
        WorkerSource* src = FindWorkerSource(sources, *count, job[j].hmon);
        if (src) {
            src->inJob = TRUE;
        } else if (*count < MAX_MONITORS) {
            WorkerSource* s = &sources[(*count)++];
            s->hmon = job[j].hmon;
            s->inJob = TRUE;
            OpenWorkerSource(s);
        }
    }
}

static void CloseUnusedSources(WorkerSource* sources, int* count) {
    int kept = 0;
    for (int i = 0; i < *count; i++) {
        if (sources[i].inJob) {
            sources[kept++] = sources[i];
        } else {
            CloseWorkerSource(&sources[i]);
        }
    }
    *count = kept;
}

static void ReleaseWorkerSources(WorkerSource* sources, int* count) {
    for (int i = 0; i < *count; i++) CloseWorkerSource(&sources[i]);
    *count = 0;
}

/* One raw VCP 0x10 read with everything about it logged. */
static BOOL ReadVcpBrightness(HANDLE handle, int uid, const wchar_t* label,
                              DWORD* current, DWORD* max, DWORD* error) {
    MC_VCP_CODE_TYPE type = MC_SET_PARAMETER;
    ULONGLONG start = GetTickCount64();
    BOOL ok = GetVCPFeatureAndVCPFeatureReply(handle, VCP_BRIGHTNESS, &type, current, max);
    *error = ok ? 0 : GetLastError();
    DebugPrint(L"[DDC] monitor %d handle=%p %s: GetVCPFeatureAndVCPFeatureReply(0x10) -> %s, "
               L"current %lu, max %lu, type %d, error %lu (0x%08lX), %lu ms\n",
               uid, (void*)handle, label, ok ? L"ok" : L"FAILED", (unsigned long)*current,
               (unsigned long)*max, (int)type, (unsigned long)*error, (unsigned long)*error,
               (unsigned long)(GetTickCount64() - start));
    return ok;
}

/* The high-level route: the capabilities string first (diagnostic: shows
 * whether the monitor lists VCP 10 at all, and how slow the channel is),
 * then GetMonitorBrightness, which some drivers accept when the raw VCP
 * call is refused. */
static BOOL ReadHighLevelBrightness(HANDLE handle, int uid, DWORD* min, DWORD* current, DWORD* max) {
    ULONGLONG start = GetTickCount64();
    DWORD caps = 0, temps = 0;
    BOOL capsOk = GetMonitorCapabilities(handle, &caps, &temps);
    DebugPrint(L"[DDC] monitor %d handle=%p GetMonitorCapabilities -> %s, caps 0x%08lX%s, temps 0x%08lX, error %lu, %lu ms\n",
               uid, (void*)handle, capsOk ? L"ok" : L"FAILED", (unsigned long)caps,
               (caps & MC_CAPS_BRIGHTNESS) ? L" (brightness)" : L"", (unsigned long)temps,
               (unsigned long)(capsOk ? 0 : GetLastError()),
               (unsigned long)(GetTickCount64() - start));

    start = GetTickCount64();
    DWORD length = 0;
    if (GetCapabilitiesStringLength(handle, &length) && length > 1 && length < 8192) {
        char* text = (char*)calloc(length + 1, 1);
        if (text && CapabilitiesRequestAndCapabilitiesReply(handle, text, length)) {
            wchar_t wide[1600];
            int n = MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 1599);
            if (n <= 0) wide[0] = L'\0';
            wide[1599] = L'\0';
            DebugPrint(L"[DDC] monitor %d capabilities (%lu bytes, %lu ms): %s\n", uid,
                       (unsigned long)length, (unsigned long)(GetTickCount64() - start), wide);
        } else {
            DebugPrint(L"[DDC] monitor %d CapabilitiesRequestAndCapabilitiesReply FAILED, error %lu, %lu ms\n",
                       uid, (unsigned long)GetLastError(), (unsigned long)(GetTickCount64() - start));
        }
        free(text);
    } else {
        DebugPrint(L"[DDC] monitor %d GetCapabilitiesStringLength -> length %lu, error %lu, %lu ms\n",
                   uid, (unsigned long)length, (unsigned long)GetLastError(),
                   (unsigned long)(GetTickCount64() - start));
    }

    start = GetTickCount64();
    *min = *current = *max = 0;
    BOOL ok = GetMonitorBrightness(handle, min, current, max);
    DebugPrint(L"[DDC] monitor %d handle=%p GetMonitorBrightness -> %s, min %lu, current %lu, max %lu, error %lu, %lu ms\n",
               uid, (void*)handle, ok ? L"ok" : L"FAILED", (unsigned long)*min, (unsigned long)*current,
               (unsigned long)*max, (unsigned long)(ok ? 0 : GetLastError()),
               (unsigned long)(GetTickCount64() - start));
    return ok && *max > *min;
}

/* Asks the monitor for VCP 0x10 (brightness) directly first (fast, no
 * capabilities string), retrying on the same handle with growing pauses;
 * then the high-level route; then, for a monitor that answered before,
 * once more on a freshly opened handle. */
static void ProbeWorkerMonitor(WorkerSource* sources, int* sourceCount, WorkerMonitor* wm) {
    DdcProbeResult* r = (DdcProbeResult*)calloc(1, sizeof(*r));
    if (!r) return;
    r->uid = wm->uid;
    ULONGLONG start = GetTickCount64();
    DWORD min = 0, current = 0, max = 0;
    BOOL ok = FALSE, highLevel = FALSE;
    HANDLE handle = NULL;
    BOOL haveHandle = WorkerMonitorHandle(sources, *sourceCount, wm, &handle);
    DebugPrint(L"[DDC] monitor %d probe start: hmon=%p physicalIndex=%d handle=%p (%s) patient=%d\n",
               wm->uid, (void*)wm->hmon, wm->physicalIndex, (void*)handle,
               haveHandle ? L"valid" : L"none", wm->patient);
    if (!haveHandle) {
        r->error = ERROR_NOT_FOUND;
    } else {
        static const DWORD waitMs[DDC_PROBE_ATTEMPTS] = { 0, 300, 800, 1500 };
        int attempts = wm->patient ? DDC_PROBE_ATTEMPTS : DDC_PROBE_ATTEMPTS_UNKNOWN;
        for (int attempt = 0; attempt < attempts && !ok; attempt++) {
            if (waitMs[attempt]) Sleep(waitMs[attempt]);
            wchar_t label[32];
            swprintf_s(label, 32, L"attempt %d", attempt + 1);
            ok = ReadVcpBrightness(handle, wm->uid, label, &current, &max, &r->error);
            if (InterlockedCompareExchange(&g_ddcStop, 0, 0)) break;
        }
        if (!ok && !InterlockedCompareExchange(&g_ddcStop, 0, 0)) {
            ok = ReadHighLevelBrightness(handle, wm->uid, &min, &current, &max);
            highLevel = ok;
        }
        if (!ok && wm->patient && !InterlockedCompareExchange(&g_ddcStop, 0, 0)) {
            WorkerSource* src = FindWorkerSource(sources, *sourceCount, wm->hmon);
            if (src) {
                DebugPrint(L"[DDC] monitor %d: reopening the handle after %lu ms pause\n",
                           wm->uid, (unsigned long)DDC_REOPEN_PAUSE_MS);
                CloseWorkerSource(src);
                Sleep(DDC_REOPEN_PAUSE_MS);
                OpenWorkerSource(src);
                if (WorkerMonitorHandle(sources, *sourceCount, wm, &handle)) {
                    ok = ReadVcpBrightness(handle, wm->uid, L"after reopen", &current, &max, &r->error);
                    if (!ok) {
                        ok = ReadHighLevelBrightness(handle, wm->uid, &min, &current, &max);
                        highLevel = ok;
                    }
                }
            }
        }
    }
    r->elapsedMs = (DWORD)(GetTickCount64() - start);
    if (ok) {
        r->supported = TRUE;
        r->highLevel = highLevel;
        r->min = highLevel ? min : 0;
        r->current = current;
        r->max = max ? max : 100;
        r->error = 0;
        wm->supported = TRUE;
        wm->highLevel = highLevel;
        wm->min = r->min;
        wm->max = r->max;
    }
    DebugPrint(L"[DDC] monitor %d probe result: %s%s, min %lu, current %lu, max %lu, error %lu, %lu ms total\n",
               wm->uid, ok ? L"supported" : L"NOT supported", highLevel ? L" (high-level route)" : L"",
               (unsigned long)r->min, (unsigned long)r->current, (unsigned long)r->max,
               (unsigned long)r->error, (unsigned long)r->elapsedMs);
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_DDC_PROBED, 0, (LPARAM)r)) free(r);
}

static BOOL WriteWorkerBrightness(HANDLE handle, WorkerMonitor* wm, int target, BOOL raw) {
    DWORD range = wm->max > wm->min ? wm->max - wm->min : 100;
    DWORD value;
    int percent;
    if (raw) {
        value = (DWORD)target;
        if (value > wm->max) value = wm->max;
        if (value < wm->min) value = wm->min;
        percent = (int)(((value - wm->min) * 100 + range / 2) / range);
    } else {
        percent = target < 0 ? 0 : (target > 100 ? 100 : target);
        value = wm->min + (DWORD)(((unsigned)percent * range + 50) / 100);
    }
    BOOL ok = FALSE;
    DWORD error = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (attempt) Sleep(100);
        ULONGLONG start = GetTickCount64();
        ok = wm->highLevel ? SetMonitorBrightness(handle, value) : SetVCPFeature(handle, VCP_BRIGHTNESS, value);
        error = ok ? 0 : GetLastError();
        DebugPrint(L"[DDC] monitor %d handle=%p %s(%lu) [%d%%, range %lu..%lu] attempt %d -> %s, error %lu, %lu ms\n",
                   wm->uid, (void*)handle, wm->highLevel ? L"SetMonitorBrightness" : L"SetVCPFeature",
                   (unsigned long)value, percent, (unsigned long)wm->min, (unsigned long)wm->max,
                   attempt + 1, ok ? L"ok" : L"FAILED", (unsigned long)error,
                   (unsigned long)(GetTickCount64() - start));
    }
    return ok;
}

/* Performs queued brightness writes. During a probe job only monitors
 * already probed in that job are written (the others stay queued), so a
 * slider does not have to wait for a slow display at the end of the job. */
static void DrainPendingWrites(WorkerSource* sources, int sourceCount,
                               WorkerMonitor* monitors, int monitorCount, BOOL onlyProbed) {
    for (;;) {
        if (InterlockedCompareExchange(&g_ddcStop, 0, 0)) break;
        int uid = -1, target = 0;
        BOOL raw = FALSE;
        WorkerMonitor* wm = NULL;
        EnterCriticalSection(&g_ddcLock);
        for (int i = 0; i < g_ddcSetCount; i++) {
            if (!g_ddcSets[i].pending) continue;
            WorkerMonitor* candidate = NULL;
            for (int j = 0; j < monitorCount; j++) {
                if (monitors[j].uid == g_ddcSets[i].uid) {
                    candidate = &monitors[j];
                    break;
                }
            }
            if (onlyProbed && !candidate) continue;
            uid = g_ddcSets[i].uid;
            target = g_ddcSets[i].target;
            raw = g_ddcSets[i].raw;
            wm = candidate;
            g_ddcSets[i].pending = FALSE;
            break;
        }
        LeaveCriticalSection(&g_ddcLock);
        if (uid < 0) break;

        BOOL ok = FALSE;
        if (wm && wm->supported) {
            HANDLE handle = NULL;
            if (WorkerMonitorHandle(sources, sourceCount, wm, &handle)) ok = WriteWorkerBrightness(handle, wm, target, raw);
            else DebugPrint(L"[DDC] monitor %d: no handle for the write\n", uid);
        } else {
            DebugPrint(L"[DDC] monitor %d: write skipped (%s)\n", uid,
                       wm ? L"not supported" : L"unknown to the worker");
        }
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DDC_SET_RESULT, (WPARAM)uid, ok ? 1 : 0);
    }
}

static DWORD WINAPI DdcWorkerThread(LPVOID param) {
    (void)param;
    WorkerSource sources[MAX_MONITORS];
    int sourceCount = 0;
    WorkerMonitor monitors[MAX_MONITORS];
    int monitorCount = 0;
    ZeroMemory(sources, sizeof(sources));
    DebugPrint(L"[DDC] worker thread started (id %lu)\n", (unsigned long)GetCurrentThreadId());

    for (;;) {
        WaitForSingleObject(g_ddcEvent, INFINITE);
        if (InterlockedCompareExchange(&g_ddcStop, 0, 0)) break;

        DdcProbeEntry job[MAX_MONITORS];
        int jobCount = 0;
        BOOL probe = FALSE;
        EnterCriticalSection(&g_ddcLock);
        if (g_ddcProbeRequested) {
            probe = TRUE;
            g_ddcProbeRequested = FALSE;
            jobCount = g_ddcProbeCount;
            memcpy(job, g_ddcProbeList, sizeof(DdcProbeEntry) * (size_t)jobCount);
        }
        LeaveCriticalSection(&g_ddcLock);

        if (probe) {
            DebugPrint(L"[DDC] probe job: %d monitor(s)\n", jobCount);
            for (int i = 0; i < jobCount; i++) {
                DebugPrint(L"[DDC]   entry %d: uid %d hmon=%p physicalIndex %d patient %d\n", i,
                           job[i].uid, (void*)job[i].hmon, job[i].physicalIndex, job[i].patient);
            }
            OpenJobSources(sources, &sourceCount, job, jobCount);
            monitorCount = 0;
            for (int i = 0; i < jobCount && monitorCount < MAX_MONITORS; i++) {
                WorkerMonitor* wm = &monitors[monitorCount++];
                wm->uid = job[i].uid;
                wm->hmon = job[i].hmon;
                wm->physicalIndex = job[i].physicalIndex;
                wm->patient = job[i].patient;
                wm->supported = FALSE;
                wm->highLevel = FALSE;
                wm->min = 0;
                wm->max = 100;
                ProbeWorkerMonitor(sources, &sourceCount, wm);
                if (InterlockedCompareExchange(&g_ddcStop, 0, 0)) break;
                /* Keep sliders responsive while the remaining displays probe. */
                DrainPendingWrites(sources, sourceCount, monitors, monitorCount, TRUE);
            }
            CloseUnusedSources(sources, &sourceCount);
            DebugPrint(L"[DDC] probe job done; %d source(s) kept open\n", sourceCount);
        }

        DrainPendingWrites(sources, sourceCount, monitors, monitorCount, FALSE);
    }

    ReleaseWorkerSources(sources, &sourceCount);
    return 0;
}

static BOOL StartDdcWorker(void) {
    g_ddcEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_ddcEvent) return FALSE;
    g_ddcThread = CreateThread(NULL, 0, DdcWorkerThread, NULL, 0, NULL);
    if (!g_ddcThread) {
        CloseHandle(g_ddcEvent);
        g_ddcEvent = NULL;
        return FALSE;
    }
    return TRUE;
}

/* A monitor wedged mid-command cannot be interrupted; wait briefly and let
 * process exit reclaim the thread if it never comes back. */
static void StopDdcWorker(void) {
    if (!g_ddcThread) return;
    InterlockedExchange(&g_ddcStop, 1);
    SetEvent(g_ddcEvent);
    if (WaitForSingleObject(g_ddcThread, 3000) != WAIT_OBJECT_0) {
        DebugPrint(L"[WARNING] DDC/CI worker did not stop in time\n");
    }
    CloseHandle(g_ddcThread);
    g_ddcThread = NULL;
    CloseHandle(g_ddcEvent);
    g_ddcEvent = NULL;
}

/* ── Brightness model ────────────────────────────────────────────────────── */

static Monitor* FindMonitorByUid(int uid) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].uid == uid) return &g_monitors[i];
    }
    return NULL;
}

static BrightnessMode MonitorMode(const Monitor* m) {
    if (m->forceSoftware) return MODE_SOFTWARE;
    if (m->hardwareState == HW_AVAILABLE) return MODE_HARDWARE;
    if (m->hardwareState == HW_UNAVAILABLE) {
        /* A monitor that has worked over DDC/CI may well have its backlight
         * below 100% right now; dimming it in software on top of that would
         * stack the two. Leave it alone and keep retrying instead. */
        return m->knownHardware ? MODE_WAITING : MODE_SOFTWARE;
    }
    return MODE_PROBING;
}

/* The slider's lower bound. Hardware monitors go to 0 (the backlight's own
 * minimum) and, when allowed, below it into software dimming; software-only
 * monitors stop at the fixed floor. */
static int MonitorMinValue(const Monitor* m) {
    switch (MonitorMode(m)) {
        case MODE_SOFTWARE: return SOFT_MIN_BRIGHTNESS;
        case MODE_HARDWARE:
        case MODE_WAITING: return g_config.allowBelowMinimum ? -SOFT_MAX_DIM : 0;
        default: return 0;
    }
}

static int ClampMonitorValue(const Monitor* m, int value) {
    int min = MonitorMinValue(m);
    if (value < min) value = min;
    if (value > 100) value = 100;
    return value;
}

/* Turns the desired value into hardware and overlay settings. Hardware
 * monitors: 0..100 is the backlight, negative values keep the backlight at
 * its minimum and add software dimming. Software monitors: the overlay
 * alone provides the whole range. */
static void ApplyMonitor(Monitor* m) {
    /* Hidden monitors are not controlled at all: no overlay, no DDC/CI
     * writes. Whatever state the display is in stays that way. */
    if (m->hidden) return;
    BrightnessMode mode = MonitorMode(m);
    if (mode == MODE_PROBING) return;   /* applied when the probe answers */
    if (mode == MODE_WAITING) return;   /* nothing is touched until it answers */

    int value = ClampMonitorValue(m, m->value);
    if (value != m->value) {
        m->value = value;
        m->dirty = TRUE;
        SchedulePersist();
    }

    int dim = 0;
    BOOL wrote = FALSE;
    if (mode == MODE_HARDWARE) {
        int hw = value < 0 ? 0 : value;
        if (value < 0) dim = -value;
        if (hw != m->lastHwSent) {
            m->lastHwSent = hw;
            DdcRequestSet(m->uid, hw);
            wrote = TRUE;
        }
    } else {
        dim = 100 - value;
    }
    DebugPrint(L"[APPLY] %s (%s): mode %s value %d -> hardware %s%d%%, overlay dim %d%%\n",
               m->name, m->device, ModeName(mode), value,
               mode == MODE_HARDWARE ? (wrote ? L"write " : L"unchanged ") : L"n/a ",
               mode == MODE_HARDWARE ? (value < 0 ? 0 : value) : 0, dim);
    SetOverlayDim(m, dim);
}

static void SetMonitorValue(Monitor* m, int value) {
    if (m->hidden) return;
    value = ClampMonitorValue(m, value);
    if (!m->hasValue || m->value != value) {
        m->value = value;
        m->hasValue = TRUE;
        m->dirty = TRUE;
        SchedulePersist();
    }
    ApplyMonitor(m);
}

static void SchedulePersist(void) {
    if (g_hwnd) SetTimer(g_hwnd, ID_TIMER_PERSIST, PERSIST_DEBOUNCE_MS, NULL);
}

static void PersistDirtyMonitors(void) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].dirty) {
            SaveMonitorSettings(&g_monitors[i]);
            g_monitors[i].dirty = FALSE;
        }
    }
}

static void ScheduleMonitorRefresh(UINT delayMs) {
    if (g_hwnd) SetTimer(g_hwnd, ID_TIMER_REFRESH_MONITORS, delayMs, NULL);
}

/* Re-probes everything after a growing delay while a known DDC/CI monitor
 * is not answering (3 s, 6 s, ... up to a minute, then every minute). */
static void ScheduleDdcRetry(void) {
    if (!g_hwnd || g_ddcRetryPending) return;
    g_ddcRetryPending = TRUE;
    SetTimer(g_hwnd, ID_TIMER_DDC_RETRY, g_ddcRetryDelayMs, NULL);
    DebugPrint(L"[INFO] Next DDC/CI retry in %u ms\n", g_ddcRetryDelayMs);
    g_ddcRetryDelayMs *= 2;
    if (g_ddcRetryDelayMs > DDC_RETRY_MAX_MS) g_ddcRetryDelayMs = DDC_RETRY_MAX_MS;
}

typedef struct {
    HMONITOR hmon;
    MONITORINFOEXW info;
} EnumEntry;

typedef struct {
    EnumEntry entries[MAX_MONITORS];
    int count;
} EnumContext;

static BOOL CALLBACK EnumMonitorProc(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lParam) {
    (void)hdc; (void)rc;
    EnumContext* ctx = (EnumContext*)lParam;
    if (ctx->count >= MAX_MONITORS) return FALSE;
    EnumEntry* e = &ctx->entries[ctx->count];
    ZeroMemory(&e->info, sizeof(e->info));
    e->info.cbSize = sizeof(e->info);
    if (!GetMonitorInfoW(hmon, (MONITORINFO*)&e->info)) return TRUE;
    e->hmon = hmon;
    ctx->count++;
    return TRUE;
}

/* Rebuilds the monitor list from the current display configuration, keeps
 * the state of monitors that are still present (matched by identity key),
 * loads saved settings for new ones, and asks the worker to (re)probe
 * DDC/CI on all of them. Overlays keep their current dimming until the
 * probe answers, so nothing flashes on a rescan. */
/* Everything Windows knows about adapters and attached monitors, for the
 * log: it shows virtual displays, clone setups, and inactive entries that
 * explain odd enumeration results. */
static void LogDisplayDevices(void) {
    DISPLAY_DEVICEW adapter;
    for (DWORD i = 0; i < 16; i++) {
        ZeroMemory(&adapter, sizeof(adapter));
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(NULL, i, &adapter, 0)) break;
        DebugPrint(L"[ENUM] adapter %lu: %s \"%s\" flags 0x%08lX%s%s id %s\n", (unsigned long)i,
                   adapter.DeviceName, adapter.DeviceString, (unsigned long)adapter.StateFlags,
                   (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? L" (attached)" : L"",
                   (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? L" (primary)" : L"",
                   adapter.DeviceID);
        DISPLAY_DEVICEW monitor;
        for (DWORD j = 0; j < 8; j++) {
            ZeroMemory(&monitor, sizeof(monitor));
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(adapter.DeviceName, j, &monitor, EDD_GET_DEVICE_INTERFACE_NAME)) break;
            DebugPrint(L"[ENUM]   monitor %lu.%lu: %s \"%s\" flags 0x%08lX%s id %s\n", (unsigned long)i,
                       (unsigned long)j, monitor.DeviceName, monitor.DeviceString,
                       (unsigned long)monitor.StateFlags,
                       (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) ? L" (active)" : L"",
                       monitor.DeviceID);
        }
    }
}

static void RefreshMonitors(void) {
    DebugPrint(L"[INFO] Refreshing monitors (%d known so far)\n", g_monitorCount);
    LogDisplayDevices();
    EnumContext ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    EnumDisplayMonitors(NULL, NULL, EnumMonitorProc, (LPARAM)&ctx);
    DebugPrint(L"[ENUM] EnumDisplayMonitors: %d display(s)\n", ctx.count);

    Monitor fresh[MAX_MONITORS];
    int freshCount = 0;
    ZeroMemory(fresh, sizeof(fresh));
    for (int i = 0; i < ctx.count; i++) {
        EnumEntry* e = &ctx.entries[i];
        DWORD physCount = 0;
        BOOL countOk = GetNumberOfPhysicalMonitorsFromHMONITOR(e->hmon, &physCount);
        DebugPrint(L"[ENUM] display %d: hmon=%p %s rect (%ld,%ld)-(%ld,%ld)%s, physical monitors %lu (%s, error %lu)\n",
                   i, (void*)e->hmon, e->info.szDevice,
                   (long)e->info.rcMonitor.left, (long)e->info.rcMonitor.top,
                   (long)e->info.rcMonitor.right, (long)e->info.rcMonitor.bottom,
                   (e->info.dwFlags & MONITORINFOF_PRIMARY) ? L" primary" : L"",
                   (unsigned long)physCount, countOk ? L"ok" : L"FAILED",
                   (unsigned long)(countOk ? 0 : GetLastError()));
        if (!countOk || physCount == 0) {
            physCount = 1;
        }
        if (physCount > MAX_PHYSICAL_PER_DISPLAY) physCount = MAX_PHYSICAL_PER_DISPLAY;
        for (DWORD p = 0; p < physCount && freshCount < MAX_MONITORS; p++) {
            Monitor* m = &fresh[freshCount++];
            m->hmon = e->hmon;
            m->physicalIndex = (int)p;
            m->rect = e->info.rcMonitor;
            m->primary = (e->info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            wcscpy_s(m->device, CCHDEVICENAME, e->info.szDevice);
            ResolveMonitorIdentity(e->info.szDevice, (int)p, m);
            m->hardwareState = HW_UNKNOWN;
            m->lastHwSent = -1;
        }
    }

    /* Two monitors can end up with the same key (identical models with a
     * blank serial on one port each); make later ones unique by connector. */
    for (int i = 1; i < freshCount; i++) {
        for (int j = 0; j < i; j++) {
            if (wcscmp(fresh[i].key, fresh[j].key) == 0) {
                wchar_t unique[128];
                const wchar_t* tail = wcsrchr(fresh[i].device, L'\\');
                swprintf_s(unique, 128, L"%s_%s_%d", fresh[i].key,
                           tail ? tail + 1 : fresh[i].device, fresh[i].physicalIndex);
                wcscpy_s(fresh[i].key, 128, unique);
                SanitizeKeyChars(fresh[i].key);
                break;
            }
        }
    }

    Monitor merged[MAX_MONITORS];
    ZeroMemory(merged, sizeof(merged));
    for (int i = 0; i < freshCount; i++) {
        Monitor* dst = &merged[i];
        *dst = fresh[i];
        Monitor* old = NULL;
        for (int j = 0; j < g_monitorCount; j++) {
            if (wcscmp(g_monitors[j].key, dst->key) == 0) {
                old = &g_monitors[j];
                break;
            }
        }
        if (old) {
            dst->uid = old->uid;
            /* Keep the last known hardware state while the re-probe runs so
             * the dialog does not flash "detecting" on every retry. */
            dst->hardwareState = old->hardwareState;
            dst->ddcMax = old->ddcMax;
            dst->ddcCurrent = old->ddcCurrent;
            dst->value = old->value;
            dst->hasValue = old->hasValue;
            dst->forceSoftware = old->forceSoftware;
            dst->hidden = old->hidden;
            dst->hasOriginal = old->hasOriginal;
            dst->knownHardware = old->knownHardware;
            dst->probeFailures = old->probeFailures;
            dst->originalRaw = old->originalRaw;
            dst->originalMax = old->originalMax;
            dst->overlay = old->overlay;
            dst->overlayDim = old->overlayDim;
            dst->dirty = old->dirty;
            old->overlay = NULL;   /* ownership moved */
        } else {
            dst->uid = g_nextUid++;
            LoadMonitorSettings(dst);
        }
    }
    for (int j = 0; j < g_monitorCount; j++) {
        if (g_monitors[j].overlay) {
            DestroyWindow(g_monitors[j].overlay);
            g_monitors[j].overlay = NULL;
        }
    }
    memcpy(g_monitors, merged, sizeof(merged));
    g_monitorCount = freshCount;

    DdcProbeEntry probe[MAX_MONITORS];
    int probeCount = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (m->physicalIndex == 0 && !m->hidden) {
            if (!m->overlay) m->overlay = CreateOverlayWindow(&m->rect);
            else PositionOverlay(m);
            if (m->overlayDim > 0) SetOverlayDim(m, m->overlayDim);
        } else {
            ReleaseMonitorOverlay(m);
        }
        /* Hidden monitors are not even probed; they are left alone until a
         * rescan brings them back. Monitors that have answered before go
         * first so their results are not held up behind a display that
         * never answers and burns through every retry. */
        if (!m->hidden) {
            int at = probeCount;
            if (m->knownHardware) {
                at = 0;
                while (at < probeCount) {
                    Monitor* other = FindMonitorByUid(probe[at].uid);
                    if (!other || !other->knownHardware) break;
                    at++;
                }
                memmove(&probe[at + 1], &probe[at], sizeof(DdcProbeEntry) * (size_t)(probeCount - at));
            }
            probe[at].uid = m->uid;
            probe[at].hmon = m->hmon;
            probe[at].physicalIndex = m->physicalIndex;
            probe[at].patient = m->knownHardware;
            probeCount++;
        }
        DebugPrint(L"[INFO] Monitor %d: %s (%s, %ldx%ld%s) key=%s value=%d%s state=%s knownHardware=%d%s%s%s original=%s%lu/%lu\n",
                   m->uid, m->name, m->device,
                   (long)(m->rect.right - m->rect.left), (long)(m->rect.bottom - m->rect.top),
                   m->primary ? L", primary" : L"", m->key,
                   m->hasValue ? m->value : -1000,
                   m->hasValue ? L"" : L" (none saved)",
                   HardwareStateName(m->hardwareState), m->knownHardware,
                   m->forceSoftware ? L", software only" : L"",
                   m->hidden ? L", hidden" : L"",
                   m->scheduled ? L", scheduled" : L"",
                   m->hasOriginal ? L"" : L"none ",
                   (unsigned long)m->originalRaw, (unsigned long)m->originalMax);
    }
    DebugPrint(L"[INFO] %d monitor(s) enumerated, %d hidden; probing DDC/CI\n",
               g_monitorCount, g_monitorCount - probeCount);
    DdcRequestProbe(probe, probeCount);
    PushMonitorsToDialog();
}

static int VisibleMonitorCount(void) {
    int count = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        if (!g_monitors[i].hidden) count++;
    }
    return count;
}

/* A rescan brings every monitor back, including hidden ones that are not
 * connected right now, so the stored flag is cleared for all known
 * monitors rather than only the current list. */
static void UnhideAllMonitors(void) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].hidden) {
            g_monitors[i].hidden = FALSE;
            g_monitors[i].dirty = TRUE;
        }
    }
    PersistDirtyMonitors();

    HKEY hMonitors;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH L"\\" REG_MONITORS_SUBKEY, 0,
                      KEY_READ, &hMonitors) != ERROR_SUCCESS) {
        return;
    }
    for (DWORD index = 0;; index++) {
        wchar_t name[256];
        DWORD nameCount = sizeof(name) / sizeof(wchar_t);
        if (RegEnumKeyExW(hMonitors, index, name, &nameCount, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
            break;
        }
        HKEY hMonitor;
        if (RegOpenKeyExW(hMonitors, name, 0, KEY_SET_VALUE, &hMonitor) == ERROR_SUCCESS) {
            WriteRegistryDword(hMonitor, REG_VALUE_MON_HIDDEN, 0);
            RegCloseKey(hMonitor);
        }
    }
    RegCloseKey(hMonitors);
}

/* Right after a probe the mode is known, so a scheduled monitor can take
 * the schedule's current value instead of first restoring a stale one. */
static void ApplyScheduleAfterProbe(Monitor* m) {
    int target;
    if (!m->scheduled || m->hidden || !ScheduleTargetNow(&target)) return;
    if (IsSchedulePaused(m, NowFileTime())) return;
    int value = ClampMonitorValue(m, target);
    if (!m->hasValue || m->value != value) {
        m->value = value;
        m->hasValue = TRUE;
        m->dirty = TRUE;
        SchedulePersist();
        DebugPrint(L"[INFO] %s (%s): schedule sets %d%% after probe\n", m->name, m->device, value);
    }
}

static void HandleDdcProbed(DdcProbeResult* r) {
    Monitor* m = FindMonitorByUid(r->uid);
    DebugPrint(L"[INFO] Probe result for uid %d (%s): supported=%d highLevel=%d min=%lu current=%lu max=%lu error=%lu elapsed=%lu ms\n",
               r->uid, m ? m->name : L"unknown monitor", r->supported, r->highLevel,
               (unsigned long)r->min, (unsigned long)r->current, (unsigned long)r->max,
               (unsigned long)r->error, (unsigned long)r->elapsedMs);
    if (m) {
        BrightnessMode before = MonitorMode(m);
        if (r->supported) {
            m->hardwareState = HW_AVAILABLE;
            m->ddcMax = r->max;
            m->ddcMin = r->highLevel ? r->min : 0;
            m->ddcHighLevel = r->highLevel;
            m->ddcCurrent = r->current;
            m->failures = 0;
            m->error[0] = L'\0';
            if (m->probeFailures > 0) {
                DebugPrint(L"[INFO] %s (%s): DDC/CI answering again after %d failed probe(s)\n",
                           m->name, m->device, m->probeFailures);
                m->probeFailures = 0;
                g_ddcRetryDelayMs = DDC_RETRY_INITIAL_MS;
            }
            if (!m->knownHardware) {
                m->knownHardware = TRUE;
                m->dirty = TRUE;
                SchedulePersist();
            }
            DWORD range = r->max > m->ddcMin ? r->max - m->ddcMin : 100;
            DWORD above = r->current > m->ddcMin ? r->current - m->ddcMin : 0;
            int percent = (int)((above * 100 + range / 2) / range);
            if (percent > 100) percent = 100;
            if (!m->hasOriginal) {
                /* The value the monitor had before this application ever
                 * wrote to it; kept for good so hiding can put it back. */
                m->hasOriginal = TRUE;
                m->originalRaw = r->current;
                m->originalMax = r->max;
                SaveMonitorSettings(m);
                DebugPrint(L"[INFO] %s (%s): recorded original brightness %lu/%lu\n",
                           m->name, m->device, (unsigned long)r->current,
                           (unsigned long)r->max);
            }
            if (!m->hasValue) {
                /* First sighting: adopt the monitor's current setting so
                 * nothing changes until the user moves the slider. */
                m->value = percent;
                m->hasValue = TRUE;
                m->dirty = TRUE;
                SchedulePersist();
            }
            ApplyScheduleAfterProbe(m);
            /* Already at the wanted level: no write needed. */
            m->lastHwSent = (m->value == percent) ? percent : -1;
            DebugPrint(L"[INFO] %s (%s): DDC/CI brightness available, current %lu/%lu (%lu ms)\n",
                       m->name, m->device, (unsigned long)r->current,
                       (unsigned long)r->max, (unsigned long)r->elapsedMs);
        } else {
            m->hardwareState = HW_UNAVAILABLE;
            m->probeFailures++;
            if (!m->hasValue) {
                m->value = 100;
                m->hasValue = TRUE;
                m->dirty = TRUE;
                SchedulePersist();
            }
            ApplyScheduleAfterProbe(m);
            if (m->knownHardware && !m->forceSoftware) {
                /* The dialog explains the waiting state itself. */
                m->error[0] = L'\0';
                DebugPrint(L"[WARNING] %s (%s): DDC/CI not answering (error %lu, %lu ms, failure %d); "
                           L"leaving the monitor alone and retrying\n",
                           m->name, m->device, (unsigned long)r->error,
                           (unsigned long)r->elapsedMs, m->probeFailures);
                ScheduleDdcRetry();
            } else {
                DebugPrint(L"[INFO] %s (%s): no DDC/CI brightness (error %lu, %lu ms); using software dimming\n",
                           m->name, m->device, (unsigned long)r->error,
                           (unsigned long)r->elapsedMs);
                if (!m->forceSoftware && m->probeFailures <= DDC_UNKNOWN_MONITOR_RETRIES) ScheduleDdcRetry();
            }
        }
        if (!r->supported) m->lastHwSent = -1;
        BrightnessMode after = MonitorMode(m);
        DebugPrint(L"[INFO] %s (%s): mode %s -> %s, value %d, lastHwSent %d, knownHardware %d, probeFailures %d\n",
                   m->name, m->device, ModeName(before), ModeName(after), m->value,
                   m->lastHwSent, m->knownHardware, m->probeFailures);
        ApplyMonitor(m);
        PushMonitorsToDialog();
    }
    free(r);
}

static void HandleDdcSetResult(int uid, BOOL success) {
    Monitor* m = FindMonitorByUid(uid);
    if (!m) return;
    if (m->hidden) {
        /* Only the restore-before-hide write reaches a hidden monitor. */
        DebugPrint(L"[%s] %s (%s): original brightness %s before hiding\n",
                   success ? L"INFO" : L"WARNING", m->name, m->device,
                   success ? L"restored" : L"could not be restored");
        return;
    }
    DebugPrint(L"[INFO] Write result for %s (%s): %s (lastHwSent %d, failures so far %d)\n",
               m->name, m->device, success ? L"ok" : L"FAILED", m->lastHwSent, m->failures);
    if (success) {
        m->failures = 0;
        if (m->lastHwSent >= 0) {
            DWORD range = m->ddcMax > m->ddcMin ? m->ddcMax - m->ddcMin : 100;
            m->ddcCurrent = m->ddcMin + (DWORD)m->lastHwSent * range / 100;
        }
        if (m->error[0]) {
            m->error[0] = L'\0';
            PushMonitorsToDialog();
        }
        return;
    }
    m->failures++;
    m->lastHwSent = -1;   /* the next change retries instead of being skipped */
    if (m->failures >= DDC_MAX_CONSECUTIVE_FAILURES && m->hardwareState == HW_AVAILABLE) {
        m->hardwareState = HW_UNAVAILABLE;
        m->error[0] = L'\0';
        DebugPrint(L"[WARNING] %s (%s): %d consecutive DDC/CI failures; leaving the monitor alone and retrying\n",
                   m->name, m->device, m->failures);
        ScheduleDdcRetry();
    } else {
        wcscpy_s(m->error, sizeof(m->error) / sizeof(wchar_t),
                 L"The monitor did not accept the last brightness change.");
    }
    PushMonitorsToDialog();
}

/* ── Automatic brightness schedule ───────────────────────────────────────── */

#define PI_D 3.14159265358979323846
#define DEG2RAD(d) ((d) * PI_D / 180.0)
#define RAD2DEG(r) ((r) * 180.0 / PI_D)

static double JulianDay(int year, int month, int day) {
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    int a = year / 100;
    int b = 2 - a + a / 4;
    return floor(365.25 * (year + 4716)) + floor(30.6001 * (month + 1)) + day + b - 1524.5;
}

static int DayNumber(const SYSTEMTIME* st) {
    return (int)floor(JulianDay(st->wYear, st->wMonth, st->wDay) + 0.5);
}

/* NOAA solar position equations: solar noon (minutes after 0h UTC of the
 * date) and the sunrise/sunset hour angle in degrees. cosHA outside -1..1
 * means the sun never rises (> 1) or never sets (< -1) that day. */
static void SolarNoonAndHourAngle(double latitude, double longitude, double julianDay,
                                  double* noonUtcMinutes, double* cosHourAngle, double* hourAngle) {
    double t = (julianDay - 2451545.0) / 36525.0;
    double l0 = fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    if (l0 < 0) l0 += 360.0;
    double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    double mr = DEG2RAD(m);
    double c = sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
               sin(2 * mr) * (0.019993 - 0.000101 * t) + sin(3 * mr) * 0.000289;
    double trueLongitude = l0 + c;
    double omega = 125.04 - 1934.136 * t;
    double lambda = trueLongitude - 0.00569 - 0.00478 * sin(DEG2RAD(omega));
    double epsilon0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    double epsilon = epsilon0 + 0.00256 * cos(DEG2RAD(omega));
    double declination = asin(sin(DEG2RAD(epsilon)) * sin(DEG2RAD(lambda)));
    double y = tan(DEG2RAD(epsilon) / 2.0);
    y *= y;
    double l0r = DEG2RAD(l0);
    double equationOfTime = 4.0 * RAD2DEG(
        y * sin(2 * l0r) - 2 * e * sin(mr) + 4 * e * y * sin(mr) * cos(2 * l0r) -
        0.5 * y * y * sin(4 * l0r) - 1.25 * e * e * sin(2 * mr));
    *noonUtcMinutes = 720.0 - 4.0 * longitude - equationOfTime;
    double latr = DEG2RAD(latitude);
    /* 90.833 degrees: the sun's upper limb at the horizon with refraction. */
    double cosHa = cos(DEG2RAD(90.833)) / (cos(latr) * cos(declination)) - tan(latr) * tan(declination);
    *cosHourAngle = cosHa;
    *hourAngle = (cosHa >= -1.0 && cosHa <= 1.0) ? RAD2DEG(acos(cosHa)) : 0.0;
}

/* Minutes after local midnight of localDate for the instant "0h UTC of
 * the same calendar date plus utcMinutes", using the current time zone
 * rules (so DST is right). May be negative or beyond 1439. */
static int UtcMinutesToLocalMinutes(const SYSTEMTIME* localDate, double utcMinutes) {
    SYSTEMTIME utcMidnight;
    ZeroMemory(&utcMidnight, sizeof(utcMidnight));
    utcMidnight.wYear = localDate->wYear;
    utcMidnight.wMonth = localDate->wMonth;
    utcMidnight.wDay = localDate->wDay;
    FILETIME ft;
    if (!SystemTimeToFileTime(&utcMidnight, &ft)) return (int)lround(utcMinutes);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    LONGLONG offset = (LONGLONG)(utcMinutes * 60.0 * 10000000.0);
    u.QuadPart = (ULONGLONG)((LONGLONG)u.QuadPart + offset);
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME utcInstant, localInstant;
    if (!FileTimeToSystemTime(&ft, &utcInstant) ||
        !SystemTimeToTzSpecificLocalTime(NULL, &utcInstant, &localInstant)) {
        return (int)lround(utcMinutes);
    }
    int dayDifference = DayNumber(&localInstant) - DayNumber(localDate);
    return dayDifference * 1440 + localInstant.wHour * 60 + localInstant.wMinute;
}

static void ComputeSolarDay(double latitude, double longitude, const SYSTEMTIME* localDate, SolarDay* out) {
    ZeroMemory(out, sizeof(*out));
    double jd = JulianDay(localDate->wYear, localDate->wMonth, localDate->wDay);
    double noonUtc, cosHa, ha;
    SolarNoonAndHourAngle(latitude, longitude, jd, &noonUtc, &cosHa, &ha);
    out->noon = UtcMinutesToLocalMinutes(localDate, noonUtc);
    if (cosHa > 1.0) {
        out->polar = -1;
        out->sunrise = out->sunset = out->noon;
    } else if (cosHa < -1.0) {
        out->polar = 1;
        out->sunrise = out->sunset = out->noon;
    } else {
        out->sunrise = UtcMinutesToLocalMinutes(localDate, noonUtc - ha * 4.0);
        out->sunset = UtcMinutesToLocalMinutes(localDate, noonUtc + ha * 4.0);
    }
}

/* The four anchors in local minutes, forced into order with a small gap so
 * a dragged transition can never invert. */
static void ScheduleAnchors(const Schedule* sc, const SolarDay* day, int anchors[4]) {
    anchors[0] = day->sunrise + sc->dawnStartOffset;
    anchors[1] = day->sunrise + sc->dawnEndOffset;
    anchors[2] = day->sunset + sc->duskStartOffset;
    anchors[3] = day->sunset + sc->duskEndOffset;
    for (int i = 1; i < 4; i++) {
        if (anchors[i] < anchors[i - 1] + SCHEDULE_MIN_GAP) anchors[i] = anchors[i - 1] + SCHEDULE_MIN_GAP;
    }
}

static double SmoothStep(double x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return x * x * (3 - 2 * x);
}

/* Brightness at a local time (minutes, fractional) on the given day. */
static int ScheduleValueAt(const Schedule* sc, const SolarDay* day, double minutes) {
    if (day->polar > 0) return sc->dayLevel;
    if (day->polar < 0) return sc->nightLevel;
    int a[4];
    ScheduleAnchors(sc, day, a);
    /* A dusk that ends after midnight (or a dawn that starts before it)
     * belongs to the neighbouring calendar day; test the shifted times too. */
    double candidates[3] = { minutes, minutes + 1440, minutes - 1440 };
    double t = minutes;
    for (int i = 0; i < 3; i++) {
        if (candidates[i] >= a[0] && candidates[i] <= a[3]) {
            t = candidates[i];
            break;
        }
    }
    double night = sc->nightLevel, dayLevel = sc->dayLevel, v;
    if (t <= a[0] || t >= a[3]) {
        v = night;
    } else if (t < a[1]) {
        v = night + (dayLevel - night) * SmoothStep((t - a[0]) / (double)(a[1] - a[0]));
    } else if (t <= a[2]) {
        v = dayLevel;
    } else {
        v = dayLevel + (night - dayLevel) * SmoothStep((t - a[2]) / (double)(a[3] - a[2]));
    }
    return (int)lround(v);
}

static ULONGLONG NowFileTime(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

/* The next occurrence of the cycle reset time of day, as a UTC FILETIME. */
static ULONGLONG NextCycleResetFileTime(void) {
    SYSTEMTIME local;
    GetLocalTime(&local);
    int nowMinutes = local.wHour * 60 + local.wMinute;
    SYSTEMTIME reset = local;
    reset.wHour = (WORD)(g_config.schedule.cycleResetMinutes / 60);
    reset.wMinute = (WORD)(g_config.schedule.cycleResetMinutes % 60);
    reset.wSecond = 0;
    reset.wMilliseconds = 0;
    FILETIME ft;
    if (!SystemTimeToFileTime(&reset, &ft)) return 0;   /* treated as a local wall clock */
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (g_config.schedule.cycleResetMinutes <= nowMinutes) {
        u.QuadPart += 24ULL * 60 * 60 * 10000000;   /* tomorrow */
    }
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME localReset, utcReset;
    if (!FileTimeToSystemTime(&ft, &localReset) ||
        !TzSpecificLocalTimeToSystemTime(NULL, &localReset, &utcReset) ||
        !SystemTimeToFileTime(&utcReset, &ft)) {
        return 0;
    }
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static void FormatLocalTimeOfDay(ULONGLONG ft, wchar_t* out, size_t count) {
    out[0] = L'\0';
    if (!ft) return;
    FILETIME f;
    ULARGE_INTEGER u;
    u.QuadPart = ft;
    f.dwLowDateTime = u.LowPart;
    f.dwHighDateTime = u.HighPart;
    SYSTEMTIME utc, local;
    if (FileTimeToSystemTime(&f, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) {
        swprintf_s(out, count, L"%02u:%02u", (unsigned)local.wHour, (unsigned)local.wMinute);
    }
}

static BOOL IsSchedulePaused(const Monitor* m, ULONGLONG nowFt) {
    return m->pausedUntil != 0 && m->pausedUntil > nowFt;
}

/* The value the schedule wants right now, or FALSE when it is off. */
static BOOL ScheduleTargetNow(int* value) {
    const Schedule* sc = &g_config.schedule;
    if (!sc->enabled || !sc->hasLocation) return FALSE;
    SYSTEMTIME now;
    GetLocalTime(&now);
    SolarDay day;
    ComputeSolarDay(sc->latitude, sc->longitude, &now, &day);
    double minutes = now.wHour * 60 + now.wMinute + now.wSecond / 60.0;
    *value = ScheduleValueAt(sc, &day, minutes);
    return TRUE;
}

/* Applies the current schedule value to every scheduled, unpaused monitor.
 * Runs on the schedule timer and after anything that changes the inputs. */
static void EvaluateSchedule(void) {
    int target;
    if (!ScheduleTargetNow(&target)) return;
    ULONGLONG now = NowFileTime();
    BOOL changed = FALSE;
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (!m->scheduled || m->hidden) continue;
        if (m->pausedUntil) {
            if (m->pausedUntil > now) continue;
            m->pausedUntil = 0;
            m->dirty = TRUE;
            changed = TRUE;
            DebugPrint(L"[INFO] %s (%s): automatic brightness resumed\n", m->name, m->device);
        }
        BrightnessMode mode = MonitorMode(m);
        if (mode == MODE_PROBING || mode == MODE_WAITING) continue;
        int value = ClampMonitorValue(m, target);
        if (!m->hasValue || m->value != value) {
            DebugPrint(L"[INFO] %s (%s): schedule sets %d%%\n", m->name, m->device, value);
            m->value = value;
            m->hasValue = TRUE;
            m->dirty = TRUE;
            ApplyMonitor(m);
            changed = TRUE;
        }
    }
    if (changed) {
        SchedulePersist();
        PushMonitorsToDialog();
    }
}

/* A manual brightness change on a scheduled monitor suspends automation
 * for it until the next cycle reset time. */
static void NoteManualChange(Monitor* m) {
    if (!g_config.schedule.enabled || !m->scheduled || m->hidden) return;
    ULONGLONG now = NowFileTime();
    if (IsSchedulePaused(m, now)) return;
    m->pausedUntil = NextCycleResetFileTime();
    m->dirty = TRUE;
    SchedulePersist();
    wchar_t until[16];
    FormatLocalTimeOfDay(m->pausedUntil, until, 16);
    DebugPrint(L"[INFO] %s (%s): manual change; automatic brightness paused until %s\n",
               m->name, m->device, until);
    PushMonitorsToDialog();
}

static void UpdateScheduleTimer(void) {
    if (!g_hwnd) return;
    if (g_config.schedule.enabled && g_config.schedule.hasLocation) {
        SetTimer(g_hwnd, ID_TIMER_SCHEDULE, SCHEDULE_INTERVAL_MS, NULL);
    } else {
        KillTimer(g_hwnd, ID_TIMER_SCHEDULE);
    }
}

/* ── Self update (ported from SystrayLauncher) ───────────────────────────── */

// --- Self update -----------------------------------------------------------

typedef struct {
    HINTERNET session;
    HINTERNET connection;
    HINTERNET request;
} UpdateHttpRequest;

static void CloseUpdateHttpRequest(UpdateHttpRequest* http) {
    if (!http) return;
    if (http->request) WinHttpCloseHandle(http->request);
    if (http->connection) WinHttpCloseHandle(http->connection);
    if (http->session) WinHttpCloseHandle(http->session);
    ZeroMemory(http, sizeof(*http));
}

static void SetUpdateTaskError(UpdateCheckTask* task, LPCWSTR message,
                               DWORD errorCode) {
    if (!task) return;
    task->kind = UPDATE_CHECK_ERROR;
    if (errorCode) {
        swprintf_s(task->message, sizeof(task->message) / sizeof(wchar_t),
                   L"%s (Windows error %lu).", message, (unsigned long)errorCode);
    } else {
        wcscpy_s(task->message, sizeof(task->message) / sizeof(wchar_t), message);
    }
}

static BOOL CancelUpdateTaskIfRequested(UpdateCheckTask* task) {
    if (!task || !g_updateCancelEvent ||
        WaitForSingleObject(g_updateCancelEvent, 0) != WAIT_OBJECT_0) {
        return FALSE;
    }
    task->kind = UPDATE_CHECK_CANCELLED;
    task->message[0] = L'\0';
    return TRUE;
}

// Sample received bytes over monotonic milliseconds; round half up to whole
// kilobytes/second. Download sizes are bounded by UPDATE_MAX_BYTES.
static DWORD CalculateUpdateSpeedKbps(ULONGLONG receivedBytes,
                                      ULONGLONG elapsedMs) {
    if (!elapsedMs) return 0;
    ULONGLONG divisor = elapsedMs * 1024ULL;
    ULONGLONG speed = (receivedBytes * 1000ULL + divisor / 2) / divisor;
    return speed > MAXLONG ? MAXLONG : (DWORD)speed;
}

static void PublishUpdateProgress(UpdateCheckTask* task, DWORD speedKbps) {
    if (!task || CancelUpdateTaskIfRequested(task) ||
        !IsWindow(task->targetWindow)) {
        return;
    }

    InterlockedExchange(&g_updateSpeedKbps, (LONG)speedKbps);
    if (InterlockedCompareExchange(&g_updateProgressPosted, TRUE, FALSE) == FALSE &&
        !PostMessageW(task->targetWindow, WM_APP_UPDATE_PROGRESS, 0, 0)) {
        InterlockedExchange(&g_updateProgressPosted, FALSE);
    }
}

static BOOL OpenUpdateHttpRequest(LPCWSTR verb, ULONGLONG cacheBuster,
                                  UpdateHttpRequest* http, DWORD* statusCode) {
    if (!verb || !http) return FALSE;
    ZeroMemory(http, sizeof(*http));
    if (statusCode) *statusCode = 0;

    wchar_t hostName[256] = L"";
    wchar_t urlPath[2048] = L"";
    wchar_t extraInfo[512] = L"";
    URL_COMPONENTS components = {0};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = hostName;
    components.dwHostNameLength = sizeof(hostName) / sizeof(wchar_t);
    components.lpszUrlPath = urlPath;
    components.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);
    components.lpszExtraInfo = extraInfo;
    components.dwExtraInfoLength = sizeof(extraInfo) / sizeof(wchar_t);
    if (!WinHttpCrackUrl(UPDATE_URL, 0, 0, &components)) return FALSE;

    wchar_t objectName[2560];
    if (wcscpy_s(objectName, sizeof(objectName) / sizeof(wchar_t), urlPath) != 0 ||
        wcscat_s(objectName, sizeof(objectName) / sizeof(wchar_t), extraInfo) != 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    // A unique query value makes each button click reach the current branch
    // artifact even when an HTTP proxy or GitHub edge cache retains the
    // previous response. HEAD and GET share the same value within one check.
    wchar_t cacheSuffix[64];
    wchar_t separator = wcschr(objectName, L'?') ? L'&' : L'?';
    int cacheSuffixLength = swprintf_s(cacheSuffix,
        sizeof(cacheSuffix) / sizeof(wchar_t), L"%lcntbUpdate=%016llx",
        separator, (unsigned long long)cacheBuster);
    if (cacheSuffixLength <= 0 ||
        wcscat_s(objectName, sizeof(objectName) / sizeof(wchar_t), cacheSuffix) != 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    http->session = WinHttpOpen(L"NotTooBright Update",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!http->session) goto fail;
    WinHttpSetTimeouts(http->session, 10000, 10000, 15000, 30000);

    http->connection = WinHttpConnect(http->session, hostName,
                                      components.nPort, 0);
    if (!http->connection) goto fail;

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (components.nScheme == INTERNET_SCHEME_HTTPS) flags |= WINHTTP_FLAG_SECURE;
    http->request = WinHttpOpenRequest(http->connection, verb, objectName,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!http->request) goto fail;

    static const wchar_t noCacheHeaders[] =
        L"Cache-Control: no-cache, no-store, max-age=0\r\nPragma: no-cache\r\n";
    WinHttpAddRequestHeaders(http->request, noCacheHeaders, (DWORD)-1L,
                            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (!WinHttpSendRequest(http->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(http->request, NULL)) {
        goto fail;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(http->request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        goto fail;
    }
    if (statusCode) *statusCode = status;
    if (status != 200) {
        CloseUpdateHttpRequest(http);
        SetLastError(ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
        return FALSE;
    }
    return TRUE;

fail: {
        DWORD errorCode = GetLastError();
        CloseUpdateHttpRequest(http);
        SetLastError(errorCode);
        return FALSE;
    }
}

static BOOL QueryUpdateContentLength(HINTERNET request, ULONGLONG* size) {
    if (!request || !size) return FALSE;
    wchar_t lengthText[64] = L"";
    DWORD lengthBytes = sizeof(lengthText);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return FALSE;
    }
    lengthText[(sizeof(lengthText) / sizeof(wchar_t)) - 1] = L'\0';

    wchar_t* end = NULL;
    unsigned long long parsed = _wcstoui64(lengthText, &end, 10);
    if (end == lengthText || !end || *end != L'\0' || parsed == 0) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    *size = (ULONGLONG)parsed;
    return TRUE;
}

static BOOL GetExecutableVersion(LPCWSTR path, ExecutableVersion* version) {
    if (!path || !*path || !version) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD ignored = 0;
    DWORD infoSize = GetFileVersionInfoSizeW(path, &ignored);
    if (infoSize == 0) {
        DWORD errorCode = GetLastError();
        SetLastError(errorCode ? errorCode : ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    BYTE* infoData = (BYTE*)malloc(infoSize);
    if (!infoData) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (!GetFileVersionInfoW(path, 0, infoSize, infoData)) {
        DWORD errorCode = GetLastError();
        free(infoData);
        SetLastError(errorCode ? errorCode : ERROR_INVALID_DATA);
        return FALSE;
    }

    VS_FIXEDFILEINFO* fixedInfo = NULL;
    UINT fixedInfoSize = 0;
    if (!VerQueryValueW(infoData, L"\\", (LPVOID*)&fixedInfo, &fixedInfoSize) ||
        !fixedInfo || fixedInfoSize < sizeof(*fixedInfo) ||
        fixedInfo->dwSignature != VS_FFI_SIGNATURE) {
        free(infoData);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    version->major = HIWORD(fixedInfo->dwFileVersionMS);
    version->minor = LOWORD(fixedInfo->dwFileVersionMS);
    version->patch = HIWORD(fixedInfo->dwFileVersionLS);
    version->build = LOWORD(fixedInfo->dwFileVersionLS);
    free(infoData);
    return TRUE;
}

static int CompareExecutableVersions(const ExecutableVersion* left,
                                     const ExecutableVersion* right) {
    const WORD leftParts[] = {
        left->major, left->minor, left->patch, left->build
    };
    const WORD rightParts[] = {
        right->major, right->minor, right->patch, right->build
    };
    for (size_t index = 0; index < sizeof(leftParts) / sizeof(leftParts[0]); index++) {
        if (leftParts[index] < rightParts[index]) return -1;
        if (leftParts[index] > rightParts[index]) return 1;
    }
    return 0;
}

static void FormatExecutableVersion(const ExecutableVersion* version,
                                    wchar_t* text, size_t textCch) {
    if (!version || !text || textCch == 0) return;
    if (swprintf_s(text, textCch, L"%u.%u.%u.%u",
                   (unsigned int)version->major,
                   (unsigned int)version->minor,
                   (unsigned int)version->patch,
                   (unsigned int)version->build) <= 0) {
        text[0] = L'\0';
    }
}

static void FormatExecutableVersionForDisplay(const ExecutableVersion* version,
                                              wchar_t* text, size_t textCch) {
    if (!version || !text || textCch == 0) return;
    if (version->build == 0) {
        if (swprintf_s(text, textCch, L"%u.%u.%u",
                       (unsigned int)version->major,
                       (unsigned int)version->minor,
                       (unsigned int)version->patch) <= 0) {
            text[0] = L'\0';
        }
        return;
    }
    FormatExecutableVersion(version, text, textCch);
}

static BOOL BuildUpdateTempPath(wchar_t path[MAX_PATH], LPCWSTR role,
                                DWORD processId) {
    if (!path || !role || !*role || processId == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    wchar_t tempDirectory[MAX_PATH];
    DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    if (tempLength == 0) return FALSE;
    if (tempLength >= MAX_PATH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    int length = swprintf_s(path, MAX_PATH, L"%s%s-%s-%lu.exe",
                            tempDirectory, APP_NAME, role,
                            (unsigned long)processId);
    if (length <= 0 || length >= MAX_PATH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return TRUE;
}

static BOOL DeleteUpdateTempFile(LPCWSTR path) {
    if (!path || !*path) return FALSE;
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path)) return TRUE;
    DWORD errorCode = GetLastError();
    return errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND;
}

static BOOL QueryRemoteUpdateSize(UpdateCheckTask* task, ULONGLONG* size) {
    if (CancelUpdateTaskIfRequested(task)) return FALSE;

    UpdateHttpRequest http;
    DWORD status = 0;
    if (!OpenUpdateHttpRequest(L"HEAD", task->cacheBuster, &http, &status)) {
        DWORD errorCode = GetLastError();
        if (CancelUpdateTaskIfRequested(task)) return FALSE;
        if (status) {
            swprintf_s(task->message, sizeof(task->message) / sizeof(wchar_t),
                       L"The update server returned HTTP status %lu.",
                       (unsigned long)status);
            task->kind = UPDATE_CHECK_ERROR;
        } else {
            SetUpdateTaskError(task, L"Could not contact the update server", errorCode);
        }
        return FALSE;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        CloseUpdateHttpRequest(&http);
        return FALSE;
    }

    BOOL ok = QueryUpdateContentLength(http.request, size);
    DWORD errorCode = ok ? ERROR_SUCCESS : GetLastError();
    CloseUpdateHttpRequest(&http);
    if (CancelUpdateTaskIfRequested(task)) return FALSE;
    if (!ok) {
        SetUpdateTaskError(task, L"The update server did not report a valid file size",
                           errorCode);
        return FALSE;
    }
    if (*size > UPDATE_MAX_BYTES) {
        SetUpdateTaskError(task, L"The available update is unexpectedly large", 0);
        return FALSE;
    }
    return TRUE;
}

static BOOL DownloadUpdateFile(UpdateCheckTask* task, ULONGLONG expectedSize) {
    if (CancelUpdateTaskIfRequested(task)) return FALSE;

    UpdateHttpRequest http;
    DWORD status = 0;
    if (!OpenUpdateHttpRequest(L"GET", task->cacheBuster, &http, &status)) {
        DWORD errorCode = GetLastError();
        if (CancelUpdateTaskIfRequested(task)) return FALSE;
        if (status) {
            swprintf_s(task->message, sizeof(task->message) / sizeof(wchar_t),
                       L"The update download returned HTTP status %lu.",
                       (unsigned long)status);
            task->kind = UPDATE_CHECK_ERROR;
        } else {
            SetUpdateTaskError(task, L"Could not download the update", errorCode);
        }
        return FALSE;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        CloseUpdateHttpRequest(&http);
        return FALSE;
    }

    ULONGLONG downloadSize = 0;
    if (QueryUpdateContentLength(http.request, &downloadSize) &&
        downloadSize != expectedSize) {
        CloseUpdateHttpRequest(&http);
        SetUpdateTaskError(task,
            L"The available update changed while it was being downloaded. Try again", 0);
        return FALSE;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        CloseUpdateHttpRequest(&http);
        return FALSE;
    }

    DeleteUpdateTempFile(task->stagedPath);
    HANDLE file = CreateFileW(task->stagedPath, GENERIC_WRITE, 0, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD errorCode = GetLastError();
        CloseUpdateHttpRequest(&http);
        SetUpdateTaskError(task, L"Could not create the staged update file", errorCode);
        return FALSE;
    }

    BOOL ok = TRUE;
    ULONGLONG totalWritten = 0;
    ULONGLONG speedWindowBytes = 0;
    ULONGLONG speedWindowStarted = GetTickCount64();
    BYTE buffer[64 * 1024];
    while (ok) {
        if (CancelUpdateTaskIfRequested(task)) {
            ok = FALSE;
            break;
        }

        DWORD bytesRead = 0;
        if (!WinHttpReadData(http.request, buffer, sizeof(buffer), &bytesRead)) {
            DWORD errorCode = GetLastError();
            if (!CancelUpdateTaskIfRequested(task)) {
                SetUpdateTaskError(task, L"The update download was interrupted", errorCode);
            }
            ok = FALSE;
            break;
        }
        if (CancelUpdateTaskIfRequested(task)) {
            ok = FALSE;
            break;
        }
        if (bytesRead == 0) break;
        if (totalWritten + bytesRead > expectedSize) {
            SetUpdateTaskError(task, L"The downloaded update has an invalid size", 0);
            ok = FALSE;
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(file, buffer, bytesRead, &bytesWritten, NULL)) {
            SetUpdateTaskError(task, L"Could not write the staged update", GetLastError());
            ok = FALSE;
            break;
        }
        if (bytesWritten != bytesRead) {
            SetUpdateTaskError(task, L"Could not write the staged update",
                               ERROR_WRITE_FAULT);
            ok = FALSE;
            break;
        }
        totalWritten += bytesWritten;
        speedWindowBytes += bytesRead;

        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsed = now - speedWindowStarted;
        if (elapsed >= UPDATE_PROGRESS_INTERVAL_MS) {
            PublishUpdateProgress(task,
                CalculateUpdateSpeedKbps(speedWindowBytes, elapsed));
            speedWindowBytes = 0;
            speedWindowStarted = now;
        }
    }

    if (ok && CancelUpdateTaskIfRequested(task)) ok = FALSE;
    if (ok && totalWritten != expectedSize) {
        SetUpdateTaskError(task, L"The downloaded update is incomplete", 0);
        ok = FALSE;
    }
    if (ok && !FlushFileBuffers(file)) {
        SetUpdateTaskError(task, L"Could not finish writing the staged update", GetLastError());
        ok = FALSE;
    }
    CloseHandle(file);
    CloseUpdateHttpRequest(&http);

    if (ok && CancelUpdateTaskIfRequested(task)) ok = FALSE;
    DWORD binaryType = 0;
    if (ok && (!GetBinaryTypeW(task->stagedPath, &binaryType) ||
               binaryType != SCS_64BIT_BINARY)) {
        SetUpdateTaskError(task, L"The downloaded file is not a valid 64-bit application", 0);
        ok = FALSE;
    }
    if (!ok) DeleteUpdateTempFile(task->stagedPath);
    return ok;
}

static void DiscardUpdateTask(UpdateCheckTask* task) {
    if (!task) return;
    if (task->stagedPath[0]) {
        DeleteUpdateTempFile(task->stagedPath);
    }
    free(task);
}

static void PublishUpdateTask(UpdateCheckTask* task) {
    CancelUpdateTaskIfRequested(task);
    InterlockedExchange(&g_updateCheckPending, FALSE);
    InterlockedExchange(&g_updateCheckAutomatic, FALSE);
    if (!task || !IsWindow(task->targetWindow)) {
        DiscardUpdateTask(task);
        return;
    }

    UpdateCheckTask* previous = (UpdateCheckTask*)InterlockedExchangePointer(
        (PVOID volatile*)&g_updatePostedResult, task);
    DiscardUpdateTask(previous);
    if (!PostMessageW(task->targetWindow, WM_APP_UPDATE_RESULT, 0, 0)) {
        UpdateCheckTask* unclaimed = (UpdateCheckTask*)InterlockedExchangePointer(
            (PVOID volatile*)&g_updatePostedResult, NULL);
        DiscardUpdateTask(unclaimed);
    }
}

static DWORD WINAPI UpdateCheckThread(LPVOID parameter) {
    UpdateCheckTask* task = (UpdateCheckTask*)parameter;
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    DWORD pathLength = GetModuleFileNameW(NULL, task->targetPath,
                                         sizeof(task->targetPath) / sizeof(wchar_t));
    if (pathLength == 0 || pathLength >= sizeof(task->targetPath) / sizeof(wchar_t)) {
        SetUpdateTaskError(task, L"Could not determine the running executable path",
                           GetLastError());
        PublishUpdateTask(task);
        return 0;
    }

    if (!GetExecutableVersion(task->targetPath, &task->runningVersion)) {
        SetUpdateTaskError(task, L"Could not read the running application version",
                           GetLastError());
        PublishUpdateTask(task);
        return 0;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    ULONGLONG remoteSize = 0;
    if (!QueryRemoteUpdateSize(task, &remoteSize)) {
        PublishUpdateTask(task);
        return 0;
    }
    if (!BuildUpdateTempPath(task->stagedPath, L"download",
                             GetCurrentProcessId())) {
        SetUpdateTaskError(task, L"Could not create the temporary update path",
                           GetLastError());
        PublishUpdateTask(task);
        return 0;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    if (!DownloadUpdateFile(task, remoteSize)) {
        PublishUpdateTask(task);
        return 0;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    if (!GetExecutableVersion(task->stagedPath, &task->availableVersion)) {
        SetUpdateTaskError(task,
            L"The downloaded application does not contain valid version information",
            GetLastError());
        PublishUpdateTask(task);
        return 0;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    DebugPrint(L"[INFO] Update versions: running %u.%u.%u.%u, available %u.%u.%u.%u\n",
               (unsigned int)task->runningVersion.major,
               (unsigned int)task->runningVersion.minor,
               (unsigned int)task->runningVersion.patch,
               (unsigned int)task->runningVersion.build,
               (unsigned int)task->availableVersion.major,
               (unsigned int)task->availableVersion.minor,
               (unsigned int)task->availableVersion.patch,
               (unsigned int)task->availableVersion.build);
    int comparison = CompareExecutableVersions(&task->availableVersion,
                                               &task->runningVersion);
    task->kind = comparison > 0 ? UPDATE_CHECK_NEWER
               : comparison < 0 ? UPDATE_CHECK_OLDER
                                : UPDATE_CHECK_SAME;
    PublishUpdateTask(task);
    return 0;
}

static BOOL ParseUpdateProcessId(LPCWSTR text, DWORD* processId) {
    if (!text || !processId || !*text) return FALSE;
    wchar_t* end = NULL;
    unsigned long value = wcstoul(text, &end, 10);
    if (!end || *end != L'\0' || value == 0) return FALSE;
    *processId = (DWORD)value;
    return TRUE;
}

static BOOL ValidateUpdateTempFilePair(LPCWSTR helperPath,
                                       LPCWSTR stagedPath,
                                       DWORD processId) {
    if (!helperPath || !stagedPath || !*helperPath || !*stagedPath) return FALSE;

    wchar_t expectedHelperName[96], expectedStagedName[96];
    int helperNameLength = swprintf_s(expectedHelperName,
        sizeof(expectedHelperName) / sizeof(wchar_t),
        APP_NAME L"-updater-%lu.exe", (unsigned long)processId);
    int stagedNameLength = swprintf_s(expectedStagedName,
        sizeof(expectedStagedName) / sizeof(wchar_t),
        APP_NAME L"-download-%lu.exe", (unsigned long)processId);
    if (helperNameLength <= 0 || stagedNameLength <= 0 ||
        _wcsicmp(PathFindFileNameW(helperPath), expectedHelperName) != 0 ||
        _wcsicmp(PathFindFileNameW(stagedPath), expectedStagedName) != 0) {
        return FALSE;
    }

    wchar_t helperDirectory[MAX_PATH], stagedDirectory[MAX_PATH];
    if (wcscpy_s(helperDirectory, MAX_PATH, helperPath) != 0 ||
        wcscpy_s(stagedDirectory, MAX_PATH, stagedPath) != 0 ||
        !PathRemoveFileSpecW(helperDirectory) ||
        !PathRemoveFileSpecW(stagedDirectory)) {
        return FALSE;
    }
    return _wcsicmp(helperDirectory, stagedDirectory) == 0;
}

static HANDLE DuplicateUpdateLaunchToken(HANDLE process) {
    HANDLE processToken = NULL;
    HANDLE launchToken = NULL;
    if (!process ||
        !OpenProcessToken(process, TOKEN_QUERY | TOKEN_DUPLICATE,
                          &processToken)) {
        return NULL;
    }
    DuplicateTokenEx(processToken, MAXIMUM_ALLOWED, NULL,
                     SecurityImpersonation, TokenPrimary, &launchToken);
    CloseHandle(processToken);
    return launchToken;
}

static BOOL LaunchUpdateTarget(LPCWSTR targetPath, LPCWSTR stagedPath,
                               LPCWSTR helperPath, DWORD helperProcessId,
                               DWORD oldProcessId, HANDLE launchToken,
                               BOOL successfulUpdate, BOOL reopenSettings) {
    wchar_t commandLine[MAX_PATH * 3 + 256];
    LPCWSTR finishAction = successfulUpdate
        ? L"--finish-update"
        : L"--finish-update-cleanup";
    int commandLength = swprintf_s(commandLine,
        sizeof(commandLine) / sizeof(wchar_t),
        L"\"%s\" %s %lu %lu \"%s\" \"%s\"%s", targetPath, finishAction,
        (unsigned long)helperProcessId, (unsigned long)oldProcessId,
        stagedPath, helperPath,
        successfulUpdate && reopenSettings ? L" --reopen-settings" : L"");
    if (commandLength <= 0 ||
        commandLength >= (int)(sizeof(commandLine) / sizeof(wchar_t))) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    STARTUPINFOW startupInfo = {0};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {0};
    BOOL launched = FALSE;
    if (launchToken) {
        wchar_t tokenCommandLine[MAX_PATH * 3 + 256];
        wcscpy_s(tokenCommandLine,
                 sizeof(tokenCommandLine) / sizeof(wchar_t), commandLine);
        LPVOID environment = NULL;
        BOOL hasEnvironment = CreateEnvironmentBlock(&environment, launchToken,
                                                     FALSE);
        launched = CreateProcessWithTokenW(launchToken, 0, targetPath,
                                           tokenCommandLine,
                                           hasEnvironment
                                               ? CREATE_UNICODE_ENVIRONMENT : 0,
                                           environment, NULL,
                                           &startupInfo, &processInfo);
        if (environment) DestroyEnvironmentBlock(environment);
    }
    if (!launched) {
        ZeroMemory(&processInfo, sizeof(processInfo));
        launched = CreateProcessW(targetPath, commandLine, NULL, NULL, FALSE,
                                  0, NULL, NULL, &startupInfo, &processInfo);
    }
    if (launched) {
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }
    return launched;
}

static int RestartAfterUpdateFailure(LPCWSTR targetPath, LPCWSTR stagedPath,
                                     LPCWSTR helperPath, DWORD oldProcessId,
                                     HANDLE launchToken, LPCWSTR message) {
    MessageBoxW(NULL, message, APP_DISPLAY_NAME_WSTRING L" Update", MB_OK | MB_ICONERROR);
    LaunchUpdateTarget(targetPath, stagedPath, helperPath,
                       GetCurrentProcessId(), oldProcessId, launchToken, FALSE, FALSE);
    if (launchToken) CloseHandle(launchToken);
    DeleteUpdateTempFile(stagedPath);
    SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
    MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 1;
}

static int RunUpdateApplyHelper(DWORD oldProcessId, LPCWSTR readyEventName,
                                LPCWSTR targetPath, LPCWSTR stagedPath,
                                BOOL reopenSettings) {
    wchar_t expectedEventPrefix[96];
    int prefixLength = swprintf_s(expectedEventPrefix,
        sizeof(expectedEventPrefix) / sizeof(wchar_t),
        L"Local\\NotTooBright_UpdateReady_%lu_",
        (unsigned long)oldProcessId);
    if (prefixLength <= 0 || !readyEventName ||
        _wcsnicmp(readyEventName, expectedEventPrefix,
                  (size_t)prefixLength) != 0) {
        return ERROR_INVALID_DATA;
    }

    HANDLE readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName);
    if (!readyEvent) return (int)GetLastError();

    HANDLE oldProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                    FALSE, oldProcessId);
    if (!oldProcess) {
        DWORD errorCode = GetLastError();
        CloseHandle(readyEvent);
        return (int)errorCode;
    }

    wchar_t oldProcessPath[MAX_PATH];
    DWORD oldProcessPathLength = sizeof(oldProcessPath) / sizeof(wchar_t);
    if (!QueryFullProcessImageNameW(oldProcess, 0, oldProcessPath,
                                    &oldProcessPathLength)) {
        DWORD errorCode = GetLastError();
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return (int)errorCode;
    }
    if (_wcsicmp(oldProcessPath, targetPath) != 0) {
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return ERROR_INVALID_DATA;
    }

    wchar_t helperPath[MAX_PATH];
    DWORD helperPathLength = GetModuleFileNameW(NULL, helperPath,
                                               sizeof(helperPath) / sizeof(wchar_t));
    DWORD binaryType = 0;
    if (helperPathLength == 0 || helperPathLength >= MAX_PATH ||
        !ValidateUpdateTempFilePair(helperPath, stagedPath, oldProcessId)) {
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return ERROR_INVALID_DATA;
    }
    if (!GetBinaryTypeW(stagedPath, &binaryType)) {
        DWORD errorCode = GetLastError();
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return (int)errorCode;
    }
    if (binaryType != SCS_64BIT_BINARY) {
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return ERROR_BAD_EXE_FORMAT;
    }

    // The helper is elevated only for file replacement. Preserve a primary
    // token from the original process so the restarted launcher normally
    // returns to the user's non-elevated session.
    HANDLE launchToken = DuplicateUpdateLaunchToken(oldProcess);

    // Only let the parent exit once this helper has verified every path and
    // owns the process handle it must wait on.
    if (!SetEvent(readyEvent)) {
        DWORD errorCode = GetLastError();
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        if (launchToken) CloseHandle(launchToken);
        return (int)errorCode;
    }
    CloseHandle(readyEvent);

    DWORD waitResult = WaitForSingleObject(oldProcess, UPDATE_HELPER_WAIT_MS);
    CloseHandle(oldProcess);
    if (waitResult != WAIT_OBJECT_0) {
        if (launchToken) CloseHandle(launchToken);
        DeleteUpdateTempFile(stagedPath);
        SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
        MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        MessageBoxW(NULL, L"The running application did not close in time.",
                    APP_DISPLAY_NAME_WSTRING L" Update", MB_OK | MB_ICONERROR);
        return ERROR_TIMEOUT;
    }

    wchar_t replacementPath[MAX_PATH], backupPath[MAX_PATH];
    DWORD helperProcessId = GetCurrentProcessId();
    int replacementLength = swprintf_s(replacementPath,
        sizeof(replacementPath) / sizeof(wchar_t), L"%s.new.%lu.exe",
        targetPath, (unsigned long)helperProcessId);
    int backupLength = swprintf_s(backupPath,
        sizeof(backupPath) / sizeof(wchar_t), L"%s.backup.%lu.exe",
        targetPath, (unsigned long)helperProcessId);
    if (replacementLength <= 0 || replacementLength >= MAX_PATH ||
        backupLength <= 0 || backupLength >= MAX_PATH) {
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The update paths were too long. The previous version will restart.");
    }

    SetFileAttributesW(replacementPath, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(replacementPath);
    SetFileAttributesW(backupPath, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(backupPath);
    if (!CopyFileW(stagedPath, replacementPath, FALSE)) {
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The update could not be prepared. The previous version will restart.");
    }

    DWORD targetAttributes = GetFileAttributesW(targetPath);
    BOOL clearedReadOnly = FALSE;
    if (targetAttributes != INVALID_FILE_ATTRIBUTES &&
        (targetAttributes & FILE_ATTRIBUTE_READONLY)) {
        clearedReadOnly = SetFileAttributesW(
            targetPath, targetAttributes & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!ReplaceFileW(targetPath, replacementPath, backupPath,
                      REPLACEFILE_WRITE_THROUGH, NULL, NULL)) {
        if (clearedReadOnly) SetFileAttributesW(targetPath, targetAttributes);
        DeleteFileW(replacementPath);
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The executable could not be replaced. The previous version will restart.");
    }

    if (!LaunchUpdateTarget(targetPath, stagedPath, helperPath,
                            helperProcessId, oldProcessId, launchToken, TRUE,
                            reopenSettings)) {
        DeleteFileW(targetPath);
        if (!MoveFileExW(backupPath, targetPath,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (launchToken) CloseHandle(launchToken);
            DeleteUpdateTempFile(stagedPath);
            SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
            MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            MessageBoxW(NULL,
                L"The updated application could not start and the previous executable "
                L"could not be restored. A backup remains beside the application.",
                APP_DISPLAY_NAME_WSTRING L" Update", MB_OK | MB_ICONERROR);
            return 1;
        }
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The updated application could not start. The previous version was restored.");
    }
    if (launchToken) CloseHandle(launchToken);

    if (!DeleteFileW(backupPath)) {
        MoveFileExW(backupPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    DeleteUpdateTempFile(stagedPath);
    SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
    MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

static BOOL FinishUpdateCleanup(DWORD helperProcessId, DWORD oldProcessId,
                                LPCWSTR stagedPath, LPCWSTR helperPath) {
    wchar_t targetPath[MAX_PATH], expectedStagedPath[MAX_PATH];
    wchar_t expectedHelperPath[MAX_PATH];
    DWORD targetLength = GetModuleFileNameW(NULL, targetPath,
                                           sizeof(targetPath) / sizeof(wchar_t));
    if (targetLength == 0 || targetLength >= MAX_PATH ||
        !BuildUpdateTempPath(expectedStagedPath, L"download", oldProcessId) ||
        !BuildUpdateTempPath(expectedHelperPath, L"updater", oldProcessId) ||
        _wcsicmp(stagedPath, expectedStagedPath) != 0 ||
        _wcsicmp(helperPath, expectedHelperPath) != 0) {
        return FALSE;
    }

    HANDLE helperProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, helperProcessId);
    if (helperProcess) {
        wchar_t runningHelperPath[MAX_PATH];
        DWORD runningHelperPathLength = MAX_PATH;
        if (QueryFullProcessImageNameW(helperProcess, 0, runningHelperPath,
                                      &runningHelperPathLength) &&
            _wcsicmp(runningHelperPath, helperPath) == 0) {
            WaitForSingleObject(helperProcess, UPDATE_HELPER_WAIT_MS);
        }
        CloseHandle(helperProcess);
    }
    for (int attempt = 0;
         attempt < 20 && !DeleteUpdateTempFile(stagedPath);
         ++attempt) {
        Sleep(100);
    }
    for (int attempt = 0;
         attempt < 20 && !DeleteUpdateTempFile(helperPath);
         ++attempt) {
        Sleep(100);
    }
    return TRUE;
}

// Returns an exit code and sets handled for the temporary updater process.
// Both finish modes perform cleanup and continue normal application startup;
// updateCompleted identifies only a successful executable replacement.
static int HandleUpdateCommandLine(BOOL* handled, BOOL* updateCompleted,
                                    BOOL* reopenSettings) {
    if (handled) *handled = FALSE;
    if (updateCompleted) *updateCompleted = FALSE;
    if (reopenSettings) *reopenSettings = FALSE;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) return 0;

    // The optional choice belongs only to a validated updater handoff. Never
    // persist it, and accept legacy handoffs with the default (closed).
    BOOL wantsSettings = argumentCount == 7 &&
        wcscmp(arguments[6], L"--reopen-settings") == 0;
    BOOL validArguments = argumentCount == 6 || wantsSettings;
    int result = 0;
    if (validArguments && wcscmp(arguments[1], L"--apply-update") == 0) {
        DWORD oldProcessId = 0;
        if (handled) *handled = TRUE;
        if (!ParseUpdateProcessId(arguments[2], &oldProcessId)) {
            result = ERROR_INVALID_PARAMETER;
        } else {
            result = RunUpdateApplyHelper(oldProcessId, arguments[3],
                                          arguments[4], arguments[5], wantsSettings);
        }
    } else if (validArguments &&
               (wcscmp(arguments[1], L"--finish-update") == 0 ||
                wcscmp(arguments[1], L"--finish-update-cleanup") == 0)) {
        DWORD helperProcessId = 0, oldProcessId = 0;
        if (ParseUpdateProcessId(arguments[2], &helperProcessId) &&
            ParseUpdateProcessId(arguments[3], &oldProcessId)) {
            BOOL recognizedHandoff = FinishUpdateCleanup(
                helperProcessId, oldProcessId, arguments[4], arguments[5]);
            if (recognizedHandoff && updateCompleted &&
                wcscmp(arguments[1], L"--finish-update") == 0) {
                *updateCompleted = TRUE;
                if (reopenSettings) *reopenSettings = wantsSettings;
            }
        }
    }
    LocalFree(arguments);
    return result;
}


static void CfgSendUpdateResultWithVersions(LPCWSTR status, LPCWSTR title,
                                            LPCWSTR message,
                                            LPCWSTR currentVersion,
                                            LPCWSTR remoteVersion,
                                            BOOL automatic) {
    if (!g_cfgWebView || !status || !title || !message ||
        !currentVersion || !remoteVersion) return;
    wchar_t escapedStatus[64], escapedTitle[256], escapedMessage[1024];
    wchar_t escapedCurrentVersion[64], escapedRemoteVersion[64];
    json_escape_wstring(status, escapedStatus,
                        sizeof(escapedStatus) / sizeof(wchar_t));
    json_escape_wstring(title, escapedTitle,
                        sizeof(escapedTitle) / sizeof(wchar_t));
    json_escape_wstring(message, escapedMessage,
                        sizeof(escapedMessage) / sizeof(wchar_t));
    json_escape_wstring(currentVersion, escapedCurrentVersion,
                        sizeof(escapedCurrentVersion) / sizeof(wchar_t));
    json_escape_wstring(remoteVersion, escapedRemoteVersion,
                        sizeof(escapedRemoteVersion) / sizeof(wchar_t));

    wchar_t script[1792];
    int written = swprintf_s(script, sizeof(script) / sizeof(wchar_t),
        L"window.onUpdateResult({\"status\":\"%s\",\"title\":\"%s\","
        L"\"message\":\"%s\",\"currentVersion\":\"%s\","
        L"\"remoteVersion\":\"%s\",\"automatic\":%s})",
        escapedStatus, escapedTitle, escapedMessage,
        escapedCurrentVersion, escapedRemoteVersion,
        automatic ? L"true" : L"false");
    if (written > 0) webview_cfg_execute_script(script);
}

static void CfgSendUpdateResult(LPCWSTR status, LPCWSTR title, LPCWSTR message) {
    CfgSendUpdateResultWithVersions(status, title, message, L"", L"", FALSE);
}

static void CfgSendUpdateProgress(DWORD speedKbps) {
    wchar_t script[160];
    int written = swprintf_s(script, sizeof(script) / sizeof(wchar_t),
        L"window.onUpdateProgress({\"kilobytesPerSecond\":%lu})",
        (unsigned long)speedKbps);
    if (written > 0) webview_cfg_execute_script(script);
}

static void DiscardPendingUpdateNotice(void) {
    UpdateCheckTask* task = g_updateNoticeTask;
    g_updateNoticeTask = NULL;
    DiscardUpdateTask(task);
}

static void StartUpdateCheck(BOOL automatic) {
    HWND targetWindow = g_hwnd ? g_hwnd : g_cfgHwnd;
    if (!targetWindow) return;
    if (automatic && (g_updateNoticeTask || g_updateReadyTask)) return;
    if (InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_updatePostedResult, NULL, NULL) != NULL) {
        if (!automatic) {
            CfgSendUpdateResult(L"error", L"Update check in progress",
                L"Another update check is still finishing. Try again shortly.");
        }
        return;
    }
    if (InterlockedCompareExchange(&g_updateCheckPending, TRUE, FALSE) != FALSE) {
        if (!automatic) {
            CfgSendUpdateResult(L"error", L"Update check in progress",
                L"Another update check is still finishing. Try again shortly.");
        }
        return;
    }
    InterlockedExchange(&g_updateCheckAutomatic, automatic ? TRUE : FALSE);

    // Every accepted request starts from scratch. Automatic requests are
    // skipped above while a result is awaiting user action, avoiding an
    // hourly re-download of the same prepared executable.
    DiscardPendingUpdateNotice();
    DiscardPreparedUpdate();

    if (!g_updateCancelEvent) {
        g_updateCancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_updateCancelEvent) {
            DWORD errorCode = GetLastError();
            InterlockedExchange(&g_updateCheckPending, FALSE);
            InterlockedExchange(&g_updateCheckAutomatic, FALSE);
            wchar_t message[256];
            swprintf_s(message, sizeof(message) / sizeof(wchar_t),
                L"Could not initialize update cancellation (Windows error %lu).",
                (unsigned long)errorCode);
            CfgSendUpdateResultWithVersions(L"error", L"Update failed", message,
                                            L"", L"", automatic);
            return;
        }
    }
    ResetEvent(g_updateCancelEvent);
    InterlockedExchange(&g_updateSpeedKbps, 0);
    InterlockedExchange(&g_updateProgressPosted, FALSE);

    UpdateCheckTask* task = (UpdateCheckTask*)calloc(1, sizeof(UpdateCheckTask));
    if (!task) {
        InterlockedExchange(&g_updateCheckPending, FALSE);
        InterlockedExchange(&g_updateCheckAutomatic, FALSE);
        CfgSendUpdateResultWithVersions(L"error", L"Update failed",
            L"There was not enough memory to check for updates.",
            L"", L"", automatic);
        return;
    }
    task->targetWindow = targetWindow;
    task->automatic = automatic;
    LONG sequence = InterlockedIncrement(&g_updateRequestSequence);
    task->cacheBuster =
        ((GetTickCount64() ^ GetCurrentProcessId()) << 32) | (DWORD)sequence;
    if (task->cacheBuster == 0) task->cacheBuster = 1;

    HANDLE thread = CreateThread(NULL, 0, UpdateCheckThread, task, 0, NULL);
    if (!thread) {
        DWORD errorCode = GetLastError();
        free(task);
        InterlockedExchange(&g_updateCheckPending, FALSE);
        InterlockedExchange(&g_updateCheckAutomatic, FALSE);
        wchar_t message[256];
        swprintf_s(message, sizeof(message) / sizeof(wchar_t),
                   L"Could not start the update check (Windows error %lu).",
                   (unsigned long)errorCode);
        CfgSendUpdateResultWithVersions(L"error", L"Update failed", message,
                                        L"", L"", automatic);
        return;
    }
    CloseHandle(thread);
}

static BOOL IsIgnoredUpdateVersion(const ExecutableVersion* version) {
    wchar_t formatted[32];
    if (!version || !g_ignoredUpdateVersion[0]) return FALSE;
    FormatExecutableVersion(version, formatted,
                            sizeof(formatted) / sizeof(wchar_t));
    return wcscmp(formatted, g_ignoredUpdateVersion) == 0;
}

static void SaveIgnoredUpdateVersion(LPCWSTR version) {
    HKEY key;
    DWORD disposition;
    if (!version) return;
    wcsncpy_s(g_ignoredUpdateVersion,
              sizeof(g_ignoredUpdateVersion) / sizeof(wchar_t), version,
              _TRUNCATE);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &key,
                        &disposition) == ERROR_SUCCESS) {
        RegSetValueExW(key, REG_VALUE_IGNORED_UPDATE_VERSION, 0, REG_SZ,
                       (const BYTE*)g_ignoredUpdateVersion,
                       (DWORD)((wcslen(g_ignoredUpdateVersion) + 1) *
                               sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void PresentPendingUpdateNotice(void) {
    if (!g_configViewReady || !g_cfgWebView || !g_updateNoticeTask) return;

    UpdateCheckTask* task = g_updateNoticeTask;
    g_updateNoticeTask = NULL;
    LPCWSTR status = NULL;
    LPCWSTR title = NULL;
    LPCWSTR message = NULL;
    wchar_t currentVersion[32] = L"";
    wchar_t remoteVersion[32] = L"";
    BOOL installable = FALSE;

    if (task->kind == UPDATE_CHECK_CANCELLED) {
        status = L"cancelled";
        title = L"";
        message = L"";
    } else if (task->kind == UPDATE_CHECK_ERROR) {
        status = L"error";
        title = L"Update failed";
        message = task->message;
    } else {
        FormatExecutableVersionForDisplay(
            &task->runningVersion, currentVersion,
            sizeof(currentVersion) / sizeof(wchar_t));
        FormatExecutableVersionForDisplay(
            &task->availableVersion, remoteVersion,
            sizeof(remoteVersion) / sizeof(wchar_t));
        if (task->kind == UPDATE_CHECK_NEWER) {
            status = L"newer";
            title = L"Update available";
            message = L"A newer version is ready to install.";
            installable = TRUE;
        } else if (task->kind == UPDATE_CHECK_SAME) {
            status = L"same";
            title = L"You're up to date";
            message = L"The remote build matches your current version. "
                      L"You can force a reinstall if needed.";
            installable = !task->automatic;
        } else if (task->kind == UPDATE_CHECK_OLDER) {
            status = L"older";
            title = L"No update available";
            message = L"The remote build is older than your current version.";
        }
    }

    if (!status) {
        DiscardUpdateTask(task);
        return;
    }
    if (installable) {
        DiscardPreparedUpdate();
        g_updateReadyTask = task;
    }
    CfgSendUpdateResultWithVersions(status, title, message,
                                    currentVersion, remoteVersion,
                                    task->automatic);
    if (!installable) DiscardUpdateTask(task);
}

static void QueueUpdateNotice(UpdateCheckTask* task) {
    DiscardPendingUpdateNotice();
    g_updateNoticeTask = task;
    PresentPendingUpdateNotice();
}

static void HandleCompletedUpdateCheck(UpdateCheckTask* task) {
    if (!task) return;
    InterlockedExchange(&g_updateProgressPosted, FALSE);
    InterlockedExchange(&g_updateSpeedKbps, 0);

    if (task->kind == UPDATE_CHECK_CANCELLED) {
        DebugPrint(L"[INFO] Update check cancelled\n");
    } else if (task->kind == UPDATE_CHECK_ERROR) {
        DebugPrint(L"[WARNING] Update check failed: %s\n", task->message);
    }

    if (task->automatic && !g_config.autoCheckForUpdates) {
        DiscardUpdateTask(task);
        return;
    }

    if (task->automatic && task->kind == UPDATE_CHECK_NEWER &&
        IsIgnoredUpdateVersion(&task->availableVersion)) {
        DebugPrint(L"[INFO] Automatic update prompt suppressed for ignored version\n");
        task->kind = UPDATE_CHECK_CANCELLED;
        if (g_cfgHwnd) {
            QueueUpdateNotice(task);
        } else {
            DiscardUpdateTask(task);
        }
        return;
    }

    if (task->automatic && task->kind == UPDATE_CHECK_NEWER) {
        QueueUpdateNotice(task);
        ShowConfigDialog();
        if (!g_cfgHwnd) DiscardPendingUpdateNotice();
        return;
    }

    if (g_cfgHwnd) {
        QueueUpdateNotice(task);
    } else {
        DiscardUpdateTask(task);
    }
}

static void IgnorePreparedUpdateVersion(const char* requestedVersion) {
    UpdateCheckTask* task = g_updateReadyTask;
    wchar_t preparedVersion[32];
    wchar_t requestedVersionW[32] = L"";
    if (!task || !task->automatic || task->kind != UPDATE_CHECK_NEWER ||
        !requestedVersion ||
        !MultiByteToWideChar(CP_UTF8, 0, requestedVersion, -1,
                             requestedVersionW,
                             sizeof(requestedVersionW) / sizeof(wchar_t))) {
        CfgSendUpdateResult(L"error", L"Update unavailable",
            L"The update version could not be ignored. Check for updates again.");
        return;
    }
    FormatExecutableVersion(&task->availableVersion, preparedVersion,
                            sizeof(preparedVersion) / sizeof(wchar_t));
    if (wcscmp(preparedVersion, requestedVersionW) != 0) {
        CfgSendUpdateResult(L"error", L"Update unavailable",
            L"The update version changed. Check for updates again.");
        return;
    }
    SaveIgnoredUpdateVersion(preparedVersion);
    DebugPrint(L"[INFO] Automatic update version added to the ignore list\n");
    DiscardPreparedUpdate();
}

static void CancelUpdateCheck(void) {
    if (InterlockedCompareExchange(&g_updateCheckPending, FALSE, FALSE) == TRUE &&
        g_updateCancelEvent) {
        DebugPrint(L"[INFO] Update check cancellation requested\n");
        SetEvent(g_updateCancelEvent);
    }
}

static HANDLE CreateUpdateReadyEvent(DWORD processId, wchar_t* eventName,
                                     size_t eventNameCch) {
    if (!processId || !eventName || eventNameCch < 96) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    ULONGLONG nonce = GetTickCount64() ^
                      ((ULONGLONG)GetCurrentThreadId() << 32);
    BCryptGenRandom(NULL, (PUCHAR)&nonce, sizeof(nonce),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    int nameLength = swprintf_s(eventName, eventNameCch,
        L"Local\\NotTooBright_UpdateReady_%lu_%016llx",
        (unsigned long)processId, (unsigned long long)nonce);
    if (nameLength <= 0 || nameLength >= (int)eventNameCch) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return NULL;
    }

    // The elevated helper can run with a split administrator token (or with
    // alternate administrator credentials). Grant interactive users access
    // to this random, session-local event so either UAC path can acknowledge
    // readiness without exposing any file or process permissions.
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;IU)(A;;GA;;;BA)(A;;GA;;;SY)",
            SDDL_REVISION_1, &descriptor, NULL)) {
        return NULL;
    }
    SECURITY_ATTRIBUTES securityAttributes = {0};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = descriptor;

    HANDLE readyEvent = CreateEventW(&securityAttributes, TRUE, FALSE,
                                     eventName);
    DWORD errorCode = readyEvent ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!readyEvent) SetLastError(errorCode);
    return readyEvent;
}

static BOOL LaunchStagedUpdate(LPCWSTR stagedPath, LPCWSTR targetPath,
                               BOOL reopenSettings) {
    if (!stagedPath || !targetPath || !*stagedPath || !*targetPath) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD oldProcessId = GetCurrentProcessId();
    wchar_t helperPath[MAX_PATH];
    if (!BuildUpdateTempPath(helperPath, L"updater", oldProcessId)) {
        return FALSE;
    }
    DeleteUpdateTempFile(helperPath);
    if (!CopyFileW(targetPath, helperPath, TRUE)) return FALSE;
    SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);

    // CopyFile preserves alternate data streams. The source is already the
    // running, user-approved executable, so do not carry its download-zone
    // marker onto the short-lived updater copy and trigger a second warning.
    wchar_t zonePath[MAX_PATH + 32];
    if (swprintf_s(zonePath, sizeof(zonePath) / sizeof(wchar_t),
                   L"%s:Zone.Identifier", helperPath) > 0) {
        DeleteFileW(zonePath);
    }

    wchar_t readyEventName[160];
    HANDLE readyEvent = CreateUpdateReadyEvent(oldProcessId, readyEventName,
        sizeof(readyEventName) / sizeof(wchar_t));
    if (!readyEvent) {
        DWORD errorCode = GetLastError();
        DeleteUpdateTempFile(helperPath);
        SetLastError(errorCode);
        return FALSE;
    }

    wchar_t parameters[MAX_PATH * 2 + 512];
    int parameterLength = swprintf_s(parameters,
        sizeof(parameters) / sizeof(wchar_t),
        L"--apply-update %lu \"%s\" \"%s\" \"%s\"%s",
        (unsigned long)oldProcessId, readyEventName, targetPath, stagedPath,
        reopenSettings ? L" --reopen-settings" : L"");
    if (parameterLength <= 0 ||
        parameterLength >= (int)(sizeof(parameters) / sizeof(wchar_t))) {
        CloseHandle(readyEvent);
        DeleteUpdateTempFile(helperPath);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    SHELLEXECUTEINFOW executeInfo = {0};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    executeInfo.hwnd = g_cfgHwnd;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = helperPath;
    executeInfo.lpParameters = parameters;
    executeInfo.nShow = SW_HIDE;
    BOOL elevated = ShellExecuteExW(&executeInfo);
    if (!elevated || !executeInfo.hProcess) {
        DWORD errorCode = elevated ? ERROR_INVALID_HANDLE : GetLastError();
        if (!errorCode) errorCode = ERROR_ACCESS_DENIED;
        CloseHandle(readyEvent);
        DeleteUpdateTempFile(helperPath);
        SetLastError(errorCode);
        return FALSE;
    }

    HANDLE waitHandles[2] = { readyEvent, executeInfo.hProcess };
    DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE,
                                              UPDATE_HELPER_READY_MS);
    DWORD errorCode = ERROR_SUCCESS;
    if (waitResult != WAIT_OBJECT_0) {
        if (waitResult == WAIT_OBJECT_0 + 1) {
            DWORD exitCode = ERROR_INSTALL_FAILURE;
            if (!GetExitCodeProcess(executeInfo.hProcess, &exitCode) ||
                exitCode == ERROR_SUCCESS || exitCode == STILL_ACTIVE) {
                exitCode = ERROR_INSTALL_FAILURE;
            }
            errorCode = exitCode;
        } else {
            errorCode = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT
                                                   : GetLastError();
            if (!errorCode) errorCode = ERROR_INSTALL_FAILURE;
        }
    }
    CloseHandle(executeInfo.hProcess);
    CloseHandle(readyEvent);

    if (waitResult != WAIT_OBJECT_0) {
        DeleteUpdateTempFile(helperPath);
        SetLastError(errorCode);
        return FALSE;
    }
    return TRUE;
}

static void DiscardPreparedUpdate(void) {
    UpdateCheckTask* task = g_updateReadyTask;
    g_updateReadyTask = NULL;
    DiscardUpdateTask(task);
}

static void InstallPreparedUpdate(BOOL reopenSettings) {
    UpdateCheckTask* task = g_updateReadyTask;
    g_updateReadyTask = NULL;
    if (!task || (task->kind != UPDATE_CHECK_NEWER &&
                  task->kind != UPDATE_CHECK_SAME)) {
        DiscardUpdateTask(task);
        CfgSendUpdateResult(L"error", L"Update unavailable",
            L"The prepared update is no longer available. Check for updates again.");
        return;
    }

    if (LaunchStagedUpdate(task->stagedPath, task->targetPath, reopenSettings)) {
        DebugPrint(L"[INFO] Update accepted; exiting for replacement\n");
        g_updateInstallReady = TRUE;
        free(task);  // The updater process now owns the staged file.
        if (g_cfgHwnd) PostMessageW(g_cfgHwnd, WM_CLOSE, 0, 0);
        return;
    }

    DWORD errorCode = GetLastError();
    wchar_t message[384];
    LPCWSTR title = L"Update failed";
    if (errorCode == ERROR_CANCELLED) {
        title = L"Update cancelled";
        wcscpy_s(message, sizeof(message) / sizeof(wchar_t),
            L"Administrator approval was cancelled. Your current version is still running.");
    } else {
        swprintf_s(message, sizeof(message) / sizeof(wchar_t),
            L"The elevated update process could not be started (Windows error %lu).",
            (unsigned long)errorCode);
    }
    DebugPrint(L"[WARNING] %s\n", message);
    CfgSendUpdateResult(L"error", title, message);
    DiscardUpdateTask(task);
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
    return fnCreateEnvironment != NULL;
}

static void ReportWebView2Unavailable(void) {
    MessageBoxW(NULL,
        L"Failed to load WebView2.\n\n"
        L"Please ensure the Microsoft Edge WebView2 Runtime is installed.\n"
        L"Download from: https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
        APP_DISPLAY_NAME_WSTRING, MB_ICONERROR | MB_OK);
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

static const wchar_t* HardwareStateName(HardwareState state) {
    switch (state) {
        case HW_AVAILABLE: return L"available";
        case HW_UNAVAILABLE: return L"unavailable";
        default: return L"probing";
    }
}

static const wchar_t* ModeName(BrightnessMode mode) {
    switch (mode) {
        case MODE_HARDWARE: return L"hardware";
        case MODE_SOFTWARE: return L"software";
        case MODE_WAITING: return L"waiting";
        default: return L"probing";
    }
}

/* JSON array describing every monitor for the dialog. Caller frees. */
static wchar_t* BuildMonitorsJson(void) {
    const size_t cap = 256 + (size_t)g_monitorCount * 2048;
    wchar_t* buf = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (!buf) return NULL;
    size_t len = 0;
    buf[len++] = L'[';
    buf[len] = L'\0';
    ULONGLONG nowFt = NowFileTime();
    for (int i = 0; i < g_monitorCount; i++) {
        const Monitor* m = &g_monitors[i];
        wchar_t eKey[256], eName[128], eDevice[64], eError[320], pausedUntil[16] = L"";
        if (IsSchedulePaused(m, nowFt)) FormatLocalTimeOfDay(m->pausedUntil, pausedUntil, 16);
        json_escape_wstring(m->key, eKey, 256);
        json_escape_wstring(m->name, eName, 128);
        json_escape_wstring(m->device, eDevice, 64);
        json_escape_wstring(m->error, eError, 320);
        int written = swprintf_s(buf + len, cap - len,
            L"%s{\"uid\":%d,\"key\":\"%s\",\"name\":\"%s\",\"device\":\"%s\","
            L"\"width\":%ld,\"height\":%ld,\"primary\":%s,\"hardware\":\"%s\","
            L"\"mode\":\"%s\",\"knownHardware\":%s,\"forceSoftware\":%s,\"hidden\":%s,\"value\":%d,"
            L"\"min\":%d,\"max\":100,\"scheduled\":%s,\"pausedUntil\":\"%s\","
            L"\"error\":\"%s\"}",
            i == 0 ? L"" : L",", m->uid, eKey, eName, eDevice,
            (long)(m->rect.right - m->rect.left), (long)(m->rect.bottom - m->rect.top),
            m->primary ? L"true" : L"false", HardwareStateName(m->hardwareState),
            ModeName(MonitorMode(m)), m->knownHardware ? L"true" : L"false",
            m->forceSoftware ? L"true" : L"false",
            m->hidden ? L"true" : L"false",
            m->hasValue ? m->value : 100, MonitorMinValue(m),
            m->scheduled ? L"true" : L"false", pausedUntil, eError);
        if (written > 0) len += (size_t)written;
    }
    if (len + 2 <= cap) {
        buf[len++] = L']';
        buf[len] = L'\0';
    }
    return buf;
}

static void webview_push_init_config(void) {
    wchar_t* monitors = BuildMonitorsJson();
    if (!monitors) return;

    const Schedule* sc = &g_config.schedule;
    wchar_t eUpdateCompletedVersion[64];
    json_escape_wstring(g_updateConfirmationPending ? APP_VERSION_WSTRING : L"",
                        eUpdateCompletedVersion, 64);
    BOOL updateCheckPending =
        InterlockedCompareExchange(&g_updateCheckPending, FALSE, FALSE) == TRUE ||
        InterlockedCompareExchangePointer((PVOID volatile*)&g_updatePostedResult, NULL, NULL) != NULL;
    const size_t cap = wcslen(monitors) + 1280;
    wchar_t* script = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (script) {
        int written = swprintf_s(script, cap,
            L"window.onInit({\"config\":{\"allowBelowMinimum\":%s,\"debugLog\":%s,"
            L"\"autoCheckForUpdates\":%s,\"updateCheckPending\":%s,\"updatePromptPending\":%s,"
            L"\"schedule\":{\"enabled\":%s,\"hasLocation\":%s,\"latitude\":%.6f,"
            L"\"longitude\":%.6f,\"dayLevel\":%d,\"nightLevel\":%d,"
            L"\"dawnStartOffset\":%d,\"dawnEndOffset\":%d,\"duskStartOffset\":%d,"
            L"\"duskEndOffset\":%d,\"cycleResetMinutes\":%d}},"
            L"\"monitors\":%s,\"updateCompletedVersion\":\"%s\"})",
            g_config.allowBelowMinimum ? L"true" : L"false",
            g_config.debugLogEnabled ? L"true" : L"false",
            g_config.autoCheckForUpdates ? L"true" : L"false",
            updateCheckPending ? L"true" : L"false",
            g_updateNoticeTask ? L"true" : L"false",
            sc->enabled ? L"true" : L"false", sc->hasLocation ? L"true" : L"false",
            sc->latitude, sc->longitude, sc->dayLevel, sc->nightLevel,
            sc->dawnStartOffset, sc->dawnEndOffset, sc->duskStartOffset,
            sc->duskEndOffset, sc->cycleResetMinutes,
            monitors, eUpdateCompletedVersion);
        if (written > 0) webview_cfg_execute_script(script);
        free(script);
    }
    free(monitors);
}

/* Live update of the monitor list while the dialog is open (probe results,
 * display changes, mode switches). */
static void PushMonitorsToDialog(void) {
    if (!g_cfgWebView) return;
    wchar_t* monitors = BuildMonitorsJson();
    if (!monitors) return;
    const size_t cap = wcslen(monitors) + 64;
    wchar_t* script = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (script) {
        int written = swprintf_s(script, cap, L"window.onMonitors && window.onMonitors(%s)", monitors);
        if (written > 0) webview_cfg_execute_script(script);
        free(script);
    }
    free(monitors);
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
    MessageBoxW(NULL, msg, APP_DISPLAY_NAME_WSTRING, MB_ICONERROR | MB_OK);
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

static int ClampInt(int value, int min, int max) {
    return value < min ? min : (value > max ? max : value);
}

/* Reads the schedule part of a saveSettings message. Saving the schedule
 * counts as a deliberate change, so any paused monitors resume. */
static void SaveScheduleFromMessage(const char* msg) {
    Schedule* sc = &g_config.schedule;
    char latitude[64] = {0}, longitude[64] = {0}, scheduledUids[512] = {0};
    json_get_string(msg, "latitude", latitude, sizeof(latitude));
    json_get_string(msg, "longitude", longitude, sizeof(longitude));
    json_get_string(msg, "scheduledUids", scheduledUids, sizeof(scheduledUids));

    char* end = NULL;
    double lat = strtod(latitude, &end);
    BOOL latOk = end != latitude && IsValidLatitude(lat);
    double lon = strtod(longitude, &end);
    BOOL lonOk = end != longitude && IsValidLongitude(lon);
    if (latOk && lonOk) {
        sc->latitude = lat;
        sc->longitude = lon;
        sc->hasLocation = TRUE;
    }

    int v;
    if (json_get_int(msg, "dayLevel", &v)) sc->dayLevel = ClampInt(v, -SOFT_MAX_DIM, 100);
    if (json_get_int(msg, "nightLevel", &v)) sc->nightLevel = ClampInt(v, -SOFT_MAX_DIM, 100);
    if (json_get_int(msg, "dawnStartOffset", &v)) sc->dawnStartOffset = ClampInt(v, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    if (json_get_int(msg, "dawnEndOffset", &v)) sc->dawnEndOffset = ClampInt(v, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    if (json_get_int(msg, "duskStartOffset", &v)) sc->duskStartOffset = ClampInt(v, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    if (json_get_int(msg, "duskEndOffset", &v)) sc->duskEndOffset = ClampInt(v, -SCHEDULE_MAX_OFFSET, SCHEDULE_MAX_OFFSET);
    if (json_get_int(msg, "cycleResetMinutes", &v)) sc->cycleResetMinutes = ClampInt(v, 0, 1439);
    sc->enabled = json_get_bool(msg, "scheduleEnabled", FALSE) && sc->hasLocation;

    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        BOOL scheduled = FALSE;
        const char* p = scheduledUids;
        while (*p) {
            char* next = NULL;
            long uid = strtol(p, &next, 10);
            if (next == p) break;
            if ((int)uid == m->uid) scheduled = TRUE;
            p = next;
            while (*p == ',' || *p == ' ') p++;
        }
        if (m->scheduled != scheduled || m->pausedUntil) {
            m->scheduled = scheduled;
            m->pausedUntil = 0;
            m->dirty = TRUE;
        }
    }
    PersistDirtyMonitors();

    int scheduledCount = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].scheduled) scheduledCount++;
    }
    DebugPrint(L"[INFO] Schedule %s: lat %.4f lon %.4f, day %d%% night %d%%, dawn %+d/%+d, dusk %+d/%+d, reset %02d:%02d, %d monitor(s) selected%s\n",
               sc->enabled ? L"enabled" : L"disabled", sc->latitude, sc->longitude,
               sc->dayLevel, sc->nightLevel, sc->dawnStartOffset, sc->dawnEndOffset,
               sc->duskStartOffset, sc->duskEndOffset,
               sc->cycleResetMinutes / 60, sc->cycleResetMinutes % 60, scheduledCount,
               (sc->enabled && scheduledCount == 0) ? L" - the schedule has nothing to control" : L"");
    UpdateScheduleTimer();
    EvaluateSchedule();
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
    if (strcmp(action, "resize") != 0) {
        /* Long messages are cut; a partial trailing UTF-8 sequence only
         * costs one replacement character. */
        wchar_t preview[512];
        int bytes = (int)strlen(msg);
        if (bytes > 480) bytes = 480;
        int n = MultiByteToWideChar(CP_UTF8, 0, msg, bytes, preview, 500);
        if (n < 0) n = 0;
        preview[n] = L'\0';
        DebugPrint(L"[DIALOG] %s%s\n", preview, strlen(msg) > 480 ? L"..." : L"");
    }

    if (strcmp(action, "getInit") == 0) {
        webview_push_init_config();
    } else if (strcmp(action, "configReady") == 0) {
        if (g_cfgHwnd) {
            BOOL updateWorkAlreadyActive =
                InterlockedCompareExchange(&g_updateCheckPending,
                                           FALSE, FALSE) == TRUE ||
                InterlockedCompareExchangePointer(
                    (PVOID volatile*)&g_updatePostedResult, NULL, NULL) != NULL ||
                g_updateNoticeTask || g_updateReadyTask;
            BOOL checkAutomatically =
                json_get_bool(msg, "checkAutomatically", FALSE);
            g_configViewReady = TRUE;
            PresentPendingUpdateNotice();
            if (checkAutomatically && g_config.autoCheckForUpdates &&
                !g_updateConfirmationPending && !updateWorkAlreadyActive) {
                StartUpdateCheck(TRUE);
            }
        }
    } else if (strcmp(action, "checkUpdate") == 0) {
        StartUpdateCheck(json_get_bool(msg, "automatic", FALSE));
    } else if (strcmp(action, "cancelUpdateCheck") == 0) {
        CancelUpdateCheck();
    } else if (strcmp(action, "installUpdate") == 0) {
        InstallPreparedUpdate(json_get_bool(msg, "reopenSettings", FALSE));
    } else if (strcmp(action, "dismissUpdate") == 0) {
        DiscardPreparedUpdate();
    } else if (strcmp(action, "ignoreUpdateVersion") == 0) {
        char version[32] = {0};
        json_get_string(msg, "version", version, sizeof(version));
        IgnorePreparedUpdateVersion(version);
    } else if (strcmp(action, "dismissUpdateConfirmation") == 0) {
        g_updateConfirmationPending = FALSE;
    } else if (strcmp(action, "setBrightness") == 0) {
        int uid = -1, value = 0;
        if (json_get_int(msg, "uid", &uid) && json_get_int(msg, "value", &value)) {
            Monitor* m = FindMonitorByUid(uid);
            if (m) {
                SetMonitorValue(m, value);
                NoteManualChange(m);
            }
        }
    } else if (strcmp(action, "setAllBrightness") == 0) {
        int value = 0;
        if (json_get_int(msg, "value", &value)) {
            for (int i = 0; i < g_monitorCount; i++) {
                if (g_monitors[i].hidden) continue;
                SetMonitorValue(&g_monitors[i], value);
                NoteManualChange(&g_monitors[i]);
            }
        }
    } else if (strcmp(action, "setMonitorSoftwareOnly") == 0) {
        int uid = -1;
        Monitor* m = json_get_int(msg, "uid", &uid) ? FindMonitorByUid(uid) : NULL;
        if (m && !m->hidden) {
            m->forceSoftware = json_get_bool(msg, "softwareOnly", FALSE);
            m->failures = 0;
            m->error[0] = L'\0';
            m->lastHwSent = -1;
            m->dirty = TRUE;
            SchedulePersist();
            DebugPrint(L"[INFO] %s (%s): software-only dimming %s\n", m->name, m->device,
                       m->forceSoftware ? L"enabled" : L"disabled");
            if (m->forceSoftware) {
                ApplyMonitor(m);
            } else {
                SetOverlayDim(m, 0);
                if (m->hardwareState == HW_AVAILABLE) ApplyMonitor(m);
                else ScheduleMonitorRefresh(0);   /* probe again now */
            }
            PushMonitorsToDialog();
        }
    } else if (strcmp(action, "setAllowBelowMinimum") == 0) {
        g_config.allowBelowMinimum = json_get_bool(msg, "enabled", FALSE);
        SaveConfigToRegistry(&g_config);
        DebugPrint(L"[INFO] Dimming below the hardware minimum %s\n",
                   g_config.allowBelowMinimum ? L"enabled" : L"disabled");
        /* Re-clamp: disabling pulls any monitor parked below 0 back up. */
        for (int i = 0; i < g_monitorCount; i++) ApplyMonitor(&g_monitors[i]);
        PushMonitorsToDialog();
    } else if (strcmp(action, "hideMonitor") == 0) {
        int uid = -1;
        Monitor* m = json_get_int(msg, "uid", &uid) ? FindMonitorByUid(uid) : NULL;
        if (m && !m->hidden) {
            /* The last visible monitor can never be hidden: there would be
             * nothing left to control and no card to bring things back. */
            if (VisibleMonitorCount() <= 1) {
                DebugPrint(L"[INFO] Refused to hide the last visible monitor (%s)\n", m->name);
            } else {
                /* Put the monitor back the way it was found, then treat
                 * the display as if it did not exist: the overlay window
                 * goes and nothing is touched again until a rescan. */
                if (m->hardwareState == HW_AVAILABLE && m->hasOriginal) {
                    DWORD currentRaw = m->lastHwSent >= 0
                        ? (DWORD)m->lastHwSent * m->ddcMax / 100 : m->ddcCurrent;
                    if (m->originalMax != m->ddcMax) {
                        /* Range changed since the original was recorded;
                         * rescale so the restored level is the same. */
                        m->originalRaw = (m->originalRaw * m->ddcMax + m->originalMax / 2) / m->originalMax;
                        m->originalMax = m->ddcMax;
                    }
                    if (currentRaw != m->originalRaw) {
                        DdcRequestSetRaw(m->uid, m->originalRaw);
                        DebugPrint(L"[INFO] %s (%s): restoring original brightness %lu/%lu before hiding\n",
                                   m->name, m->device, (unsigned long)m->originalRaw,
                                   (unsigned long)m->originalMax);
                    }
                } else if (m->hardwareState == HW_AVAILABLE) {
                    DebugPrint(L"[WARNING] %s (%s): no original brightness recorded; leaving it as is\n",
                               m->name, m->device);
                }
                m->hidden = TRUE;
                m->dirty = TRUE;
                SchedulePersist();
                ReleaseMonitorOverlay(m);
                DebugPrint(L"[INFO] %s (%s): hidden until the next rescan\n", m->name, m->device);
            }
            PushMonitorsToDialog();
        }
    } else if (strcmp(action, "refreshMonitors") == 0) {
        DebugPrint(L"[INFO] Rescan requested from the configuration dialog\n");
        g_ddcRetryDelayMs = DDC_RETRY_INITIAL_MS;
        UnhideAllMonitors();
        RefreshMonitors();
    } else if (strcmp(action, "resumeSchedule") == 0) {
        int uid = -1;
        Monitor* m = json_get_int(msg, "uid", &uid) ? FindMonitorByUid(uid) : NULL;
        if (m && m->pausedUntil) {
            m->pausedUntil = 0;
            m->dirty = TRUE;
            DebugPrint(L"[INFO] %s (%s): automatic brightness resumed by the user\n", m->name, m->device);
            EvaluateSchedule();
            SchedulePersist();
            PushMonitorsToDialog();
        }
    } else if (strcmp(action, "saveSettings") == 0) {
        g_config.debugLogEnabled = json_get_bool(msg, "debugLog", FALSE);
        g_config.autoCheckForUpdates = json_get_bool(msg, "autoCheckForUpdates", TRUE);
        SaveScheduleFromMessage(msg);
        InterlockedExchange(&g_debugLogEnabled, g_config.debugLogEnabled ? TRUE : FALSE);
        if (!SaveConfigToRegistry(&g_config)) {
            DebugPrint(L"[WARNING] Could not save all settings to the registry\n");
            MessageBoxW(g_cfgHwnd,
                L"Some settings could not be saved. Check that the current user "
                L"can write to HKEY_CURRENT_USER.",
                APP_DISPLAY_NAME_WSTRING, MB_ICONWARNING | MB_OK);
        }
        DebugPrint(L"[INFO] Settings saved (debugLog=%d, autoCheckForUpdates=%d)\n",
                   g_config.debugLogEnabled, g_config.autoCheckForUpdates);
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
        case WM_APP_UPDATE_PROGRESS:
            InterlockedExchange(&g_updateProgressPosted, FALSE);
            if (InterlockedCompareExchange(&g_updateCheckPending,
                                           FALSE, FALSE) == TRUE) {
                DWORD speedKbps = (DWORD)InterlockedCompareExchange(
                    &g_updateSpeedKbps, 0, 0);
                CfgSendUpdateProgress(speedKbps);
            }
            return 0;

        case WM_APP_UPDATE_RESULT: {
            UpdateCheckTask* task = (UpdateCheckTask*)InterlockedExchangePointer(
                (PVOID volatile*)&g_updatePostedResult, NULL);
            HandleCompletedUpdateCheck(task);
            return 0;
        }

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
            /* A manual check dies with the dialog; an automatic one keeps
             * running in the background and may reopen the dialog. */
            if (InterlockedCompareExchange(&g_updateCheckPending,
                                           FALSE, FALSE) == TRUE &&
                (InterlockedCompareExchange(&g_updateCheckAutomatic,
                                            FALSE, FALSE) == FALSE ||
                 !g_hwnd) &&
                g_updateCancelEvent) {
                SetEvent(g_updateCancelEvent);
            }
            if (!g_hwnd) {
                DiscardUpdateTask((UpdateCheckTask*)InterlockedExchangePointer(
                    (PVOID volatile*)&g_updatePostedResult, NULL));
            }
            DiscardPendingUpdateNotice();
            DiscardPreparedUpdate();
            g_cfgHwnd = NULL;
            g_cfgWindowShown = FALSE;
            g_configViewReady = FALSE;
            KillTimer(hwnd, ID_TIMER_CFG_SHOW_FALLBACK);
            /* The updater now owns the staged file and waits for this
             * process to exit; shut down cleanly through the tray path. */
            if (g_updateInstallReady && g_hwnd) {
                PostMessageW(g_hwnd, WM_COMMAND, ID_TRAY_MENU_EXIT, 0);
            }
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
    g_configViewReady = FALSE;
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
                case WM_LBUTTONUP:
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

        case WM_APP_UPDATE_PROGRESS:
            InterlockedExchange(&g_updateProgressPosted, FALSE);
            if (InterlockedCompareExchange(&g_updateCheckPending,
                                           FALSE, FALSE) == TRUE &&
                g_configViewReady) {
                DWORD speedKbps = (DWORD)InterlockedCompareExchange(
                    &g_updateSpeedKbps, 0, 0);
                CfgSendUpdateProgress(speedKbps);
            }
            return 0;

        case WM_APP_UPDATE_RESULT: {
            UpdateCheckTask* task = (UpdateCheckTask*)InterlockedExchangePointer(
                (PVOID volatile*)&g_updatePostedResult, NULL);
            HandleCompletedUpdateCheck(task);
            return 0;
        }

        case WM_APP_DDC_PROBED:
            HandleDdcProbed((DdcProbeResult*)lParam);
            return 0;

        case WM_APP_DDC_SET_RESULT:
            HandleDdcSetResult((int)wParam, lParam != 0);
            return 0;

        case WM_TIMER:
            switch (wParam) {
                case ID_TIMER_REFRESH_MONITORS:
                    KillTimer(hwnd, ID_TIMER_REFRESH_MONITORS);
                    RefreshMonitors();
                    return 0;
                case ID_TIMER_OVERLAY_TOPMOST:
                    KeepOverlaysOnTop();
                    return 0;
                case ID_TIMER_PERSIST:
                    KillTimer(hwnd, ID_TIMER_PERSIST);
                    PersistDirtyMonitors();
                    return 0;
                case ID_TIMER_SCHEDULE:
                    EvaluateSchedule();
                    return 0;
                case ID_TIMER_DDC_RETRY:
                    KillTimer(hwnd, ID_TIMER_DDC_RETRY);
                    g_ddcRetryPending = FALSE;
                    DebugPrint(L"[INFO] Retrying DDC/CI\n");
                    RefreshMonitors();
                    return 0;
                case ID_TIMER_AUTO_UPDATE:
                    if (g_config.autoCheckForUpdates) StartUpdateCheck(TRUE);
                    return 0;
            }
            break;

        /* Monitors come and go, change resolution, or wake up: re-enumerate
         * after the burst of notifications settles. */
        case WM_DISPLAYCHANGE:
            DebugPrint(L"[INFO] Display change (%ux%u)\n",
                       (unsigned)LOWORD(lParam), (unsigned)HIWORD(lParam));
            ScheduleMonitorRefresh(REFRESH_MONITORS_DEBOUNCE_MS);
            return 0;

        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVNODES_CHANGED) {
                ScheduleMonitorRefresh(REFRESH_MONITORS_DEBOUNCE_MS);
            }
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                DebugPrint(L"[INFO] Resumed from sleep; monitors will be re-applied\n");
                ScheduleMonitorRefresh(REFRESH_MONITORS_RESUME_DELAY_MS);
            } else if (wParam == PBT_POWERSETTINGCHANGE) {
                /* Display power state: 0 off, 1 on, 2 dimmed. Monitors that
                 * were switched off need a moment before DDC/CI answers, and
                 * some forget their brightness while off. */
                const POWERBROADCAST_SETTING* setting = (const POWERBROADCAST_SETTING*)lParam;
                if (setting && IsEqualGUID(&setting->PowerSetting, &kGuidConsoleDisplayState) &&
                    setting->DataLength >= sizeof(DWORD)) {
                    LONG state = (LONG)*(const DWORD*)setting->Data;
                    LONG previous = InterlockedExchange(&g_lastDisplayState, state);
                    if (state == 1 && previous == 0) {
                        DebugPrint(L"[INFO] Displays switched on; monitors will be re-applied\n");
                        ScheduleMonitorRefresh(REFRESH_MONITORS_RESUME_DELAY_MS);
                    }
                }
            }
            return TRUE;

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

typedef struct {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} OsVersionInfo;
typedef LONG (WINAPI *PFN_RtlGetVersion)(OsVersionInfo*);

static void LogEnvironment(void) {
    OsVersionInfo os;
    ZeroMemory(&os, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    PFN_RtlGetVersion rtlGetVersion = (PFN_RtlGetVersion)(void*)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtlGetVersion) rtlGetVersion(&os);
    TIME_ZONE_INFORMATION tz;
    DWORD tzState = GetTimeZoneInformation(&tz);
    wchar_t exePath[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    DebugPrint(L"[ENV] Windows %lu.%lu build %lu, %d monitor(s) per GetSystemMetrics, DPI aware, exe %s\n",
               (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
               (unsigned long)os.dwBuildNumber, GetSystemMetrics(SM_CMONITORS), exePath);
    DebugPrint(L"[ENV] time zone \"%s\" bias %ld min (state %lu)\n", tz.StandardName,
               (long)tz.Bias, (unsigned long)tzState);
    const Schedule* sc = &g_config.schedule;
    DebugPrint(L"[ENV] settings: allowBelowMinimum=%d debugLog=%d schedule=%d location=%d (%.4f, %.4f) day=%d night=%d dawn=%+d/%+d dusk=%+d/%+d reset=%02d:%02d\n",
               g_config.allowBelowMinimum, g_config.debugLogEnabled, sc->enabled, sc->hasLocation,
               sc->latitude, sc->longitude, sc->dayLevel, sc->nightLevel,
               sc->dawnStartOffset, sc->dawnEndOffset, sc->duskStartOffset, sc->duskEndOffset,
               sc->cycleResetMinutes / 60, sc->cycleResetMinutes % 60);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* The temporary updater copy and the relaunch after an update both
     * arrive through the command line and must be handled before the
     * single-instance check. */
    BOOL updateHelperHandled = FALSE;
    BOOL updateCompleted = FALSE;
    BOOL reopenSettings = FALSE;
    int updateHelperResult = HandleUpdateCommandLine(
        &updateHelperHandled, &updateCompleted, &reopenSettings);
    if (updateHelperHandled) return updateHelperResult;

    g_hInstance = hInstance;
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_ddcLock);

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
            APP_DISPLAY_NAME_WSTRING L" is already running.\n\nCheck your system tray for the application icon.",
            L"Already Running", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"COM initialization failed", APP_DISPLAY_NAME_WSTRING, MB_OK | MB_ICONERROR);
        if (g_hMutex) { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); }
        return 1;
    }

    LoadConfigFromRegistry(&g_config);
    InterlockedExchange(&g_debugLogEnabled, g_config.debugLogEnabled ? TRUE : FALSE);
    DebugPrint(L"[INFO] " APP_DISPLAY_NAME_WSTRING L" " APP_VERSION_WSTRING L" starting\n");
    LogEnvironment();

    /* The loader is only needed for the configuration dialog; resolving it
     * up front surfaces a missing runtime in the log immediately rather
     * than on first use. */
    if (!load_webview2_loader()) {
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
        MessageBoxW(NULL, L"Failed to register window class", APP_DISPLAY_NAME_WSTRING, MB_OK | MB_ICONERROR);
        CoUninitialize();
        if (g_hMutex) { ReleaseMutex(g_hMutex); CloseHandle(g_hMutex); }
        return 1;
    }

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"NotTooBrightMainClass", APP_NAME,
                             WS_OVERLAPPED, 0, 0, 0, 0,
                             NULL, NULL, hInstance, NULL);
    if (!g_hwnd) {
        MessageBoxW(NULL, L"Failed to create window", APP_DISPLAY_NAME_WSTRING, MB_OK | MB_ICONERROR);
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

    if (!StartDdcWorker()) {
        DebugPrint(L"[ERROR] Could not start the DDC/CI worker thread; hardware control disabled\n");
    }
    g_displayStateNotify = RegisterPowerSettingNotification(
        g_hwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    RefreshMonitors();
    SetTimer(g_hwnd, ID_TIMER_OVERLAY_TOPMOST, OVERLAY_TOPMOST_INTERVAL_MS, NULL);
    UpdateScheduleTimer();

    /* A successful replacement starts exactly once with --finish-update.
     * Reopen settings and show the confirmation only when that update's
     * confirmation requested it; recovery and ordinary launches stay silent. */
    if (updateCompleted) DebugPrint(L"[INFO] Started after a completed update\n");
    if (updateCompleted && reopenSettings) {
        g_updateConfirmationPending = TRUE;
        ShowConfigDialog();
    }
    SetTimer(g_hwnd, ID_TIMER_AUTO_UPDATE, AUTO_UPDATE_INTERVAL_MS, NULL);
    if (g_config.autoCheckForUpdates) StartUpdateCheck(TRUE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DebugPrint(L"[INFO] Exiting\n");
    KillTimer(g_hwnd, ID_TIMER_AUTO_UPDATE);
    if (g_updateCancelEvent) SetEvent(g_updateCancelEvent);
    DiscardUpdateTask((UpdateCheckTask*)InterlockedExchangePointer(
        (PVOID volatile*)&g_updatePostedResult, NULL));
    DiscardPendingUpdateNotice();
    DiscardPreparedUpdate();
    KillTimer(g_hwnd, ID_TIMER_REFRESH_MONITORS);
    KillTimer(g_hwnd, ID_TIMER_OVERLAY_TOPMOST);
    KillTimer(g_hwnd, ID_TIMER_PERSIST);
    KillTimer(g_hwnd, ID_TIMER_SCHEDULE);
    KillTimer(g_hwnd, ID_TIMER_DDC_RETRY);
    PersistDirtyMonitors();
    DestroyAllOverlays();
    StopDdcWorker();
    if (g_displayStateNotify) UnregisterPowerSettingNotification(g_displayStateNotify);
    RemoveTrayIcon();
    CoUninitialize();
    if (g_hwnd) DestroyWindow(g_hwnd);
    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
    }
    return (int)msg.wParam;
}

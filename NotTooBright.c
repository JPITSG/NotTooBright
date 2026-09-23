/*
 * NotTooBright - NotTooBright.c
 *
 * System tray utility for controlling the brightness of every display from
 * one place: desktop monitors that offer no native Windows brightness
 * control, and a laptop's built-in panel.
 *
 * How brightness is controlled:
 *   - Built-in displays (a laptop's own panel): Windows' own brightness
 *     control through WMI, the same one the brightness keys and the Settings
 *     slider use. Changes made in Windows are picked up as they happen.
 *   - Hardware first: DDC/CI through the Windows Monitor Configuration API
 *     (dxva2), which drives the monitor's own backlight exactly like its
 *     on-screen menu. All DDC/CI and WMI traffic runs on a worker thread
 *     because a single command can block for hundreds of milliseconds.
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
 *   - Registry-persisted settings, optional start with Windows
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
#include <oleauto.h>
#include <wbemidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dbt.h>
#include <sddl.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <winver.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <hidusage.h>
#include <cfgmgr32.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
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
/* Set once a monitor has answered DDC/CI (or, for a built-in display,
 * Windows' brightness control); such a monitor is never dimmed in software
 * behind the user's back (its backlight may sit below 100%). */
#define REG_VALUE_MON_HARDWARE L"HardwareControl"
/* The hardware value the monitor reported the very first time it was seen,
 * before anything was written to it; restored when the monitor is hidden. */
#define REG_VALUE_MON_ORIGINAL L"OriginalBrightness"
#define REG_VALUE_MON_ORIGINAL_MAX L"OriginalBrightnessMax"
#define REG_VALUE_MON_SCHEDULED L"Scheduled"
/* Sun-based automatic brightness. */
#define REG_VALUE_SCHEDULE_ENABLED L"ScheduleEnabled"
#define REG_VALUE_SCHEDULE_PAUSED_UNTIL L"SchedulePausedUntil"   /* REG_QWORD, UTC FILETIME */
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
#define REG_VALUE_PAUSE_REMOTE L"PauseInRemoteSession"
#define REG_VALUE_BRIGHTNESS_KEYS L"BrightnessKeys"
#define REG_VALUE_IGNORED_UPDATE_VERSION L"IgnoredUpdateVersion"
/* Start with Windows: this user's Run entry for the executable, named
 * APP_NAME, and the marker Task Manager and Settings use to disable it. */
#define STARTUP_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_APPROVED_RUN_KEY \
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
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
#define WM_APP_BRIGHTNESS_KEY (WM_APP + 6)   /* wParam: +1 up / -1 down, from a reader thread */
#define WM_APP_PANEL_BRIGHTNESS (WM_APP + 7) /* lParam: PanelBrightnessEvent* (receiver frees) */
#define ID_TRAY_MENU_CONFIGURE 1
#define ID_TRAY_MENU_EXIT 2
#define ID_TRAY_MENU_BRIGHTER 3
#define ID_TRAY_MENU_DIMMER 4
#define ID_TRAY_MENU_RESUME_SCHEDULE 5
/* Step applied by the tray menu's Increase/Decrease brightness items. */
#define TRAY_STEP_PERCENT 10
/* Which monitor the tray menu items control: "" for none (items hidden),
 * "*" for every visible monitor, otherwise a monitor identity key. */
#define REG_VALUE_TRAY_TARGET L"TrayMenuTarget"
#define TRAY_TARGET_ALL L"*"
/* Preset levels listed between Increase and Decrease in the tray menu, in
 * the order the user typed them; stored as "100,75,50,25". */
#define REG_VALUE_TRAY_PRESETS L"TrayMenuPresets"
#define TRAY_MAX_PRESETS 20
#define ID_TRAY_MENU_PRESET_FIRST 1000   /* + preset index */
/* The tray tooltip lists every visible monitor; rebuilding it is deferred
 * briefly so a slider drag does not rewrite it dozens of times a second. */
#define ID_TIMER_TOOLTIP 9

/* Keyboard brightness keys: HID consumer-control usages read straight from
 * the keyboard's collection (see "Keyboard brightness keys" below). A key
 * counts again while it stays down only every KEY_REPEAT_MIN_MS. */
#define ID_TIMER_KEY_DEVICES 6
#define KEY_DEVICES_DEBOUNCE_MS 1500
#define KEY_MAX_READERS 8
#define KEY_REPEAT_MIN_MS 250
#define HID_USAGE_CONSUMER_BRIGHTNESS_UP 0x6F
#define HID_USAGE_CONSUMER_BRIGHTNESS_DOWN 0x70
#define TOOLTIP_UPDATE_DELAY_MS 200

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
 * checks and re-raises them only while software dimming is visible. */
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

/* Built-in displays (see "Built-in displays" below). A level Windows
 * reports for one is looked at PANEL_SETTLE_MS after it arrives, so that
 * the result of our own write, or the power notification behind the
 * change, has come in first; the timer only runs while something waits. */
#define ID_TIMER_PANEL 10
#define PANEL_SETTLE_MS 1500
/* A level reported this soon after our own write is that write's echo. */
#define PANEL_ECHO_MS 1500
/* Windows applies a level of its own to a built-in display after a resume,
 * a display switched back on, a display change, or a power source or
 * battery saver change; what it reports this soon after one of those is
 * not the user's doing. */
#define PANEL_QUIET_MS 5000
/* How long one WMI call may take before it counts as failed. */
#define PANEL_WMI_TIMEOUT_MS 5000
/* Delay before listening again after WMI dropped the change notifications. */
#define PANEL_EVENTS_RETRY_MIN_MS 5000
#define PANEL_EVENTS_RETRY_MAX_MS (5 * 60 * 1000)
#define PANEL_MAX_LEVELS 101
#define PANEL_NAME_CHARS 200
#define PANEL_PATH_CHARS 256

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
#define SCHEDULE_DAY_RADIUS 2
#define SCHEDULE_DAY_COUNT (2 * SCHEDULE_DAY_RADIUS + 1)
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
    ULONGLONG pausedUntil;    /* UTC FILETIME; 0 unless paused by a manual change (persisted) */
} Schedule;

typedef struct {
    BOOL allowBelowMinimum;   /* let hardware monitors dim further in software */
    BOOL debugLogEnabled;
    BOOL autoCheckForUpdates;
    BOOL pauseInRemoteSession; /* leave the monitors alone while viewed through Remote Desktop */
    BOOL brightnessKeys;      /* the keyboard's brightness keys step every monitor */
    wchar_t trayTarget[128];  /* "", "*", or a monitor key */
    int trayPresets[TRAY_MAX_PRESETS];  /* -SOFT_MAX_DIM..100, menu order */
    int trayPresetCount;
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

/* Only the solar anchors are cached; brightness and pause state always
 * use the current clock. Accessed on the main thread. */
typedef struct {
    BOOL valid;
    SYSTEMTIME date;
    double latitude;
    double longitude;
    DWORD zoneId;
    DYNAMIC_TIME_ZONE_INFORMATION zone;
    SolarDay days[SCHEDULE_DAY_COUNT];
} SolarCache;

/* Where the schedule's curve is: on a plateau or in one of the transitions. */
typedef enum {
    SCHEDULE_PHASE_NIGHT = 0,
    SCHEDULE_PHASE_DAWN,      /* night -> daytime transition */
    SCHEDULE_PHASE_DAY,
    SCHEDULE_PHASE_DUSK       /* daytime -> night transition */
} SchedulePhase;

typedef enum {
    HW_UNKNOWN = 0,   /* probe in flight */
    HW_AVAILABLE,     /* monitor answers VCP 0x10, or Windows controls its brightness */
    HW_UNAVAILABLE    /* no hardware brightness: software dimming only */
} HardwareState;

typedef enum {
    MODE_PROBING = 0,
    MODE_HARDWARE,
    MODE_SOFTWARE,
    MODE_WAITING      /* known hardware monitor not answering: left alone, retried */
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
    BOOL knownHardware;           /* has answered DDC/CI or WMI at some point (persisted) */
    int probeFailures;            /* consecutive failed probes in this outage */
    DWORD ddcMax;                 /* 100 for a built-in display (percent) */
    DWORD ddcMin;                 /* non-zero only on the high-level route */
    DWORD ddcCurrent;
    BOOL ddcHighLevel;            /* GetMonitorBrightness/SetMonitorBrightness route */
    BOOL builtin;                 /* built-in display: brightness through Windows (WMI), not DDC/CI */
    BOOL panelChecked;            /* Windows answered that it does not control this display */
    wchar_t instancePath[160];    /* DISPLAY\<PnP id>\<instance>, how WMI names the display */
    BYTE panelLevels[PANEL_MAX_LEVELS]; /* levels a built-in display supports, in percent */
    int panelLevelCount;
    ULONGLONG panelEchoUntil;     /* levels reported before this tick echo our own writes */
    BOOL panelReportPending;      /* a level reported by Windows waits for PANEL_SETTLE_MS */
    int panelReport;
    ULONGLONG panelReportTick;
    BOOL panelReapply;            /* not at our level through no action of the user: put it back */
    BOOL forceSoftware;           /* user asked for software dimming only */
    BOOL hidden;                  /* removed from the dialog and left alone until a rescan */
    BOOL hasOriginal;             /* original hardware value recorded */
    DWORD originalRaw;            /* raw VCP value at first sighting */
    DWORD originalMax;            /* its maximum, for display as a percent */
    BOOL scheduled;               /* follows the automatic brightness schedule */
    int value;                    /* desired brightness, -SOFT_MAX_DIM..100 */
    BOOL hasValue;
    int lastHwSent;               /* last percent handed to the worker, -1 = none */
    int failures;                 /* consecutive failed hardware writes */
    HWND overlay;                 /* software dimming window, first entry only */
    int overlayDim;               /* current overlay dim 0..SOFT_MAX_DIM */
    BOOL dirty;                   /* needs persisting */
    wchar_t error[160];           /* user-facing status, empty when fine */
} Monitor;

/* Main-to-worker probe request: which physical monitors to open and query.
 * patient: the monitor has answered before, so it is worth the long retry
 * sequence including a handle reopen. checkPanel: ask Windows (WMI) whether
 * it controls this display's brightness; builtin: it did last time, so
 * DDC/CI is not tried if WMI does not answer now. */
typedef struct {
    int uid;
    HMONITOR hmon;
    int physicalIndex;
    BOOL patient;
    BOOL checkPanel;
    BOOL builtin;
    wchar_t instancePath[160];
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
 * high-level GetMonitorBrightness/SetMonitorBrightness route works.
 * builtin: a built-in display, controlled through Windows (WMI), with the
 * levels it supports; current and max are then percent. panelChecked:
 * Windows answered and does not control this display. */
typedef struct {
    int uid;
    BOOL supported;
    BOOL highLevel;
    BOOL builtin;
    BOOL panelChecked;
    DWORD min;
    DWORD current;
    DWORD max;
    DWORD error;
    DWORD elapsedMs;
    BYTE levels[PANEL_MAX_LEVELS];
    int levelCount;
} DdcProbeResult;

/* Change-notification-to-main: Windows reports a built-in display's level. */
typedef struct {
    wchar_t instanceName[PANEL_NAME_CHARS];
    int brightness;
} PanelBrightnessEvent;

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
static BOOL g_overlayTimerRunning = FALSE;
static SolarCache g_solarCache;
static int g_loggedSchedulePhase = -1;      /* last SchedulePhase written to the log */
static BOOL g_remoteSession = FALSE;        /* paused: the session is viewed through Remote Desktop */

/* Built-in displays: the change listener and the power notifications that
 * explain Windows' own adjustments, started with the first such display. */
static BOOL g_panelWatchStarted = FALSE;
static HANDLE g_panelEventThread = NULL;
static HANDLE g_panelEventStop = NULL;      /* manual-reset: the listener should exit */
static HPOWERNOTIFY g_powerSourceNotify = NULL;
static HPOWERNOTIFY g_batterySaverNotify = NULL;
static HPOWERNOTIFY g_energySaverNotify = NULL;
static LONG g_lastPowerSource = -1;
static LONG g_lastBatterySaver = -1;
static LONG g_lastEnergySaver = -1;
static ULONGLONG g_panelQuietUntil = 0;     /* tick: reports before it are Windows' own adjustments */
static ULONGLONG g_panelHoldUntil = 0;      /* tick: our level is not put back before it */

/* One open consumer-control collection with its reader thread. */
typedef struct {
    HANDLE device;
    HANDLE thread;
    HANDLE stop;                  /* manual-reset: the reader should exit */
    PHIDP_PREPARSED_DATA preparsed;
    USHORT reportLength;
    wchar_t name[128];
} KeyReader;
static KeyReader g_keyReaders[KEY_MAX_READERS];
static int g_keyReaderCount = 0;

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
static SIZE g_cfgFrameSize = {0, 0};
static int g_cfgShowFallbackTries = 0;

static PFN_CreateCoreWebView2EnvironmentWithOptions fnCreateEnvironment = NULL;
static WCHAR g_extractedDllPath[MAX_PATH] = {0};

/* GUID_CONSOLE_DISPLAY_STATE, declared locally so no GUID library is needed. */
static const GUID kGuidConsoleDisplayState =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
/* GUID_ACDC_POWER_SOURCE, GUID_POWER_SAVING_STATUS (battery saver) and
 * GUID_ENERGY_SAVER_STATUS (Windows 11): Windows applies its own level to a
 * built-in display when these change. */
static const GUID kGuidAcDcPowerSource =
    { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };
static const GUID kGuidPowerSavingStatus =
    { 0xe00958c0, 0xc213, 0x4ace, { 0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5 } };
static const GUID kGuidEnergySaverStatus =
    { 0x550e8400, 0xe29b, 0x41d4, { 0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00 } };
/* CLSID_WbemLocator and IID_IWbemLocator, for the same reason. */
static const CLSID kClsidWbemLocator =
    { 0x4590f811, 0x1d3a, 0x11d0, { 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };
static const IID kIidWbemLocator =
    { 0xdc12a687, 0x737f, 0x11cf, { 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };

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
static void UpdateRemoteSessionState(void);
static void UpdateBrightnessKeyReaders(void);
static void CloseBrightnessKeyReaders(void);
static void ScheduleDdcRetry(void);
static void SchedulePersist(void);
static void PersistDirtyMonitors(void);
static int VisibleMonitorCount(void);
static void UnhideAllMonitors(void);
static void DdcRequestSet(int uid, int percent);
static void DdcRequestProbe(const DdcProbeEntry* entries, int count);
static void StartPanelWatch(void);
static void SchedulePanelService(void);
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
static void ScheduleTooltipUpdate(void);
static void UpdateTrayTooltip(void);
static void RefreshTrayIcon(void);
static void RemoveTrayIcon(void);
static void ShowContextMenu(HWND hwnd);
static BOOL ScheduleTargetNow(int* value);
static void EvaluateSchedule(void);
static void NoteManualChange(Monitor* m);
static void UpdateScheduleTimer(void);
static ULONGLONG NowFileTime(void);
static BOOL IsSchedulePaused(ULONGLONG nowFt);
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

/* Reads "100, 75, 50" style text into tray preset levels: whole numbers
 * within the brightness range, kept in the order given, duplicates and
 * anything else dropped, at most TRAY_MAX_PRESETS. Returns the count. The
 * dialog validates what the user types; this guards the registry value. */
static int ParseTrayPresets(const wchar_t* text, int* out, int max) {
    int count = 0;
    const wchar_t* p = text;
    while (*p && count < max) {
        while (*p == L' ' || *p == L'\t' || *p == L',') p++;
        if (!*p) break;
        const wchar_t* start = p;
        if (*p == L'-') p++;
        const wchar_t* digits = p;
        while (*p >= L'0' && *p <= L'9') p++;
        BOOL valid = p != digits && p - digits <= 3;
        while (*p == L' ' || *p == L'\t') p++;
        if (*p && *p != L',') valid = FALSE;
        while (*p && *p != L',') p++;   /* skip the rest of a bad token */
        if (!valid) continue;
        int value = _wtoi(start);
        if (value < -SOFT_MAX_DIM || value > 100) continue;
        BOOL duplicate = FALSE;
        for (int i = 0; i < count && !duplicate; i++) duplicate = out[i] == value;
        if (!duplicate) out[count++] = value;
    }
    return count;
}

/* The inverse, "100,75,50", for the registry and the dialog. */
static void FormatTrayPresets(const int* presets, int count, wchar_t* out, size_t cap) {
    size_t len = 0;
    out[0] = L'\0';
    for (int i = 0; i < count; i++) {
        int written = swprintf_s(out + len, cap - len, i ? L",%d" : L"%d", presets[i]);
        if (written < 0) break;
        len += (size_t)written;
    }
}

static BOOL LoadConfigFromRegistry(Configuration* config) {
    ZeroMemory(config, sizeof(*config));
    SetScheduleDefaults(&config->schedule);
    config->autoCheckForUpdates = TRUE;   /* default enabled */
    config->pauseInRemoteSession = TRUE;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return FALSE;
    }
    ReadRegistryBool(hKey, REG_VALUE_DEBUGLOG, &config->debugLogEnabled);
    ReadRegistryBool(hKey, REG_VALUE_ALLOW_BELOW_MIN, &config->allowBelowMinimum);
    ReadRegistryBool(hKey, REG_VALUE_AUTO_UPDATE, &config->autoCheckForUpdates);
    ReadRegistryBool(hKey, REG_VALUE_PAUSE_REMOTE, &config->pauseInRemoteSession);
    ReadRegistryBool(hKey, REG_VALUE_BRIGHTNESS_KEYS, &config->brightnessKeys);

    DWORD dataType = 0;
    DWORD dataSize = sizeof(config->trayTarget) - sizeof(wchar_t);
    if (RegQueryValueExW(hKey, REG_VALUE_TRAY_TARGET, NULL, &dataType,
                         (LPBYTE)config->trayTarget, &dataSize) != ERROR_SUCCESS ||
        dataType != REG_SZ) {
        config->trayTarget[0] = L'\0';
    }
    config->trayTarget[(sizeof(config->trayTarget) / sizeof(wchar_t)) - 1] = L'\0';

    wchar_t presetText[128] = {0};
    dataSize = sizeof(presetText) - sizeof(wchar_t);
    if (RegQueryValueExW(hKey, REG_VALUE_TRAY_PRESETS, NULL, &dataType,
                         (LPBYTE)presetText, &dataSize) == ERROR_SUCCESS &&
        dataType == REG_SZ) {
        config->trayPresetCount = ParseTrayPresets(presetText, config->trayPresets, TRAY_MAX_PRESETS);
    }

    dataSize = sizeof(g_ignoredUpdateVersion);
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
    ReadRegistryQword(hKey, REG_VALUE_SCHEDULE_PAUSED_UNTIL, &sc->pausedUntil);
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
    if (!WriteRegistryDword(hKey, REG_VALUE_PAUSE_REMOTE, config->pauseInRemoteSession ? 1 : 0)) success = FALSE;
    if (!WriteRegistryDword(hKey, REG_VALUE_BRIGHTNESS_KEYS, config->brightnessKeys ? 1 : 0)) success = FALSE;
    RegSetValueExW(hKey, REG_VALUE_TRAY_TARGET, 0, REG_SZ, (const BYTE*)config->trayTarget,
                   (DWORD)((wcslen(config->trayTarget) + 1) * sizeof(wchar_t)));
    wchar_t presetText[128];
    FormatTrayPresets(config->trayPresets, config->trayPresetCount, presetText, 128);
    RegSetValueExW(hKey, REG_VALUE_TRAY_PRESETS, 0, REG_SZ, (const BYTE*)presetText,
                   (DWORD)((wcslen(presetText) + 1) * sizeof(wchar_t)));
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
    WriteRegistryQword(hKey, REG_VALUE_SCHEDULE_PAUSED_UNTIL, sc->pausedUntil);
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
    /* Up to 0.0.28 the pause was per monitor; the value is now on the root key. */
    RegDeleteValueW(hKey, L"SchedulePausedUntil");
    if (m->hasOriginal) {
        WriteRegistryDword(hKey, REG_VALUE_MON_ORIGINAL, m->originalRaw);
        WriteRegistryDword(hKey, REG_VALUE_MON_ORIGINAL_MAX, m->originalMax);
    }
    /* Informational: lets a user recognise entries when browsing the registry. */
    RegSetValueExW(hKey, REG_VALUE_MON_NAME, 0, REG_SZ, (const BYTE*)m->name,
                   (DWORD)((wcslen(m->name) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

/* ── Start with Windows ──────────────────────────────────────────────────── */

/* Ported from ../c_APIMonitor; keep the two in step. A per-user Run entry
 * launches this executable at sign-in. Task Manager and Settings can
 * disable that entry without deleting it (odd first byte of its
 * StartupApproved value), so a disabled entry counts as off. The option is
 * the entry itself, not a setting of our own: the dialog shows whether one
 * is registered for this copy of the executable. */

static BOOL GetStartupCommand(wchar_t* command, size_t commandCch) {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return FALSE;
    return swprintf_s(command, commandCch, L"\"%s\"", path) > 0;
}

static BOOL IsStartWithWindowsEnabled(void) {
    wchar_t expected[MAX_PATH + 2];
    wchar_t actual[MAX_PATH + 2];
    DWORD size = sizeof(actual);
    if (!GetStartupCommand(expected, sizeof(expected) / sizeof(wchar_t)) ||
        RegGetValueW(HKEY_CURRENT_USER, STARTUP_RUN_KEY, APP_NAME,
                     RRF_RT_REG_SZ, NULL, actual, &size) != ERROR_SUCCESS ||
        _wcsicmp(actual, expected) != 0) {
        return FALSE;
    }

    BYTE approved[64];
    size = sizeof(approved);
    if (RegGetValueW(HKEY_CURRENT_USER, STARTUP_APPROVED_RUN_KEY, APP_NAME,
                     RRF_RT_REG_BINARY, NULL, approved, &size) != ERROR_SUCCESS ||
        size == 0) {
        return TRUE; /* No marker (or Windows 7, which has none) means enabled. */
    }
    return (approved[0] & 1) == 0;
}

static LONG SetStartWithWindows(BOOL enable) {
    LONG result;
    if (enable) {
        wchar_t command[MAX_PATH + 2];
        HKEY key;
        if (!GetStartupCommand(command, sizeof(command) / sizeof(wchar_t))) {
            return ERROR_BAD_PATHNAME;
        }
        result = RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_RUN_KEY, 0, NULL,
                                 REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                                 &key, NULL);
        if (result != ERROR_SUCCESS) return result;
        result = RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE*)command,
                                (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    } else {
        result = RegDeleteKeyValueW(HKEY_CURRENT_USER, STARTUP_RUN_KEY, APP_NAME);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    if (result != ERROR_SUCCESS) return result;

    /* Drop any disabled marker so turning the option on takes effect and
     * turning it off leaves nothing behind. */
    result = RegDeleteKeyValueW(HKEY_CURRENT_USER, STARTUP_APPROVED_RUN_KEY,
                                APP_NAME);
    return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
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
    /* The monitor's device instance, which is how WMI names a display whose
     * brightness Windows controls. */
    m->instancePath[0] = L'\0';
    if (pnpId[0] && instance[0]) {
        swprintf_s(m->instancePath, sizeof(m->instancePath) / sizeof(wchar_t),
                   L"DISPLAY\\%s\\%s", pnpId, instance);
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

/* Hardware-only brightness needs no z-order polling. Keep the original
 * cadence whenever an overlay is visible, including across lock/unlock;
 * an overlay on the user's desktop still needs protecting after unlock. */
static void UpdateOverlayTimer(void) {
    if (!g_hwnd) return;
    BOOL needed = FALSE;
    if (!g_remoteSession) {
        for (int i = 0; i < g_monitorCount; i++) {
            const Monitor* m = &g_monitors[i];
            if (!m->hidden && m->overlay && m->overlayDim > 0 && IsWindowVisible(m->overlay)) {
                needed = TRUE;
                break;
            }
        }
    }
    if (needed && !g_overlayTimerRunning) {
        g_overlayTimerRunning = SetTimer(g_hwnd, ID_TIMER_OVERLAY_TOPMOST,
                                          OVERLAY_TOPMOST_INTERVAL_MS, NULL) != 0;
    } else if (!needed && g_overlayTimerRunning) {
        KillTimer(g_hwnd, ID_TIMER_OVERLAY_TOPMOST);
        g_overlayTimerRunning = FALSE;
    }
}

/* dim is the percentage of darkening, 0..SOFT_MAX_DIM. */
static void SetOverlayDim(Monitor* m, int dim) {
    if (dim < 0) dim = 0;
    if (dim > SOFT_MAX_DIM) dim = SOFT_MAX_DIM;
    int previous = m->overlayDim;
    m->overlayDim = dim;
    if (!m->overlay) {
        UpdateOverlayTimer();
        return;
    }
    if (dim == 0) {
        if (IsWindowVisible(m->overlay)) {
            ShowWindow(m->overlay, SW_HIDE);
            DebugPrint(L"[OVERLAY] %s: hidden (was %d%%)\n", m->name, previous);
        }
        UpdateOverlayTimer();
        return;
    }
    BYTE alpha = (BYTE)((dim * 255 + 50) / 100);
    SetLayeredWindowAttributes(m->overlay, 0, alpha, LWA_ALPHA);
    if (!IsWindowVisible(m->overlay)) {
        ShowWindow(m->overlay, SW_SHOWNOACTIVATE);
        PositionOverlay(m);
    }
    if (previous != dim) DebugPrint(L"[OVERLAY] %s: dim %d%% (alpha %u)\n", m->name, dim, (unsigned)alpha);
    UpdateOverlayTimer();
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
    UpdateOverlayTimer();
}

static void DestroyAllOverlays(void) {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].overlay) {
            DestroyWindow(g_monitors[i].overlay);
            g_monitors[i].overlay = NULL;
        }
        g_monitors[i].overlayDim = 0;
    }
    UpdateOverlayTimer();
}

/* ── Built-in displays (Windows brightness control over WMI) ─────────────── */

/* A laptop's own panel has no DDC/CI: Windows drives its backlight itself
 * (the brightness keys, the Settings slider) and offers the same control to
 * programs through WMI in root\WMI - WmiMonitorBrightness for the level and
 * the steps the panel supports, WmiMonitorBrightnessMethods.WmiSetBrightness
 * to set it, and WmiMonitorBrightnessEvent whenever it changes. A display
 * WMI lists there is driven this way instead of over DDC/CI. Reads and
 * writes run on the worker thread like all other hardware access; waiting
 * for change notifications blocks, so that has a thread of its own. The
 * process never calls CoInitializeSecurity (WebView2 shares it), so every
 * WMI proxy gets its security set directly. Logged under [PANEL]. */

/* One display as WMI lists it. */
typedef struct {
    wchar_t instanceName[PANEL_NAME_CHARS];   /* DISPLAY\<PnP id>\<instance>_0 */
    wchar_t methodPath[PANEL_PATH_CHARS];     /* its WmiMonitorBrightnessMethods object */
    int current;                              /* percent */
    BOOL active;
    BYTE levels[PANEL_MAX_LEVELS];
    int levelCount;
} PanelInfo;

/* The worker's WMI connection: made on first use, dropped after a failure. */
typedef struct {
    IWbemServices* services;
    IWbemClassObject* setParams;              /* WmiSetBrightness input parameters */
} PanelWmi;

/* WMI names a display after its device instance with a "_0" suffix, for
 * example DISPLAY\SDC4161\4&2e4de1c1&0&UID265988_0. */
static BOOL PanelInstanceMatches(const wchar_t* instanceName, const wchar_t* instancePath) {
    size_t n = wcslen(instancePath);
    return n > 0 && _wcsnicmp(instanceName, instancePath, n) == 0 && instanceName[n] == L'_';
}

/* The nearest level the display supports (the value itself while that list
 * is unknown); a tie goes to the level listed first. */
static int SnapPanelLevel(const BYTE* levels, int count, int percent) {
    int best = percent, bestDistance = INT_MAX;
    for (int i = 0; i < count; i++) {
        int distance = abs((int)levels[i] - percent);
        if (distance < bestDistance) {
            best = levels[i];
            bestDistance = distance;
        }
    }
    return best;
}

static void SetWmiProxySecurity(IUnknown* proxy) {
    CoSetProxyBlanket(proxy, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
}

/* root\WMI on this computer, or NULL. */
static IWbemServices* ConnectWmi(void) {
    IWbemLocator* locator = NULL;
    IWbemServices* services = NULL;
    HRESULT hr = CoCreateInstance(&kClsidWbemLocator, NULL, CLSCTX_INPROC_SERVER,
                                  &kIidWbemLocator, (void**)&locator);
    if (SUCCEEDED(hr)) {
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        hr = ns ? IWbemLocator_ConnectServer(locator, ns, NULL, NULL, NULL,
                                             WBEM_FLAG_CONNECT_USE_MAX_WAIT, NULL, NULL, &services)
                : E_OUTOFMEMORY;
        SysFreeString(ns);
        IWbemLocator_Release(locator);
    }
    if (FAILED(hr) || !services) {
        DebugPrint(L"[PANEL] Could not connect to WMI (0x%08lX)\n", (unsigned long)hr);
        return NULL;
    }
    SetWmiProxySecurity((IUnknown*)services);
    return services;
}

static void DropPanelWmi(PanelWmi* w) {
    if (w->setParams) IWbemClassObject_Release(w->setParams);
    if (w->services) IWbemServices_Release(w->services);
    w->setParams = NULL;
    w->services = NULL;
}

/* A forward-only, semisynchronous WQL query; its results are read with a
 * timeout (NextWmiObject), so a stuck WMI cannot hold the worker forever. */
static HRESULT ExecWmiQuery(IWbemServices* services, const wchar_t* text, IEnumWbemClassObject** out) {
    *out = NULL;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(text);
    HRESULT hr = (language && query)
        ? IWbemServices_ExecQuery(services, language, query,
                                  WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, out)
        : E_OUTOFMEMORY;
    SysFreeString(language);
    SysFreeString(query);
    if (SUCCEEDED(hr) && !*out) hr = E_POINTER;
    if (SUCCEEDED(hr)) SetWmiProxySecurity((IUnknown*)*out);
    return hr;
}

/* S_OK with the next object, S_FALSE at the end, or a failure (including a
 * reply that took longer than PANEL_WMI_TIMEOUT_MS). */
static HRESULT NextWmiObject(IEnumWbemClassObject* e, IWbemClassObject** obj) {
    ULONG got = 0;
    *obj = NULL;
    HRESULT hr = IEnumWbemClassObject_Next(e, PANEL_WMI_TIMEOUT_MS, 1, obj, &got);
    if (hr == (HRESULT)WBEM_S_TIMEDOUT) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (FAILED(hr)) {
        if (*obj) IWbemClassObject_Release(*obj);
        *obj = NULL;
        return hr;
    }
    return (got && *obj) ? S_OK : S_FALSE;
}

/* uint8 properties arrive as VT_UI1, uint32 as VT_I4, booleans as VT_BOOL. */
static BOOL WmiGetInt(IWbemClassObject* obj, const wchar_t* name, int* out) {
    VARIANT v;
    VariantInit(&v);
    BOOL ok = SUCCEEDED(IWbemClassObject_Get(obj, name, 0, &v, NULL, NULL)) &&
              v.vt != VT_NULL && v.vt != VT_EMPTY &&
              SUCCEEDED(VariantChangeType(&v, &v, 0, VT_I4));
    if (ok) *out = (int)v.lVal;
    VariantClear(&v);
    return ok;
}

static BOOL WmiGetString(IWbemClassObject* obj, const wchar_t* name, wchar_t* out, size_t count) {
    VARIANT v;
    VariantInit(&v);
    BOOL ok = SUCCEEDED(IWbemClassObject_Get(obj, name, 0, &v, NULL, NULL)) &&
              v.vt == VT_BSTR && v.bstrVal;
    if (ok) wcsncpy_s(out, count, v.bstrVal, _TRUNCATE);
    VariantClear(&v);
    return ok;
}

/* Level: the steps the panel supports, in percent (uint8[]). */
static int WmiGetLevels(IWbemClassObject* obj, BYTE* out, int max) {
    VARIANT v;
    VariantInit(&v);
    int count = 0;
    if (SUCCEEDED(IWbemClassObject_Get(obj, L"Level", 0, &v, NULL, NULL)) &&
        v.vt == (VT_ARRAY | VT_UI1) && v.parray) {
        LONG lower = 0, upper = -1;
        BYTE* data = NULL;
        if (SUCCEEDED(SafeArrayGetLBound(v.parray, 1, &lower)) &&
            SUCCEEDED(SafeArrayGetUBound(v.parray, 1, &upper)) &&
            SUCCEEDED(SafeArrayAccessData(v.parray, (void**)&data))) {
            for (LONG i = 0; i <= upper - lower && count < max; i++) {
                if (data[i] <= 100) out[count++] = data[i];
            }
            SafeArrayUnaccessData(v.parray);
        }
    }
    VariantClear(&v);
    return count;
}

/* How WMI says that Windows controls no display's brightness here (a
 * desktop answers "not supported"): a definite none, not a failure. */
static BOOL IsNoPanelAnswer(HRESULT hr) {
    return hr == (HRESULT)WBEM_E_NOT_SUPPORTED || hr == (HRESULT)WBEM_E_INVALID_CLASS ||
           hr == (HRESULT)WBEM_E_NOT_FOUND;
}

/* Lists the displays whose brightness Windows controls, normally a laptop's
 * built-in panel. Returns how many, 0 for none, or -1 when WMI could not
 * answer (the connection is dropped and made again next time; the caller
 * treats those displays as it did before). */
static int QueryPanels(PanelWmi* w, PanelInfo* out, int max) {
    ULONGLONG start = GetTickCount64();
    if (!w->services) w->services = ConnectWmi();
    if (!w->services) return -1;
    IEnumWbemClassObject* e = NULL;
    IWbemClassObject* obj = NULL;
    int count = 0;
    HRESULT hr = ExecWmiQuery(w->services, L"SELECT * FROM WmiMonitorBrightness", &e);
    while (SUCCEEDED(hr) && (hr = NextWmiObject(e, &obj)) == S_OK) {
        int current = -1, active = 1;
        if (count < max) {
            PanelInfo* p = &out[count];
            ZeroMemory(p, sizeof(*p));
            if (WmiGetString(obj, L"InstanceName", p->instanceName, PANEL_NAME_CHARS) &&
                WmiGetInt(obj, L"CurrentBrightness", &current) && current >= 0 && current <= 100) {
                WmiGetInt(obj, L"Active", &active);
                p->current = current;
                p->active = active != 0;
                p->levelCount = WmiGetLevels(obj, p->levels, PANEL_MAX_LEVELS);
                count++;
            }
        }
        IWbemClassObject_Release(obj);
    }
    if (e) IEnumWbemClassObject_Release(e);
    e = NULL;
    if (FAILED(hr)) {
        if (IsNoPanelAnswer(hr)) {
            DebugPrint(L"[PANEL] Windows controls no display's brightness here (0x%08lX, %lu ms)\n",
                       (unsigned long)hr, (unsigned long)(GetTickCount64() - start));
            return 0;
        }
        DebugPrint(L"[PANEL] WmiMonitorBrightness query FAILED (0x%08lX, %lu ms)\n",
                   (unsigned long)hr, (unsigned long)(GetTickCount64() - start));
        DropPanelWmi(w);
        return -1;
    }
    /* The object that carries WmiSetBrightness for each of them. */
    if (count > 0) {
        hr = ExecWmiQuery(w->services, L"SELECT * FROM WmiMonitorBrightnessMethods", &e);
        while (SUCCEEDED(hr) && (hr = NextWmiObject(e, &obj)) == S_OK) {
            wchar_t name[PANEL_NAME_CHARS];
            if (WmiGetString(obj, L"InstanceName", name, PANEL_NAME_CHARS)) {
                for (int i = 0; i < count; i++) {
                    if (_wcsicmp(out[i].instanceName, name) == 0) {
                        WmiGetString(obj, L"__RELPATH", out[i].methodPath, PANEL_PATH_CHARS);
                    }
                }
            }
            IWbemClassObject_Release(obj);
        }
        if (e) IEnumWbemClassObject_Release(e);
        if (FAILED(hr)) {
            DebugPrint(L"[PANEL] WmiMonitorBrightnessMethods query FAILED (0x%08lX, %lu ms)\n",
                       (unsigned long)hr, (unsigned long)(GetTickCount64() - start));
            DropPanelWmi(w);
            return -1;
        }
    }
    /* A display without that object cannot be set; it stays with DDC/CI. */
    int kept = 0;
    for (int i = 0; i < count; i++) {
        const PanelInfo* p = &out[i];
        int low = 100, high = 0;
        for (int l = 0; l < p->levelCount; l++) {
            if (p->levels[l] < low) low = p->levels[l];
            if (p->levels[l] > high) high = p->levels[l];
        }
        DebugPrint(L"[PANEL] %s: current %d%%, %d level(s) %d..%d, active %d, method %s\n",
                   p->instanceName, p->current, p->levelCount, p->levelCount ? low : 0,
                   p->levelCount ? high : 100, p->active, p->methodPath[0] ? p->methodPath : L"MISSING");
        if (p->methodPath[0]) out[kept++] = *p;
    }
    DebugPrint(L"[PANEL] Windows controls the brightness of %d display(s) (%lu ms)\n", kept,
               (unsigned long)(GetTickCount64() - start));
    return kept;
}

/* WmiSetBrightness(Timeout, Brightness) on the display's method object,
 * waiting at most PANEL_WMI_TIMEOUT_MS for it. Timeout 0, as Windows tools
 * use it: the level stays until something changes it. */
static HRESULT SetPanelBrightness(PanelWmi* w, const wchar_t* methodPath, int percent) {
    if (!w->services) w->services = ConnectWmi();
    if (!w->services) return E_FAIL;
    HRESULT hr = S_OK;
    if (!w->setParams) {
        IWbemClassObject* cls = NULL;
        BSTR name = SysAllocString(L"WmiMonitorBrightnessMethods");
        hr = name ? IWbemServices_GetObject(w->services, name, 0, NULL, &cls, NULL) : E_OUTOFMEMORY;
        SysFreeString(name);
        if (SUCCEEDED(hr) && cls) {
            hr = IWbemClassObject_GetMethod(cls, L"WmiSetBrightness", 0, &w->setParams, NULL);
        }
        if (cls) IWbemClassObject_Release(cls);
        if (SUCCEEDED(hr) && !w->setParams) hr = E_POINTER;
    }
    IWbemClassObject* in = NULL;
    if (SUCCEEDED(hr)) hr = IWbemClassObject_SpawnInstance(w->setParams, 0, &in);
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(hr)) {
        v.vt = VT_I4;   /* a uint32 travels as VT_I4 */
        v.lVal = 0;
        hr = IWbemClassObject_Put(in, L"Timeout", 0, &v, 0);
    }
    if (SUCCEEDED(hr)) {
        v.vt = VT_UI1;
        v.bVal = (BYTE)percent;
        hr = IWbemClassObject_Put(in, L"Brightness", 0, &v, 0);
    }
    IWbemCallResult* call = NULL;
    if (SUCCEEDED(hr)) {
        BSTR path = SysAllocString(methodPath);
        BSTR method = SysAllocString(L"WmiSetBrightness");
        hr = (path && method)
            ? IWbemServices_ExecMethod(w->services, path, method, WBEM_FLAG_RETURN_IMMEDIATELY,
                                       NULL, in, NULL, &call)
            : E_OUTOFMEMORY;
        SysFreeString(path);
        SysFreeString(method);
    }
    if (SUCCEEDED(hr) && call) {
        SetWmiProxySecurity((IUnknown*)call);
        LONG status = 0;
        HRESULT wait = IWbemCallResult_GetCallStatus(call, PANEL_WMI_TIMEOUT_MS, &status);
        hr = wait == (HRESULT)WBEM_S_TIMEDOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
           : FAILED(wait) ? wait : (HRESULT)status;
    }
    if (call) IWbemCallResult_Release(call);
    if (in) IWbemClassObject_Release(in);
    return hr;
}

/* Waits for WmiMonitorBrightnessEvent - Windows reporting a new level for a
 * built-in display, whoever changed it - and posts each one to the main
 * window, which works out whether it was the user. The wait has no timeout
 * (no wake-ups while nothing changes); if WMI drops the subscription it is
 * made again after a growing delay. Started with the first built-in display
 * and left running until exit. */
static DWORD WINAPI PanelEventThread(LPVOID param) {
    (void)param;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        DebugPrint(L"[PANEL] Brightness change notifications unavailable: COM could not start\n");
        return 0;
    }
    DWORD retryMs = PANEL_EVENTS_RETRY_MIN_MS;
    while (WaitForSingleObject(g_panelEventStop, 0) != WAIT_OBJECT_0) {
        IWbemServices* services = ConnectWmi();
        IEnumWbemClassObject* events = NULL;
        HRESULT hr = E_FAIL;
        if (services) {
            BSTR language = SysAllocString(L"WQL");
            BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessEvent");
            hr = (language && query)
                ? IWbemServices_ExecNotificationQuery(services, language, query,
                      WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, NULL, &events)
                : E_OUTOFMEMORY;
            SysFreeString(language);
            SysFreeString(query);
        }
        if (SUCCEEDED(hr) && events) {
            SetWmiProxySecurity((IUnknown*)events);
            DebugPrint(L"[PANEL] Listening for brightness changes made in Windows\n");
            retryMs = PANEL_EVENTS_RETRY_MIN_MS;
            for (;;) {
                IWbemClassObject* obj = NULL;
                ULONG got = 0;
                hr = IEnumWbemClassObject_Next(events, (LONG)WBEM_INFINITE, 1, &obj, &got);
                BOOL stop = WaitForSingleObject(g_panelEventStop, 0) == WAIT_OBJECT_0;
                if (FAILED(hr) || !got || !obj || stop) {
                    if (obj) IWbemClassObject_Release(obj);
                    if (SUCCEEDED(hr)) hr = S_FALSE;   /* ended without an error */
                    break;
                }
                PanelBrightnessEvent* report = (PanelBrightnessEvent*)calloc(1, sizeof(*report));
                int brightness = -1;
                if (report && WmiGetString(obj, L"InstanceName", report->instanceName, PANEL_NAME_CHARS) &&
                    WmiGetInt(obj, L"Brightness", &brightness) && brightness >= 0 && brightness <= 100) {
                    report->brightness = brightness;
                    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_PANEL_BRIGHTNESS, 0, (LPARAM)report)) free(report);
                } else {
                    free(report);
                }
                IWbemClassObject_Release(obj);
            }
        }
        if (events) IEnumWbemClassObject_Release(events);
        if (services) IWbemServices_Release(services);
        if (WaitForSingleObject(g_panelEventStop, 0) == WAIT_OBJECT_0) break;
        DebugPrint(L"[PANEL] Brightness change notifications %s (0x%08lX); trying again in %lu s\n",
                   events ? L"stopped" : L"unavailable", (unsigned long)hr, (unsigned long)(retryMs / 1000));
        if (WaitForSingleObject(g_panelEventStop, retryMs) == WAIT_OBJECT_0) break;
        retryMs = retryMs * 2 > PANEL_EVENTS_RETRY_MAX_MS ? PANEL_EVENTS_RETRY_MAX_MS : retryMs * 2;
    }
    CoUninitialize();
    return 0;
}

/* ── DDC/CI worker ───────────────────────────────────────────────────────── */

/* Everything that talks to a monitor over DDC/CI happens on this thread: a
 * single VCP read or write can take hundreds of milliseconds, and a
 * misbehaving monitor can stall for seconds, so the UI thread only ever
 * queues requests and receives posted results. Built-in displays are read
 * and set here as well, through WMI. */

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
    BOOL builtin;                 /* set through WMI (methodPath), not DDC/CI */
    BOOL panelChecked;            /* WMI answered and does not list it */
    DWORD min;
    DWORD max;
    wchar_t methodPath[PANEL_PATH_CHARS];
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
            /* A failed initial open (or reopen) leaves an empty cached
             * source. Retry it without disturbing valid, settled handles. */
            if (src->count == 0) OpenWorkerSource(src);
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
    r->panelChecked = wm->panelChecked;
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

/* One level to a built-in display (raw values are percent too); a failed
 * call is tried once more on a fresh WMI connection. */
static BOOL WritePanelBrightness(PanelWmi* wmi, const WorkerMonitor* wm, int target) {
    int percent = target < 0 ? 0 : (target > 100 ? 100 : target);
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 2 && FAILED(hr); attempt++) {
        if (attempt) DropPanelWmi(wmi);
        ULONGLONG start = GetTickCount64();
        hr = SetPanelBrightness(wmi, wm->methodPath, percent);
        DebugPrint(L"[PANEL] monitor %d WmiSetBrightness(%d%%) attempt %d -> %s (0x%08lX), %lu ms\n",
                   wm->uid, percent, attempt + 1, SUCCEEDED(hr) ? L"ok" : L"FAILED",
                   (unsigned long)hr, (unsigned long)(GetTickCount64() - start));
    }
    return SUCCEEDED(hr);
}

/* Built-in displays go first: WMI answers for them in milliseconds, while a
 * DDC/CI probe can take seconds. Each display Windows controls gets its
 * result posted right away; the others are returned in ddcJob (with
 * panelChecked: WMI answered and does not list it) for DDC/CI. A display
 * that was built-in last time is never handed to DDC/CI, which a laptop
 * panel does not speak: if WMI does not list it now it is reported as not
 * answering and retried like any known monitor. */
static int ProbePanels(PanelWmi* wmi, const DdcProbeEntry* job, int jobCount,
                       WorkerMonitor* monitors, int* monitorCount,
                       DdcProbeEntry* ddcJob, BOOL* panelChecked) {
    static PanelInfo panels[MAX_MONITORS];   /* worker thread only */
    BOOL wanted = FALSE;
    for (int i = 0; i < jobCount; i++) wanted = wanted || job[i].checkPanel;
    ULONGLONG start = GetTickCount64();
    int panelCount = (wanted && wmi) ? QueryPanels(wmi, panels, MAX_MONITORS) : -1;
    DWORD elapsed = (DWORD)(GetTickCount64() - start);
    int ddcCount = 0;
    for (int i = 0; i < jobCount; i++) {
        const DdcProbeEntry* entry = &job[i];
        const PanelInfo* panel = NULL;
        for (int p = 0; entry->checkPanel && p < panelCount && !panel; p++) {
            if (PanelInstanceMatches(panels[p].instanceName, entry->instancePath)) panel = &panels[p];
        }
        if (!panel && !entry->builtin) {
            panelChecked[ddcCount] = entry->checkPanel && panelCount >= 0;
            ddcJob[ddcCount++] = *entry;
            continue;
        }
        if (*monitorCount >= MAX_MONITORS) continue;
        WorkerMonitor* wm = &monitors[(*monitorCount)++];
        ZeroMemory(wm, sizeof(*wm));
        wm->uid = entry->uid;
        wm->hmon = entry->hmon;
        wm->physicalIndex = entry->physicalIndex;
        wm->patient = entry->patient;
        wm->builtin = TRUE;
        wm->max = 100;
        DdcProbeResult* r = (DdcProbeResult*)calloc(1, sizeof(*r));
        if (!r) continue;
        r->uid = entry->uid;
        r->builtin = TRUE;
        r->max = 100;
        r->elapsedMs = elapsed;
        if (panel) {
            wm->supported = TRUE;
            wcscpy_s(wm->methodPath, PANEL_PATH_CHARS, panel->methodPath);
            r->supported = TRUE;
            r->current = (DWORD)panel->current;
            memcpy(r->levels, panel->levels, sizeof(r->levels));
            r->levelCount = panel->levelCount;
            DebugPrint(L"[PANEL] monitor %d (%s): Windows controls its brightness, current %d%%\n",
                       entry->uid, entry->instancePath, panel->current);
        } else {
            r->error = panelCount < 0 ? ERROR_GEN_FAILURE : ERROR_NOT_FOUND;
            DebugPrint(L"[PANEL] monitor %d (%s): built-in display %s; not trying DDC/CI\n",
                       entry->uid, entry->instancePath,
                       panelCount < 0 ? L"but WMI did not answer" : L"no longer listed by WMI");
        }
        if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_DDC_PROBED, 0, (LPARAM)r)) free(r);
    }
    return ddcCount;
}

/* Performs queued brightness writes. During a probe job only monitors
 * already probed in that job are written (the others stay queued), so a
 * slider does not have to wait for a slow display at the end of the job. */
static void DrainPendingWrites(PanelWmi* wmi, WorkerSource* sources, int sourceCount,
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
        if (wm && wm->supported && wm->builtin) {
            ok = WritePanelBrightness(wmi, wm, target);
        } else if (wm && wm->supported) {
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
    PanelWmi wmi;
    ZeroMemory(&wmi, sizeof(wmi));
    /* Built-in displays are reached through WMI, which needs COM here. */
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DebugPrint(L"[DDC] worker thread started (id %lu), COM 0x%08lX\n", (unsigned long)GetCurrentThreadId(),
               (unsigned long)com);

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
                DebugPrint(L"[DDC]   entry %d: uid %d hmon=%p physicalIndex %d patient %d checkPanel %d builtin %d instance %s\n",
                           i, job[i].uid, (void*)job[i].hmon, job[i].physicalIndex, job[i].patient,
                           job[i].checkPanel, job[i].builtin, job[i].instancePath);
            }
            monitorCount = 0;
            DdcProbeEntry ddcJob[MAX_MONITORS];
            BOOL panelChecked[MAX_MONITORS];
            int ddcCount = ProbePanels(SUCCEEDED(com) ? &wmi : NULL, job, jobCount, monitors,
                                       &monitorCount, ddcJob, panelChecked);
            /* Built-in displays take their writes while DDC/CI probes. */
            DrainPendingWrites(&wmi, sources, sourceCount, monitors, monitorCount, TRUE);
            OpenJobSources(sources, &sourceCount, ddcJob, ddcCount);
            for (int i = 0; i < ddcCount && monitorCount < MAX_MONITORS; i++) {
                WorkerMonitor* wm = &monitors[monitorCount++];
                ZeroMemory(wm, sizeof(*wm));
                wm->uid = ddcJob[i].uid;
                wm->hmon = ddcJob[i].hmon;
                wm->physicalIndex = ddcJob[i].physicalIndex;
                wm->patient = ddcJob[i].patient;
                wm->panelChecked = panelChecked[i];
                wm->min = 0;
                wm->max = 100;
                ProbeWorkerMonitor(sources, &sourceCount, wm);
                if (InterlockedCompareExchange(&g_ddcStop, 0, 0)) break;
                /* Keep sliders responsive while the remaining displays probe. */
                DrainPendingWrites(&wmi, sources, sourceCount, monitors, monitorCount, TRUE);
            }
            CloseUnusedSources(sources, &sourceCount);
            DebugPrint(L"[DDC] probe job done; %d source(s) kept open\n", sourceCount);
        }

        DrainPendingWrites(&wmi, sources, sourceCount, monitors, monitorCount, FALSE);
    }

    ReleaseWorkerSources(sources, &sourceCount);
    DropPanelWmi(&wmi);
    if (SUCCEEDED(com)) CoUninitialize();
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

/* The backlight level the value asks for: 0 while software dimming goes
 * below the minimum, and on a built-in display the nearest level it has. */
static int MonitorHardwareTarget(const Monitor* m) {
    int hw = m->value < 0 ? 0 : (m->value > 100 ? 100 : m->value);
    return m->builtin ? SnapPanelLevel(m->panelLevels, m->panelLevelCount, hw) : hw;
}

/* GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed by Windows for
 * inactivity; -1 until the first notification. */
static BOOL DisplayIsOn(void) {
    return g_lastDisplayState != 0 && g_lastDisplayState != 2;
}

/* Turns the desired value into hardware and overlay settings. Hardware
 * monitors: 0..100 is the backlight, negative values keep the backlight at
 * its minimum and add software dimming. Software monitors: the overlay
 * alone provides the whole range. */
/* ── Keyboard brightness keys ────────────────────────────────────────────── */

/* Brightness keys have no virtual-key code: they are usages on the
 * keyboard's HID Consumer Control collection, which Windows itself handles
 * (that is what shows the brightness flyout, and on a laptop moves the
 * built-in panel). The collection is opened directly and its input
 * reports are read on a thread per device, the way hidapi does it. This is
 * a plain device read, not part of input routing, so it works whatever
 * window is focused, including elevated ones that withhold raw input
 * (RIDEV_INPUTSINK) from a normal process - the likely reason the raw
 * input attempt in 0.0.9 seemed unreliable. Keyboards and mice refuse to be
 * opened this way (their collections belong to kbdhid/mouhid); consumer
 * collections are shared with Windows' own reader. Everything is logged
 * under [INPUT] because a keyboard that does not send the standard usages
 * can only be diagnosed from a log. */

/* Whether a report counts as a press: the first report with the usage
 * set, then again every KEY_REPEAT_MIN_MS while it stays set. That covers
 * keyboards that send press and release, ones that repeat while held, and
 * ones that never send a release at all. */
static BOOL BrightnessKeyPressed(BOOL down, BOOL* wasDown, ULONGLONG* lastPress, ULONGLONG now) {
    BOOL press = down && (!*wasDown || now - *lastPress >= KEY_REPEAT_MIN_MS);
    if (press) *lastPress = now;
    *wasDown = down;
    return press;
}

static DWORD WINAPI KeyReaderThread(LPVOID param) {
    KeyReader* r = (KeyReader*)param;
    ULONG maxUsages = HidP_MaxUsageListLength(HidP_Input, 0, r->preparsed);
    if (maxUsages == 0 || maxUsages > 64) maxUsages = 64;
    USAGE_AND_PAGE* usages = (USAGE_AND_PAGE*)calloc(maxUsages, sizeof(USAGE_AND_PAGE));
    BYTE* report = (BYTE*)calloc(r->reportLength, 1);
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!usages || !report || !ov.hEvent) {
        free(usages);
        free(report);
        if (ov.hEvent) CloseHandle(ov.hEvent);
        return 0;
    }
    HANDLE waits[2] = { r->stop, ov.hEvent };
    BOOL upDown = FALSE, downDown = FALSE;
    ULONGLONG lastUp = 0, lastDown = 0;
    for (;;) {
        ResetEvent(ov.hEvent);
        DWORD read = 0;
        if (!ReadFile(r->device, report, r->reportLength, &read, &ov)) {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                DebugPrint(L"[INPUT] %s: read failed (error %lu); the device is gone\n", r->name, (unsigned long)error);
                break;
            }
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(r->device, &ov);
                break;
            }
            if (!GetOverlappedResult(r->device, &ov, &read, FALSE)) {
                DebugPrint(L"[INPUT] %s: read failed (error %lu); the device is gone\n", r->name, (unsigned long)GetLastError());
                break;
            }
        }
        if (read == 0) continue;
        ULONG count = maxUsages;
        NTSTATUS status = HidP_GetUsagesEx(HidP_Input, 0, usages, &count, r->preparsed, (PCHAR)report, read);
        if (status != HIDP_STATUS_SUCCESS) {
            /* Reports of another report id in the same collection, typically. */
            if (status != HIDP_STATUS_INCOMPATIBLE_REPORT_ID) {
                DebugPrint(L"[INPUT] %s: %lu-byte report not decoded (status 0x%08lX)\n", r->name,
                           (unsigned long)read, (unsigned long)status);
            }
            continue;
        }
        BOOL up = FALSE, down = FALSE;
        wchar_t list[256] = L"";
        size_t len = 0;
        for (ULONG i = 0; i < count; i++) {
            if (usages[i].UsagePage == HID_USAGE_PAGE_CONSUMER) {
                if (usages[i].Usage == HID_USAGE_CONSUMER_BRIGHTNESS_UP) up = TRUE;
                else if (usages[i].Usage == HID_USAGE_CONSUMER_BRIGHTNESS_DOWN) down = TRUE;
            }
            if (len + 16 < 256) {
                len += (size_t)swprintf_s(list + len, 256 - len, L" %02X:%04X",
                                          (unsigned)usages[i].UsagePage, (unsigned)usages[i].Usage);
            }
        }
        DebugPrint(L"[INPUT] %s: report id %u, %lu byte(s), %lu usage(s)%s%s\n", r->name,
                   (unsigned)report[0], (unsigned long)read, (unsigned long)count, list,
                   up ? L" -> brightness up" : down ? L" -> brightness down" : L"");
        ULONGLONG now = GetTickCount64();
        if (BrightnessKeyPressed(up, &upDown, &lastUp, now)) {
            PostMessageW(g_hwnd, WM_APP_BRIGHTNESS_KEY, (WPARAM)1, 0);
        }
        if (BrightnessKeyPressed(down, &downDown, &lastDown, now)) {
            PostMessageW(g_hwnd, WM_APP_BRIGHTNESS_KEY, (WPARAM)-1, 0);
        }
    }
    free(usages);
    free(report);
    CloseHandle(ov.hEvent);
    return 0;
}

/* Opens every present HID collection on the Consumer page that has input
 * reports and starts a reader for it. */
static void OpenBrightnessKeyReaders(void) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    ULONG chars = 0;
    if (CM_Get_Device_Interface_List_SizeW(&chars, &hidGuid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        chars == 0) {
        DebugPrint(L"[INPUT] No HID device interfaces could be listed\n");
        return;
    }
    wchar_t* list = (wchar_t*)calloc(chars, sizeof(wchar_t));
    if (!list) return;
    if (CM_Get_Device_Interface_ListW(&hidGuid, NULL, list, chars, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        DebugPrint(L"[INPUT] HID device interfaces could not be listed\n");
        free(list);
        return;
    }
    int seen = 0, refused = 0;
    for (const wchar_t* path = list; *path; path += wcslen(path) + 1) {
        seen++;
        if (g_keyReaderCount >= KEY_MAX_READERS) break;
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            /* Keyboards and mice are expected to refuse; anything else is
             * worth a line, it may be the collection with the keys. */
            DWORD error = GetLastError();
            refused++;
            if (error != ERROR_ACCESS_DENIED) {
                DebugPrint(L"[INPUT] %s: cannot be opened (error %lu)\n", path, (unsigned long)error);
            }
            continue;
        }
        PHIDP_PREPARSED_DATA preparsed = NULL;
        HIDP_CAPS caps;
        if (!HidD_GetPreparsedData(h, &preparsed) || HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) {
            if (preparsed) HidD_FreePreparsedData(preparsed);
            CloseHandle(h);
            continue;
        }
        if (caps.UsagePage != HID_USAGE_PAGE_CONSUMER || caps.InputReportByteLength == 0) {
            HidD_FreePreparsedData(preparsed);
            CloseHandle(h);
            continue;
        }
        KeyReader* r = &g_keyReaders[g_keyReaderCount];
        ZeroMemory(r, sizeof(*r));
        r->device = h;
        r->preparsed = preparsed;
        r->reportLength = caps.InputReportByteLength;
        if (!HidD_GetProductString(h, r->name, sizeof(r->name)) || !r->name[0]) {
            wcsncpy_s(r->name, 128, path, _TRUNCATE);
        }
        r->name[127] = L'\0';
        r->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        r->thread = r->stop ? CreateThread(NULL, 0, KeyReaderThread, r, 0, NULL) : NULL;
        if (!r->thread) {
            DebugPrint(L"[INPUT] %s: reader could not be started (error %lu)\n", r->name, (unsigned long)GetLastError());
            if (r->stop) CloseHandle(r->stop);
            HidD_FreePreparsedData(preparsed);
            CloseHandle(h);
            continue;
        }
        DebugPrint(L"[INPUT] Listening on \"%s\" (consumer usage 0x%04X, input reports of %u bytes) %s\n",
                   r->name, (unsigned)caps.Usage, (unsigned)caps.InputReportByteLength, path);
        g_keyReaderCount++;
    }
    free(list);
    DebugPrint(L"[INPUT] %d HID interface(s), %d refused to open, %d consumer control reader(s) running\n",
               seen, refused, g_keyReaderCount);
}

static void CloseBrightnessKeyReaders(void) {
    for (int i = 0; i < g_keyReaderCount; i++) {
        KeyReader* r = &g_keyReaders[i];
        SetEvent(r->stop);
        CancelIoEx(r->device, NULL);
        if (WaitForSingleObject(r->thread, 2000) != WAIT_OBJECT_0) {
            DebugPrint(L"[WARNING] Brightness key reader for %s did not stop in time\n", r->name);
        }
        CloseHandle(r->thread);
        CloseHandle(r->stop);
        HidD_FreePreparsedData(r->preparsed);
        CloseHandle(r->device);
    }
    if (g_keyReaderCount) DebugPrint(L"[INPUT] %d reader(s) closed\n", g_keyReaderCount);
    g_keyReaderCount = 0;
}

/* Reopens the readers to match the option and the devices present now;
 * called on enable/disable, after device changes, and after a resume. */
static void UpdateBrightnessKeyReaders(void) {
    CloseBrightnessKeyReaders();
    if (g_config.brightnessKeys) OpenBrightnessKeyReaders();
}

static void ScheduleBrightnessKeyReaderRestart(void) {
    if (g_hwnd && g_config.brightnessKeys) {
        SetTimer(g_hwnd, ID_TIMER_KEY_DEVICES, KEY_DEVICES_DEBOUNCE_MS, NULL);
    }
}

/* One press: every listed monitor moves by the tray step from its own
 * value, as a manual change (so scheduled monitors pause), like the tray
 * menu's Increase/Decrease. A built-in display is left out: Windows moves
 * it for the same key press itself, and that change is picked up like any
 * other made in Windows. */
static void ApplyBrightnessKey(int direction) {
    if (!g_config.brightnessKeys) return;   /* posted just before the option went off */
    if (g_remoteSession) {
        DebugPrint(L"[INPUT] Brightness key ignored during the remote session\n");
        return;
    }
    int changed = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (m->hidden || m->builtin || MonitorMode(m) == MODE_PROBING) continue;
        int before = m->value;
        SetMonitorValue(m, m->value + direction * TRAY_STEP_PERCENT);
        NoteManualChange(m);
        if (m->value != before) changed++;
    }
    DebugPrint(L"[INPUT] Brightness key %s: %d monitor(s) changed\n", direction > 0 ? L"up" : L"down", changed);
    if (changed) PushMonitorsToDialog();
}

/* ── Remote Desktop sessions ─────────────────────────────────────────────── */

/* Microsoft's documented test: SM_REMOTESESSION, and for the sessions that
 * metric misses (RemoteFX and similar) whether this session owns the
 * physical console ("glass"). A session that was disconnected without
 * logging off also counts as remote: its displays belong to whoever is at
 * the console now. The details are returned for the log. */
static BOOL QueryRemoteSession(BOOL* metric, DWORD* sessionId, DWORD* glassId, BOOL* glassKnown) {
    *metric = GetSystemMetrics(SM_REMOTESESSION) != 0;
    *sessionId = 0;
    *glassId = 0;
    *glassKnown = FALSE;
    ProcessIdToSessionId(GetCurrentProcessId(), sessionId);
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0, size = sizeof(*glassId);
        if (RegQueryValueExW(hKey, L"GlassSessionId", NULL, &type, (LPBYTE)glassId, &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            *glassKnown = TRUE;
        }
        RegCloseKey(hKey);
    }
    if (*metric) return TRUE;
    return *glassKnown && *sessionId != *glassId;
}

static BOOL IsRemoteSession(void) {
    BOOL metric, glassKnown;
    DWORD sessionId, glassId;
    return QueryRemoteSession(&metric, &sessionId, &glassId, &glassKnown);
}

static void PushRemoteSessionToDialog(void) {
    if (!g_cfgWebView) return;
    webview_cfg_execute_script(g_remoteSession
        ? L"window.onRemoteSession && window.onRemoteSession(true)"
        : L"window.onRemoteSession && window.onRemoteSession(false)");
}

/* Enters or leaves the paused state. Called after anything that can change
 * how the session is viewed (session notifications, display changes, every
 * refresh) so a missed notification cannot leave the state stale. While
 * paused nothing is enumerated, probed, written, or dimmed: the monitor
 * list stays as it was at the console and the displays keep their state. */
static void UpdateRemoteSessionState(void) {
    BOOL remote = g_config.pauseInRemoteSession && IsRemoteSession();
    if (remote == g_remoteSession) return;
    g_remoteSession = remote;
    if (remote) {
        DebugPrint(L"[INFO] Remote Desktop session: pausing; %d monitor(s) keep their state\n", g_monitorCount);
        if (g_hwnd) {
            KillTimer(g_hwnd, ID_TIMER_REFRESH_MONITORS);
            KillTimer(g_hwnd, ID_TIMER_DDC_RETRY);
        }
        g_ddcRetryPending = FALSE;
        PersistDirtyMonitors();
        /* The overlays would cover the remote desktop; hide them without
         * forgetting their dimming. The physical monitor handles belong to
         * the console's displays, so the worker lets go of them. */
        for (int i = 0; i < g_monitorCount; i++) {
            Monitor* m = &g_monitors[i];
            if (m->overlay && IsWindowVisible(m->overlay)) {
                ShowWindow(m->overlay, SW_HIDE);
                DebugPrint(L"[OVERLAY] %s: hidden for the remote session (dim %d%% kept)\n", m->name, m->overlayDim);
            }
        }
        DdcProbeEntry none[1];
        DdcRequestProbe(none, 0);
    } else {
        DebugPrint(L"[INFO] Back at the console: resuming, monitors will be re-detected and re-applied\n");
        g_ddcRetryDelayMs = DDC_RETRY_INITIAL_MS;
        ScheduleMonitorRefresh(REFRESH_MONITORS_RESUME_DELAY_MS);
    }
    UpdateOverlayTimer();
    PushRemoteSessionToDialog();
    ScheduleTooltipUpdate();
}

/* ── Applying values ─────────────────────────────────────────────────────── */

static void ApplyMonitor(Monitor* m) {
    /* Hidden monitors are not controlled at all: no overlay, no hardware
     * writes. Whatever state the display is in stays that way. */
    if (m->hidden) return;
    /* Paused for a remote session: the value is kept and applied once the
     * console is back and the monitor has been re-probed. */
    if (g_remoteSession) return;
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
    BOOL wrote = FALSE, deferred = FALSE;
    int hw = MonitorHardwareTarget(m);
    if (mode == MODE_HARDWARE) {
        if (value < 0) dim = -value;
        if (hw != m->lastHwSent) {
            if (m->builtin && !DisplayIsOn()) {
                /* Windows has dimmed or switched off the display for
                 * inactivity; a write now would light it up behind its
                 * back. The level goes out once the display is on. */
                m->panelReapply = TRUE;
                deferred = TRUE;
            } else {
                m->lastHwSent = hw;
                DdcRequestSet(m->uid, hw);
                if (m->builtin) {
                    m->panelEchoUntil = GetTickCount64() + PANEL_ECHO_MS;
                    m->panelReapply = FALSE;
                }
                wrote = TRUE;
            }
        }
    } else {
        dim = 100 - value;
    }
    DebugPrint(L"[APPLY] %s (%s): mode %s value %d -> hardware %s%d%%, overlay dim %d%%\n",
               m->name, m->device, ModeName(mode), value,
               mode == MODE_HARDWARE ? (wrote ? L"write " : deferred ? L"deferred " : L"unchanged ") : L"n/a ",
               mode == MODE_HARDWARE ? hw : 0, dim);
    SetOverlayDim(m, dim);
    ScheduleTooltipUpdate();
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
    /* Through Remote Desktop the enumeration would only show the remote
     * display; keep the list from the console instead. */
    UpdateRemoteSessionState();
    if (g_remoteSession) {
        DebugPrint(L"[INFO] Refresh skipped: paused for the Remote Desktop session\n");
        return;
    }
    /* A removed monitor will no longer be in the array when the debounced
     * save runs. Flush its pending brightness/pause before replacing it. */
    PersistDirtyMonitors();
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
            /* A monitor seen before keeps everything it had - persisted
             * flags (scheduled, hidden, software-only, pause), probe state,
             * overlay - and takes only identity and geometry from the fresh
             * enumeration. Copying the whole record is deliberate: listing
             * fields one by one is how the scheduled flag got lost on every
             * refresh (0.0.19). The hardware state stays as it was while the
             * re-probe runs so the dialog does not flash "detecting". */
            Monitor seen = *dst;
            *dst = *old;
            dst->hmon = seen.hmon;
            dst->physicalIndex = seen.physicalIndex;
            dst->rect = seen.rect;
            dst->primary = seen.primary;
            wcscpy_s(dst->device, CCHDEVICENAME, seen.device);
            wcscpy_s(dst->name, sizeof(dst->name) / sizeof(wchar_t), seen.name);
            wcscpy_s(dst->instancePath, sizeof(dst->instancePath) / sizeof(wchar_t), seen.instancePath);
            dst->lastHwSent = -1;   /* the re-probe decides whether to write */
            dst->failures = 0;
            dst->error[0] = L'\0';
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
            /* Windows is asked about every display once; after it said it
             * does not control one, only a rescan asks again. */
            probe[at].checkPanel = m->builtin || !m->panelChecked;
            probe[at].builtin = m->builtin;
            wcscpy_s(probe[at].instancePath, sizeof(probe[at].instancePath) / sizeof(wchar_t),
                     m->instancePath);
            probeCount++;
        }
        DebugPrint(L"[INFO] Monitor %d: %s (%s, %ldx%ld%s) key=%s value=%d%s state=%s knownHardware=%d%s%s%s%s original=%s%lu/%lu\n",
                   m->uid, m->name, m->device,
                   (long)(m->rect.right - m->rect.left), (long)(m->rect.bottom - m->rect.top),
                   m->primary ? L", primary" : L"", m->key,
                   m->hasValue ? m->value : -1000,
                   m->hasValue ? L"" : L" (none saved)",
                   HardwareStateName(m->hardwareState), m->knownHardware,
                   m->builtin ? L", built-in" : L"",
                   m->forceSoftware ? L", software only" : L"",
                   m->hidden ? L", hidden" : L"",
                   m->scheduled ? L", scheduled" : L"",
                   m->hasOriginal ? L"" : L"none ",
                   (unsigned long)m->originalRaw, (unsigned long)m->originalMax);
    }
    DebugPrint(L"[INFO] %d monitor(s) enumerated, %d hidden; probing\n",
               g_monitorCount, g_monitorCount - probeCount);
    DdcRequestProbe(probe, probeCount);
    UpdateOverlayTimer();
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
    if (IsSchedulePaused(NowFileTime())) return;
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
    if (g_remoteSession) {
        /* From a job started before the pause; the state from the console
         * must not be overwritten with failures against remote displays. */
        DebugPrint(L"[INFO] Probe result for uid %d ignored during the remote session\n", r->uid);
        free(r);
        return;
    }
    Monitor* m = FindMonitorByUid(r->uid);
    DebugPrint(L"[INFO] Probe result for uid %d (%s): supported=%d builtin=%d highLevel=%d min=%lu current=%lu max=%lu error=%lu elapsed=%lu ms\n",
               r->uid, m ? m->name : L"unknown monitor", r->supported, r->builtin, r->highLevel,
               (unsigned long)r->min, (unsigned long)r->current, (unsigned long)r->max,
               (unsigned long)r->error, (unsigned long)r->elapsedMs);
    if (m) {
        BrightnessMode before = MonitorMode(m);
        const wchar_t* control = r->builtin ? L"Windows brightness control" : L"DDC/CI";
        m->builtin = r->builtin;
        if (r->panelChecked) m->panelChecked = TRUE;
        if (r->builtin && r->supported) {
            int count = r->levelCount < 0 ? 0 : (r->levelCount > PANEL_MAX_LEVELS ? PANEL_MAX_LEVELS : r->levelCount);
            memcpy(m->panelLevels, r->levels, (size_t)count);
            m->panelLevelCount = count;
            StartPanelWatch();
        }
        if (r->supported) {
            m->hardwareState = HW_AVAILABLE;
            m->ddcMax = r->max;
            m->ddcMin = r->highLevel ? r->min : 0;
            m->ddcHighLevel = r->highLevel;
            m->ddcCurrent = r->current;
            m->failures = 0;
            m->error[0] = L'\0';
            if (m->probeFailures > 0) {
                DebugPrint(L"[INFO] %s (%s): %s answering again after %d failed probe(s)\n",
                           m->name, m->device, control, m->probeFailures);
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
            m->lastHwSent = (MonitorHardwareTarget(m) == percent) ? percent : -1;
            if (m->builtin && m->panelReportPending && m->lastHwSent < 0) {
                /* A level Windows reported is still waiting to be looked at
                 * and may be the user's: it decides first, and ours goes
                 * back afterwards unless it was. */
                m->lastHwSent = MonitorHardwareTarget(m);
                m->panelReapply = TRUE;
            }
            DebugPrint(L"[INFO] %s (%s): %s brightness available, current %lu/%lu (%lu ms)\n",
                       m->name, m->device, control, (unsigned long)r->current,
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
                DebugPrint(L"[WARNING] %s (%s): %s not answering (error %lu, %lu ms, failure %d); "
                           L"leaving the monitor alone and retrying\n",
                           m->name, m->device, control, (unsigned long)r->error,
                           (unsigned long)r->elapsedMs, m->probeFailures);
                ScheduleDdcRetry();
            } else {
                DebugPrint(L"[INFO] %s (%s): no %s brightness (error %lu, %lu ms); using software dimming\n",
                           m->name, m->device, control, (unsigned long)r->error,
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
    if (!m || g_remoteSession) return;
    /* Windows reports the level a write to a built-in display set (even a
     * write that failed half-way may have); that report is an echo. */
    if (m->builtin) m->panelEchoUntil = GetTickCount64() + PANEL_ECHO_MS;
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
        DebugPrint(L"[WARNING] %s (%s): %d consecutive %s failures; leaving the monitor alone and retrying\n",
                   m->name, m->device, m->failures, m->builtin ? L"Windows brightness control" : L"DDC/CI");
    } else {
        wcscpy_s(m->error, sizeof(m->error) / sizeof(wchar_t),
                 L"The monitor did not accept the last brightness change.");
    }
    /* A constant schedule target will not call ApplyMonitor again. Retry
     * even the first failed write, independently of future slider changes. */
    ScheduleDdcRetry();
    PushMonitorsToDialog();
}

/* ── Built-in displays: changes made in Windows ──────────────────────────── */

/* Windows reports every new level of a built-in display, whoever set it.
 * Only the latest report per display counts, looked at PANEL_SETTLE_MS
 * after it arrived. It is one of:
 *   - our level: nothing to do;
 *   - not the user's doing: the level we wrote before (Windows restores it
 *     when it undims the display, possibly after we changed ours), a
 *     display dimmed or switched off for inactivity, or a level Windows
 *     applied after a resume, a display change, or a power source or
 *     battery saver change. Ours is put back once the display is on and
 *     Windows is done (g_panelHoldUntil);
 *   - the echo of one of our own earlier writes (a slider drag): ignored;
 *   - otherwise the user changed it in Windows (the brightness keys, the
 *     Settings or quick settings slider): adopted like a slider move, so it
 *     shows in the dialog and the tooltip, is remembered, and pauses the
 *     schedule like any manual change. */

static BOOL PanelReapplyDue(const Monitor* m) {
    return m->builtin && m->panelReapply && !m->hidden && !g_remoteSession && DisplayIsOn();
}

static void ProcessPanelReport(Monitor* m) {
    if (g_remoteSession || m->hidden || MonitorMode(m) != MODE_HARDWARE) return;
    int reported = m->panelReport;
    int target = MonitorHardwareTarget(m);
    if (reported == target) {
        m->panelReapply = FALSE;   /* at our level, whatever put it there */
        return;
    }
    if (reported == m->lastHwSent || !DisplayIsOn() || m->panelReportTick < g_panelQuietUntil) {
        DebugPrint(L"[PANEL] %s (%s): Windows set %d%% itself; %d%% goes back once the display is on and settled\n",
                   m->name, m->device, reported, target);
        m->panelReapply = TRUE;
        return;
    }
    if (m->panelReportTick < m->panelEchoUntil) return;   /* an earlier level of ours */
    DebugPrint(L"[PANEL] %s (%s): changed in Windows from %d%% to %d%%; adopted as a manual change\n",
               m->name, m->device, target, reported);
    m->value = reported;
    m->hasValue = TRUE;
    m->dirty = TRUE;
    SchedulePersist();
    m->lastHwSent = reported;   /* already there: nothing to write */
    m->panelReapply = FALSE;
    ApplyMonitor(m);            /* drops software dimming below the minimum */
    NoteManualChange(m);
    PushMonitorsToDialog();
}

/* Arms ID_TIMER_PANEL for the earliest waiting work: a report to look at,
 * or our level to put back. A display with a report still waiting is not
 * written to before that report has been looked at: it may be the user's.
 * Nothing waiting, no timer. */
static void SchedulePanelService(void) {
    if (!g_hwnd) return;
    ULONGLONG now = GetTickCount64(), due = 0;
    BOOL waiting = FALSE;
    for (int i = 0; i < g_monitorCount; i++) {
        const Monitor* m = &g_monitors[i];
        ULONGLONG at;
        if (m->builtin && m->panelReportPending) {
            at = m->panelReportTick + PANEL_SETTLE_MS;
        } else if (PanelReapplyDue(m)) {
            at = g_panelHoldUntil > now ? g_panelHoldUntil : now;
        } else {
            continue;
        }
        if (!waiting || at < due) due = at;
        waiting = TRUE;
    }
    if (!waiting) {
        KillTimer(g_hwnd, ID_TIMER_PANEL);
        return;
    }
    SetTimer(g_hwnd, ID_TIMER_PANEL, due > now ? (UINT)(due - now) : USER_TIMER_MINIMUM, NULL);
}

static void ServicePanels(void) {
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (m->builtin && m->panelReportPending && now >= m->panelReportTick + PANEL_SETTLE_MS) {
            m->panelReportPending = FALSE;
            ProcessPanelReport(m);
        }
    }
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (!PanelReapplyDue(m) || m->panelReportPending || now < g_panelHoldUntil) continue;
        m->panelReapply = FALSE;
        if (MonitorMode(m) != MODE_HARDWARE) continue;
        DebugPrint(L"[PANEL] %s (%s): putting %d%% back\n", m->name, m->device, MonitorHardwareTarget(m));
        m->lastHwSent = -1;
        ApplyMonitor(m);
    }
    SchedulePanelService();
}

/* WM_APP_PANEL_BRIGHTNESS from PanelEventThread. */
static void HandlePanelBrightness(PanelBrightnessEvent* report) {
    Monitor* m = NULL;
    for (int i = 0; i < g_monitorCount && !m; i++) {
        if (g_monitors[i].builtin && PanelInstanceMatches(report->instanceName, g_monitors[i].instancePath)) {
            m = &g_monitors[i];
        }
    }
    DebugPrint(L"[PANEL] Windows reports %d%% for %s (%s)\n", report->brightness, report->instanceName,
               m ? m->name : L"not a controlled display");
    if (m && !m->hidden && !g_remoteSession) {
        m->panelReport = report->brightness;
        m->panelReportTick = GetTickCount64();
        m->panelReportPending = TRUE;
        SchedulePanelService();
    }
    free(report);
}

/* Windows is about to adjust built-in displays: with quiet, it applies a
 * level of its own and what it reports meanwhile is not the user's; either
 * way our level is not put back before it is done. */
static void NotePanelTransition(BOOL quiet, DWORD ms) {
    ULONGLONG until = GetTickCount64() + ms;
    if (quiet && until > g_panelQuietUntil) g_panelQuietUntil = until;
    if (until > g_panelHoldUntil) g_panelHoldUntil = until;
    SchedulePanelService();
}

/* Power source, battery saver and energy saver notifications; the first one
 * after registering only tells the current state. */
static void NotePowerCondition(LONG* last, LONG value, const wchar_t* what) {
    LONG previous = *last;
    *last = value;
    if (previous == -1 || previous == value) return;
    DebugPrint(L"[PANEL] %s changed (%ld -> %ld)\n", what, (long)previous, (long)value);
    NotePanelTransition(TRUE, PANEL_QUIET_MS);
}

/* Started with the first built-in display found: the listener for changes
 * made in Windows, and the power notifications that explain Windows' own
 * adjustments. A computer without such a display runs none of this. */
static void StartPanelWatch(void) {
    if (g_panelWatchStarted || !g_hwnd) return;
    g_panelWatchStarted = TRUE;
    g_powerSourceNotify = RegisterPowerSettingNotification(g_hwnd, &kGuidAcDcPowerSource,
                                                           DEVICE_NOTIFY_WINDOW_HANDLE);
    g_batterySaverNotify = RegisterPowerSettingNotification(g_hwnd, &kGuidPowerSavingStatus,
                                                            DEVICE_NOTIFY_WINDOW_HANDLE);
    g_energySaverNotify = RegisterPowerSettingNotification(g_hwnd, &kGuidEnergySaverStatus,
                                                           DEVICE_NOTIFY_WINDOW_HANDLE);
    g_panelEventStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_panelEventThread = g_panelEventStop ? CreateThread(NULL, 0, PanelEventThread, NULL, 0, NULL) : NULL;
    DebugPrint(L"[PANEL] Watching built-in displays: change listener %s; power source %s, battery saver %s, energy saver %s notifications\n",
               g_panelEventThread ? L"started" : L"FAILED",
               g_powerSourceNotify ? L"on" : L"off", g_batterySaverNotify ? L"on" : L"off",
               g_energySaverNotify ? L"on" : L"off");
}

/* At exit. The listener is blocked in WMI until the next report, so it is
 * told to stop but not waited for; process exit ends it. */
static void StopPanelWatch(void) {
    if (g_panelEventStop) SetEvent(g_panelEventStop);
    if (g_panelEventThread) CloseHandle(g_panelEventThread);
    g_panelEventThread = NULL;
    if (g_powerSourceNotify) UnregisterPowerSettingNotification(g_powerSourceNotify);
    if (g_batterySaverNotify) UnregisterPowerSettingNotification(g_batterySaverNotify);
    if (g_energySaverNotify) UnregisterPowerSettingNotification(g_energySaverNotify);
    g_powerSourceNotify = g_batterySaverNotify = g_energySaverNotify = NULL;
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

/* Include neighbouring calendar dates so a transition keeps the same
 * sunrise/sunset anchors across midnight. Two days either side also cover
 * locations whose longitude and the computer's time zone straddle the date line. */
static BOOL ComputeSolarDays(double latitude, double longitude, const SYSTEMTIME* date,
                             SolarDay days[SCHEDULE_DAY_COUNT]) {
    SYSTEMTIME midnight = *date;
    midnight.wHour = midnight.wMinute = midnight.wSecond = midnight.wMilliseconds = 0;
    FILETIME ft;
    if (!SystemTimeToFileTime(&midnight, &ft)) return FALSE;
    ULARGE_INTEGER base;
    base.LowPart = ft.dwLowDateTime;
    base.HighPart = ft.dwHighDateTime;
    for (int i = 0; i < SCHEDULE_DAY_COUNT; i++) {
        ULARGE_INTEGER shifted;
        shifted.QuadPart = (ULONGLONG)((LONGLONG)base.QuadPart +
            (LONGLONG)(i - SCHEDULE_DAY_RADIUS) * 24 * 60 * 60 * 10000000);
        ft.dwLowDateTime = shifted.LowPart;
        ft.dwHighDateTime = shifted.HighPart;
        SYSTEMTIME localDate;
        if (!FileTimeToSystemTime(&ft, &localDate)) return FALSE;
        ComputeSolarDay(latitude, longitude, &localDate, &days[i]);
    }
    return TRUE;
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

/* Daytime weight (0 night .. 1 day) at a local time (minutes, fractional)
 * on the given day, and which part of the curve produced it. When
 * yesterday's dusk overlaps today's dawn, the greater daytime contribution
 * wins. Both curves are continuous, so their maximum is too; this also
 * works when the daytime level is darker than the night level. */
static double ScheduleDaylightAt(const Schedule* sc, const SolarDay days[SCHEDULE_DAY_COUNT],
                                 double minutes, SchedulePhase* phase) {
    const SolarDay* today = &days[SCHEDULE_DAY_RADIUS];
    if (today->polar) {
        *phase = today->polar > 0 ? SCHEDULE_PHASE_DAY : SCHEDULE_PHASE_NIGHT;
        return today->polar > 0 ? 1 : 0;
    }
    double daylight = 0;
    *phase = SCHEDULE_PHASE_NIGHT;
    for (int i = 0; i < SCHEDULE_DAY_COUNT; i++) {
        if (days[i].polar) continue;
        int a[4];
        ScheduleAnchors(sc, &days[i], a);
        double t = minutes - (i - SCHEDULE_DAY_RADIUS) * 1440;
        double weight = 0;
        SchedulePhase part = SCHEDULE_PHASE_NIGHT;
        if (t > a[0] && t < a[3]) {
            if (t < a[1]) {
                weight = SmoothStep((t - a[0]) / (double)(a[1] - a[0]));
                part = SCHEDULE_PHASE_DAWN;
            } else if (t <= a[2]) {
                weight = 1;
                part = SCHEDULE_PHASE_DAY;
            } else {
                weight = 1 - SmoothStep((t - a[2]) / (double)(a[3] - a[2]));
                part = SCHEDULE_PHASE_DUSK;
            }
        }
        if (weight > daylight) {
            daylight = weight;
            *phase = part;
        }
    }
    return daylight;
}

/* Brightness at a local time (minutes, fractional) on the given day. */
static int ScheduleValueAt(const Schedule* sc, const SolarDay days[SCHEDULE_DAY_COUNT], double minutes) {
    /* Clamp endpoints before interpolation, just like the dialog preview.
     * Keeping the stored levels lets the extended range be enabled again. */
    int minimum = g_config.allowBelowMinimum ? -SOFT_MAX_DIM : 0;
    double night = sc->nightLevel < minimum ? minimum : sc->nightLevel;
    double dayLevel = sc->dayLevel < minimum ? minimum : sc->dayLevel;
    SchedulePhase phase;
    double daylight = ScheduleDaylightAt(sc, days, minutes, &phase);
    return (int)lround(night + (dayLevel - night) * daylight);
}

static const wchar_t* SchedulePhaseName(SchedulePhase phase) {
    switch (phase) {
        case SCHEDULE_PHASE_DAY: return L"Daytime";
        case SCHEDULE_PHASE_DAWN: return L"Night \u2192 Daytime";
        case SCHEDULE_PHASE_DUSK: return L"Daytime \u2192 Night";
        default: return L"Night";
    }
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

/* A manual change pauses the whole schedule (every monitor it controls)
 * until the next cycle reset time. */
static BOOL IsSchedulePaused(ULONGLONG nowFt) {
    const Schedule* sc = &g_config.schedule;
    return sc->enabled && sc->pausedUntil != 0 && sc->pausedUntil > nowFt;
}

/* The solar days around today and the current local time, or FALSE when
 * the schedule is off. */
static BOOL ScheduleNow(SolarDay days[SCHEDULE_DAY_COUNT], double* minutes) {
    const Schedule* sc = &g_config.schedule;
    if (!sc->enabled || !sc->hasLocation) return FALSE;
    SYSTEMTIME now;
    GetLocalTime(&now);
    DYNAMIC_TIME_ZONE_INFORMATION zone;
    ZeroMemory(&zone, sizeof(zone));
    DWORD zoneId = GetDynamicTimeZoneInformation(&zone);
    /* Check the inputs even without a Windows notification: midnight,
     * DST, a time-zone/location change, or waking on another date must
     * never reuse stale anchors. A failed zone query disables caching. */
    if (!g_solarCache.valid || zoneId == TIME_ZONE_ID_INVALID ||
        now.wYear != g_solarCache.date.wYear || now.wMonth != g_solarCache.date.wMonth ||
        now.wDay != g_solarCache.date.wDay ||
        sc->latitude != g_solarCache.latitude || sc->longitude != g_solarCache.longitude ||
        zoneId != g_solarCache.zoneId || memcmp(&zone, &g_solarCache.zone, sizeof(zone)) != 0) {
        g_solarCache.valid = FALSE;
        if (!ComputeSolarDays(sc->latitude, sc->longitude, &now, g_solarCache.days)) return FALSE;
        g_solarCache.date = now;
        g_solarCache.latitude = sc->latitude;
        g_solarCache.longitude = sc->longitude;
        g_solarCache.zoneId = zoneId;
        g_solarCache.zone = zone;
        g_solarCache.valid = zoneId != TIME_ZONE_ID_INVALID;
    }
    memcpy(days, g_solarCache.days, sizeof(g_solarCache.days));
    *minutes = now.wHour * 60 + now.wMinute + now.wSecond / 60.0;
    return TRUE;
}

/* The value the schedule wants right now, or FALSE when it is off. */
static BOOL ScheduleTargetNow(int* value) {
    SolarDay days[SCHEDULE_DAY_COUNT];
    double minutes;
    if (!ScheduleNow(days, &minutes)) return FALSE;
    *value = ScheduleValueAt(&g_config.schedule, days, minutes);
    return TRUE;
}

/* Where the schedule is in its day/night cycle right now, or FALSE when
 * it is off. */
static BOOL SchedulePhaseNow(SchedulePhase* phase) {
    SolarDay days[SCHEDULE_DAY_COUNT];
    double minutes;
    if (!ScheduleNow(days, &minutes)) return FALSE;
    ScheduleDaylightAt(&g_config.schedule, days, minutes, phase);
    return TRUE;
}

/* Applies the current schedule value to every scheduled monitor unless the
 * schedule is paused. Runs on the schedule timer and after anything that
 * changes the inputs. */
static void EvaluateSchedule(void) {
    Schedule* sc = &g_config.schedule;
    SolarDay days[SCHEDULE_DAY_COUNT];
    double minutes;
    if (!ScheduleNow(days, &minutes)) {
        g_loggedSchedulePhase = -1;
        ScheduleTooltipUpdate();
        return;
    }
    int target = ScheduleValueAt(sc, days, minutes);
    SchedulePhase phase;
    ScheduleDaylightAt(sc, days, minutes, &phase);
    if ((int)phase != g_loggedSchedulePhase) {
        DebugPrint(L"[INFO] Schedule state: %s, target %d%%\n", SchedulePhaseName(phase), target);
        g_loggedSchedulePhase = (int)phase;
        /* The phase can change before the rounded brightness does, or
         * while paused. Unchanged ticks need no separate tooltip timer. */
        ScheduleTooltipUpdate();
    }
    ULONGLONG now = NowFileTime();
    BOOL changed = FALSE;
    if (sc->pausedUntil) {
        if (sc->pausedUntil > now) return;
        sc->pausedUntil = 0;
        SaveConfigToRegistry(&g_config);
        ScheduleTooltipUpdate();
        changed = TRUE;
        DebugPrint(L"[INFO] Schedule resumed at the cycle reset time\n");
    }
    /* Nothing is applied through Remote Desktop; the re-probe after the
     * console is back brings every scheduled monitor to the current value. */
    if (g_remoteSession) return;
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (!m->scheduled || m->hidden) continue;
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

/* A manual brightness change on a scheduled monitor pauses the schedule
 * for every monitor it controls until the next cycle reset time. */
static void NoteManualChange(Monitor* m) {
    Schedule* sc = &g_config.schedule;
    if (!sc->enabled || !m->scheduled || m->hidden) return;
    if (IsSchedulePaused(NowFileTime())) return;
    sc->pausedUntil = NextCycleResetFileTime();
    SaveConfigToRegistry(&g_config);
    wchar_t until[16];
    FormatLocalTimeOfDay(sc->pausedUntil, until, 16);
    DebugPrint(L"[INFO] %s (%s): manual change; schedule paused until %s\n",
               m->name, m->device, until);
    PushMonitorsToDialog();
}

/* "Resume now" in the dialog: the schedule takes over again everywhere. */
static void ResumeSchedule(void) {
    Schedule* sc = &g_config.schedule;
    if (!sc->pausedUntil) return;
    sc->pausedUntil = 0;
    SaveConfigToRegistry(&g_config);
    DebugPrint(L"[INFO] Schedule resumed by the user\n");
    EvaluateSchedule();
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

/* ── Fixed-size dialog frame ─────────────────────────────────────────────── */

/* The dialog keeps the standard overlapped frame, so Windows draws the
 * normal caption height, but only the app sizes it (to fit the page's
 * content); the user cannot. Edge and corner hits become caption or border
 * hits, Size and Maximize leave the system menu and are refused as commands,
 * and the track size is pinned to the size the app last chose, which also
 * keeps Aero Snap and the taskbar's window arrangements from stretching it.
 * Every size the app gives the window goes through FixedFrameSetPos. */
#define FIXED_FRAME_STYLE (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX)

/* Runs first in the dialog's window procedure; returns TRUE with *result
 * set for a message it answered. */
static BOOL FixedFrameMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                              SIZE *size, LRESULT *result) {
    switch (msg) {
        case WM_NCHITTEST:
            *result = DefWindowProcW(hwnd, msg, wParam, lParam);
            switch (*result) {
                case HTTOP: case HTTOPLEFT: case HTTOPRIGHT:
                    *result = HTCAPTION;
                    break;
                case HTLEFT: case HTRIGHT: case HTBOTTOM:
                case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
                    *result = HTBORDER;
                    break;
            }
            return TRUE;

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) != SC_SIZE && (wParam & 0xFFF0) != SC_MAXIMIZE) {
                return FALSE;
            }
            *result = 0;
            return TRUE;

        case WM_GETMINMAXINFO: {
            if (size->cx <= 0 || size->cy <= 0) return FALSE;
            MINMAXINFO *info = (MINMAXINFO *)lParam;
            if (info->ptMinTrackSize.x < size->cx) info->ptMinTrackSize.x = size->cx;
            if (info->ptMinTrackSize.y < size->cy) info->ptMinTrackSize.y = size->cy;
            info->ptMaxTrackSize = info->ptMinTrackSize;
            *result = 0;
            return TRUE;
        }

        case WM_NCDESTROY:
            size->cx = size->cy = 0;
            return FALSE;
    }
    return FALSE;
}

/* Called once CreateWindowExW has returned: pins the size it gave the
 * window and takes Size and Maximize out of the system menu. */
static void FixedFrameInit(HWND hwnd, SIZE *size) {
    RECT rect;
    if (GetWindowRect(hwnd, &rect)) {
        size->cx = rect.right - rect.left;
        size->cy = rect.bottom - rect.top;
    }
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (menu) {
        DeleteMenu(menu, SC_SIZE, MF_BYCOMMAND);
        DeleteMenu(menu, SC_MAXIMIZE, MF_BYCOMMAND);
    }
}

/* SetWindowPos for the app's own sizing: the new size is pinned first, so
 * the track limits admit exactly it. */
static BOOL FixedFrameSetPos(HWND hwnd, SIZE *size, int x, int y, int width,
                             int height, UINT flags) {
    if (!(flags & SWP_NOSIZE)) {
        size->cx = width;
        size->cy = height;
    }
    return SetWindowPos(hwnd, NULL, x, y, width, height, flags);
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
    wchar_t schedulePausedUntil[16] = L"";
    if (IsSchedulePaused(NowFileTime())) {
        FormatLocalTimeOfDay(g_config.schedule.pausedUntil, schedulePausedUntil, 16);
    }
    for (int i = 0; i < g_monitorCount; i++) {
        const Monitor* m = &g_monitors[i];
        wchar_t eKey[256], eName[128], eDevice[64], eError[320];
        const wchar_t* pausedUntil = m->scheduled ? schedulePausedUntil : L"";
        json_escape_wstring(m->key, eKey, 256);
        json_escape_wstring(m->name, eName, 128);
        json_escape_wstring(m->device, eDevice, 64);
        json_escape_wstring(m->error, eError, 320);
        int written = swprintf_s(buf + len, cap - len,
            L"%s{\"uid\":%d,\"key\":\"%s\",\"name\":\"%s\",\"device\":\"%s\","
            L"\"width\":%ld,\"height\":%ld,\"primary\":%s,\"hardware\":\"%s\",\"builtin\":%s,"
            L"\"mode\":\"%s\",\"knownHardware\":%s,\"forceSoftware\":%s,\"hidden\":%s,\"value\":%d,"
            L"\"min\":%d,\"max\":100,\"scheduled\":%s,\"pausedUntil\":\"%s\","
            L"\"error\":\"%s\"}",
            i == 0 ? L"" : L",", m->uid, eKey, eName, eDevice,
            (long)(m->rect.right - m->rect.left), (long)(m->rect.bottom - m->rect.top),
            m->primary ? L"true" : L"false", HardwareStateName(m->hardwareState),
            m->builtin ? L"true" : L"false",
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
    wchar_t eUpdateCompletedVersion[64], eTrayTarget[256], presetText[128];
    json_escape_wstring(g_updateConfirmationPending ? APP_VERSION_WSTRING : L"",
                        eUpdateCompletedVersion, 64);
    json_escape_wstring(g_config.trayTarget, eTrayTarget, 256);
    FormatTrayPresets(g_config.trayPresets, g_config.trayPresetCount, presetText, 128);
    BOOL updateCheckPending =
        InterlockedCompareExchange(&g_updateCheckPending, FALSE, FALSE) == TRUE ||
        InterlockedCompareExchangePointer((PVOID volatile*)&g_updatePostedResult, NULL, NULL) != NULL;
    const size_t cap = wcslen(monitors) + 1536;
    wchar_t* script = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (script) {
        int written = swprintf_s(script, cap,
            L"window.onInit({\"config\":{\"allowBelowMinimum\":%s,\"debugLog\":%s,"
            L"\"autoCheckForUpdates\":%s,\"startWithWindows\":%s,"
            L"\"pauseInRemoteSession\":%s,\"remoteSession\":%s,"
            L"\"brightnessKeys\":%s,"
            L"\"updateCheckPending\":%s,\"updatePromptPending\":%s,"
            L"\"trayTarget\":\"%s\",\"trayPresets\":\"%s\","
            L"\"schedule\":{\"enabled\":%s,\"hasLocation\":%s,\"latitude\":%.6f,"
            L"\"longitude\":%.6f,\"dayLevel\":%d,\"nightLevel\":%d,"
            L"\"dawnStartOffset\":%d,\"dawnEndOffset\":%d,\"duskStartOffset\":%d,"
            L"\"duskEndOffset\":%d,\"cycleResetMinutes\":%d}},"
            L"\"monitors\":%s,\"updateCompletedVersion\":\"%s\"})",
            g_config.allowBelowMinimum ? L"true" : L"false",
            g_config.debugLogEnabled ? L"true" : L"false",
            g_config.autoCheckForUpdates ? L"true" : L"false",
            IsStartWithWindowsEnabled() ? L"true" : L"false",
            g_config.pauseInRemoteSession ? L"true" : L"false",
            g_remoteSession ? L"true" : L"false",
            g_config.brightnessKeys ? L"true" : L"false",
            updateCheckPending ? L"true" : L"false",
            g_updateNoticeTask ? L"true" : L"false",
            eTrayTarget, presetText,
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
    ScheduleTooltipUpdate();
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
    FixedFrameSetPos(g_cfgHwnd, &g_cfgFrameSize, posX, posY, windowW, windowH, flags);
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

/* Identity keys contain only ASCII letters, digits, underscores, hyphens,
 * ampersands and dots (SanitizeKeyChars), so commas delimit them safely. */
static BOOL MonitorKeyInList(const wchar_t* key, const char* list) {
    size_t length = wcslen(key);
    while (*list) {
        const char* end = strchr(list, ',');
        size_t count = end ? (size_t)(end - list) : strlen(list);
        if (count == length) {
            size_t i = 0;
            while (i < count && key[i] == (wchar_t)(unsigned char)list[i]) i++;
            if (i == count) return TRUE;
        }
        if (!end) break;
        list = end + 1;
    }
    return FALSE;
}

/* The Start with Windows checkbox of a saveSettings message. Only a changed
 * choice touches the Run entry, so an entry for another copy of the
 * executable is left alone unless the user turns this on. */
static void SaveStartWithWindows(const char* msg) {
    BOOL before = IsStartWithWindowsEnabled();
    BOOL wanted = json_get_bool(msg, "startWithWindows", before);
    if (wanted == before) return;
    LONG result = SetStartWithWindows(wanted);
    if (result != ERROR_SUCCESS) {
        DebugPrint(L"[WARNING] Could not %s start with Windows (error %ld)\n",
                   wanted ? L"enable" : L"disable", (long)result);
    }
}

/* Reads the schedule part of a saveSettings message. Saving the schedule
 * counts as a deliberate change, so any paused monitors resume. */
static void SaveScheduleFromMessage(const char* msg) {
    Schedule* sc = &g_config.schedule;
    char latitude[64] = {0}, longitude[64] = {0};
    char scheduledKeys[MAX_MONITORS * 128] = {0};
    char shownKeys[MAX_MONITORS * 128] = {0};
    json_get_string(msg, "latitude", latitude, sizeof(latitude));
    json_get_string(msg, "longitude", longitude, sizeof(longitude));
    json_get_string(msg, "scheduledKeys", scheduledKeys, sizeof(scheduledKeys));
    json_get_string(msg, "scheduleMonitorKeys", shownKeys, sizeof(shownKeys));

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
        /* Hidden monitors are not in the dialog, so the list says nothing
         * about them; their flag waits untouched for the next rescan. */
        if (m->hidden || !MonitorKeyInList(m->key, shownKeys)) continue;
        /* Stable keys survive disconnect/reconnect. A display arriving
         * after the dialog sent Save keeps its existing selection. */
        BOOL scheduled = MonitorKeyInList(m->key, scheduledKeys);
        if (m->scheduled != scheduled) {
            m->scheduled = scheduled;
            m->dirty = TRUE;
        }
    }
    PersistDirtyMonitors();
    sc->pausedUntil = 0;

    int scheduledCount = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitors[i].scheduled && !g_monitors[i].hidden) scheduledCount++;
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
        /* Recompute the curve with the new endpoint range immediately;
         * paused/manual monitors still only need their value re-clamped. */
        EvaluateSchedule();
        for (int i = 0; i < g_monitorCount; i++) ApplyMonitor(&g_monitors[i]);
        PushMonitorsToDialog();
    } else if (strcmp(action, "hideMonitor") == 0 && g_remoteSession) {
        DebugPrint(L"[INFO] Hide ignored during the remote session\n");
    } else if (strcmp(action, "refreshMonitors") == 0 && g_remoteSession) {
        DebugPrint(L"[INFO] Rescan ignored during the remote session\n");
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
        /* Windows is asked again which displays it controls. */
        for (int i = 0; i < g_monitorCount; i++) g_monitors[i].panelChecked = FALSE;
        UnhideAllMonitors();
        RefreshMonitors();
    } else if (strcmp(action, "resumeSchedule") == 0) {
        ResumeSchedule();
    } else if (strcmp(action, "saveSettings") == 0) {
        g_config.debugLogEnabled = json_get_bool(msg, "debugLog", FALSE);
        g_config.autoCheckForUpdates = json_get_bool(msg, "autoCheckForUpdates", TRUE);
        g_config.pauseInRemoteSession = json_get_bool(msg, "pauseInRemoteSession", TRUE);
        BOOL brightnessKeysBefore = g_config.brightnessKeys;
        g_config.brightnessKeys = json_get_bool(msg, "brightnessKeys", FALSE);
        char trayTarget[256] = {0};
        json_get_string(msg, "trayTarget", trayTarget, sizeof(trayTarget));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, trayTarget, -1, g_config.trayTarget,
                                sizeof(g_config.trayTarget) / sizeof(wchar_t)) == 0) {
            g_config.trayTarget[0] = L'\0';
        }
        char presetUtf8[256] = {0};
        wchar_t presetText[256] = {0};
        json_get_string(msg, "trayPresets", presetUtf8, sizeof(presetUtf8));
        MultiByteToWideChar(CP_UTF8, 0, presetUtf8, -1, presetText, 256);
        g_config.trayPresetCount = ParseTrayPresets(presetText, g_config.trayPresets, TRAY_MAX_PRESETS);
        SaveScheduleFromMessage(msg);
        InterlockedExchange(&g_debugLogEnabled, g_config.debugLogEnabled ? TRUE : FALSE);
        if (!SaveConfigToRegistry(&g_config)) {
            DebugPrint(L"[WARNING] Could not save all settings to the registry\n");
            MessageBoxW(g_cfgHwnd,
                L"Some settings could not be saved. Check that the current user "
                L"can write to HKEY_CURRENT_USER.",
                APP_DISPLAY_NAME_WSTRING, MB_ICONWARNING | MB_OK);
        }
        SaveStartWithWindows(msg);
        FormatTrayPresets(g_config.trayPresets, g_config.trayPresetCount, presetText, 256);
        DebugPrint(L"[INFO] Settings saved (debugLog=%d, autoCheckForUpdates=%d, startWithWindows=%d, pauseInRemoteSession=%d, brightnessKeys=%d, trayTarget=\"%s\", trayPresets=\"%s\")\n",
                   g_config.debugLogEnabled, g_config.autoCheckForUpdates, IsStartWithWindowsEnabled(),
                   g_config.pauseInRemoteSession, g_config.brightnessKeys, g_config.trayTarget, presetText);
        /* Turning the pause off from inside a remote session resumes at once. */
        UpdateRemoteSessionState();
        if (brightnessKeysBefore != g_config.brightnessKeys) UpdateBrightnessKeyReaders();
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
    LRESULT frameResult;
    if (FixedFrameMessage(hwnd, msg, wParam, lParam, &g_cfgFrameSize, &frameResult)) {
        return frameResult;
    }
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
            FixedFrameSetPos(hwnd, &g_cfgFrameSize, suggested->left, suggested->top,
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
        FIXED_FRAME_STYLE,
        posX, posY, width, height,
        NULL, NULL, g_hInstance, NULL);
    if (!g_cfgHwnd) {
        DebugPrint(L"[ERROR] Failed to create the configuration window (error %lu)\n",
                   (unsigned long)GetLastError());
        return;
    }
    FixedFrameInit(g_cfgHwnd, &g_cfgFrameSize);
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

static void ScheduleTooltipUpdate(void) {
    if (g_hwnd) SetTimer(g_hwnd, ID_TIMER_TOOLTIP, TOOLTIP_UPDATE_DELAY_MS, NULL);
}

/* "State: Daytime" (or Night, or the transition in progress) while the
 * schedule is enabled, "Schedule: Disabled/Active/Paused" always, then one
 * "name: NN%" line per visible monitor. szTip holds 128 characters, so
 * long names are shortened and, if the list still does not fit, the tail
 * is replaced by an ellipsis. */
static void UpdateTrayTooltip(void) {
    if (!g_nid.hWnd) return;
    const size_t cap = sizeof(g_nid.szTip) / sizeof(wchar_t);
    wchar_t tip[128] = L"";
    if (g_remoteSession) wcscpy_s(tip, cap, L"Remote Desktop session: paused\n");
    SchedulePhase phase;
    if (SchedulePhaseNow(&phase)) {
        wcscat_s(tip, cap, L"State: ");
        wcscat_s(tip, cap, SchedulePhaseName(phase));
        wcscat_s(tip, cap, L"\n");
    }
    const wchar_t* scheduleState = !g_config.schedule.enabled ? L"Disabled"
                                 : IsSchedulePaused(NowFileTime()) ? L"Paused" : L"Active";
    wcscat_s(tip, cap, L"Schedule: ");
    wcscat_s(tip, cap, scheduleState);
    for (int i = 0; i < g_monitorCount; i++) {
        const Monitor* m = &g_monitors[i];
        if (m->hidden) continue;
        wchar_t name[40];
        wcsncpy_s(name, 40, m->name, _TRUNCATE);
        if (wcslen(m->name) > 39) wcscpy_s(name + 36, 4, L"...");
        wchar_t line[64];
        if (m->hasValue && MonitorMode(m) != MODE_PROBING) {
            swprintf_s(line, 64, L"\n%s: %d%%", name, m->value);
        } else {
            swprintf_s(line, 64, L"\n%s: ...", name);
        }
        if (wcslen(tip) + wcslen(line) >= cap - 1) {
            /* Out of room: mark the list as cut and stop. */
            if (wcslen(tip) + 4 < cap) wcscat_s(tip, cap, L"\n...");
            break;
        }
        wcscat_s(tip, cap, line);
    }
    if (wcscmp(tip, g_nid.szTip) == 0) return;
    wcscpy_s(g_nid.szTip, cap, tip);
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* The monitor the tray menu's brightness items act on; NULL with allVisible
 * set means every visible monitor, NULL without it means the items are off. */
static Monitor* TrayTargetMonitor(BOOL* allVisible) {
    *allVisible = FALSE;
    if (!g_config.trayTarget[0]) return NULL;
    if (wcscmp(g_config.trayTarget, TRAY_TARGET_ALL) == 0) {
        *allVisible = TRUE;
        return NULL;
    }
    for (int i = 0; i < g_monitorCount; i++) {
        if (!g_monitors[i].hidden && wcscmp(g_monitors[i].key, g_config.trayTarget) == 0) {
            return &g_monitors[i];
        }
    }
    return NULL;
}

/* A tray menu brightness item: Increase/Decrease step each target from its
 * own current value, a preset sets them all to that level. Either is a
 * manual change, so scheduled monitors pause. */
static void ApplyTrayMenuValue(int value, BOOL relative) {
    if (g_remoteSession) return;
    BOOL allVisible = FALSE;
    Monitor* target = TrayTargetMonitor(&allVisible);
    int changed = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        Monitor* m = &g_monitors[i];
        if (m->hidden || MonitorMode(m) == MODE_PROBING) continue;
        if (!allVisible && m != target) continue;
        int before = m->value;
        SetMonitorValue(m, relative ? m->value + value : value);
        NoteManualChange(m);
        if (m->value != before) changed++;
    }
    const wchar_t* what = allVisible ? L"all monitors" : (target ? target->name : L"no monitor");
    if (relative) {
        DebugPrint(L"[INFO] Tray menu: brightness %s by %d%% on %s (%d monitor(s) changed)\n",
                   value > 0 ? L"up" : L"down", value > 0 ? value : -value, what, changed);
    } else {
        DebugPrint(L"[INFO] Tray menu: brightness set to %d%% on %s (%d monitor(s) changed)\n",
                   value, what, changed);
    }
    if (changed) PushMonitorsToDialog();
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
    /* The brightness items exist only once a target has been chosen in the
     * dialog; they are greyed out while that target is not available. */
    if (g_config.trayTarget[0]) {
        BOOL allVisible = FALSE;
        Monitor* target = TrayTargetMonitor(&allVisible);
        BOOL usable = (allVisible ? VisibleMonitorCount() > 0 : target != NULL) && !g_remoteSession;
        UINT state = usable ? MF_ENABLED : MF_GRAYED;
        wchar_t brighter[96], dimmer[96];
        if (allVisible) {
            wcscpy_s(brighter, 96, L"Increase brightness");
            wcscpy_s(dimmer, 96, L"Decrease brightness");
        } else {
            swprintf_s(brighter, 96, L"Increase brightness (%s)", target ? target->name : L"unavailable");
            swprintf_s(dimmer, 96, L"Decrease brightness (%s)", target ? target->name : L"unavailable");
        }
        AppendMenuW(hMenu, MF_STRING | state, ID_TRAY_MENU_BRIGHTER, brighter);
        /* Preset levels sit between the two steps, as plain items: no
         * bullet on the current level (removed on request). */
        for (int i = 0; i < g_config.trayPresetCount; i++) {
            wchar_t label[16];
            swprintf_s(label, 16, L"%d%%", g_config.trayPresets[i]);
            AppendMenuW(hMenu, MF_STRING | state, ID_TRAY_MENU_PRESET_FIRST + i, label);
        }
        AppendMenuW(hMenu, MF_STRING | state, ID_TRAY_MENU_DIMMER, dimmer);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    }
    /* While a manual change has paused the schedule, it can be resumed
     * from here as well as from the dialog. No bold default item: Configure
     * is deliberately shown like the others. */
    if (IsSchedulePaused(NowFileTime())) {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_MENU_RESUME_SCHEDULE, L"Resume schedule");
    }
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MENU_CONFIGURE, L"Configure");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MENU_EXIT, L"Exit");

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
                /* A single left click does nothing (on request); the dialog
                 * opens on a double click or the menu's Configure item. */
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
            if (LOWORD(wParam) >= ID_TRAY_MENU_PRESET_FIRST &&
                LOWORD(wParam) < ID_TRAY_MENU_PRESET_FIRST + g_config.trayPresetCount) {
                ApplyTrayMenuValue(g_config.trayPresets[LOWORD(wParam) - ID_TRAY_MENU_PRESET_FIRST], FALSE);
                return 0;
            }
            switch (LOWORD(wParam)) {
                case ID_TRAY_MENU_CONFIGURE:
                    ShowConfigDialog();
                    return 0;
                case ID_TRAY_MENU_BRIGHTER:
                    ApplyTrayMenuValue(+TRAY_STEP_PERCENT, TRUE);
                    return 0;
                case ID_TRAY_MENU_DIMMER:
                    ApplyTrayMenuValue(-TRAY_STEP_PERCENT, TRUE);
                    return 0;
                case ID_TRAY_MENU_RESUME_SCHEDULE:
                    DebugPrint(L"[INFO] Resume schedule selected from the tray menu\n");
                    ResumeSchedule();
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

        case WM_APP_BRIGHTNESS_KEY:
            ApplyBrightnessKey((int)(INT_PTR)wParam);
            return 0;

        case WM_APP_PANEL_BRIGHTNESS:
            HandlePanelBrightness((PanelBrightnessEvent*)lParam);
            return 0;

        case WM_TIMER:
            switch (wParam) {
                case ID_TIMER_REFRESH_MONITORS:
                    KillTimer(hwnd, ID_TIMER_REFRESH_MONITORS);
                    RefreshMonitors();
                    return 0;
                case ID_TIMER_OVERLAY_TOPMOST:
                    /* KillTimer can leave an already queued WM_TIMER. */
                    if (g_overlayTimerRunning) KeepOverlaysOnTop();
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
                case ID_TIMER_TOOLTIP:
                    KillTimer(hwnd, ID_TIMER_TOOLTIP);
                    UpdateTrayTooltip();
                    return 0;
                case ID_TIMER_KEY_DEVICES:
                    KillTimer(hwnd, ID_TIMER_KEY_DEVICES);
                    UpdateBrightnessKeyReaders();
                    return 0;
                case ID_TIMER_PANEL:
                    KillTimer(hwnd, ID_TIMER_PANEL);
                    ServicePanels();
                    return 0;
            }
            break;

        case WM_TIMECHANGE:
        case WM_SETTINGCHANGE:
            /* Time-zone rules can also change without a new date or bias.
             * Reload on broadcasts as well as checking the cache key. */
            g_solarCache.valid = FALSE;
            EvaluateSchedule();
            return 0;

        /* Monitors come and go, change resolution, or wake up: re-enumerate
         * after the burst of notifications settles. */
        case WM_DISPLAYCHANGE:
            DebugPrint(L"[INFO] Display change (%ux%u)\n",
                       (unsigned)LOWORD(lParam), (unsigned)HIWORD(lParam));
            /* Remote Desktop swaps the displays; catch it here as well so
             * the overlays leave the remote desktop without waiting. */
            UpdateRemoteSessionState();
            NotePanelTransition(TRUE, PANEL_QUIET_MS);
            ScheduleMonitorRefresh(REFRESH_MONITORS_DEBOUNCE_MS);
            return 0;

        /* The session moved between the console and Remote Desktop (or was
         * disconnected); the state is re-evaluated rather than trusted. */
        case WM_WTSSESSION_CHANGE:
            DebugPrint(L"[INFO] Session change %u (1 console connect, 2 console disconnect, "
                       L"3 remote connect, 4 remote disconnect, 7 lock, 8 unlock)\n", (unsigned)wParam);
            UpdateRemoteSessionState();
            return 0;

        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVNODES_CHANGED) {
                ScheduleMonitorRefresh(REFRESH_MONITORS_DEBOUNCE_MS);
                ScheduleBrightnessKeyReaderRestart();   /* a keyboard may have come or gone */
            }
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                g_solarCache.valid = FALSE;
                DebugPrint(L"[INFO] Resumed from sleep; monitors will be re-applied\n");
                NotePanelTransition(TRUE, PANEL_QUIET_MS);
                ScheduleMonitorRefresh(REFRESH_MONITORS_RESUME_DELAY_MS);
                ScheduleBrightnessKeyReaderRestart();
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
                    /* A built-in display switched off or back on gets a level
                     * from Windows; undimmed, it gets its previous one back
                     * (and a brightness key pressed to wake it is the user's).
                     * Either way, writes held back meanwhile wait for that. */
                    if (previous != -1 && previous != state) {
                        BOOL undimmed = state == 1 && previous == 2;
                        NotePanelTransition(previous == 0 || state == 0,
                                            undimmed ? PANEL_SETTLE_MS : PANEL_QUIET_MS);
                    }
                } else if (setting && setting->DataLength >= sizeof(DWORD)) {
                    /* Windows applies its own level to a built-in display for
                     * the new power source or saver mode. */
                    LONG value = (LONG)*(const DWORD*)setting->Data;
                    if (IsEqualGUID(&setting->PowerSetting, &kGuidAcDcPowerSource)) {
                        NotePowerCondition(&g_lastPowerSource, value, L"Power source");
                    } else if (IsEqualGUID(&setting->PowerSetting, &kGuidPowerSavingStatus)) {
                        NotePowerCondition(&g_lastBatterySaver, value, L"Battery saver");
                    } else if (IsEqualGUID(&setting->PowerSetting, &kGuidEnergySaverStatus)) {
                        NotePowerCondition(&g_lastEnergySaver, value, L"Energy saver");
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
    BOOL metric, glassKnown;
    DWORD sessionId, glassId;
    BOOL remote = QueryRemoteSession(&metric, &sessionId, &glassId, &glassKnown);
    DebugPrint(L"[ENV] session %lu, SM_REMOTESESSION=%d, console session %lu%s -> %s; pauseInRemoteSession=%d\n",
               (unsigned long)sessionId, metric, (unsigned long)glassId, glassKnown ? L"" : L" (unknown)",
               remote ? L"remote" : L"local", g_config.pauseInRemoteSession);
    const Schedule* sc = &g_config.schedule;
    DebugPrint(L"[ENV] settings: allowBelowMinimum=%d debugLog=%d brightnessKeys=%d startWithWindows=%d schedule=%d location=%d (%.4f, %.4f) day=%d night=%d dawn=%+d/%+d dusk=%+d/%+d reset=%02d:%02d\n",
               g_config.allowBelowMinimum, g_config.debugLogEnabled, g_config.brightnessKeys,
               IsStartWithWindowsEnabled(), sc->enabled, sc->hasLocation,
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
    if (!WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION)) {
        DebugPrint(L"[WARNING] Session notifications unavailable (error %lu); Remote Desktop is "
                   L"detected on display changes only\n", (unsigned long)GetLastError());
    }
    RefreshMonitors();
    UpdateScheduleTimer();
    UpdateBrightnessKeyReaders();

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
    KillTimer(g_hwnd, ID_TIMER_TOOLTIP);
    KillTimer(g_hwnd, ID_TIMER_KEY_DEVICES);
    KillTimer(g_hwnd, ID_TIMER_PANEL);
    CloseBrightnessKeyReaders();
    StopPanelWatch();
    PersistDirtyMonitors();
    DestroyAllOverlays();
    StopDdcWorker();
    if (g_displayStateNotify) UnregisterPowerSettingNotification(g_displayStateNotify);
    WTSUnRegisterSessionNotification(g_hwnd);
    RemoveTrayIcon();
    CoUninitialize();
    if (g_hwnd) DestroyWindow(g_hwnd);
    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
    }
    return (int)msg.wParam;
}

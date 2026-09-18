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
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
#define REG_VALUE_MON_NAME L"Name"

#define TRAY_ICON_ID 100
#define WM_TRAYICON (WM_APP + 1)
/* Posted by the DDC/CI worker thread. */
#define WM_APP_DDC_PROBED (WM_APP + 2)      /* lParam: DdcProbeResult* (receiver frees) */
#define WM_APP_DDC_SET_RESULT (WM_APP + 3)  /* wParam: uid, lParam: success */
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
 * software-only until the next rescan. */
#define DDC_MAX_CONSECUTIVE_FAILURES 3

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

typedef struct {
    BOOL allowBelowMinimum;   /* let hardware monitors dim further in software */
    BOOL debugLogEnabled;
} Configuration;

typedef enum {
    HW_UNKNOWN = 0,   /* probe in flight */
    HW_AVAILABLE,     /* monitor answers VCP 0x10 */
    HW_UNAVAILABLE    /* no DDC/CI brightness: software dimming only */
} HardwareState;

typedef enum {
    MODE_PROBING = 0,
    MODE_HARDWARE,
    MODE_SOFTWARE
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
    DWORD ddcMax;
    DWORD ddcCurrent;
    BOOL forceSoftware;           /* user asked for software dimming only */
    BOOL hidden;                  /* removed from the dialog and left alone until a rescan */
    int value;                    /* desired brightness, -SOFT_MAX_DIM..100 */
    BOOL hasValue;
    int lastHwSent;               /* last percent handed to the worker, -1 = none */
    int failures;                 /* consecutive failed DDC/CI writes */
    HWND overlay;                 /* software dimming window, first entry only */
    int overlayDim;               /* current overlay dim 0..SOFT_MAX_DIM */
    BOOL dirty;                   /* needs persisting */
    wchar_t error[160];           /* user-facing status, empty when fine */
} Monitor;

/* Main-to-worker probe request: which physical monitors to open and query. */
typedef struct {
    int uid;
    HMONITOR hmon;
    int physicalIndex;
} DdcProbeEntry;

/* Main-to-worker brightness request; one slot per monitor so a burst of
 * slider moves collapses into the latest value. */
typedef struct {
    int uid;
    int target;   /* percent 0..100 */
    BOOL pending;
} DdcSetSlot;

/* Worker-to-main probe result. */
typedef struct {
    int uid;
    BOOL supported;
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
static int MonitorMinValue(const Monitor* m);
static void ApplyMonitor(Monitor* m);
static void SetMonitorValue(Monitor* m, int value);
static void RefreshMonitors(void);
static void ScheduleMonitorRefresh(UINT delayMs);
static void SchedulePersist(void);
static void PersistDirtyMonitors(void);
static int VisibleMonitorCount(void);
static void UnhideAllMonitors(void);
static void DdcRequestSet(int uid, int percent);
static void DdcRequestProbe(const DdcProbeEntry* entries, int count);
static BOOL load_webview2_loader(void);
static void ShowConfigDialog(void);
static void PushMonitorsToDialog(void);
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

static BOOL LoadConfigFromRegistry(Configuration* config) {
    ZeroMemory(config, sizeof(*config));

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return FALSE;
    }
    ReadRegistryBool(hKey, REG_VALUE_DEBUGLOG, &config->debugLogEnabled);
    ReadRegistryBool(hKey, REG_VALUE_ALLOW_BELOW_MIN, &config->allowBelowMinimum);
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
    if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        DebugPrint(L"[INFO] Overlay capture exclusion unavailable (error %lu)\n",
                   (unsigned long)GetLastError());
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
    m->overlayDim = dim;
    if (!m->overlay) return;
    if (dim == 0) {
        if (IsWindowVisible(m->overlay)) ShowWindow(m->overlay, SW_HIDE);
        return;
    }
    BYTE alpha = (BYTE)((dim * 255 + 50) / 100);
    SetLayeredWindowAttributes(m->overlay, 0, alpha, LWA_ALPHA);
    if (!IsWindowVisible(m->overlay)) {
        ShowWindow(m->overlay, SW_SHOWNOACTIVATE);
        PositionOverlay(m);
    }
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
} WorkerSource;

typedef struct {
    int uid;
    HANDLE handle;
    BOOL supported;
    DWORD max;
} WorkerMonitor;

static void DdcRequestSet(int uid, int percent) {
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
        slot->target = percent;
        slot->pending = TRUE;
    }
    LeaveCriticalSection(&g_ddcLock);
    if (g_ddcEvent) SetEvent(g_ddcEvent);
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

static void ReleaseWorkerSources(WorkerSource* sources, int* count) {
    for (int i = 0; i < *count; i++) {
        if (sources[i].monitors) {
            DestroyPhysicalMonitors(sources[i].count, sources[i].monitors);
            free(sources[i].monitors);
            sources[i].monitors = NULL;
        }
        sources[i].count = 0;
    }
    *count = 0;
}

static WorkerSource* AcquireWorkerSource(WorkerSource* sources, int* count, HMONITOR hmon) {
    for (int i = 0; i < *count; i++) {
        if (sources[i].hmon == hmon) return &sources[i];
    }
    if (*count >= MAX_MONITORS) return NULL;
    WorkerSource* s = &sources[(*count)++];
    s->hmon = hmon;
    s->count = 0;
    s->monitors = NULL;
    DWORD n = 0;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &n) && n > 0) {
        s->monitors = (PHYSICAL_MONITOR*)calloc(n, sizeof(PHYSICAL_MONITOR));
        if (s->monitors && GetPhysicalMonitorsFromHMONITOR(hmon, n, s->monitors)) {
            s->count = n;
        } else {
            DebugPrint(L"[WARNING] GetPhysicalMonitorsFromHMONITOR failed (error %lu)\n",
                       (unsigned long)GetLastError());
            free(s->monitors);
            s->monitors = NULL;
        }
    } else {
        DebugPrint(L"[WARNING] GetNumberOfPhysicalMonitorsFromHMONITOR failed (error %lu)\n",
                   (unsigned long)GetLastError());
    }
    return s;
}

/* Asks the monitor for VCP 0x10 (brightness) directly instead of going
 * through the capabilities string, which is far slower and which some
 * monitors do not report correctly even though brightness works. */
static void ProbeWorkerMonitor(WorkerMonitor* wm) {
    DdcProbeResult* r = (DdcProbeResult*)calloc(1, sizeof(*r));
    if (!r) return;
    r->uid = wm->uid;
    if (wm->handle) {
        ULONGLONG start = GetTickCount64();
        MC_VCP_CODE_TYPE type = MC_SET_PARAMETER;
        DWORD current = 0, max = 0;
        BOOL ok = GetVCPFeatureAndVCPFeatureReply(wm->handle, VCP_BRIGHTNESS, &type, &current, &max);
        if (!ok) {
            /* A first request right after a display change is often lost. */
            r->error = GetLastError();
            Sleep(200);
            ok = GetVCPFeatureAndVCPFeatureReply(wm->handle, VCP_BRIGHTNESS, &type, &current, &max);
            if (!ok) r->error = GetLastError();
        }
        r->elapsedMs = (DWORD)(GetTickCount64() - start);
        if (ok) {
            r->supported = TRUE;
            r->current = current;
            r->max = max ? max : 100;
            r->error = 0;
            wm->supported = TRUE;
            wm->max = r->max;
        }
    } else {
        r->error = ERROR_NOT_FOUND;
    }
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_DDC_PROBED, 0, (LPARAM)r)) free(r);
}

static BOOL WriteWorkerBrightness(WorkerMonitor* wm, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    DWORD value = (DWORD)(((unsigned)percent * wm->max + 50) / 100);
    ULONGLONG start = GetTickCount64();
    BOOL ok = SetVCPFeature(wm->handle, VCP_BRIGHTNESS, value);
    DWORD error = ok ? 0 : GetLastError();
    if (!ok) {
        Sleep(100);
        ok = SetVCPFeature(wm->handle, VCP_BRIGHTNESS, value);
        if (!ok) error = GetLastError();
    }
    DebugPrint(L"[%s] DDC/CI monitor %d: set brightness %lu/%lu (%d%%) in %lu ms%s%lu\n",
               ok ? L"INFO" : L"WARNING", wm->uid, (unsigned long)value,
               (unsigned long)wm->max, percent,
               (unsigned long)(GetTickCount64() - start),
               ok ? L"" : L", error ", (unsigned long)error);
    return ok;
}

static DWORD WINAPI DdcWorkerThread(LPVOID param) {
    (void)param;
    WorkerSource sources[MAX_MONITORS];
    int sourceCount = 0;
    WorkerMonitor monitors[MAX_MONITORS];
    int monitorCount = 0;
    ZeroMemory(sources, sizeof(sources));

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
            ReleaseWorkerSources(sources, &sourceCount);
            monitorCount = 0;
            for (int i = 0; i < jobCount && monitorCount < MAX_MONITORS; i++) {
                WorkerSource* src = AcquireWorkerSource(sources, &sourceCount, job[i].hmon);
                WorkerMonitor* wm = &monitors[monitorCount++];
                wm->uid = job[i].uid;
                wm->supported = FALSE;
                wm->max = 100;
                wm->handle = (src && (DWORD)job[i].physicalIndex < src->count)
                    ? src->monitors[job[i].physicalIndex].hPhysicalMonitor : NULL;
                ProbeWorkerMonitor(wm);
                if (InterlockedCompareExchange(&g_ddcStop, 0, 0)) break;
            }
        }

        for (;;) {
            if (InterlockedCompareExchange(&g_ddcStop, 0, 0)) break;
            int uid = -1, target = 0;
            EnterCriticalSection(&g_ddcLock);
            for (int i = 0; i < g_ddcSetCount; i++) {
                if (g_ddcSets[i].pending) {
                    uid = g_ddcSets[i].uid;
                    target = g_ddcSets[i].target;
                    g_ddcSets[i].pending = FALSE;
                    break;
                }
            }
            LeaveCriticalSection(&g_ddcLock);
            if (uid < 0) break;

            WorkerMonitor* wm = NULL;
            for (int i = 0; i < monitorCount; i++) {
                if (monitors[i].uid == uid) {
                    wm = &monitors[i];
                    break;
                }
            }
            BOOL ok = FALSE;
            if (wm && wm->handle && wm->supported) ok = WriteWorkerBrightness(wm, target);
            if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DDC_SET_RESULT, (WPARAM)uid, ok ? 1 : 0);
        }
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
    if (m->hardwareState == HW_UNAVAILABLE) return MODE_SOFTWARE;
    return MODE_PROBING;
}

/* The slider's lower bound. Hardware monitors go to 0 (the backlight's own
 * minimum) and, when allowed, below it into software dimming; software-only
 * monitors stop at the fixed floor. */
static int MonitorMinValue(const Monitor* m) {
    switch (MonitorMode(m)) {
        case MODE_SOFTWARE: return SOFT_MIN_BRIGHTNESS;
        case MODE_HARDWARE: return g_config.allowBelowMinimum ? -SOFT_MAX_DIM : 0;
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

    int value = ClampMonitorValue(m, m->value);
    if (value != m->value) {
        m->value = value;
        m->dirty = TRUE;
        SchedulePersist();
    }

    int dim = 0;
    if (mode == MODE_HARDWARE) {
        int hw = value < 0 ? 0 : value;
        if (value < 0) dim = -value;
        if (hw != m->lastHwSent) {
            m->lastHwSent = hw;
            DdcRequestSet(m->uid, hw);
        }
    } else {
        dim = 100 - value;
    }
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
static void RefreshMonitors(void) {
    EnumContext ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    EnumDisplayMonitors(NULL, NULL, EnumMonitorProc, (LPARAM)&ctx);

    Monitor fresh[MAX_MONITORS];
    int freshCount = 0;
    ZeroMemory(fresh, sizeof(fresh));
    for (int i = 0; i < ctx.count; i++) {
        EnumEntry* e = &ctx.entries[i];
        DWORD physCount = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(e->hmon, &physCount) || physCount == 0) {
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
            dst->value = old->value;
            dst->hasValue = old->hasValue;
            dst->forceSoftware = old->forceSoftware;
            dst->hidden = old->hidden;
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
         * rescan brings them back. */
        if (!m->hidden) {
            probe[probeCount].uid = m->uid;
            probe[probeCount].hmon = m->hmon;
            probe[probeCount].physicalIndex = m->physicalIndex;
            probeCount++;
        }
        DebugPrint(L"[INFO] Monitor %d: %s (%s, %ldx%ld%s) key=%s value=%d%s%s%s\n",
                   m->uid, m->name, m->device,
                   (long)(m->rect.right - m->rect.left), (long)(m->rect.bottom - m->rect.top),
                   m->primary ? L", primary" : L"", m->key,
                   m->hasValue ? m->value : -1000,
                   m->hasValue ? L"" : L" (none saved)",
                   m->forceSoftware ? L", software only" : L"",
                   m->hidden ? L", hidden" : L"");
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

static void HandleDdcProbed(DdcProbeResult* r) {
    Monitor* m = FindMonitorByUid(r->uid);
    if (m) {
        if (r->supported) {
            m->hardwareState = HW_AVAILABLE;
            m->ddcMax = r->max;
            m->ddcCurrent = r->current;
            m->failures = 0;
            m->error[0] = L'\0';
            int percent = (int)((r->current * 100 + r->max / 2) / r->max);
            if (percent > 100) percent = 100;
            if (!m->hasValue) {
                /* First sighting: adopt the monitor's current setting so
                 * nothing changes until the user moves the slider. */
                m->value = percent;
                m->hasValue = TRUE;
                m->dirty = TRUE;
                SchedulePersist();
            }
            /* Already at the wanted level: no write needed. */
            m->lastHwSent = (m->value == percent) ? percent : -1;
            DebugPrint(L"[INFO] %s (%s): DDC/CI brightness available, current %lu/%lu (%lu ms)\n",
                       m->name, m->device, (unsigned long)r->current,
                       (unsigned long)r->max, (unsigned long)r->elapsedMs);
        } else {
            m->hardwareState = HW_UNAVAILABLE;
            if (!m->hasValue) {
                m->value = 100;
                m->hasValue = TRUE;
                m->dirty = TRUE;
                SchedulePersist();
            }
            DebugPrint(L"[INFO] %s (%s): no DDC/CI brightness (error %lu, %lu ms); using software dimming\n",
                       m->name, m->device, (unsigned long)r->error,
                       (unsigned long)r->elapsedMs);
        }
        if (!r->supported) m->lastHwSent = -1;
        ApplyMonitor(m);
        PushMonitorsToDialog();
    }
    free(r);
}

static void HandleDdcSetResult(int uid, BOOL success) {
    Monitor* m = FindMonitorByUid(uid);
    if (!m) return;
    if (success) {
        m->failures = 0;
        if (m->lastHwSent >= 0) m->ddcCurrent = (DWORD)m->lastHwSent * m->ddcMax / 100;
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
        wcscpy_s(m->error, sizeof(m->error) / sizeof(wchar_t),
                 L"The monitor stopped answering DDC/CI; switched to software dimming. "
                 L"Rescan to try hardware control again.");
        DebugPrint(L"[WARNING] %s (%s): %d consecutive DDC/CI failures; falling back to software\n",
                   m->name, m->device, m->failures);
        ApplyMonitor(m);
    } else {
        wcscpy_s(m->error, sizeof(m->error) / sizeof(wchar_t),
                 L"The monitor did not accept the last brightness change.");
    }
    PushMonitorsToDialog();
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
    for (int i = 0; i < g_monitorCount; i++) {
        const Monitor* m = &g_monitors[i];
        wchar_t eKey[256], eName[128], eDevice[64], eError[320];
        json_escape_wstring(m->key, eKey, 256);
        json_escape_wstring(m->name, eName, 128);
        json_escape_wstring(m->device, eDevice, 64);
        json_escape_wstring(m->error, eError, 320);
        int written = swprintf_s(buf + len, cap - len,
            L"%s{\"uid\":%d,\"key\":\"%s\",\"name\":\"%s\",\"device\":\"%s\","
            L"\"width\":%ld,\"height\":%ld,\"primary\":%s,\"hardware\":\"%s\","
            L"\"mode\":\"%s\",\"forceSoftware\":%s,\"hidden\":%s,\"value\":%d,"
            L"\"min\":%d,\"max\":100,\"error\":\"%s\"}",
            i == 0 ? L"" : L",", m->uid, eKey, eName, eDevice,
            (long)(m->rect.right - m->rect.left), (long)(m->rect.bottom - m->rect.top),
            m->primary ? L"true" : L"false", HardwareStateName(m->hardwareState),
            ModeName(MonitorMode(m)), m->forceSoftware ? L"true" : L"false",
            m->hidden ? L"true" : L"false",
            m->hasValue ? m->value : 100, MonitorMinValue(m), eError);
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

    const size_t cap = wcslen(monitors) + 512;
    wchar_t* script = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (script) {
        int written = swprintf_s(script, cap,
            L"window.onInit({\"config\":{\"allowBelowMinimum\":%s,\"debugLog\":%s},"
            L"\"monitors\":%s})",
            g_config.allowBelowMinimum ? L"true" : L"false",
            g_config.debugLogEnabled ? L"true" : L"false",
            monitors);
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
    } else if (strcmp(action, "setBrightness") == 0) {
        int uid = -1, value = 0;
        if (json_get_int(msg, "uid", &uid) && json_get_int(msg, "value", &value)) {
            Monitor* m = FindMonitorByUid(uid);
            if (m) SetMonitorValue(m, value);
        }
    } else if (strcmp(action, "setAllBrightness") == 0) {
        int value = 0;
        if (json_get_int(msg, "value", &value)) {
            for (int i = 0; i < g_monitorCount; i++) {
                if (!g_monitors[i].hidden) SetMonitorValue(&g_monitors[i], value);
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
            ApplyMonitor(m);
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
                m->hidden = TRUE;
                m->dirty = TRUE;
                SchedulePersist();
                /* From here on the display is treated as if it did not
                 * exist: the overlay window goes, nothing else is touched. */
                ReleaseMonitorOverlay(m);
                DebugPrint(L"[INFO] %s (%s): hidden until the next rescan\n", m->name, m->device);
            }
            PushMonitorsToDialog();
        }
    } else if (strcmp(action, "refreshMonitors") == 0) {
        DebugPrint(L"[INFO] Rescan requested from the configuration dialog\n");
        UnhideAllMonitors();
        RefreshMonitors();
    } else if (strcmp(action, "saveSettings") == 0) {
        g_config.debugLogEnabled = json_get_bool(msg, "debugLog", FALSE);
        InterlockedExchange(&g_debugLogEnabled, g_config.debugLogEnabled ? TRUE : FALSE);
        if (!SaveConfigToRegistry(&g_config)) {
            DebugPrint(L"[WARNING] Could not save all settings to the registry\n");
            MessageBoxW(g_cfgHwnd,
                L"Some settings could not be saved. Check that the current user "
                L"can write to HKEY_CURRENT_USER.",
                APP_DISPLAY_NAME_WSTRING, MB_ICONWARNING | MB_OK);
        }
        DebugPrint(L"[INFO] Settings saved (debugLog=%d)\n", g_config.debugLogEnabled);
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

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

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DebugPrint(L"[INFO] Exiting\n");
    KillTimer(g_hwnd, ID_TIMER_REFRESH_MONITORS);
    KillTimer(g_hwnd, ID_TIMER_OVERLAY_TOPMOST);
    KillTimer(g_hwnd, ID_TIMER_PERSIST);
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

"""Keep monitors awake: the power request, its timer, the tray command, the
settings it persists, and the duration wording shared with the dialog.

The power request API, timers and the registry are stubbed; the production
functions run unchanged.
"""
import json
import re
import subprocess
import unittest

from test_idle import defines
from test_schedule import ROOT, function, run_c, structure


def all_defines(prefix):
    source = (ROOT / "NotTooBright.c").read_text()
    return "\n".join(re.findall(r"^#define " + prefix + r"\w* .*$", source, re.M)) + "\n"


PRELUDE = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define DebugPrint(...) ((void)0)
#define APP_DISPLAY_NAME_WSTRING L"Not Too Bright"
typedef int BOOL;
typedef unsigned UINT;
typedef unsigned char BYTE;
typedef unsigned long DWORD, ULONG;
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef void *HANDLE, *HWND;
typedef wchar_t* LPWSTR;
/* The Windows CRT reads %s as a wide string in wide formats; glibc needs %ls. */
int swprintf_s(wchar_t* out, size_t count, const wchar_t* format, ...) {
    wchar_t fixed[256]; size_t n = 0;
    for (const wchar_t* p = format; *p; p++) {
        if (p[0] == L'%' && p[1] == L's') { fixed[n++] = L'%'; fixed[n++] = L'l'; fixed[n++] = L's'; p++; }
        else fixed[n++] = *p;
    }
    fixed[n] = 0;
    va_list args; va_start(args, format);
    int written = vswprintf(out, count, fixed, args);
    va_end(args);
    return written;
}
void wcscpy_s(wchar_t* out, size_t count, const wchar_t* in) { assert(wcslen(in) < count); wcscpy(out, in); }
''' + defines("KEEP_AWAKE_DEFAULT_MINUTES", "KEEP_AWAKE_MAX_MINUTES", "KEEP_AWAKE_FILETIME_PER_MINUTE",
              "ID_TIMER_KEEP_AWAKE")


class KeepAwakeTests(unittest.TestCase):
    def test_power_request_lasts_until_the_end_time_and_never_leaks(self):
        run_c(PRELUDE + r'''
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define POWER_REQUEST_CONTEXT_VERSION 0
#define POWER_REQUEST_CONTEXT_SIMPLE_STRING 1
#define ZeroMemory(p, n) memset((p), 0, (n))
#define MB_OK 0
#define MB_ICONWARNING 0x30
#define MIN(m) ((ULONGLONG)(m) * KEEP_AWAKE_FILETIME_PER_MINUTE)
#define DISPLAY 1
#define SYSTEM 2
typedef enum { PowerRequestDisplayRequired, PowerRequestSystemRequired } POWER_REQUEST_TYPE;
typedef struct { ULONG Version; DWORD Flags; union { LPWSTR SimpleReasonString; } Reason; } REASON_CONTEXT;
struct { int keepAwakeMinutes; ULONGLONG keepAwakeUntil; } g_config;
HANDLE g_keepAwakeRequest;
HWND g_hwnd = (HWND)1;
void* g_cfgWebView;
ULONGLONG now, savedUntil;
HANDLE live;                 /* the one request Windows holds for us */
unsigned requested;          /* DISPLAY | SYSTEM parts set on it */
int creates, closes, failCreate, failSystem, saves, tooltips, boxes, timerArmed;
UINT timerMs;
ULONG lastVersion;
DWORD lastFlags;
wchar_t reason[200], lastScript[200], boxText[200];
ULONGLONG NowFileTime(void) { return now; }
void FormatLocalTimeOfDay(ULONGLONG ft, wchar_t* out, size_t count) {
    ULONGLONG minutes = ft / KEEP_AWAKE_FILETIME_PER_MINUTE;
    swprintf(out, count, L"%02llu:%02llu", minutes / 60 % 24, minutes % 60);
}
HANDLE PowerCreateRequest(REASON_CONTEXT* context) {
    lastVersion = context->Version;
    lastFlags = context->Flags;
    wcscpy(reason, context->Reason.SimpleReasonString);
    if (failCreate) return INVALID_HANDLE_VALUE;
    assert(!live);   /* never a second request beside the first */
    creates++;
    requested = 0;
    return live = (HANDLE)(long long)(0x100 + creates);
}
BOOL PowerSetRequest(HANDLE h, POWER_REQUEST_TYPE type) {
    assert(h == live);
    if (type == PowerRequestSystemRequired && failSystem) return FALSE;
    requested |= type == PowerRequestDisplayRequired ? DISPLAY : SYSTEM;
    return TRUE;
}
BOOL PowerClearRequest(HANDLE h, POWER_REQUEST_TYPE type) {
    assert(h == live);
    requested &= ~(type == PowerRequestDisplayRequired ? DISPLAY : SYSTEM);
    return TRUE;
}
BOOL CloseHandle(HANDLE h) { assert(h == live); live = NULL; requested = 0; closes++; return TRUE; }
DWORD GetLastError(void) { return 5; }
UINT SetTimer(HWND hwnd, UINT id, UINT ms, void* callback) {
    (void)hwnd; (void)callback; assert(id == ID_TIMER_KEEP_AWAKE);
    timerArmed = 1; timerMs = ms; return 1;
}
BOOL KillTimer(HWND hwnd, UINT id) { (void)hwnd; assert(id == ID_TIMER_KEEP_AWAKE); timerArmed = 0; return TRUE; }
BOOL SaveConfigToRegistry(const void* config) { (void)config; saves++; savedUntil = g_config.keepAwakeUntil; return TRUE; }
void ScheduleTooltipUpdate(void) { tooltips++; }
void webview_cfg_execute_script(const wchar_t* script) { wcscpy(lastScript, script); }
int MessageBoxW(HWND owner, const wchar_t* text, const wchar_t* caption, UINT flags) {
    (void)owner; (void)flags; assert(wcscmp(caption, L"Not Too Bright") == 0);
    wcscpy(boxText, text); boxes++; return 1;
}
''' + function("FormatKeepAwakeDuration") + function("IsKeepingAwake") +
            function("AcquireKeepAwakeRequest") + function("ReleaseKeepAwakeRequest") +
            function("PushKeepAwakeToDialog") + function("StopKeepAwake") +
            function("UpdateKeepAwake") + function("StartKeepAwake") + r'''
static int Told(const wchar_t* until) {
    wchar_t expected[100];
    swprintf(expected, 100, L"window.onKeepAwake && window.onKeepAwake(\"%ls\")", until);
    return wcscmp(lastScript, expected) == 0;
}
int main(void) {
    g_config.keepAwakeMinutes = KEEP_AWAKE_DEFAULT_MINUTES;
    assert(KEEP_AWAKE_DEFAULT_MINUTES == 120 && KEEP_AWAKE_MAX_MINUTES == 1440);
    now = MIN(14 * 60 + 42);
    g_cfgWebView = (void*)1;
    /* Off: nothing is taken, written or told. */
    UpdateKeepAwake();
    StopKeepAwake(L"already off");
    assert(!creates && !saves && !tooltips && !timerArmed && !lastScript[0]);

    /* Chosen in the tray menu: the monitors and Windows are kept awake for
     * two hours, the end time is saved, the timer set for it, and the
     * tooltip and the open dialog are told. */
    StartKeepAwake();
    ULONGLONG until = now + MIN(120);
    assert(creates == 1 && requested == (DISPLAY | SYSTEM) && g_keepAwakeRequest == live);
    assert(lastVersion == POWER_REQUEST_CONTEXT_VERSION && lastFlags == POWER_REQUEST_CONTEXT_SIMPLE_STRING);
    assert(wcscmp(reason, L"Keep monitors awake until 16:42 (Not Too Bright tray menu)") == 0);
    assert(g_config.keepAwakeUntil == until && saves == 1 && savedUntil == until);
    assert(timerArmed && timerMs == 2 * 60 * 60 * 1000);
    assert(tooltips == 1 && Told(L"16:42") && IsKeepingAwake(now));

    /* Woken before the end (the clock moved): the same request stays and the
     * timer is set again for what is left, rounded up, never early. */
    now = until - 15000;
    UpdateKeepAwake();
    assert(creates == 1 && g_keepAwakeRequest == live && requested == (DISPLAY | SYSTEM));
    assert(timerArmed && timerMs == 2 && saves == 1);

    /* The time is up: both parts are cleared and the request closed, the end
     * time cleared and saved, the timer gone, the dialog told. */
    now = until;
    UpdateKeepAwake();
    assert(!g_keepAwakeRequest && !live && closes == 1 && !timerArmed);
    assert(!g_config.keepAwakeUntil && saves == 2 && !savedUntil && Told(L"") && !IsKeepingAwake(now));

    /* Turned off again before the end. Stopping twice writes nothing more. */
    StartKeepAwake();
    assert(creates == 2 && IsKeepingAwake(now) && saves == 3);
    StopKeepAwake(L"turned off in the tray menu");
    assert(!g_keepAwakeRequest && closes == 2 && !g_config.keepAwakeUntil && saves == 4 && !timerArmed);
    StopKeepAwake(L"turned off again");
    assert(saves == 4 && closes == 2);

    /* After a restart (an update's): the saved end time is picked up and a
     * new request taken for what is left. */
    g_config.keepAwakeUntil = now + MIN(30);
    UpdateKeepAwake();
    assert(creates == 3 && requested == (DISPLAY | SYSTEM) && saves == 4);
    assert(wcscmp(reason, L"Keep monitors awake until 17:12 (Not Too Bright tray menu)") == 0);
    assert(timerArmed && timerMs == 30 * 60 * 1000 && Told(L"17:12"));

    /* An end time further ahead than the longest duration means the clock
     * went back; it is dropped instead of keeping the monitors on for days. */
    g_config.keepAwakeUntil = now + MIN(KEEP_AWAKE_MAX_MINUTES) + 1;
    UpdateKeepAwake();
    assert(!g_keepAwakeRequest && !live && !g_config.keepAwakeUntil && saves == 5 && !timerArmed);
    /* The longest duration itself is fine. */
    g_config.keepAwakeMinutes = KEEP_AWAKE_MAX_MINUTES;
    StartKeepAwake();
    assert(g_keepAwakeRequest && timerMs == 24 * 60 * 60 * 1000 && g_config.keepAwakeUntil == now + MIN(1440));
    StopKeepAwake(L"done");
    assert(saves == 7);

    /* Windows refuses: the user is told, nothing is held, saved or timed. */
    g_config.keepAwakeMinutes = 45;
    failCreate = 1;
    StartKeepAwake();
    assert(boxes == 1 && wcsstr(boxText, L"keep the monitors awake") && wcsstr(boxText, L"error 5"));
    assert(!g_keepAwakeRequest && !live && !g_config.keepAwakeUntil && saves == 7 && !timerArmed);
    failCreate = 0;
    /* Only half of it granted: the request is closed, nothing kept. */
    failSystem = 1;
    StartKeepAwake();
    assert(boxes == 2 && !g_keepAwakeRequest && !live && closes == 5 && !g_config.keepAwakeUntil);
    failSystem = 0;
    /* A saved end time Windows now refuses is dropped, without a message. */
    g_config.keepAwakeUntil = now + MIN(10);
    failCreate = 1;
    UpdateKeepAwake();
    assert(boxes == 2 && !g_config.keepAwakeUntil && saves == 8 && !timerArmed);
    failCreate = 0;

    /* An end time that passed before its timer fired counts as off: the
     * item starts afresh from now, letting go of the old request first. */
    StartKeepAwake();
    assert(timerMs == 45 * 60 * 1000);
    now += MIN(46);
    assert(!IsKeepingAwake(now) && g_keepAwakeRequest);
    StartKeepAwake();
    assert(closes == 6 && creates == 7 && g_keepAwakeRequest == live);
    assert(g_config.keepAwakeUntil == now + MIN(45) && timerMs == 45 * 60 * 1000);

    /* Without the dialog open, nothing is sent to it. */
    g_cfgWebView = NULL;
    lastScript[0] = 0;
    StopKeepAwake(L"closed dialog");
    assert(!lastScript[0] && !g_keepAwakeRequest);
}
''')

    def test_tray_item_toggles_and_exit_ends_it_except_for_an_update(self):
        proc = function("WindowProc")
        commands = proc[proc.index("        case WM_COMMAND:"):proc.index("        case WM_APP_UPDATE_PROGRESS:")]
        run_c(PRELUDE + r'''
#include <stdint.h>
#define LOWORD(w) ((unsigned)((uintptr_t)(w) & 0xFFFF))
#define WM_COMMAND 0x111
#define WM_CLOSE 0x10
typedef intptr_t WPARAM, LPARAM;
''' + defines("ID_TRAY_MENU_CONFIGURE", "ID_TRAY_MENU_EXIT", "ID_TRAY_MENU_BRIGHTER",
              "ID_TRAY_MENU_DIMMER", "ID_TRAY_MENU_RESUME_SCHEDULE", "ID_TRAY_MENU_KEEP_AWAKE",
              "ID_TRAY_MENU_PRESET_FIRST", "TRAY_STEP_PERCENT") + r'''
struct { int trayPresetCount; int trayPresets[4]; } g_config;
HWND g_cfgHwnd;
BOOL g_updateInstallReady, keeping;
int starts, stops, quits, closes;
const wchar_t* stopReason;
ULONGLONG NowFileTime(void) { return 7; }
BOOL IsKeepingAwake(ULONGLONG now) { assert(now == 7); return keeping; }
void StartKeepAwake(void) { starts++; }
void StopKeepAwake(const wchar_t* why) { stops++; stopReason = why; }
void ApplyTrayMenuValue(int value, BOOL relative) { (void)value; (void)relative; assert(!"not a brightness item"); }
void ShowConfigDialog(void) {}
void ResumeSchedule(void) {}
void StopTrayRegistration(void) {}
void RemoveTrayIcon(void) {}
void PostQuitMessage(int code) { (void)code; quits++; }
long SendMessageW(HWND hwnd, UINT msg, WPARAM w, LPARAM l) { (void)hwnd; (void)w; (void)l; assert(msg == WM_CLOSE); closes++; return 0; }
static long Dispatch(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    (void)hwnd; (void)lParam;
    switch (uMsg) {
''' + commands + r'''
    }
    return 0;
}
int main(void) {
    /* The item starts keeping the monitors awake, and ends it while on. */
    Dispatch(NULL, WM_COMMAND, ID_TRAY_MENU_KEEP_AWAKE, 0);
    assert(starts == 1 && stops == 0);
    keeping = TRUE;
    Dispatch(NULL, WM_COMMAND, ID_TRAY_MENU_KEEP_AWAKE, 0);
    assert(starts == 1 && stops == 1 && wcscmp(stopReason, L"turned off in the tray menu") == 0);
    /* Exit lets the monitors sleep again... */
    g_cfgHwnd = (HWND)2;
    Dispatch(NULL, WM_COMMAND, ID_TRAY_MENU_EXIT, 0);
    assert(stops == 2 && wcscmp(stopReason, L"the application exits") == 0 && quits == 1 && closes == 1);
    /* ...but the exit for an update's restart keeps the saved end time, so
     * the new version carries on until then. */
    g_updateInstallReady = TRUE;
    Dispatch(NULL, WM_COMMAND, ID_TRAY_MENU_EXIT, 0);
    assert(stops == 2 && quits == 2 && starts == 1);
}
''')

    def test_duration_defaults_to_two_hours_and_round_trips_through_the_registry(self):
        run_c(PRELUDE + r'''
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define REG_SZ 1
#define REG_DWORD 4
#define REG_QWORD 11
#define REG_OPTION_NON_VOLATILE 0
#define KEY_READ 1
#define KEY_WRITE 2
typedef void* HKEY;
typedef BYTE* LPBYTE;
#define HKEY_CURRENT_USER ((HKEY)1)
''' + all_defines("REG_KEY_PATH") + all_defines("REG_VALUE_") + defines(
            "SOFT_MIN_BRIGHTNESS", "SOFT_MAX_DIM", "TRAY_MAX_PRESETS", "SCHEDULE_MAX_OFFSET",
            "SCHEDULE_DEFAULT_DAY", "SCHEDULE_DEFAULT_NIGHT", "SCHEDULE_DEFAULT_DAWN_START",
            "SCHEDULE_DEFAULT_DAWN_END", "SCHEDULE_DEFAULT_DUSK_START", "SCHEDULE_DEFAULT_DUSK_END",
            "SCHEDULE_DEFAULT_RESET_MINUTES", "SCHEDULE_DEFAULT_DEEP_LEVEL",
            "SCHEDULE_DEFAULT_DEEP_MINUTES") + structure("Schedule") + structure("Configuration") + r'''
#define ZeroMemory(p, n) memset((p), 0, (n))
wchar_t g_ignoredUpdateVersion[32];
/* The settings key as a small table of values. */
typedef struct { wchar_t name[64]; DWORD type; BYTE data[512]; DWORD size; } Value;
Value values[64];
int valueCount, keyExists;
static Value* Find(const wchar_t* name) {
    for (int i = 0; i < valueCount; i++) if (wcscmp(values[i].name, name) == 0) return &values[i];
    return NULL;
}
LONG RegOpenKeyExW(HKEY root, const wchar_t* path, DWORD options, DWORD access, HKEY* out) {
    (void)options; (void)access;
    assert(root == HKEY_CURRENT_USER && wcscmp(path, REG_KEY_PATH) == 0);
    if (!keyExists) return ERROR_FILE_NOT_FOUND;
    *out = (HKEY)2; return ERROR_SUCCESS;
}
LONG RegCreateKeyExW(HKEY root, const wchar_t* path, DWORD reserved, wchar_t* cls, DWORD options,
                     DWORD access, void* security, HKEY* out, DWORD* disposition) {
    (void)reserved; (void)cls; (void)options; (void)access; (void)security; (void)disposition;
    assert(root == HKEY_CURRENT_USER && wcscmp(path, REG_KEY_PATH) == 0);
    keyExists = 1; *out = (HKEY)2; return ERROR_SUCCESS;
}
LONG RegCloseKey(HKEY key) { (void)key; return ERROR_SUCCESS; }
LONG RegQueryValueExW(HKEY key, const wchar_t* name, DWORD* reserved, DWORD* type, BYTE* data, DWORD* size) {
    (void)key; (void)reserved;
    Value* v = Find(name);
    if (!v) return ERROR_FILE_NOT_FOUND;
    if (*size < v->size) return 234;
    memcpy(data, v->data, v->size); *size = v->size; *type = v->type;
    return ERROR_SUCCESS;
}
LONG RegSetValueExW(HKEY key, const wchar_t* name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) {
    (void)key; (void)reserved;
    Value* v = Find(name);
    if (!v) { v = &values[valueCount++]; wcscpy(v->name, name); }
    assert(size <= sizeof(v->data));
    v->type = type; v->size = size; memcpy(v->data, data, size);
    return ERROR_SUCCESS;
}
static void PutDword(const wchar_t* name, DWORD value) { RegSetValueExW(NULL, name, 0, REG_DWORD, (BYTE*)&value, sizeof(value)); }
int _wtoi(const wchar_t* s) { return (int)wcstol(s, NULL, 10); }
''' + function("ReadRegistryBool") + function("WriteRegistryDword") + function("ReadRegistryInt") +
            function("ReadRegistryQword") + function("WriteRegistryQword") +
            function("ReadRegistryDouble") + function("WriteRegistryDouble") +
            function("SetScheduleDefaults") + function("IsValidLatitude") + function("IsValidLongitude") +
            function("ParseTrayPresets") + function("FormatTrayPresets") +
            function("LoadConfigFromRegistry") + function("SaveConfigToRegistry") + r'''
int main(void) {
    Configuration c, d;
    /* A first start, before anything is saved: two hours, not kept awake. */
    assert(!LoadConfigFromRegistry(&c));
    assert(c.keepAwakeMinutes == 120 && c.keepAwakeMinutes == KEEP_AWAKE_DEFAULT_MINUTES && !c.keepAwakeUntil);
    /* Saved with the rest and read back. */
    c.keepAwakeMinutes = 45;
    c.keepAwakeUntil = 0x01DC12345678ABCDULL;
    assert(SaveConfigToRegistry(&c));
    Value* v = Find(L"KeepAwakeMinutes");
    assert(v && v->type == REG_DWORD && *(DWORD*)v->data == 45);
    v = Find(L"KeepAwakeUntil");
    assert(v && v->type == REG_QWORD && *(ULONGLONG*)v->data == 0x01DC12345678ABCDULL);
    assert(LoadConfigFromRegistry(&d));
    assert(d.keepAwakeMinutes == 45 && d.keepAwakeUntil == 0x01DC12345678ABCDULL);
    /* An existing key without the values (an older version's): the default. */
    valueCount = 0;
    PutDword(L"DebugLog", 0);
    assert(LoadConfigFromRegistry(&d) && d.keepAwakeMinutes == 120 && !d.keepAwakeUntil);
    /* A stored duration outside 1 minute to 24 hours is brought inside. */
    PutDword(L"KeepAwakeMinutes", 0);
    assert(LoadConfigFromRegistry(&d) && d.keepAwakeMinutes == 1);
    PutDword(L"KeepAwakeMinutes", 5000);
    assert(LoadConfigFromRegistry(&d) && d.keepAwakeMinutes == KEEP_AWAKE_MAX_MINUTES);
    /* Ended: the cleared end time is what the next start reads. */
    d.keepAwakeUntil = 0;
    assert(SaveConfigToRegistry(&d));
    assert(LoadConfigFromRegistry(&c) && !c.keepAwakeUntil && c.keepAwakeMinutes == KEEP_AWAKE_MAX_MINUTES);
}
''')

    def test_save_reads_the_duration_and_stop_now_ends_it(self):
        proc = function("CfgMsgReceived_Invoke")
        start = proc.index('    } else if (strcmp(action, "stopKeepAwake") == 0) {')
        end = proc.index('    } else if (strcmp(action, "close") == 0) {')
        branch = proc[start + len("    } else "):end] + "    }\n"
        run_c(PRELUDE + r'''
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 8
#define MB_ICONWARNING 0x30
#define MB_OK 0
#define WM_CLOSE 0x10
''' + defines("TRAY_MAX_PRESETS") + r'''
struct { BOOL debugLogEnabled, autoCheckForUpdates, pauseInRemoteSession, brightnessKeys;
    wchar_t trayTarget[128]; int trayPresets[TRAY_MAX_PRESETS]; int trayPresetCount;
    int keepAwakeMinutes; ULONGLONG keepAwakeUntil; } g_config;
LONG g_debugLogEnabled;
BOOL g_configCloseApproved;
HWND g_cfgHwnd = (HWND)2;
int saves, stops, closes;
const wchar_t* stopReason;
int MultiByteToWideChar(UINT page, DWORD flags, const char* in, int length, wchar_t* out, int count) {
    (void)page; (void)flags; (void)length;
    int n = 0;
    while (n < count - 1 && in[n]) { out[n] = (wchar_t)in[n]; n++; }
    out[n] = 0;
    return n + 1;
}
BOOL json_get_string(const char* json, const char* key, char* out, size_t length) {
    (void)json; (void)key; (void)length; out[0] = 0; return FALSE;
}
int ParseTrayPresets(const wchar_t* text, int* out, int max) { (void)text; (void)out; (void)max; return 0; }
void FormatTrayPresets(const int* presets, int count, wchar_t* out, size_t cap) { (void)presets; (void)count; (void)cap; out[0] = 0; }
void SaveScheduleFromMessage(const char* msg) { (void)msg; }
LONG InterlockedExchange(LONG* target, LONG value) { LONG old = *target; *target = value; return old; }
BOOL SaveConfigToRegistry(const void* config) { (void)config; saves++; return TRUE; }
int MessageBoxW(HWND owner, const wchar_t* text, const wchar_t* caption, UINT flags) {
    (void)owner; (void)text; (void)caption; (void)flags; assert(!"saving works"); return 0;
}
void SaveStartWithWindows(const char* msg) { (void)msg; }
BOOL IsStartWithWindowsEnabled(void) { return FALSE; }
void UpdateRemoteSessionState(void) {}
void UpdateBrightnessKeyReaders(void) {}
BOOL PostMessageW(HWND hwnd, UINT msg, long w, long l) { (void)hwnd; (void)w; (void)l; assert(msg == WM_CLOSE); closes++; return TRUE; }
void StopKeepAwake(const wchar_t* why) { stops++; stopReason = why; }
''' + function("json_get_bool") + function("json_get_int") + function("ClampInt") + r'''
static void Handle(const char* action, const char* msg) {
''' + branch + r'''
}
int main(void) {
    Handle("saveSettings", "{\"action\":\"saveSettings\",\"keepAwakeMinutes\":240}");
    assert(g_config.keepAwakeMinutes == 240 && saves == 1 && closes == 1 && !stops);
    /* Out of range is brought inside 1 minute to 24 hours. */
    Handle("saveSettings", "{\"action\":\"saveSettings\",\"keepAwakeMinutes\":0}");
    assert(g_config.keepAwakeMinutes == 1);
    Handle("saveSettings", "{\"action\":\"saveSettings\",\"keepAwakeMinutes\":99999}");
    assert(g_config.keepAwakeMinutes == KEEP_AWAKE_MAX_MINUTES);
    /* A message without it keeps what there is. */
    Handle("saveSettings", "{\"action\":\"saveSettings\"}");
    assert(g_config.keepAwakeMinutes == KEEP_AWAKE_MAX_MINUTES && saves == 4);
    /* A new duration does not move an end time that is already running. */
    g_config.keepAwakeUntil = 777;
    Handle("saveSettings", "{\"action\":\"saveSettings\",\"keepAwakeMinutes\":30}");
    assert(g_config.keepAwakeMinutes == 30 && g_config.keepAwakeUntil == 777 && !stops);
    /* Stop now in the dialog. */
    Handle("stopKeepAwake", "{\"action\":\"stopKeepAwake\"}");
    assert(stops == 1 && wcscmp(stopReason, L"stopped in the configuration dialog") == 0 && saves == 5);
}
''')

    def test_menu_and_dialog_word_every_duration_alike(self):
        dialog = json.loads(subprocess.check_output(["node", "-e", r'''
const { loadTs } = require('./tests/load_ts.cjs');
const bridge = loadTs('assets/src/lib/bridge.ts', { window: {} });
const words = [];
for (let minutes = 1; minutes <= 1440; minutes++) words.push(bridge.formatDuration(minutes));
console.log(JSON.stringify({ words, durations: bridge.KEEP_AWAKE_DURATIONS,
  defaultMinutes: bridge.KEEP_AWAKE_DEFAULT_MINUTES }));
'''], cwd=ROOT, text=True))
        source = (ROOT / "NotTooBright.c").read_text()
        host_default = int(re.search(r"^#define KEEP_AWAKE_DEFAULT_MINUTES (\d+)", source, re.M)[1])
        self.assertEqual(dialog["defaultMinutes"], host_default)
        self.assertEqual(host_default, 120)
        self.assertIn(host_default, dialog["durations"])
        self.assertEqual(dialog["durations"], sorted(set(dialog["durations"])))
        self.assertGreaterEqual(min(dialog["durations"]), 1)
        self.assertEqual(max(dialog["durations"]), 24 * 60)
        self.assertEqual(dialog["words"][119], "2 hours")
        self.assertEqual(dialog["words"][89], "1 hour 30 minutes")
        self.assertEqual(dialog["words"][0], "1 minute")
        run_c(PRELUDE + function("FormatKeepAwakeDuration") + r'''
int main(void) {
    char line[128];
    int minutes = 0;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        wchar_t expected[64], words[64];
        mbstowcs(expected, line, 64);
        FormatKeepAwakeDuration(++minutes, words, 64);
        assert(wcscmp(words, expected) == 0);
    }
    assert(minutes == KEEP_AWAKE_MAX_MINUTES);
}
''', "\n".join(dialog["words"]) + "\n")


if __name__ == "__main__":
    unittest.main()

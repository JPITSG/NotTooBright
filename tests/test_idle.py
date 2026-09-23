"""Idle-work regressions using production C functions with Windows I/O stubbed."""
import re
import unittest

from test_schedule import ROOT, enumeration, function, run_c, structure


def defines(*names):
    source = (ROOT / "NotTooBright.c").read_text()
    return "\n".join(re.search(r"^#define " + name + r" .*$", source, re.M)[0]
                     for name in names) + "\n"


class IdleTests(unittest.TestCase):
    def test_solar_cache_reuses_anchors_but_always_reads_the_clock(self):
        run_c(r'''
#include <assert.h>
#include <string.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define TIME_ZONE_ID_INVALID ((DWORD)-1)
#define ZeroMemory(p,n) memset(p,0,n)
typedef int BOOL;
typedef unsigned DWORD;
typedef unsigned long long ULONGLONG;
typedef struct { unsigned short wYear, wMonth, wDayOfWeek, wDay,
    wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { long Bias; SYSTEMTIME DaylightDate;
    wchar_t TimeZoneKeyName[128]; BOOL DynamicDaylightTimeDisabled;
} DYNAMIC_TIME_ZONE_INFORMATION;
''' + defines("SCHEDULE_DAY_RADIUS", "SCHEDULE_DAY_COUNT") +
            structure("Schedule") + structure("SolarDay") + structure("SolarCache") + r'''
struct { Schedule schedule; } g_config;
SolarCache g_solarCache;
SYSTEMTIME clockNow = {.wYear=2026, .wMonth=9, .wDay=21, .wHour=12};
DYNAMIC_TIME_ZONE_INFORMATION zoneNow;
DWORD zoneId = 1;
int computations, failCompute;
void GetLocalTime(SYSTEMTIME* out) { *out = clockNow; }
DWORD GetDynamicTimeZoneInformation(DYNAMIC_TIME_ZONE_INFORMATION* out) {
    *out = zoneNow; return zoneId;
}
BOOL ComputeSolarDays(double lat, double lon, const SYSTEMTIME* date, SolarDay* days) {
    computations++;
    if (failCompute) { days[0].sunrise = -999; return FALSE; }
    for (int i = 0; i < SCHEDULE_DAY_COUNT; i++) {
        days[i] = (SolarDay){.sunrise=300 + (int)lat + date->wDay + i,
            .sunset=1000 + (int)lon + zoneNow.Bias + i, .noon=720};
    }
    return TRUE;
}
''' + function("ScheduleNow") + r'''
int main(void) {
    SolarDay days[SCHEDULE_DAY_COUNT], first[SCHEDULE_DAY_COUNT];
    double minutes;
    g_config.schedule = (Schedule){.enabled=TRUE, .hasLocation=TRUE, .latitude=10, .longitude=20};
    assert(ScheduleNow(first, &minutes) && computations == 1 && minutes == 720);
    /* One hour of unchanged inputs costs one solar computation. The
     * current time still advances on every tick, including while locked. */
    for (int i = 0; i < 120; i++) {
        clockNow.wMinute = i / 2;
        clockNow.wSecond = i % 2 * 30;
        assert(ScheduleNow(days, &minutes));
        assert(minutes == 720 + i * 0.5);
        assert(memcmp(days, first, sizeof(days)) == 0);
    }
    assert(computations == 1);
    /* Callers receive a copy, so they cannot damage the cache. */
    days[0].sunrise = 0;
    assert(ScheduleNow(days, &minutes) && days[0].sunrise == first[0].sunrise);
    /* A clock correction within the day changes the value's time input. */
    clockNow.wHour = 6; clockNow.wMinute = 0; clockNow.wSecond = 0;
    assert(ScheduleNow(days, &minutes) && minutes == 360 && computations == 1);
    /* Midnight, month/year rollover and a date jump after sleep. */
    clockNow.wDay++;
    assert(ScheduleNow(days, &minutes) && computations == 2);
    assert(days[0].sunrise == first[0].sunrise + 1);
    clockNow.wMonth++;
    assert(ScheduleNow(days, &minutes) && computations == 3);
    clockNow.wYear++;
    assert(ScheduleNow(days, &minutes) && computations == 4);
    clockNow.wDay += 3;
    assert(ScheduleNow(days, &minutes) && computations == 5);
    g_config.schedule.latitude++;
    assert(ScheduleNow(days, &minutes) && computations == 6);
    g_config.schedule.longitude++;
    assert(ScheduleNow(days, &minutes) && computations == 7);
    /* Detect zone, DST status, rule and DST-option changes even when no
     * broadcast arrived and the UTC offset happens to be the same. */
    zoneNow.Bias = -60;
    assert(ScheduleNow(days, &minutes) && computations == 8);
    zoneId = 2;
    assert(ScheduleNow(days, &minutes) && computations == 9);
    wcscpy(zoneNow.TimeZoneKeyName, L"another zone");
    assert(ScheduleNow(days, &minutes) && computations == 10);
    zoneNow.DaylightDate.wMonth = 3;
    assert(ScheduleNow(days, &minutes) && computations == 11);
    zoneNow.DynamicDaylightTimeDisabled = TRUE;
    assert(ScheduleNow(days, &minutes) && computations == 12);
    g_solarCache.valid = FALSE; /* resume or system-settings notification */
    assert(ScheduleNow(days, &minutes) && computations == 13);
    /* Unknown zone: calculate normally, but never reuse unverifiable data. */
    zoneId = TIME_ZONE_ID_INVALID;
    assert(ScheduleNow(days, &minutes) && computations == 14 && !g_solarCache.valid);
    assert(ScheduleNow(days, &minutes) && computations == 15);
    zoneId = 1;
    assert(ScheduleNow(days, &minutes) && computations == 16 && g_solarCache.valid);
    /* A failed calculation cannot poison a later cache hit. */
    failCompute = TRUE; clockNow.wDay++;
    assert(!ScheduleNow(days, &minutes) && computations == 17 && !g_solarCache.valid);
    failCompute = FALSE;
    assert(ScheduleNow(days, &minutes) && computations == 18 && days[0].sunrise != -999);
    g_config.schedule.enabled = FALSE;
    assert(!ScheduleNow(days, &minutes) && computations == 18);
    g_config.schedule.enabled = TRUE; g_config.schedule.hasLocation = FALSE;
    assert(!ScheduleNow(days, &minutes) && computations == 18);
}
''')

    def test_overlay_polling_tracks_actual_dimming_and_remote_sessions(self):
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#define TRUE 1
#define FALSE 0
#define SW_HIDE 0
#define SW_SHOWNOACTIVATE 4
#define LWA_ALPHA 2
#define DebugPrint(...) ((void)0)
typedef int BOOL;
typedef unsigned UINT;
typedef unsigned char BYTE;
typedef void* HWND;
typedef struct { int hidden; HWND overlay; int overlayDim; } Monitor;
typedef struct { BOOL visible; BYTE alpha; } FakeWindow;
''' + defines("ID_TIMER_OVERLAY_TOPMOST", "OVERLAY_TOPMOST_INTERVAL_MS",
              "SOFT_MIN_BRIGHTNESS", "SOFT_MAX_DIM") + r'''
Monitor g_monitors[2];
int g_monitorCount = 2, timerStarts, timerStops, timerFails;
BOOL g_remoteSession, g_overlayTimerRunning;
HWND g_hwnd = (HWND)1;
BOOL IsWindowVisible(HWND hwnd) { return ((FakeWindow*)hwnd)->visible; }
BOOL ShowWindow(HWND hwnd, int cmd) { ((FakeWindow*)hwnd)->visible = cmd != SW_HIDE; return TRUE; }
void DestroyWindow(HWND hwnd) { ((FakeWindow*)hwnd)->visible = FALSE; }
void SetLayeredWindowAttributes(HWND hwnd, int color, BYTE alpha, int flags) {
    (void)color; (void)flags; ((FakeWindow*)hwnd)->alpha = alpha;
}
void PositionOverlay(Monitor* m) { (void)m; }
int SetTimer(HWND hwnd, UINT id, UINT interval, void* callback) {
    assert(hwnd == g_hwnd && id == ID_TIMER_OVERLAY_TOPMOST && interval == 1500 && !callback);
    timerStarts++; return !timerFails;
}
BOOL KillTimer(HWND hwnd, UINT id) {
    assert(hwnd == g_hwnd && id == ID_TIMER_OVERLAY_TOPMOST); timerStops++; return TRUE;
}
''' + function("UpdateOverlayTimer") + function("SetOverlayDim") +
            function("ReleaseMonitorOverlay") + function("DestroyAllOverlays") + r'''
int main(void) {
    FakeWindow windows[2] = {0};
    for (int i = 0; i < 2; i++) g_monitors[i].overlay = &windows[i];
    UpdateOverlayTimer();
    SetOverlayDim(&g_monitors[0], 0);
    assert(!g_overlayTimerRunning && timerStarts == 0 && timerStops == 0);
    SetOverlayDim(&g_monitors[0], 30);
    assert(g_overlayTimerRunning && timerStarts == 1 && windows[0].visible);
    SetOverlayDim(&g_monitors[0], 40);
    SetOverlayDim(&g_monitors[1], 20);
    /* Repeated updates do not postpone the next z-order check. */
    assert(timerStarts == 1);
    SetOverlayDim(&g_monitors[0], 0);
    assert(g_overlayTimerRunning && !windows[0].visible && timerStops == 0);
    ReleaseMonitorOverlay(&g_monitors[1]);
    assert(!g_overlayTimerRunning && timerStops == 1);
    /* Re-creation with a remembered dim must restore both alpha and timer. */
    g_monitors[1].overlay = &windows[1];
    g_monitors[1].overlayDim = 20;
    SetOverlayDim(&g_monitors[1], 20);
    assert(g_overlayTimerRunning && timerStarts == 2 && windows[1].alpha > 0);
    g_remoteSession = TRUE;
    ShowWindow(g_monitors[1].overlay, SW_HIDE);
    UpdateOverlayTimer();
    assert(!g_overlayTimerRunning && timerStops == 2 && g_monitors[1].overlayDim == 20);
    g_remoteSession = FALSE;
    UpdateOverlayTimer();
    assert(!g_overlayTimerRunning); /* still waiting for console refresh */
    SetOverlayDim(&g_monitors[1], g_monitors[1].overlayDim);
    assert(g_overlayTimerRunning && timerStarts == 3);
    g_monitors[1].hidden = TRUE;
    ReleaseMonitorOverlay(&g_monitors[1]);
    assert(!g_overlayTimerRunning && timerStops == 3);
    SetOverlayDim(&g_monitors[1], 50); /* missing windows never need polling */
    assert(!g_overlayTimerRunning);
    timerFails = TRUE;
    SetOverlayDim(&g_monitors[0], 30);
    assert(!g_overlayTimerRunning && timerStarts == 4);
    timerFails = FALSE;
    UpdateOverlayTimer();
    assert(g_overlayTimerRunning && timerStarts == 5);
    DestroyAllOverlays();
    assert(!g_overlayTimerRunning && timerStops == 4);
}
''')

    def test_idle_ticks_skip_work_but_transitions_and_pause_expiry_still_apply(self):
        run_c(r'''
#define SCHEDULE_DEEP_SLEEP_RAMP 5
#include <assert.h>
#include <math.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define MODE_PROBING 1
#define MODE_WAITING 2
#define MODE_HARDWARE 3
#define DebugPrint(...) ((void)0)
typedef int BOOL;
typedef unsigned long long ULONGLONG;
typedef int BrightnessMode;
typedef struct { int scheduled, hidden, value, hasValue, dirty, mode; } Monitor;
''' + defines("SCHEDULE_DAY_RADIUS", "SCHEDULE_DAY_COUNT", "SCHEDULE_MIN_GAP",
              "SOFT_MIN_BRIGHTNESS", "SOFT_MAX_DIM") + enumeration("SchedulePhase") +
            structure("Schedule") + structure("SolarDay") + r'''
struct { Schedule schedule; BOOL allowBelowMinimum; } g_config;
Monitor g_monitors[2];
int g_monitorCount = 2, g_loggedSchedulePhase = -1;
int applied, saved, persisted, pushed, tooltip;
BOOL g_remoteSession;
ULONGLONG clockFt = 100;
double clockMinutes = 720;
ULONGLONG NowFileTime(void) { return clockFt; }
void SaveConfigToRegistry(const void* config) { (void)config; saved++; }
void ScheduleTooltipUpdate(void) { tooltip++; }
void SchedulePersist(void) { persisted++; }
void PushMonitorsToDialog(void) { pushed++; ScheduleTooltipUpdate(); }
void ApplyMonitor(Monitor* m) { (void)m; applied++; ScheduleTooltipUpdate(); }
BrightnessMode MonitorMode(const Monitor* m) { return m->mode; }
int ClampMonitorValue(const Monitor* m, int value) { (void)m; return value; }
BOOL ScheduleNow(SolarDay* days, double* minutes) {
    if (!g_config.schedule.enabled) return FALSE;
    for (int i = 0; i < SCHEDULE_DAY_COUNT; i++)
        days[i] = (SolarDay){.sunrise=360, .sunset=1080, .noon=720};
    *minutes = clockMinutes; return TRUE;
}
''' + function("ScheduleAnchors") + function("SmoothStep") +
            function("ScheduleDaylightAt") + function("ScheduleDeepSleepAt") +
            function("ScheduleValueAt") + function("SchedulePhaseAt") +
            function("EvaluateSchedule") + r'''
int main(void) {
    g_config.schedule = (Schedule){.enabled=TRUE, .hasLocation=TRUE,
        .dayLevel=100, .nightLevel=30, .dawnStartOffset=-30, .dawnEndOffset=30,
        .duskStartOffset=-30, .duskEndOffset=30};
    for (int i = 0; i < 2; i++) g_monitors[i] = (Monitor){1, 0, 100, 1, 0, MODE_HARDWARE};
    EvaluateSchedule();
    assert(tooltip == 1 && applied == 0);
    tooltip = 0;
    for (int i = 0; i < 120; i++) {
        clockMinutes += 0.5; clockFt++;
        EvaluateSchedule();
    }
    assert(!tooltip && !applied && !persisted && !pushed && !saved);
    /* Beginning dusk changes the phase before rounding moves the level. */
    clockMinutes = 1050.5;
    EvaluateSchedule();
    assert(tooltip == 1 && applied == 0 && g_loggedSchedulePhase == SCHEDULE_PHASE_DUSK);
    clockMinutes = 1080;
    EvaluateSchedule();
    assert(applied == 2 && g_monitors[0].value == 65 && g_monitors[1].value == 65);
    tooltip = 0;
    EvaluateSchedule();
    assert(!tooltip && applied == 2);
    g_config.schedule.pausedUntil = clockFt + 10;
    clockMinutes = 1120;
    EvaluateSchedule();
    assert(tooltip == 1 && applied == 2 && g_loggedSchedulePhase == SCHEDULE_PHASE_NIGHT);
    /* Expiry refreshes the tooltip even through RDP, without hardware I/O. */
    tooltip = 0; clockFt += 10; g_remoteSession = TRUE;
    EvaluateSchedule();
    assert(tooltip == 1 && saved == 1 && !g_config.schedule.pausedUntil && applied == 2);
    g_remoteSession = FALSE;
    EvaluateSchedule();
    assert(applied == 4 && g_monitors[0].value == 30 && g_monitors[1].value == 30);
    /* Hidden and temporarily unavailable displays keep their old values. */
    g_monitors[0].hidden = TRUE; g_monitors[1].mode = MODE_WAITING;
    clockMinutes = 720;
    EvaluateSchedule();
    assert(applied == 4);
    g_monitors[0].hidden = FALSE; g_monitors[1].mode = MODE_HARDWARE;
    EvaluateSchedule();
    assert(applied == 6 && g_monitors[0].value == 100 && g_monitors[1].value == 100);
}
''')

    def test_lock_resume_and_clock_messages_keep_schedule_recovery(self):
        # Execute the actual timer/session/power cases, omitting unrelated
        # tray commands and updater completion handlers from the harness.
        proc = function("WindowProc")
        handlers = proc[proc.index("        case WM_TIMER:"):proc.index("        case WM_DPICHANGED:")]
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define TRUE 1
#define FALSE 0
#define DebugPrint(...) ((void)0)
typedef int BOOL;
typedef unsigned UINT, DWORD;
typedef long LONG;
typedef intptr_t WPARAM, LPARAM;
typedef void* HWND;
enum { WM_TIMER=100, WM_TIMECHANGE, WM_SETTINGCHANGE, WM_DISPLAYCHANGE,
    WM_WTSSESSION_CHANGE, WM_DEVICECHANGE, WM_POWERBROADCAST };
enum { PBT_APMRESUMEAUTOMATIC=200, PBT_APMRESUMESUSPEND, PBT_POWERSETTINGCHANGE,
    DBT_DEVNODES_CHANGED, WTS_SESSION_LOCK, WTS_SESSION_UNLOCK };
''' + defines("ID_TIMER_REFRESH_MONITORS", "ID_TIMER_OVERLAY_TOPMOST", "ID_TIMER_PERSIST",
              "ID_TIMER_SCHEDULE", "ID_TIMER_DDC_RETRY", "ID_TIMER_AUTO_UPDATE",
              "ID_TIMER_TOOLTIP", "ID_TIMER_KEY_DEVICES", "ID_TIMER_PANEL", "SCHEDULE_INTERVAL_MS",
              "REFRESH_MONITORS_DEBOUNCE_MS", "REFRESH_MONITORS_RESUME_DELAY_MS",
              "PANEL_SETTLE_MS", "PANEL_QUIET_MS") + r'''
struct { BOOL autoCheckForUpdates; struct { BOOL enabled, hasLocation; } schedule; } g_config;
struct { BOOL valid; } g_solarCache;
typedef struct { int PowerSetting; DWORD DataLength; DWORD Data[1]; } POWERBROADCAST_SETTING;
const int kGuidConsoleDisplayState = 1, kGuidAcDcPowerSource = 2, kGuidPowerSavingStatus = 3,
    kGuidEnergySaverStatus = 4;
LONG g_lastDisplayState = -1, g_lastPowerSource = -1, g_lastBatterySaver = -1, g_lastEnergySaver = -1;
int quietTransitions, holdTransitions, panelServices, powerConditions;
UINT lastTransitionMs;
LONG* lastCondition;
LONG lastConditionValue;
void NotePanelTransition(BOOL quiet, DWORD ms) {
    if (quiet) quietTransitions++; else holdTransitions++;
    lastTransitionMs = ms;
}
void NotePowerCondition(LONG* last, LONG value, const wchar_t* what) {
    (void)what; powerConditions++; lastCondition = last; lastConditionValue = value;
}
void ServicePanels(void) { panelServices++; }
BOOL g_overlayTimerRunning, g_ddcRetryPending;
HWND g_hwnd = (HWND)1;
int evaluations, refreshes, keyRestarts, sessionChecks, scheduleTimer, raised;
void EvaluateSchedule(void) { evaluations++; }
void RefreshMonitors(void) { refreshes++; }
void KeepOverlaysOnTop(void) { raised++; }
void PersistDirtyMonitors(void) {}
void StartUpdateCheck(BOOL automatic) { (void)automatic; }
void UpdateTrayTooltip(void) {}
void UpdateBrightnessKeyReaders(void) {}
void UpdateRemoteSessionState(void) { sessionChecks++; }
void ScheduleMonitorRefresh(UINT ms) { assert(ms == 1500 || ms == 3000); refreshes++; }
void ScheduleBrightnessKeyReaderRestart(void) { keyRestarts++; }
BOOL IsEqualGUID(const int* a, const int* b) { return *a == *b; }
LONG InterlockedExchange(LONG* p, LONG value) { LONG old = *p; *p = value; return old; }
int SetTimer(HWND hwnd, UINT id, UINT interval, void* callback) {
    (void)hwnd; (void)callback;
    assert(id == ID_TIMER_SCHEDULE && interval == 30000); scheduleTimer = TRUE; return 1;
}
void KillTimer(HWND hwnd, UINT id) { (void)hwnd; if (id == ID_TIMER_SCHEDULE) scheduleTimer = FALSE; }
''' + function("UpdateScheduleTimer") + r'''
static long Dispatch(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
''' + handlers + r'''
    }
    return 0;
}
int main(void) {
    g_config.schedule.enabled = g_config.schedule.hasLocation = TRUE;
    UpdateScheduleTimer();
    assert(scheduleTimer);
    Dispatch(g_hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
    assert(scheduleTimer && sessionChecks == 1);
    for (int i = 0; i < 120; i++) Dispatch(g_hwnd, WM_TIMER, ID_TIMER_SCHEDULE, 0);
    assert(evaluations == 120 && scheduleTimer);
    Dispatch(g_hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    assert(scheduleTimer && sessionChecks == 2);
    for (int message = WM_TIMECHANGE; message <= WM_SETTINGCHANGE; message++) {
        g_solarCache.valid = TRUE;
        Dispatch(g_hwnd, message, 0, 0);
        assert(!g_solarCache.valid && scheduleTimer);
    }
    assert(evaluations == 122);
    g_solarCache.valid = TRUE;
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    assert(!g_solarCache.valid && scheduleTimer && refreshes == 1 && keyRestarts == 1);
    g_solarCache.valid = TRUE;
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
    assert(!g_solarCache.valid && scheduleTimer && refreshes == 2 && keyRestarts == 2);
    /* After a resume Windows applies its own level to a built-in display. */
    assert(quietTransitions == 2 && lastTransitionMs == PANEL_QUIET_MS);
    POWERBROADCAST_SETTING power = {1, sizeof(DWORD), {0}};
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&power);
    assert(scheduleTimer && refreshes == 2);
    /* The first display state only tells the current one. */
    assert(quietTransitions == 2 && holdTransitions == 0);
    power.Data[0] = 1;
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&power);
    assert(scheduleTimer && refreshes == 3);
    /* Switched back on: a level of Windows' own. */
    assert(quietTransitions == 3 && lastTransitionMs == PANEL_QUIET_MS);
    /* Dimmed and undimmed: Windows restores the previous level, and a key
     * pressed to wake the display is the user's, so no quiet period. */
    power.Data[0] = 2;
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&power);
    assert(quietTransitions == 3 && holdTransitions == 1 && refreshes == 3);
    power.Data[0] = 1;
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&power);
    assert(quietTransitions == 3 && holdTransitions == 2 && lastTransitionMs == PANEL_SETTLE_MS);
    assert(refreshes == 3 && g_lastDisplayState == 1);
    /* Power source and saver modes go to their own trackers. */
    POWERBROADCAST_SETTING source = {2, sizeof(DWORD), {1}};
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&source);
    assert(powerConditions == 1 && lastCondition == &g_lastPowerSource && lastConditionValue == 1);
    source.PowerSetting = 3;
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&source);
    assert(powerConditions == 2 && lastCondition == &g_lastBatterySaver);
    source.PowerSetting = 4;
    Dispatch(g_hwnd, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&source);
    assert(powerConditions == 3 && lastCondition == &g_lastEnergySaver && g_lastDisplayState == 1);
    Dispatch(g_hwnd, WM_DISPLAYCHANGE, 0, 0);
    assert(quietTransitions == 4 && refreshes == 4);
    Dispatch(g_hwnd, WM_TIMER, ID_TIMER_PANEL, 0);
    assert(panelServices == 1);
    Dispatch(g_hwnd, WM_TIMER, ID_TIMER_SCHEDULE, 0);
    assert(evaluations == 123);
    /* A queued overlay timer cannot do work after it was disabled. */
    Dispatch(g_hwnd, WM_TIMER, ID_TIMER_OVERLAY_TOPMOST, 0);
    assert(raised == 0);
    g_overlayTimerRunning = TRUE;
    Dispatch(g_hwnd, WM_TIMER, ID_TIMER_OVERLAY_TOPMOST, 0);
    assert(raised == 1);
    g_config.schedule.enabled = FALSE;
    UpdateScheduleTimer();
    assert(!scheduleTimer);
}
''')


if __name__ == "__main__":
    unittest.main()

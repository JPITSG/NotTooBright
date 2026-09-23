"""Portable regression checks against functions extracted from the C source.

Run with python3 -m unittest discover -s tests. Requires a native C compiler.
Windows I/O is stubbed; real DDC/CI still requires Windows testing.
"""
import os
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(name):
    source = (ROOT / "NotTooBright.c").read_text()
    match = re.search(r"static [^\n]*\b" + name + r"\([^;]*?\) \{", source)
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]



def structure(name):
    source = (ROOT / "NotTooBright.c").read_text()
    return re.search(r"typedef struct \{[^}]*\} " + name + r";", source)[0]


def enumeration(name):
    source = (ROOT / "NotTooBright.c").read_text()
    return re.search(r"typedef enum \{[^}]*\} " + name + r";", source)[0]


def run_c(code, data=None):
    with tempfile.TemporaryDirectory(prefix="ntb-test-") as directory:
        binary = str(Path(directory) / "test")
        subprocess.run([os.environ.get("CC", "cc"), "-x", "c", "-", "-o", binary, "-lm"],
                       input=code, text=True, check=True)
        subprocess.run([binary], input=data, text=True, check=True)


class ScheduleTests(unittest.TestCase):
    def test_failed_write_retries_without_a_new_target(self):
        run_c(r'''
#include <assert.h>
#include <wchar.h>
#define DebugPrint(...) ((void)0)
#define DDC_MAX_CONSECUTIVE_FAILURES 3
#define HW_AVAILABLE 1
#define HW_UNAVAILABLE 2
#define PANEL_ECHO_MS 1500
typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef struct { int hidden, failures, lastHwSent, hardwareState, builtin;
    DWORD ddcMax, ddcMin, ddcCurrent; ULONGLONG panelEchoUntil; wchar_t error[200]; } Monitor;
Monitor monitor;
int retries;
BOOL g_remoteSession;
ULONGLONG GetTickCount64(void) { return 7000; }
Monitor* FindMonitorByUid(int uid) { (void)uid; return &monitor; }
void PushMonitorsToDialog(void) {}
void ScheduleDdcRetry(void) { retries++; }
void wcscpy_s(wchar_t* out, size_t count, const wchar_t* value) {
    (void)count; wcscpy(out, value);
}
''' + function("HandleDdcSetResult") + r'''
int main(void) {
    monitor.hardwareState = HW_AVAILABLE;
    monitor.lastHwSent = 50;
    HandleDdcSetResult(1, 0);
    assert(retries == 1 && monitor.lastHwSent == -1);
    assert(monitor.hardwareState == HW_AVAILABLE);
    HandleDdcSetResult(1, 0);
    HandleDdcSetResult(1, 0);
    assert(retries == 3 && monitor.hardwareState == HW_UNAVAILABLE);
    HandleDdcSetResult(1, 1);
    assert(retries == 3 && monitor.failures == 0);
    assert(monitor.panelEchoUntil == 0);   /* DDC/CI writes have no echo */
    /* Windows reports the level a write to a built-in display set; that
     * report must count as the write's echo, even after a failure. */
    monitor.builtin = 1;
    HandleDdcSetResult(1, 1);
    assert(monitor.panelEchoUntil == 7000 + PANEL_ECHO_MS);
    monitor.hidden = 1;
    monitor.panelEchoUntil = 0;
    HandleDdcSetResult(1, 0);
    assert(retries == 3 && monitor.panelEchoUntil == 7000 + PANEL_ECHO_MS);
}
''')


    def test_empty_source_is_reopened_but_valid_handles_are_kept(self):
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#define MAX_MONITORS 16
#define FALSE 0
#define TRUE 1
typedef int HMONITOR;
typedef struct { HMONITOR hmon; int inJob, count; } WorkerSource;
typedef struct { HMONITOR hmon; } DdcProbeEntry;
int opens;
int OpenWorkerSource(WorkerSource* s) {
    opens++;
    s->count = opens == 1 ? 0 : 1;
    return s->count > 0;
}
''' + function("FindWorkerSource") + function("OpenJobSources") + r'''
int main(void) {
    WorkerSource sources[MAX_MONITORS] = {0};
    int count = 0;
    DdcProbeEntry job = {.hmon = 1};
    OpenJobSources(sources, &count, &job, 1);
    assert(opens == 1 && count == 1 && sources[0].count == 0);
    OpenJobSources(sources, &count, &job, 1);
    assert(opens == 2 && count == 1 && sources[0].count == 1);
    OpenJobSources(sources, &count, &job, 1);
    assert(opens == 2 && sources[0].inJob);
    sources[0].count = 0; /* A later reopen failed. */
    OpenJobSources(sources, &count, &job, 1);
    assert(opens == 3 && sources[0].count == 1);
}
''')


    def test_monitor_key_lists_match_whole_identities(self):
        run_c(r'''
#include <assert.h>
#include <string.h>
#include <wchar.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
''' + function("MonitorKeyInList") + r'''
int main(void) {
    assert(MonitorKeyInList(L"ABC_123", "XYZ,ABC_123,END"));
    assert(MonitorKeyInList(L"END", "XYZ,ABC_123,END"));
    assert(!MonitorKeyInList(L"ABC", "XYZ,ABC_123,END"));
    assert(!MonitorKeyInList(L"ABC_123", ""));
    assert(!MonitorKeyInList(L"ABC_123", "ABC_1234"));
}
''')


    def test_extended_range_clamps_endpoints_before_interpolation(self):
        run_c(r'''
#include <assert.h>
#include <math.h>
#define SOFT_MAX_DIM 90
#define SCHEDULE_MIN_GAP 5
#define SCHEDULE_DAY_RADIUS 2
#define SCHEDULE_DAY_COUNT 5
#define SCHEDULE_DEEP_SLEEP_RAMP 5
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset, deepSleepEnabled, deepSleepLevel, deepSleepMinutes; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
struct { int allowBelowMinimum; } g_config;
''' + enumeration("SchedulePhase") + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleDaylightAt") + function("ScheduleDeepSleepAt") + function("ScheduleValueAt") + r'''
int main(void) {
    Schedule sc = {100, -90, -30, 30, -30, 30};
    SolarDay days[5];
    for (int i = 0; i < 5; i++) days[i] = (SolarDay){360, 1080, 720, 0};
    assert(ScheduleValueAt(&sc, days, 360) == 50);
    assert(ScheduleValueAt(&sc, days, 1080) == 50);
    assert(ScheduleValueAt(&sc, days, 0) == 0);
    assert(sc.nightLevel == -90);
    g_config.allowBelowMinimum = 1;
    assert(ScheduleValueAt(&sc, days, 360) == 5);
    assert(ScheduleValueAt(&sc, days, 0) == -90);
    days[2].polar = -1;
    assert(ScheduleValueAt(&sc, days, 0) == -90);
    g_config.allowBelowMinimum = 0;
    assert(ScheduleValueAt(&sc, days, 0) == 0);
    /* The deep sleep level is clamped the same way. */
    days[2].polar = 0;
    sc.nightLevel = 30;
    sc.deepSleepEnabled = 1; sc.deepSleepLevel = -50; sc.deepSleepMinutes = 1410;
    assert(ScheduleValueAt(&sc, days, 0) == 0);
    g_config.allowBelowMinimum = 1;
    assert(ScheduleValueAt(&sc, days, 0) == -50);
}
''')


    def test_deep_sleep_fades_in_and_lasts_until_the_morning(self):
        run_c(r'''
#include <assert.h>
#include <math.h>
#define SOFT_MAX_DIM 90
#define SCHEDULE_MIN_GAP 5
#define SCHEDULE_DAY_RADIUS 2
#define SCHEDULE_DAY_COUNT 5
#define SCHEDULE_DEEP_SLEEP_RAMP 5
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset, deepSleepEnabled, deepSleepLevel, deepSleepMinutes; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
struct { int allowBelowMinimum; } g_config;
''' + enumeration("SchedulePhase") + function("ScheduleAnchors") + function("SmoothStep") +
              function("ScheduleDaylightAt") + function("ScheduleDeepSleepAt") + function("ScheduleValueAt") + r'''
int main(void) {
    /* Dawn 05:30-06:30, dusk 17:30-18:30; night 30%, deep sleep 10% at 23:30. */
    Schedule sc = {100, 30, -30, 30, -30, 30, 1, 10, 1410};
    SolarDay days[5];
    for (int i = 0; i < 5; i++) days[i] = (SolarDay){360, 1080, 720, 0};
    assert(ScheduleValueAt(&sc, days, 1200) == 30);     /* evening: the night level */
    assert(ScheduleValueAt(&sc, days, 1410) == 30);     /* the fade starts here... */
    assert(ScheduleValueAt(&sc, days, 1412.5) == 20);   /* ...halfway after 2.5 minutes... */
    assert(ScheduleValueAt(&sc, days, 1415) == 10);     /* ...and is done after five */
    assert(ScheduleValueAt(&sc, days, 1439) == 10);
    assert(ScheduleValueAt(&sc, days, 0) == 10);        /* through midnight */
    assert(ScheduleValueAt(&sc, days, 330) == 10);      /* until the dawn ramp starts */
    assert(ScheduleValueAt(&sc, days, 360) == 55);      /* which rises from the deep level */
    assert(ScheduleValueAt(&sc, days, 390) == 100);
    assert(ScheduleValueAt(&sc, days, 1080) == 65);     /* dusk still falls to the night level */
    /* Off: exactly the old curve. */
    sc.deepSleepEnabled = 0;
    assert(ScheduleValueAt(&sc, days, 0) == 30 && ScheduleValueAt(&sc, days, 360) == 65);
    sc.deepSleepEnabled = 1;
    /* After midnight, and above the night level. */
    sc.deepSleepMinutes = 90; sc.deepSleepLevel = 60;
    assert(ScheduleValueAt(&sc, days, 60) == 30 && ScheduleValueAt(&sc, days, 95) == 60);
    assert(ScheduleValueAt(&sc, days, 1439) == 30);
    /* In the daytime it only shows once dusk falls: to the deep level. */
    sc.deepSleepMinutes = 840; sc.deepSleepLevel = 10;
    assert(ScheduleValueAt(&sc, days, 900) == 100 && ScheduleValueAt(&sc, days, 1080) == 55);
    assert(ScheduleValueAt(&sc, days, 1200) == 10 && ScheduleValueAt(&sc, days, 300) == 10);
    /* Inside the dawn ramp: over by the time the day level is reached, and
     * yesterday's has ended long before tonight. */
    sc.deepSleepMinutes = 360;
    assert(ScheduleValueAt(&sc, days, 370) == 77 && ScheduleValueAt(&sc, days, 390) == 100);
    assert(ScheduleValueAt(&sc, days, 0) == 30 && ScheduleValueAt(&sc, days, 1200) == 30);
    /* Polar night: from its time until solar noon. */
    sc.deepSleepMinutes = 1410;
    for (int i = 0; i < 5; i++) days[i] = (SolarDay){720, 720, 720, -1};
    assert(ScheduleValueAt(&sc, days, 0) == 10 && ScheduleValueAt(&sc, days, 700) == 10);
    assert(ScheduleValueAt(&sc, days, 800) == 30 && ScheduleValueAt(&sc, days, 1420) == 10);
    /* Polar day: the day level all day. */
    for (int i = 0; i < 5; i++) days[i].polar = 1;
    assert(ScheduleValueAt(&sc, days, 0) == 100 && ScheduleValueAt(&sc, days, 1420) == 100);
}
''')

    def test_schedule_phase_follows_the_curve(self):
        run_c(r'''
#include <assert.h>
#include <math.h>
#define SCHEDULE_MIN_GAP 5
#define SCHEDULE_DAY_RADIUS 2
#define SCHEDULE_DAY_COUNT 5
#define SCHEDULE_DEEP_SLEEP_RAMP 5
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset, deepSleepEnabled, deepSleepLevel, deepSleepMinutes; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
''' + enumeration("SchedulePhase") + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleDaylightAt") + function("ScheduleDeepSleepAt") + function("SchedulePhaseAt") + r'''
static SchedulePhase phaseAt(const Schedule* sc, const SolarDay* days, double minutes) {
    SchedulePhase phase;
    ScheduleDaylightAt(sc, days, minutes, &phase);
    return phase;
}
int main(void) {
    Schedule sc = {100, 20, -30, 30, -30, 30};
    SolarDay days[5];
    for (int i = 0; i < 5; i++) days[i] = (SolarDay){360, 1080, 720, 0};
    assert(phaseAt(&sc, days, 0) == SCHEDULE_PHASE_NIGHT);
    assert(phaseAt(&sc, days, 330) == SCHEDULE_PHASE_NIGHT);   /* dawn starts here */
    assert(phaseAt(&sc, days, 330.5) == SCHEDULE_PHASE_DAWN);
    assert(phaseAt(&sc, days, 389.5) == SCHEDULE_PHASE_DAWN);
    assert(phaseAt(&sc, days, 390) == SCHEDULE_PHASE_DAY);     /* dawn ends here */
    assert(phaseAt(&sc, days, 720) == SCHEDULE_PHASE_DAY);
    assert(phaseAt(&sc, days, 1050) == SCHEDULE_PHASE_DAY);    /* dusk starts here */
    assert(phaseAt(&sc, days, 1050.5) == SCHEDULE_PHASE_DUSK);
    assert(phaseAt(&sc, days, 1109.5) == SCHEDULE_PHASE_DUSK);
    assert(phaseAt(&sc, days, 1110) == SCHEDULE_PHASE_NIGHT);  /* dusk ends here */
    assert(phaseAt(&sc, days, 1439) == SCHEDULE_PHASE_NIGHT);
    /* The phase follows the time of day even when both levels are equal. */
    sc.nightLevel = 100;
    assert(phaseAt(&sc, days, 360) == SCHEDULE_PHASE_DAWN);
    sc.nightLevel = 20;
    /* Yesterday's dusk running into today's dawn: whichever contributes
     * more daylight decides, so the phase flips at the crossing. */
    sc.duskEndOffset = 6 * 60;
    sc.dawnStartOffset = -6 * 60;
    days[1] = (SolarDay){60, 1380, 720, 0};    /* yesterday: dusk ends at 1740 = 300 today */
    days[2] = (SolarDay){300, 1080, 720, 0};   /* today: dawn starts at -60 */
    assert(phaseAt(&sc, days, 0) == SCHEDULE_PHASE_DUSK);
    assert(phaseAt(&sc, days, 250) == SCHEDULE_PHASE_DAWN);
    /* Polar day and night are plateaus. */
    days[2].polar = 1;
    assert(phaseAt(&sc, days, 0) == SCHEDULE_PHASE_DAY);
    days[2].polar = -1;
    assert(phaseAt(&sc, days, 720) == SCHEDULE_PHASE_NIGHT);
    /* Deep sleep: its fade, then the plateau until the dawn ramp. */
    sc = (Schedule){100, 20, -30, 30, -30, 30, 1, 5, 1410};
    for (int i = 0; i < 5; i++) days[i] = (SolarDay){360, 1080, 720, 0};
    assert(SchedulePhaseAt(&sc, days, 1400) == SCHEDULE_PHASE_NIGHT);
    assert(SchedulePhaseAt(&sc, days, 1410) == SCHEDULE_PHASE_NIGHT);   /* fade starts here */
    assert(SchedulePhaseAt(&sc, days, 1412) == SCHEDULE_PHASE_SLEEP_FADE);
    assert(SchedulePhaseAt(&sc, days, 1415) == SCHEDULE_PHASE_DEEP_SLEEP);
    assert(SchedulePhaseAt(&sc, days, 200) == SCHEDULE_PHASE_DEEP_SLEEP);
    assert(SchedulePhaseAt(&sc, days, 330.5) == SCHEDULE_PHASE_DAWN);
    assert(SchedulePhaseAt(&sc, days, 720) == SCHEDULE_PHASE_DAY);
    assert(SchedulePhaseAt(&sc, days, 1200) == SCHEDULE_PHASE_NIGHT);
    sc.deepSleepEnabled = 0;
    assert(SchedulePhaseAt(&sc, days, 200) == SCHEDULE_PHASE_NIGHT);
}
''')

    def test_tooltip_shows_the_schedule_state_before_the_monitors(self):
        run_c(r'''
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#define APP_DISPLAY_NAME_WSTRING L"Not Too Bright"
#define MODE_PROBING 1
#define MODE_HARDWARE 2
#define NIF_TIP 4
#define NIM_MODIFY 1
#define _TRUNCATE ((size_t)-1)
typedef int BOOL;
typedef unsigned UINT;
typedef unsigned long long ULONGLONG;
typedef void* HWND;
typedef struct { HWND hWnd; UINT uFlags; wchar_t szTip[128]; } NOTIFYICONDATAW;
typedef struct { int hidden, hasValue, value, mode; wchar_t name[128]; } Monitor;
''' + enumeration("SchedulePhase") + r'''
NOTIFYICONDATAW g_nid = { (HWND)1 };
Monitor g_monitors[4];
struct { struct { BOOL enabled; } schedule; } g_config;
int g_monitorCount, modifies, paused;
BOOL g_remoteSession;
SchedulePhase currentPhase;
int MonitorMode(const Monitor* m) { return m->mode; }
BOOL SchedulePhaseNow(SchedulePhase* phase) { *phase = currentPhase; return g_config.schedule.enabled; }
ULONGLONG NowFileTime(void) { return 0; }
BOOL IsSchedulePaused(ULONGLONG now) { (void)now; return paused; }
void Shell_NotifyIconW(int op, NOTIFYICONDATAW* nid) { assert(op == NIM_MODIFY && nid->uFlags == NIF_TIP); modifies++; }
void wcscpy_s(wchar_t* out, size_t count, const wchar_t* in) { (void)count; wcscpy(out, in); }
void wcscat_s(wchar_t* out, size_t count, const wchar_t* in) { (void)count; wcscat(out, in); }
void wcsncpy_s(wchar_t* out, size_t count, const wchar_t* in, size_t n) {
    (void)n; wcsncpy(out, in, count - 1); out[count - 1] = 0;
}
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
''' + function("SchedulePhaseName") + function("UpdateTrayTooltip") + r'''
int main(void) {
    g_monitorCount = 2;
    g_monitors[0] = (Monitor){0, 1, 75, MODE_HARDWARE, L"Left"};
    g_monitors[1] = (Monitor){0, 0, 0, MODE_PROBING, L"Right"};
    UpdateTrayTooltip();
    assert(modifies == 1 && wcscmp(g_nid.szTip, L"Schedule: Disabled\nLeft: 75%\nRight: ...") == 0);
    g_config.schedule.enabled = 1;
    currentPhase = SCHEDULE_PHASE_DUSK;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"State: Daytime \u2192 Night\nSchedule: Active\nLeft: 75%\nRight: ...") == 0);
    currentPhase = SCHEDULE_PHASE_DAWN;
    paused = 1;
    UpdateTrayTooltip();
    assert(wcsncmp(g_nid.szTip, L"State: Night \u2192 Daytime\nSchedule: Paused\n", 39) == 0);
    paused = 0;
    currentPhase = SCHEDULE_PHASE_DAY;
    g_monitors[1].hidden = 1;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"State: Daytime\nSchedule: Active\nLeft: 75%") == 0);
    currentPhase = SCHEDULE_PHASE_SLEEP_FADE;
    g_monitorCount = 0;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"State: Night \u2192 Deep sleep\nSchedule: Active") == 0);
    currentPhase = SCHEDULE_PHASE_DEEP_SLEEP;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"State: Deep sleep\nSchedule: Active") == 0);
    currentPhase = SCHEDULE_PHASE_NIGHT;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"State: Night\nSchedule: Active") == 0);
    /* Unchanged text is not sent to the shell again. */
    UpdateTrayTooltip();
    assert(modifies == 7);
    g_config.schedule.enabled = 0;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"Schedule: Disabled") == 0);
    /* A Remote Desktop session is announced first. */
    g_remoteSession = 1;
    g_monitorCount = 1;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"Remote Desktop session: paused\nSchedule: Disabled\nLeft: 75%") == 0);
    g_config.schedule.enabled = 1;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"Remote Desktop session: paused\nState: Night\nSchedule: Active\nLeft: 75%") == 0);
    g_remoteSession = 0;
    g_monitorCount = 0;
    /* Long lists are cut with an ellipsis and the schedule lines always fit. */
    g_config.schedule.enabled = 1;
    g_monitorCount = 4;
    for (int i = 0; i < 4; i++) {
        g_monitors[i] = (Monitor){0, 1, 50, MODE_HARDWARE, L""};
        for (int j = 0; j < 45; j++) g_monitors[i].name[j] = L'a' + i;
    }
    UpdateTrayTooltip();
    assert(wcslen(g_nid.szTip) < 128);
    assert(wcsstr(g_nid.szTip, L"aaa...: 50%") && wcsstr(g_nid.szTip, L"\n...") && !wcsstr(g_nid.szTip, L"ccc"));
}
''')

    def test_manual_change_pauses_and_resume_restores_the_whole_schedule(self):
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define SCHEDULE_DAY_COUNT 5
#define SCHEDULE_DEEP_SLEEP_RAMP 5
#define MODE_PROBING 1
#define MODE_WAITING 2
#define MODE_HARDWARE 3
#define DebugPrint(...) ((void)0)
typedef int BOOL;
typedef unsigned long long ULONGLONG;
typedef int BrightnessMode;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
typedef struct { int scheduled, hidden, value, hasValue, dirty, mode; wchar_t name[8], device[8]; } Monitor;
''' + enumeration("SchedulePhase") + structure("Schedule") + r'''
struct { Schedule schedule; } g_config;
Monitor g_monitors[3];
int g_monitorCount = 3, g_loggedSchedulePhase = -1, target = 30, saves, applied;
BOOL g_remoteSession;
ULONGLONG now = 1000, nextReset = 5000;
ULONGLONG NowFileTime(void) { return now; }
ULONGLONG NextCycleResetFileTime(void) { return nextReset; }
BOOL SaveConfigToRegistry(const void* config) { (void)config; saves++; return TRUE; }
void FormatLocalTimeOfDay(ULONGLONG ft, wchar_t* out, size_t count) { (void)ft; (void)count; out[0] = 0; }
void PushMonitorsToDialog(void) {}
void ScheduleTooltipUpdate(void) {}
void SchedulePersist(void) {}
BOOL ScheduleNow(SolarDay days[SCHEDULE_DAY_COUNT], double* minutes) {
    (void)days; *minutes = 0; return g_config.schedule.enabled;
}
int ScheduleValueAt(const Schedule* sc, const SolarDay days[SCHEDULE_DAY_COUNT], double minutes) {
    (void)sc; (void)days; (void)minutes; return target;
}
SchedulePhase SchedulePhaseAt(const Schedule* sc, const SolarDay days[SCHEDULE_DAY_COUNT], double minutes) {
    (void)sc; (void)days; (void)minutes; return SCHEDULE_PHASE_DAY;
}
const wchar_t* SchedulePhaseName(SchedulePhase phase) { (void)phase; return L""; }
BrightnessMode MonitorMode(const Monitor* m) { return m->mode; }
int ClampMonitorValue(const Monitor* m, int value) { (void)m; return value; }
void ApplyMonitor(Monitor* m) { (void)m; applied++; }
''' + function("IsSchedulePaused") + function("EvaluateSchedule") + function("NoteManualChange") + function("ResumeSchedule") + r'''
int main(void) {
    g_config.schedule.enabled = TRUE;
    g_config.schedule.hasLocation = TRUE;
    for (int i = 0; i < 3; i++) g_monitors[i] = (Monitor){1, 0, 50, 1, 0, MODE_HARDWARE, L"", L""};
    g_monitors[2].scheduled = 0;
    EvaluateSchedule();
    assert(applied == 2 && g_monitors[0].value == 30 && g_monitors[1].value == 30 && g_monitors[2].value == 50);
    /* A manual change on one scheduled monitor pauses the schedule for all of them. */
    g_monitors[0].value = 80;
    NoteManualChange(&g_monitors[0]);
    assert(g_config.schedule.pausedUntil == 5000 && saves == 1 && IsSchedulePaused(now));
    target = 40;
    EvaluateSchedule();
    assert(applied == 2 && g_monitors[0].value == 80 && g_monitors[1].value == 30);
    /* A second change while paused does not move the deadline. */
    nextReset = 9000;
    NoteManualChange(&g_monitors[1]);
    assert(g_config.schedule.pausedUntil == 5000 && saves == 1);
    /* Resume now brings every scheduled monitor back at once. */
    ResumeSchedule();
    assert(!g_config.schedule.pausedUntil && saves == 2 && applied == 4);
    assert(g_monitors[0].value == 40 && g_monitors[1].value == 40 && g_monitors[2].value == 50);
    ResumeSchedule();
    assert(saves == 2);
    /* Unscheduled and hidden monitors never pause the schedule. */
    NoteManualChange(&g_monitors[2]);
    g_monitors[1].hidden = 1;
    NoteManualChange(&g_monitors[1]);
    assert(!g_config.schedule.pausedUntil && saves == 2);
    g_monitors[1].hidden = 0;
    /* The pause expires at the cycle reset time for everyone. */
    NoteManualChange(&g_monitors[0]);
    assert(g_config.schedule.pausedUntil == 9000 && saves == 3);
    now = 9000;
    target = 55;
    EvaluateSchedule();
    assert(!g_config.schedule.pausedUntil && saves == 4);
    assert(g_monitors[0].value == 55 && g_monitors[1].value == 55 && g_monitors[2].value == 50);
    /* Nothing is applied through Remote Desktop. */
    g_remoteSession = TRUE;
    target = 70;
    EvaluateSchedule();
    assert(g_monitors[0].value == 55 && applied == 6);
    g_remoteSession = FALSE;
    EvaluateSchedule();
    assert(g_monitors[0].value == 70 && applied == 8);
    /* A disabled schedule is never paused, whatever the stored deadline says. */
    g_config.schedule.pausedUntil = 99999;
    g_config.schedule.enabled = FALSE;
    assert(!IsSchedulePaused(now));
    NoteManualChange(&g_monitors[0]);
    assert(saves == 4);
}
''')

    def test_tray_menu_offers_resume_above_configure_only_while_paused(self):
        run_c(r'''
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define MF_STRING 0
#define MF_SEPARATOR 0x800
#define MF_ENABLED 0
#define MF_GRAYED 1
#define MF_BYCOMMAND 0
#define TPM_BOTTOMALIGN 0
#define TPM_LEFTALIGN 0
#define TPM_RIGHTBUTTON 0
#define ID_TRAY_MENU_CONFIGURE 1
#define ID_TRAY_MENU_EXIT 2
#define ID_TRAY_MENU_BRIGHTER 3
#define ID_TRAY_MENU_DIMMER 4
#define ID_TRAY_MENU_RESUME_SCHEDULE 5
#define ID_TRAY_MENU_PRESET_FIRST 1000
#define TRAY_MAX_PRESETS 8
#define PostMessageW(h, m, w, l) ((void)0)
#define DestroyMenu(m) ((void)0)
#define swprintf_s(...) ((void)0)
typedef int BOOL;
typedef unsigned UINT;
typedef unsigned long long ULONGLONG;
typedef void *HWND, *HMENU;
typedef struct { long x, y; } POINT;
typedef struct { wchar_t name[128]; } Monitor;
struct { wchar_t trayTarget[128]; int trayPresets[TRAY_MAX_PRESETS]; int trayPresetCount; } g_config;
int paused, items, defaults;
UINT ids[16], greyed[16];
BOOL g_remoteSession;
void GetCursorPos(POINT* pt) { pt->x = pt->y = 0; }
HMENU CreatePopupMenu(void) { return (HMENU)1; }
BOOL AppendMenuW(HMENU menu, UINT flags, UINT id, const wchar_t* text) {
    (void)menu;
    greyed[items] = flags & MF_GRAYED;
    if (flags & MF_SEPARATOR) { ids[items++] = 0; return TRUE; }
    if (id == ID_TRAY_MENU_RESUME_SCHEDULE) assert(wcscmp(text, L"Resume schedule") == 0);
    if (id == ID_TRAY_MENU_CONFIGURE) assert(wcscmp(text, L"Configure") == 0);
    ids[items++] = id;
    return TRUE;
}
BOOL CheckMenuRadioItem(HMENU m, UINT a, UINT b, UINT c, UINT d) {
    (void)m; (void)a; (void)b; (void)c; (void)d; assert(!"no bullet on the presets"); return TRUE;
}
BOOL SetMenuDefaultItem(HMENU m, UINT id, UINT byPos) { (void)m; (void)id; (void)byPos; defaults++; return TRUE; }
BOOL SetForegroundWindow(HWND h) { (void)h; return TRUE; }
BOOL TrackPopupMenu(HMENU m, UINT f, int x, int y, int r, HWND h, const void* rc) {
    (void)m; (void)f; (void)x; (void)y; (void)r; (void)h; (void)rc; return TRUE;
}
Monitor* TrayTargetMonitor(BOOL* allVisible) { *allVisible = TRUE; return NULL; }
int VisibleMonitorCount(void) { return 1; }
BOOL IsSchedulePaused(ULONGLONG now) { (void)now; return paused; }
ULONGLONG NowFileTime(void) { return 0; }
void wcscpy_s(wchar_t* out, size_t count, const wchar_t* in) { (void)count; wcscpy(out, in); }
''' + function("ShowContextMenu") + r'''
int main(void) {
    ShowContextMenu((HWND)1);
    assert(items == 3 && ids[0] == ID_TRAY_MENU_CONFIGURE && ids[1] == 0 && ids[2] == ID_TRAY_MENU_EXIT);
    /* Configure is no longer the bold default item. */
    assert(defaults == 0);
    paused = 1;
    items = 0;
    ShowContextMenu((HWND)1);
    assert(items == 4 && ids[0] == ID_TRAY_MENU_RESUME_SCHEDULE && ids[1] == ID_TRAY_MENU_CONFIGURE);
    assert(ids[2] == 0 && ids[3] == ID_TRAY_MENU_EXIT && defaults == 0);
    /* With brightness items above, Resume stays in Configure's group; the
     * presets are plain items between the two steps, without a bullet. */
    wcscpy(g_config.trayTarget, L"*");
    g_config.trayPresets[0] = 100;
    g_config.trayPresets[1] = 50;
    g_config.trayPresetCount = 2;
    items = 0;
    ShowContextMenu((HWND)1);
    assert(items == 9 && ids[0] == ID_TRAY_MENU_BRIGHTER && ids[1] == ID_TRAY_MENU_PRESET_FIRST);
    assert(ids[2] == ID_TRAY_MENU_PRESET_FIRST + 1 && ids[3] == ID_TRAY_MENU_DIMMER && ids[4] == 0);
    assert(ids[5] == ID_TRAY_MENU_RESUME_SCHEDULE && ids[6] == ID_TRAY_MENU_CONFIGURE && ids[7] == 0 && ids[8] == ID_TRAY_MENU_EXIT);
    assert(!greyed[0] && !greyed[1] && !greyed[3]);
    /* Through Remote Desktop the brightness items stay but are greyed out. */
    g_remoteSession = 1;
    items = 0;
    ShowContextMenu((HWND)1);
    assert(items == 9 && greyed[0] && greyed[1] && greyed[2] && greyed[3] && !greyed[5] && !greyed[6]);
}
''')

    def test_remote_session_pauses_refreshes_and_hides_overlays(self):
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define MAX_MONITORS 16
#define MAX_PHYSICAL_PER_DISPLAY 8
#define CCHDEVICENAME 32
#define HW_UNKNOWN 0
#define MONITORINFOF_PRIMARY 1
#define SW_HIDE 0
#define ID_TIMER_REFRESH_MONITORS 2
#define ID_TIMER_DDC_RETRY 5
#define DDC_RETRY_INITIAL_MS 3000
#define REFRESH_MONITORS_RESUME_DELAY_MS 3000
#define DebugPrint(...) ((void)0)
#define ZeroMemory(p,n) memset(p,0,n)
#define swprintf_s swprintf
#define EnumMonitorProc NULL
typedef int BOOL;
typedef unsigned UINT;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef void *HMONITOR, *HWND;
typedef intptr_t LPARAM;
typedef int HardwareState;
typedef unsigned char BYTE;
#define PANEL_MAX_LEVELS 101
typedef struct { long left, top, right, bottom; } RECT;
typedef struct { RECT rcMonitor; DWORD dwFlags; wchar_t szDevice[32]; } MONITORINFOEXW;
''' + structure("Monitor") + structure("DdcProbeEntry") + structure("EnumEntry") + structure("EnumContext") + r'''
Monitor g_monitors[MAX_MONITORS], saved;
int g_monitorCount, g_nextUid = 20, connected = 1, remote = 0, hidden = 0, probes = -1;
int killed[8], scheduledRefreshes = 0, tooltipUpdates = 0, dialogPushes = 0, overlayUpdates = 0;
BOOL g_remoteSession, g_ddcRetryPending = TRUE;
UINT g_ddcRetryDelayMs = 60000;
HWND g_hwnd = (HWND)1;
struct { BOOL pauseInRemoteSession; } g_config = { TRUE };
BOOL IsRemoteSession(void) { return remote; }
void SaveMonitorSettings(const Monitor* m) { saved = *m; }
void LoadMonitorSettings(Monitor* m) { m->value = 25; m->hasValue = TRUE; }
void LogDisplayDevices(void) {}
void EnumDisplayMonitors(void* a,void* b,void* c,LPARAM data) {
    (void)a; (void)b; (void)c;
    EnumContext* ctx = (EnumContext*)data;
    ctx->count = connected;
    ctx->entries[0].hmon = (void*)1;
    wcscpy(ctx->entries[0].info.szDevice, connected == 1 ? L"DISPLAY" : L"RDP");
}
BOOL GetNumberOfPhysicalMonitorsFromHMONITOR(HMONITOR h,DWORD* count) { (void)h; *count=1; return TRUE; }
void wcscpy_s(wchar_t* out,size_t count,const wchar_t* in) { (void)count; wcscpy(out,in); }
void ResolveMonitorIdentity(const wchar_t* device,int index,Monitor* m) { (void)index; wcscpy(m->key,device); }
void SanitizeKeyChars(wchar_t* key) { (void)key; }
void DestroyWindow(HWND hwnd) { (void)hwnd; }
HWND CreateOverlayWindow(const RECT* rect) { (void)rect; return (void*)2; }
void PositionOverlay(Monitor* m) { (void)m; }
void SetOverlayDim(Monitor* m,int dim) { (void)m; (void)dim; }
void ReleaseMonitorOverlay(Monitor* m) { m->overlay=NULL; }
void UpdateOverlayTimer(void) { overlayUpdates++; }
Monitor* FindMonitorByUid(int uid) {
    for (int i=0;i<g_monitorCount;i++) if(g_monitors[i].uid==uid)return &g_monitors[i];
    return NULL;
}
void DdcRequestProbe(const DdcProbeEntry* entries,int count) { (void)entries; probes = count; }
void PushMonitorsToDialog(void) {}
void PushRemoteSessionToDialog(void) { dialogPushes++; }
void ScheduleTooltipUpdate(void) { tooltipUpdates++; }
void ScheduleMonitorRefresh(UINT delayMs) { (void)delayMs; scheduledRefreshes++; }
BOOL KillTimer(HWND hwnd, UINT id) { (void)hwnd; killed[id]++; return TRUE; }
BOOL IsWindowVisible(HWND hwnd) { return hwnd != NULL; }
BOOL ShowWindow(HWND hwnd, int cmd) { (void)hwnd; if (cmd == SW_HIDE) hidden++; return TRUE; }
''' + function("PersistDirtyMonitors") + function("UpdateRemoteSessionState") + function("RefreshMonitors") + r'''
int main(void) {
    RefreshMonitors();
    assert(g_monitorCount == 1 && wcscmp(g_monitors[0].key, L"DISPLAY") == 0 && probes == 1);
    assert(overlayUpdates == 1);
    g_monitors[0].overlayDim = 40;
    g_monitors[0].value = 60;
    g_monitors[0].dirty = TRUE;
    /* Remote Desktop takes over: the list is kept, the overlay hidden, the
     * worker released, pending state saved, and retries stopped. */
    remote = 1;
    connected = 2;
    RefreshMonitors();
    assert(g_remoteSession && g_monitorCount == 1 && wcscmp(g_monitors[0].key, L"DISPLAY") == 0);
    assert(probes == 0 && hidden == 1 && g_monitors[0].overlayDim == 40 && g_monitors[0].overlay);
    assert(saved.value == 60 && !g_ddcRetryPending && killed[ID_TIMER_REFRESH_MONITORS] == 1 && killed[ID_TIMER_DDC_RETRY] == 1);
    assert(dialogPushes == 1 && tooltipUpdates == 1 && overlayUpdates == 2);
    /* Further refreshes and state checks stay quiet while remote. */
    UpdateRemoteSessionState();
    RefreshMonitors();
    assert(probes == 0 && hidden == 1 && dialogPushes == 1 && scheduledRefreshes == 0 && overlayUpdates == 2);
    /* Back at the console: a refresh is scheduled and retries start afresh. */
    remote = 0;
    connected = 1;
    UpdateRemoteSessionState();
    assert(!g_remoteSession && scheduledRefreshes == 1 && g_ddcRetryDelayMs == DDC_RETRY_INITIAL_MS && dialogPushes == 2);
    RefreshMonitors();
    assert(probes == 1 && g_monitorCount == 1 && g_monitors[0].value == 60 && g_monitors[0].overlayDim == 40);
    assert(overlayUpdates == 4);
    /* With the option off, a remote session changes nothing. */
    g_config.pauseInRemoteSession = FALSE;
    remote = 1;
    UpdateRemoteSessionState();
    assert(!g_remoteSession && hidden == 1);
}
''')

    def test_brightness_keys_step_every_monitor_once_per_press(self):
        run_c(r'''
#include <assert.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define KEY_REPEAT_MIN_MS 250
#define TRAY_STEP_PERCENT 10
#define MODE_PROBING 1
#define MODE_HARDWARE 2
#define DebugPrint(...) ((void)0)
typedef int BOOL;
typedef unsigned long long ULONGLONG;
typedef struct { int hidden, value, mode, builtin; } Monitor;
struct { BOOL brightnessKeys; } g_config = { TRUE };
BOOL g_remoteSession;
Monitor g_monitors[4];
int g_monitorCount = 4, manual, pushes;
int MonitorMode(const Monitor* m) { return m->mode; }
void SetMonitorValue(Monitor* m, int value) { m->value = value < 0 ? 0 : value > 100 ? 100 : value; }
void NoteManualChange(Monitor* m) { (void)m; manual++; }
void PushMonitorsToDialog(void) { pushes++; }
''' + function("BrightnessKeyPressed") + function("ApplyBrightnessKey") + r'''
int main(void) {
    /* Press and release: one step. Held with repeating reports: a step
     * every KEY_REPEAT_MIN_MS. Never released: each new report counts once
     * the interval has passed, so a keyboard without release reports works. */
    BOOL down = FALSE;
    ULONGLONG last = 0;
    assert(BrightnessKeyPressed(TRUE, &down, &last, 1000) && last == 1000);
    assert(!BrightnessKeyPressed(TRUE, &down, &last, 1100));
    assert(!BrightnessKeyPressed(FALSE, &down, &last, 1150) && !down);
    assert(BrightnessKeyPressed(TRUE, &down, &last, 1160));
    assert(!BrightnessKeyPressed(TRUE, &down, &last, 1300));
    assert(BrightnessKeyPressed(TRUE, &down, &last, 1410) && last == 1410);
    assert(!BrightnessKeyPressed(FALSE, &down, &last, 1420));
    assert(!BrightnessKeyPressed(FALSE, &down, &last, 9999));

    g_monitors[0] = (Monitor){0, 50, MODE_HARDWARE};
    g_monitors[1] = (Monitor){0, 95, MODE_HARDWARE};
    g_monitors[2] = (Monitor){1, 50, MODE_HARDWARE};   /* hidden */
    /* A built-in display: Windows moves it for the same key press. */
    g_monitors[3] = (Monitor){0, 40, MODE_HARDWARE, 1};
    ApplyBrightnessKey(+1);
    assert(g_monitors[0].value == 60 && g_monitors[1].value == 100 && g_monitors[2].value == 50);
    assert(g_monitors[3].value == 40 && manual == 2 && pushes == 1);
    /* Clamped at the top: nothing changes, nothing pushed, still a manual change. */
    g_monitors[0].value = 100;
    ApplyBrightnessKey(+1);
    assert(g_monitors[0].value == 100 && manual == 4 && pushes == 1);
    ApplyBrightnessKey(-1);
    assert(g_monitors[0].value == 90 && g_monitors[1].value == 90 && pushes == 2);
    /* Probing monitors are skipped; remote sessions and a disabled option ignore the key. */
    g_monitors[1].mode = MODE_PROBING;
    ApplyBrightnessKey(-1);
    assert(g_monitors[0].value == 80 && g_monitors[1].value == 90);
    g_remoteSession = TRUE;
    ApplyBrightnessKey(-1);
    assert(g_monitors[0].value == 80);
    g_remoteSession = FALSE;
    g_config.brightnessKeys = FALSE;
    ApplyBrightnessKey(-1);
    assert(g_monitors[0].value == 80 && pushes == 3 && g_monitors[3].value == 40);
}
''')

    def test_c_curve_matches_preview_across_dates_and_ranges(self):
        cases = json.loads(subprocess.check_output(["node", "-e", r'''
const { loadTs } = require('./tests/load_ts.cjs');
const solar = loadTs('assets/src/lib/solar.ts');
const cases = [];
for (const [tz, lat, lon] of [['Atlantic/Reykjavik',64.15,-21.94],
    ['Europe/Warsaw',52.23,21.01], ['Pacific/Apia',-13.83,-171.76],
    ['Arctic/Longyearbyen',78.22,15.65]]) {
  process.env.TZ = tz;
  for (const month of [0,2,5,9]) {
    const days = solar.computeSolarDays(lat,lon,new Date(2026,month,21,12));
    for (const extended of [0,1]) for (const offsets of [[-30,30,-30,30],[-360,30,-30,360],[180,360,-360,-180]])
    for (const [deepOn, deepLevel, deepMinutes] of [[0,10,1410],[1,10,1410],[1,60,90],[1,-50,840],[1,50,360]]) {
      const shape = {dayLevel:100,nightLevel:-90,dawnStartOffset:offsets[0],
        dawnEndOffset:offsets[1],duskStartOffset:offsets[2],duskEndOffset:offsets[3],
        deepSleepEnabled:deepOn,deepSleepLevel:deepLevel,deepSleepMinutes:deepMinutes};
      const minutes = [];
      for (let minute=0; minute<=1440; minute+=7.5) minutes.push(minute);
      for (const step of [0.5,1.25,2.5,3.75,4.9]) minutes.push(deepMinutes + step);
      for (const minute of minutes) {
        // The dialog clamps the levels it passes in; the host clamps inside.
        cases.push([extended,shape,days,minute,solar.scheduleValueAt(
          {...shape,nightLevel:extended ? -90 : 0,
           deepSleepLevel:extended ? deepLevel : Math.max(deepLevel,0)},days,minute)]);
      }
    }
  }
}
console.log(JSON.stringify(cases));
'''], cwd=ROOT, text=True))
        rows = []
        for extended, shape, days, minute, expected in cases:
            row = [extended, shape['dayLevel'], shape['nightLevel'],
                   shape['dawnStartOffset'], shape['dawnEndOffset'],
                   shape['duskStartOffset'], shape['duskEndOffset'],
                   shape['deepSleepEnabled'], shape['deepSleepLevel'], shape['deepSleepMinutes']]
            for day in days:
                row += [day['sunrise'], day['sunset'], day['noon'], day['polar']]
            rows.append(' '.join(map(str, row + [minute, expected])))
        run_c(r'''
#include <assert.h>
#include <stdio.h>
#include <math.h>
#define SOFT_MAX_DIM 90
#define SCHEDULE_MIN_GAP 5
#define SCHEDULE_DAY_RADIUS 2
#define SCHEDULE_DAY_COUNT 5
#define SCHEDULE_DEEP_SLEEP_RAMP 5
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset, deepSleepEnabled, deepSleepLevel, deepSleepMinutes; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
struct { int allowBelowMinimum; } g_config;
''' + enumeration("SchedulePhase") + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleDaylightAt") + function("ScheduleDeepSleepAt") + function("ScheduleValueAt") + r'''
int main(void) {
    Schedule sc; SolarDay days[5]; double minutes; int expected, rows = 0, deep = 0;
    while (scanf("%d", &g_config.allowBelowMinimum) == 1) {
        assert(scanf("%d%d%d%d%d%d%d%d%d", &sc.dayLevel, &sc.nightLevel,
            &sc.dawnStartOffset, &sc.dawnEndOffset, &sc.duskStartOffset, &sc.duskEndOffset,
            &sc.deepSleepEnabled, &sc.deepSleepLevel, &sc.deepSleepMinutes) == 9);
        for (int i = 0; i < 5; i++)
            assert(scanf("%d%d%d%d", &days[i].sunrise, &days[i].sunset, &days[i].noon, &days[i].polar) == 4);
        assert(scanf("%lf%d", &minutes, &expected) == 2);
        assert(ScheduleValueAt(&sc, days, minutes) == expected);
        rows++;
        if (ScheduleDeepSleepAt(&sc, days, minutes) > 0) deep++;
    }
    /* Deep sleep actually took part in a good share of the comparisons. */
    assert(rows > 90000 && deep > rows / 10);
}
''', '\n'.join(rows))


    def test_refresh_persists_pause_before_removal_and_preserves_matching_records(self):
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define MAX_MONITORS 16
#define MAX_PHYSICAL_PER_DISPLAY 8
#define CCHDEVICENAME 32
#define HW_UNKNOWN 0
#define MONITORINFOF_PRIMARY 1
#define DebugPrint(...) ((void)0)
#define ZeroMemory(p,n) memset(p,0,n)
#define swprintf_s swprintf
#define EnumMonitorProc NULL
typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
typedef void *HMONITOR, *HWND;
typedef intptr_t LPARAM;
typedef int HardwareState;
typedef unsigned char BYTE;
#define PANEL_MAX_LEVELS 101
typedef struct { long left, top, right, bottom; } RECT;
typedef struct { RECT rcMonitor; DWORD dwFlags; wchar_t szDevice[32]; } MONITORINFOEXW;
''' + structure("Monitor") + structure("DdcProbeEntry") + structure("EnumEntry") + structure("EnumContext") + r'''
Monitor g_monitors[MAX_MONITORS], saved;
int g_monitorCount, g_nextUid = 20, connected = 0, saves = 0;
void SaveMonitorSettings(const Monitor* m) { saved = *m; saves++; }
void LoadMonitorSettings(Monitor* m) {
    m->scheduled = saved.scheduled;
    m->value = saved.value; m->hasValue = saved.hasValue;
}
void LogDisplayDevices(void) {}
void EnumDisplayMonitors(void* a,void* b,void* c,LPARAM data) {
    (void)a; (void)b; (void)c;
    EnumContext* ctx = (EnumContext*)data;
    ctx->count = connected;
    ctx->entries[0].hmon = (void*)1;
    wcscpy(ctx->entries[0].info.szDevice,L"DISPLAY");
}
BOOL GetNumberOfPhysicalMonitorsFromHMONITOR(HMONITOR h,DWORD* count) {
    (void)h; *count=1; return TRUE;
}
void wcscpy_s(wchar_t* out,size_t count,const wchar_t* in) { (void)count; wcscpy(out,in); }
wchar_t instance[160] = L"DISPLAY\\ABC1234\\4&1&0&UID1";
DdcProbeEntry lastProbe[MAX_MONITORS];
int lastProbeCount;
void ResolveMonitorIdentity(const wchar_t* device,int index,Monitor* m) {
    (void)device; (void)index; wcscpy(m->key,L"monitor"); wcscpy(m->instancePath, instance);
}
void SanitizeKeyChars(wchar_t* key) { (void)key; }
void DestroyWindow(HWND hwnd) { (void)hwnd; }
HWND CreateOverlayWindow(const RECT* rect) { (void)rect; return (void*)2; }
void PositionOverlay(Monitor* m) { (void)m; }
void SetOverlayDim(Monitor* m,int dim) { (void)m; (void)dim; }
void ReleaseMonitorOverlay(Monitor* m) { m->overlay=NULL; }
void UpdateOverlayTimer(void) {}
Monitor* FindMonitorByUid(int uid) {
    for (int i=0;i<g_monitorCount;i++) if(g_monitors[i].uid==uid)return &g_monitors[i];
    return NULL;
}
void DdcRequestProbe(const DdcProbeEntry* entries,int count) {
    memcpy(lastProbe, entries, sizeof(DdcProbeEntry) * (size_t)count); lastProbeCount = count;
}
void PushMonitorsToDialog(void) {}
void UpdateRemoteSessionState(void) {}
BOOL g_remoteSession;
''' + function("PersistDirtyMonitors") + function("RefreshMonitors") + r'''
int main(void) {
    g_monitorCount = 1;
    g_monitors[0] = (Monitor){.uid=1,.dirty=TRUE,.scheduled=TRUE,.value=25,.hasValue=TRUE};
    wcscpy(g_monitors[0].key,L"monitor");
    RefreshMonitors(); /* Removed before the delayed persistence timer. */
    assert(g_monitorCount==0 && saves==1 && saved.scheduled && saved.value==25);
    connected=1;
    RefreshMonitors();
    assert(g_monitorCount==1 && g_monitors[0].scheduled && g_monitors[0].value==25);
    int uid=g_monitors[0].uid;
    g_monitors[0].value=40;
    g_monitors[0].dirty=TRUE;
    RefreshMonitors(); /* Existing whole-record merge must still survive. */
    assert(saves==2 && g_monitors[0].uid==uid);
    assert(g_monitors[0].value==40 && g_monitors[0].scheduled);
    /* Windows is asked about a display until it has answered for it... */
    assert(lastProbeCount==1 && lastProbe[0].checkPanel && !lastProbe[0].builtin);
    assert(wcscmp(lastProbe[0].instancePath, instance)==0);
    g_monitors[0].panelChecked=TRUE;
    RefreshMonitors();
    assert(!lastProbe[0].checkPanel && g_monitors[0].panelChecked);
    /* ...and every time for a built-in display, whose probe state and
     * levels survive while its device path follows the enumeration. */
    g_monitors[0].builtin=TRUE;
    g_monitors[0].panelLevelCount=11;
    wcscpy(instance, L"DISPLAY\\ABC1234\\4&1&0&UID2");
    RefreshMonitors();
    assert(lastProbe[0].checkPanel && lastProbe[0].builtin && g_monitors[0].builtin);
    assert(g_monitors[0].panelLevelCount==11 && wcscmp(g_monitors[0].instancePath, instance)==0);
    assert(wcscmp(lastProbe[0].instancePath, instance)==0);
}
''')


if __name__ == "__main__":
    unittest.main()

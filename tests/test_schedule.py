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
typedef int BOOL;
typedef unsigned long DWORD;
typedef struct { int hidden, failures, lastHwSent, hardwareState;
    DWORD ddcMax, ddcMin, ddcCurrent; wchar_t error[200]; } Monitor;
Monitor monitor;
int retries;
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
    monitor.hidden = 1;
    HandleDdcSetResult(1, 0);
    assert(retries == 3);
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
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
struct { int allowBelowMinimum; } g_config;
''' + enumeration("SchedulePhase") + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleDaylightAt") + function("ScheduleValueAt") + r'''
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
}
''')


    def test_schedule_phase_follows_the_curve(self):
        run_c(r'''
#include <assert.h>
#include <math.h>
#define SCHEDULE_MIN_GAP 5
#define SCHEDULE_DAY_RADIUS 2
#define SCHEDULE_DAY_COUNT 5
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
''' + enumeration("SchedulePhase") + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleDaylightAt") + r'''
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
    currentPhase = SCHEDULE_PHASE_NIGHT;
    g_monitorCount = 0;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"State: Night\nSchedule: Active") == 0);
    /* Unchanged text is not sent to the shell again. */
    UpdateTrayTooltip();
    assert(modifies == 5);
    g_config.schedule.enabled = 0;
    UpdateTrayTooltip();
    assert(wcscmp(g_nid.szTip, L"Schedule: Disabled") == 0);
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
double ScheduleDaylightAt(const Schedule* sc, const SolarDay days[SCHEDULE_DAY_COUNT], double minutes, SchedulePhase* phase) {
    (void)sc; (void)days; (void)minutes; *phase = SCHEDULE_PHASE_DAY; return 1;
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
    /* A disabled schedule is never paused, whatever the stored deadline says. */
    g_config.schedule.pausedUntil = 99999;
    g_config.schedule.enabled = FALSE;
    assert(!IsSchedulePaused(now));
    NoteManualChange(&g_monitors[0]);
    assert(saves == 4);
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
    for (const extended of [0,1]) for (const offsets of [[-30,30,-30,30],[-360,30,-30,360],[180,360,-360,-180]]) {
      const shape = {dayLevel:100,nightLevel:-90,dawnStartOffset:offsets[0],
        dawnEndOffset:offsets[1],duskStartOffset:offsets[2],duskEndOffset:offsets[3]};
      for (let minute=0; minute<=1440; minute+=7.5) {
        cases.push([extended,shape,days,minute,solar.scheduleValueAt(
          {...shape,nightLevel:extended ? -90 : 0},days,minute)]);
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
                   shape['duskStartOffset'], shape['duskEndOffset']]
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
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
struct { int allowBelowMinimum; } g_config;
''' + enumeration("SchedulePhase") + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleDaylightAt") + function("ScheduleValueAt") + r'''
int main(void) {
    Schedule sc; SolarDay days[5]; double minutes; int expected;
    while (scanf("%d", &g_config.allowBelowMinimum) == 1) {
        assert(scanf("%d%d%d%d%d%d", &sc.dayLevel, &sc.nightLevel,
            &sc.dawnStartOffset, &sc.dawnEndOffset, &sc.duskStartOffset, &sc.duskEndOffset) == 6);
        for (int i = 0; i < 5; i++)
            assert(scanf("%d%d%d%d", &days[i].sunrise, &days[i].sunset, &days[i].noon, &days[i].polar) == 4);
        assert(scanf("%lf%d", &minutes, &expected) == 2);
        assert(ScheduleValueAt(&sc, days, minutes) == expected);
    }
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
void ResolveMonitorIdentity(const wchar_t* device,int index,Monitor* m) {
    (void)device; (void)index; wcscpy(m->key,L"monitor");
}
void SanitizeKeyChars(wchar_t* key) { (void)key; }
void DestroyWindow(HWND hwnd) { (void)hwnd; }
HWND CreateOverlayWindow(const RECT* rect) { (void)rect; return (void*)2; }
void PositionOverlay(Monitor* m) { (void)m; }
void SetOverlayDim(Monitor* m,int dim) { (void)m; (void)dim; }
void ReleaseMonitorOverlay(Monitor* m) { m->overlay=NULL; }
Monitor* FindMonitorByUid(int uid) {
    for (int i=0;i<g_monitorCount;i++) if(g_monitors[i].uid==uid)return &g_monitors[i];
    return NULL;
}
void DdcRequestProbe(const DdcProbeEntry* entries,int count) { (void)entries; (void)count; }
void PushMonitorsToDialog(void) {}
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
}
''')


if __name__ == "__main__":
    unittest.main()

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
''' + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleValueAt") + r'''
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
''' + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleValueAt") + r'''
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
    m->pausedUntil = saved.pausedUntil; m->scheduled = saved.scheduled;
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
    g_monitors[0] = (Monitor){.uid=1,.dirty=TRUE,.scheduled=TRUE,
        .pausedUntil=123456789,.value=25,.hasValue=TRUE};
    wcscpy(g_monitors[0].key,L"monitor");
    RefreshMonitors(); /* Removed before the delayed persistence timer. */
    assert(g_monitorCount==0 && saves==1 && saved.pausedUntil==123456789);
    connected=1;
    RefreshMonitors();
    assert(g_monitorCount==1 && g_monitors[0].scheduled);
    assert(g_monitors[0].pausedUntil==123456789 && g_monitors[0].value==25);
    int uid=g_monitors[0].uid;
    g_monitors[0].pausedUntil=987654321;
    g_monitors[0].dirty=TRUE;
    RefreshMonitors(); /* Existing whole-record merge must still survive. */
    assert(saves==2 && g_monitors[0].uid==uid);
    assert(g_monitors[0].pausedUntil==987654321 && g_monitors[0].scheduled);
}
''')


if __name__ == "__main__":
    unittest.main()

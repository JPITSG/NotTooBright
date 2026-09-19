"""Portable regression checks against functions extracted from the C source.

Run with python3 -m unittest discover -s tests. Requires a native C compiler.
Windows I/O is stubbed; real DDC/CI still requires Windows testing.
"""
import os
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


def run_c(code):
    with tempfile.TemporaryDirectory(prefix="ntb-test-") as directory:
        binary = str(Path(directory) / "test")
        subprocess.run([os.environ.get("CC", "cc"), "-x", "c", "-", "-o", binary, "-lm"],
                       input=code, text=True, check=True)
        subprocess.run([binary], check=True)


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
typedef struct { int dayLevel, nightLevel, dawnStartOffset, dawnEndOffset,
    duskStartOffset, duskEndOffset; } Schedule;
typedef struct { int sunrise, sunset, noon, polar; } SolarDay;
struct { int allowBelowMinimum; } g_config;
''' + function("ScheduleAnchors") + function("SmoothStep") + function("ScheduleValueAt") + r'''
int main(void) {
    Schedule sc = {100, -90, -30, 30, -30, 30};
    SolarDay day = {360, 1080, 720, 0};
    assert(ScheduleValueAt(&sc, &day, 360) == 50);
    assert(ScheduleValueAt(&sc, &day, 1080) == 50);
    assert(ScheduleValueAt(&sc, &day, 0) == 0);
    assert(sc.nightLevel == -90);
    g_config.allowBelowMinimum = 1;
    assert(ScheduleValueAt(&sc, &day, 360) == 5);
    assert(ScheduleValueAt(&sc, &day, 0) == -90);
    day.polar = -1;
    assert(ScheduleValueAt(&sc, &day, 0) == -90);
    g_config.allowBelowMinimum = 0;
    assert(ScheduleValueAt(&sc, &day, 0) == 0);
}
''')


if __name__ == "__main__":
    unittest.main()

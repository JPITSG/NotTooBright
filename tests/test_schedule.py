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


if __name__ == "__main__":
    unittest.main()

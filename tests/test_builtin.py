"""Built-in display (Windows brightness control over WMI) regressions.

The WMI calls themselves need Windows; these tests run the production logic
around them - matching, level snapping, the worker's job split, probe
results, and how reported levels are told apart - with Windows stubbed.
"""
import unittest

from test_idle import defines
from test_schedule import enumeration, function, run_c, structure


PRELUDE = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define CCHDEVICENAME 32
#define USER_TIMER_MINIMUM 10
#define DebugPrint(...) ((void)0)
#define ZeroMemory(p,n) memset(p,0,n)
#define _wcsnicmp wcsncasecmp
typedef int BOOL;
typedef unsigned UINT;
typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef void *HMONITOR, *HWND;
typedef struct { long left, top, right, bottom; } RECT;
void wcscpy_s(wchar_t* out, size_t count, const wchar_t* in) { (void)count; wcscpy(out, in); }
''' + defines("PANEL_MAX_LEVELS", "PANEL_NAME_CHARS", "PANEL_PATH_CHARS", "PANEL_SETTLE_MS",
              "PANEL_ECHO_MS", "PANEL_QUIET_MS", "ID_TIMER_PANEL", "MAX_MONITORS",
              "SOFT_MIN_BRIGHTNESS", "SOFT_MAX_DIM")

PANEL = r'L"DISPLAY\\SDC4161\\4&1a2b3c4d&0&UID265988"'


class BuiltinDisplayTests(unittest.TestCase):
    def test_wmi_names_match_the_device_and_levels_snap_to_supported_steps(self):
        run_c(PRELUDE + function("PanelInstanceMatches") + function("SnapPanelLevel") + r'''
int main(void) {
    const wchar_t* path = ''' + PANEL + r''';
    assert(PanelInstanceMatches(L"DISPLAY\\SDC4161\\4&1a2b3c4d&0&UID265988_0", path));
    /* Device instance ids are case-insensitive. */
    assert(PanelInstanceMatches(L"display\\sdc4161\\4&1A2B3C4D&0&uid265988_0", path));
    /* A longer id sharing the prefix is another display. */
    assert(!PanelInstanceMatches(L"DISPLAY\\SDC4161\\4&1a2b3c4d&0&UID2659881_0", path));
    assert(!PanelInstanceMatches(L"DISPLAY\\SDC4161\\4&1a2b3c4d&0&UID265988", path));
    /* A monitor without a known device id never matches. */
    assert(!PanelInstanceMatches(L"_0", L""));

    BYTE coarse[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    assert(SnapPanelLevel(coarse, 11, 43) == 40);
    assert(SnapPanelLevel(coarse, 11, 46) == 50);
    assert(SnapPanelLevel(coarse, 11, 45) == 40);   /* a tie: the level listed first */
    assert(SnapPanelLevel(coarse, 11, 100) == 100 && SnapPanelLevel(coarse, 11, 0) == 0);
    BYTE unsorted[] = {100, 5, 50};
    assert(SnapPanelLevel(unsorted, 3, 0) == 5 && SnapPanelLevel(unsorted, 3, 80) == 100);
    /* Without the list the value is used as is. */
    assert(SnapPanelLevel(NULL, 0, 37) == 37);
}
''')

    def test_worker_probes_built_in_displays_first_and_never_hands_them_to_ddc(self):
        run_c(PRELUDE + r'''
#define WM_APP_DDC_PROBED 0x8002
#define ERROR_GEN_FAILURE 31
#define ERROR_NOT_FOUND 1168
typedef void IWbemServices;
typedef void IWbemClassObject;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
''' + structure("DdcProbeEntry") + structure("DdcProbeResult") + structure("PanelInfo") +
              structure("PanelWmi") + structure("WorkerMonitor") + r'''
HWND g_hwnd = (HWND)1;
PanelInfo listed[2];
int listedCount, queries;
DdcProbeResult* posted[8];
int postedCount;
ULONGLONG GetTickCount64(void) { return 5000; }
int QueryPanels(PanelWmi* w, PanelInfo* out, int max) {
    (void)w; (void)max; queries++;
    if (listedCount > 0) memcpy(out, listed, sizeof(PanelInfo) * (size_t)listedCount);
    return listedCount;
}
BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam; assert(hwnd == g_hwnd && msg == WM_APP_DDC_PROBED);
    posted[postedCount++] = (DdcProbeResult*)lParam; return TRUE;
}
''' + function("PanelInstanceMatches") + function("ProbePanels") + r'''
static DdcProbeEntry Entry(int uid, BOOL checkPanel, BOOL builtin, const wchar_t* path) {
    DdcProbeEntry e;
    memset(&e, 0, sizeof(e));
    e.uid = uid; e.checkPanel = checkPanel; e.builtin = builtin;
    wcscpy(e.instancePath, path);
    return e;
}
static void Reset(void) {
    for (int i = 0; i < postedCount; i++) free(posted[i]);
    postedCount = 0; queries = 0;
}
int main(void) {
    PanelWmi wmi;
    memset(&wmi, 0, sizeof(wmi));
    WorkerMonitor monitors[MAX_MONITORS];
    DdcProbeEntry ddcJob[MAX_MONITORS];
    BOOL checked[MAX_MONITORS];
    int monitorCount = 0;

    /* A laptop: its panel is listed by WMI, the external monitor is not. */
    wcscpy(listed[0].instanceName, L"DISPLAY\\SDC4161\\4&1a2b3c4d&0&UID265988_0");
    wcscpy(listed[0].methodPath, L"WmiMonitorBrightnessMethods.InstanceName=\"x\"");
    listed[0].current = 70;
    listed[0].levelCount = 3;
    listed[0].levels[0] = 0; listed[0].levels[1] = 50; listed[0].levels[2] = 100;
    listedCount = 1;
    DdcProbeEntry job[3] = {
        Entry(1, TRUE, FALSE, ''' + PANEL + r'''),
        Entry(2, TRUE, FALSE, L"DISPLAY\\DELA0B1\\5&1&0&UID4353"),
        Entry(3, FALSE, FALSE, L"DISPLAY\\GSM5B09\\5&1&0&UID4354"),   /* already asked */
    };
    int ddcCount = ProbePanels(&wmi, job, 3, monitors, &monitorCount, ddcJob, checked);
    assert(queries == 1 && postedCount == 1 && monitorCount == 1);
    DdcProbeResult* r = posted[0];
    assert(r->uid == 1 && r->supported && r->builtin && r->current == 70 && r->max == 100);
    assert(r->levelCount == 3 && r->levels[1] == 50);
    assert(monitors[0].uid == 1 && monitors[0].builtin && monitors[0].supported);
    assert(wcscmp(monitors[0].methodPath, listed[0].methodPath) == 0);
    /* The rest go to DDC/CI; WMI has answered for the one it was asked about. */
    assert(ddcCount == 2 && ddcJob[0].uid == 2 && checked[0] && ddcJob[1].uid == 3 && !checked[1]);

    /* WMI fails: a display that was built-in is reported as not answering
     * (retried like any known monitor) instead of being probed over DDC/CI;
     * the others are probed as before and asked about again next time. */
    Reset(); monitorCount = 0; listedCount = -1;
    job[0].builtin = TRUE;
    ddcCount = ProbePanels(&wmi, job, 3, monitors, &monitorCount, ddcJob, checked);
    assert(postedCount == 1 && posted[0]->uid == 1 && posted[0]->builtin && !posted[0]->supported);
    assert(posted[0]->error == ERROR_GEN_FAILURE && monitorCount == 1 && !monitors[0].supported);
    assert(ddcCount == 2 && ddcJob[0].uid == 2 && !checked[0] && !checked[1]);

    /* It is no longer listed at all: still not handed to DDC/CI. */
    Reset(); monitorCount = 0; listedCount = 0;
    ddcCount = ProbePanels(&wmi, job, 3, monitors, &monitorCount, ddcJob, checked);
    assert(postedCount == 1 && posted[0]->error == ERROR_NOT_FOUND && ddcCount == 2 && checked[0]);

    /* A desktop: "not supported" is a definite answer for every display. */
    Reset(); monitorCount = 0; listedCount = 0;
    ddcCount = ProbePanels(&wmi, &job[1], 1, monitors, &monitorCount, ddcJob, checked);
    assert(queries == 1 && postedCount == 0 && ddcCount == 1 && checked[0]);

    /* Once every display has been asked about, WMI is not queried again. */
    Reset(); monitorCount = 0;
    ddcCount = ProbePanels(&wmi, &job[2], 1, monitors, &monitorCount, ddcJob, checked);
    assert(queries == 0 && ddcCount == 1 && !checked[0]);
    /* Without COM on the worker nothing is asked either. */
    ddcCount = ProbePanels(NULL, job, 2, monitors, &monitorCount, ddcJob, checked);
    assert(queries == 0 && ddcCount == 1 && ddcJob[0].uid == 2 && !checked[0]);
    assert(postedCount == 1 && posted[0]->uid == 1 && !posted[0]->supported);
    Reset();
}
''')

    def test_probe_result_makes_a_display_built_in_and_starts_watching_it(self):
        run_c(PRELUDE + enumeration("HardwareState") + enumeration("BrightnessMode") +
              structure("Monitor") + structure("DdcProbeResult") + r'''
#define DDC_RETRY_INITIAL_MS 3000
#define DDC_UNKNOWN_MONITOR_RETRIES 3
Monitor g_monitors[1];
BOOL g_remoteSession;
UINT g_ddcRetryDelayMs = 3000;
int applied, watch, retries, saves, persists, pushes;
Monitor* FindMonitorByUid(int uid) { return uid == g_monitors[0].uid ? &g_monitors[0] : NULL; }
void ApplyMonitor(Monitor* m) { (void)m; applied++; }
void ApplyScheduleAfterProbe(Monitor* m) { (void)m; }
void SaveMonitorSettings(const Monitor* m) { (void)m; saves++; }
void SchedulePersist(void) { persists++; }
void ScheduleDdcRetry(void) { retries++; }
void PushMonitorsToDialog(void) { pushes++; }
void StartPanelWatch(void) { watch++; }
''' + function("MonitorMode") + function("SnapPanelLevel") + function("MonitorHardwareTarget") +
              function("HandleDdcProbed") + r'''
static DdcProbeResult* Result(BOOL supported, BOOL builtin, BOOL panelChecked, DWORD current) {
    DdcProbeResult* r = calloc(1, sizeof(*r));
    r->uid = 4; r->supported = supported; r->builtin = builtin; r->panelChecked = panelChecked;
    r->current = current; r->max = 100;
    r->levelCount = 3; r->levels[0] = 0; r->levels[1] = 50; r->levels[2] = 100;
    return r;
}
int main(void) {
    Monitor* m = &g_monitors[0];
    m->uid = 4;
    /* First sighting of a laptop panel: Windows controls it; its current
     * level is adopted and recorded as the original, nothing is written. */
    HandleDdcProbed(Result(TRUE, TRUE, FALSE, 50));
    assert(m->builtin && m->hardwareState == HW_AVAILABLE && m->knownHardware);
    assert(m->ddcMax == 100 && m->ddcMin == 0 && m->panelLevelCount == 3 && m->panelLevels[2] == 100);
    assert(m->hasValue && m->value == 50 && m->hasOriginal && m->originalRaw == 50 && m->originalMax == 100);
    assert(m->lastHwSent == 50 && applied == 1 && watch == 1);
    /* A stored value between two supported steps is already applied when
     * the panel sits on the nearest one. */
    m->value = 60;
    HandleDdcProbed(Result(TRUE, TRUE, FALSE, 50));
    assert(m->lastHwSent == 50 && watch == 2);
    m->value = 80;
    HandleDdcProbed(Result(TRUE, TRUE, FALSE, 50));
    assert(m->lastHwSent == -1 && watch == 3 && !m->panelReapply);
    /* A reported level still waiting to be looked at may be the user's
     * (a key press just before a rescan): the probe leaves the decision to
     * it instead of writing ours over it. */
    m->panelReportPending = TRUE;
    HandleDdcProbed(Result(TRUE, TRUE, FALSE, 50));
    assert(m->lastHwSent == 100 && m->panelReapply && watch == 4);
    m->panelReportPending = FALSE;
    m->panelReapply = FALSE;
    /* WMI stops answering: a known built-in display waits and is retried,
     * it is not dimmed in software. */
    HandleDdcProbed(Result(FALSE, TRUE, FALSE, 0));
    assert(m->builtin && MonitorMode(m) == MODE_WAITING && retries == 1 && watch == 4);
    /* An external monitor that WMI does not know is remembered as checked. */
    HandleDdcProbed(Result(TRUE, FALSE, TRUE, 30));
    assert(!m->builtin && m->panelChecked && MonitorMode(m) == MODE_HARDWARE && watch == 4);
}
''')

    def test_reported_levels_tell_the_user_apart_from_echoes_and_windows(self):
        run_c(PRELUDE + enumeration("HardwareState") + enumeration("BrightnessMode") +
              structure("Monitor") + structure("PanelBrightnessEvent") + r'''
ULONGLONG clockNow = 1000000, g_panelQuietUntil, g_panelHoldUntil;
LONG g_lastDisplayState = -1;
LONG g_lastPowerSource = -1;
BOOL g_remoteSession;
HWND g_hwnd = (HWND)1;
struct { BOOL allowBelowMinimum; } g_config;
Monitor g_monitors[2];
int g_monitorCount = 2;
int writes, lastWrite = -1, manual, pushes, persists, overlayDim = -1, timerArmed;
UINT timerDelay;
ULONGLONG GetTickCount64(void) { return clockNow; }
void DdcRequestSet(int uid, int percent) { assert(uid == 7); writes++; lastWrite = percent; }
void SchedulePersist(void) { persists++; }
void SetOverlayDim(Monitor* m, int dim) { if (m == &g_monitors[0]) overlayDim = dim; }
void ScheduleTooltipUpdate(void) {}
void NoteManualChange(Monitor* m) { assert(m == &g_monitors[0]); manual++; }
void PushMonitorsToDialog(void) { pushes++; }
UINT SetTimer(HWND hwnd, UINT id, UINT delay, void* callback) {
    (void)hwnd; (void)callback; assert(id == ID_TIMER_PANEL);
    timerArmed = 1; timerDelay = delay; return 1;
}
BOOL KillTimer(HWND hwnd, UINT id) { (void)hwnd; assert(id == ID_TIMER_PANEL); timerArmed = 0; return TRUE; }
''' + function("MonitorMode") + function("MonitorMinValue") + function("ClampMonitorValue") +
              function("SnapPanelLevel") + function("PanelInstanceMatches") +
              function("MonitorHardwareTarget") + function("DisplayIsOn") + function("ApplyMonitor") +
              function("PanelReapplyDue") + function("ProcessPanelReport") +
              function("SchedulePanelService") + function("ServicePanels") +
              function("HandlePanelBrightness") + function("NotePanelTransition") +
              function("NotePowerCondition") + r'''
static void Report(const wchar_t* instance, int level) {
    PanelBrightnessEvent* e = calloc(1, sizeof(*e));
    wcscpy(e->instanceName, instance);
    e->brightness = level;
    HandlePanelBrightness(e);
}
#define REPORT(level) Report(L"DISPLAY\\SDC4161\\4&1a2b3c4d&0&UID265988_0", level)
/* Lets time pass until nothing is waiting any more, running the timer. */
static void Settle(void) {
    for (int guard = 0; timerArmed; guard++) {
        assert(guard < 10);
        clockNow += timerDelay;
        timerArmed = 0;
        ServicePanels();
    }
}
int main(void) {
    Monitor* m = &g_monitors[0];
    m->uid = 7; m->builtin = TRUE; m->hardwareState = HW_AVAILABLE; m->knownHardware = TRUE;
    wcscpy(m->instancePath, ''' + PANEL + r''');
    static const BYTE coarse[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    memcpy(m->panelLevels, coarse, sizeof(coarse));
    m->panelLevelCount = 11;
    m->value = 43; m->hasValue = TRUE; m->lastHwSent = -1;
    Monitor* external = &g_monitors[1];   /* DDC/CI: never part of this */
    external->uid = 8; external->hardwareState = HW_AVAILABLE; external->value = 30;

    /* Our write goes out at the nearest supported step; its echo is ours. */
    ApplyMonitor(m);
    assert(writes == 1 && lastWrite == 40 && m->lastHwSent == 40);
    clockNow += 100; REPORT(40);
    assert(timerArmed && timerDelay == PANEL_SETTLE_MS);
    Settle();
    assert(m->value == 43 && manual == 0 && writes == 1 && !m->panelReapply && !timerArmed);

    /* A slider drag: a late echo of an earlier step is not a change. */
    m->value = 20; ApplyMonitor(m);
    m->value = 30; ApplyMonitor(m);
    clockNow += 50; REPORT(20);
    Settle();
    assert(m->value == 30 && manual == 0 && writes == 3);

    /* The brightness keys or the Settings slider: adopted as a manual change. */
    clockNow += 5000; REPORT(50);
    Settle();
    assert(m->value == 50 && m->lastHwSent == 50 && manual == 1 && pushes == 1 && writes == 3);
    assert(persists > 0 && m->dirty);

    /* Unplugged: Windows applies its battery level; ours goes back once it
     * is done, and that is not a manual change. The first notification
     * only tells the current state. */
    clockNow += 10000;
    NotePowerCondition(&g_lastPowerSource, 0, L"Power source");
    assert(g_panelQuietUntil < clockNow);
    NotePowerCondition(&g_lastPowerSource, 0, L"Power source");
    assert(g_panelQuietUntil < clockNow);
    NotePowerCondition(&g_lastPowerSource, 1, L"Power source");
    assert(g_panelQuietUntil == clockNow + PANEL_QUIET_MS);
    clockNow += 200; REPORT(30);
    Settle();
    assert(m->value == 50 && manual == 1 && writes == 4 && lastWrite == 50);
    clockNow += 100; REPORT(50);   /* our write's echo */
    Settle();
    assert(writes == 4 && manual == 1 && !m->panelReapply);

    /* Dimmed for inactivity: nothing is written while it is dim, even when
     * the schedule moves on. Undimmed, Windows restores the level we wrote
     * before; then the new one goes out. */
    clockNow += 10000;
    g_lastDisplayState = 1;
    g_lastDisplayState = 2; NotePanelTransition(FALSE, PANEL_QUIET_MS);
    clockNow += 100; REPORT(25);
    Settle();
    assert(writes == 4 && m->panelReapply && !timerArmed && manual == 1 && m->value == 50);
    m->value = 60; ApplyMonitor(m);
    assert(writes == 4 && m->lastHwSent == 50);
    clockNow += 60000;
    g_lastDisplayState = 1; NotePanelTransition(FALSE, PANEL_SETTLE_MS);
    clockNow += 100; REPORT(50);
    Settle();
    assert(writes == 5 && lastWrite == 60 && manual == 1 && m->value == 60 && !m->panelReapply);

    /* A brightness key pressed to wake a dimmed display is the user's:
     * adopted, although it arrives right as the display undims. */
    clockNow += 10000;
    g_lastDisplayState = 2; NotePanelTransition(FALSE, PANEL_QUIET_MS);
    clockNow += 100; REPORT(30);
    Settle();
    clockNow += 30000;
    g_lastDisplayState = 1; NotePanelTransition(FALSE, PANEL_SETTLE_MS);
    clockNow += 50; REPORT(70);
    Settle();
    assert(m->value == 70 && manual == 2 && writes == 5 && !m->panelReapply);

    /* Switched off and on again: Windows applies a level of its own. */
    clockNow += 10000;
    g_lastDisplayState = 0; NotePanelTransition(TRUE, PANEL_QUIET_MS);
    clockNow += 100; REPORT(0);
    Settle();
    assert(writes == 5 && m->panelReapply);
    clockNow += 600000;
    g_lastDisplayState = 1; NotePanelTransition(TRUE, PANEL_QUIET_MS);
    clockNow += 300; REPORT(80);
    Settle();
    assert(writes == 6 && lastWrite == 70 && manual == 2 && m->value == 70);

    /* Software dimming below the minimum: the backlight sits at 0. */
    clockNow += 10000;
    g_config.allowBelowMinimum = TRUE;
    m->value = -20; ApplyMonitor(m);
    assert(writes == 7 && lastWrite == 0 && overlayDim == 20);
    clockNow += 100; REPORT(0);
    Settle();
    assert(m->value == -20 && manual == 2);
    clockNow += 5000; REPORT(30);   /* a key press brightens it again */
    Settle();
    assert(m->value == 30 && overlayDim == 0 && manual == 3 && writes == 7);

    /* Remote Desktop, hidden, or software-only: reports change nothing,
     * and nothing is left waiting. */
    clockNow += 10000;
    g_remoteSession = TRUE; REPORT(90);
    assert(!m->panelReportPending && !timerArmed);
    g_remoteSession = FALSE;
    m->hidden = TRUE; REPORT(90);
    assert(!m->panelReportPending && !timerArmed);
    m->hidden = FALSE;
    m->forceSoftware = TRUE; REPORT(90);
    Settle();
    assert(m->value == 30 && manual == 3);
    m->forceSoftware = FALSE;
    Report(L"DISPLAY\\OTHER\\1&2&3_0", 90);
    assert(!m->panelReportPending && !timerArmed);
    assert(external->value == 30 && !external->panelReportPending);
    assert(writes == 7 && m->value == 30);
}
''')


if __name__ == "__main__":
    unittest.main()

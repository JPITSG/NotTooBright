"""Run the production tray lifecycle against a controllable shell on Linux."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "NotTooBright.c").read_text()
PROC = "WindowProc"


def function(name):
    match = re.search(r"^(?:static )?(?:void|LRESULT CALLBACK) " + name +
                      r"\([^;{]*\{.*?^}", SOURCE, re.M | re.S)
    if not match:
        raise AssertionError("Missing production function: " + name)
    return match.group()


class TrayRegistrationTests(unittest.TestCase):
    def test_shell_lifecycle(self):
        # Use the real message interception too: a killed timer's queued tick
        # and an Explorer broadcast must follow the same guards as production.
        messages = function(PROC).split("{", 1)[1].split("    switch (", 1)[0]
        messages = re.sub(r"\buMsg\b", "msg", messages)
        stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int BOOL;
typedef unsigned UINT;
typedef unsigned DWORD;
typedef uintptr_t WPARAM;
typedef long LRESULT;
typedef void* HWND;
#define TRUE 1
#define FALSE 0
#define NIM_ADD 0
#define NIM_MODIFY 1
#define NIM_DELETE 2
#define NIF_ICON 2
#define NIF_MESSAGE 1
#define NIF_TIP 4
#define WM_TIMER 0x113
#define LogMessage(...) ((void)0)
#define DebugPrint(...) ((void)0)
#define OutputDebugStringW(...) ((void)0)
typedef struct {
    UINT cbSize, uID, uFlags, uCallbackMessage;
    HWND hWnd;
    void* hIcon;
    wchar_t szTip[128];
} NOTIFYICONDATAW;
typedef NOTIFYICONDATAW NOTIFYICONDATAA;
static NOTIFYICONDATAW cache, published;
#define g_nid cache
#define nid cache
static BOOL g_trayActive, g_trayRegistered, g_trayRetryPending;
static UINT g_WM_TASKBARCREATED = 0xc001;
static int ready, held, timer, failTimer, arms, calls, deletes;
static DWORD commands[4096];
static BOOL Notify(DWORD command, NOTIFYICONDATAW* data) {
    assert(calls < 4096);
    commands[calls++] = command;
    assert(data->hWnd == (HWND)1 && data->uID == 17);
    if (command == NIM_DELETE) { held = 0; deletes++; return TRUE; }
    assert(data->uFlags == (NIF_ICON | NIF_MESSAGE | NIF_TIP));
    assert(data->uCallbackMessage == 0x8001);
    if (!ready || (command == NIM_ADD && held) || (command == NIM_MODIFY && !held)) return FALSE;
    held = 1;
    published = *data;
    return TRUE;
}
#define Shell_NotifyIconW Notify
#define Shell_NotifyIconA Notify
static UINT SetTimer(HWND window, UINT id, UINT delay, void* callback) {
    assert(window == (HWND)1 && id == ID_TIMER_TRAY_RETRY);
    assert(delay == 2000 && callback == NULL);
    arms++;
    if (failTimer) return 0;
    timer = 1;
    return id;
}
static void KillTimer(HWND window, UINT id) {
    assert(window == (HWND)1 && id == ID_TIMER_TRAY_RETRY);
    timer = 0;
}
'''
        assertions = r'''
int main(void) {
    cache.hWnd = (HWND)1;
    cache.uID = 17;
    cache.uCallbackMessage = 0x8001;
    cache.hIcon = (void*)100;
    cache.uFlags = NIF_TIP; /* Last ordinary update was tooltip-only. */
    wcscpy(cache.szTip, L"initial");
    TrayMessages(g_WM_TASKBARCREATED, 0);
    assert(calls == 0); /* Broadcast before initialization. */
    g_trayActive = TRUE;
    PublishTrayIcon(); /* Explorer is not ready at logon. */
    assert(!g_trayRegistered && g_trayRetryPending && timer && arms == 1);
    assert(commands[0] == NIM_ADD && commands[1] == NIM_MODIFY);
    for (int i = 0; i < 120; i++) TrayMessages(WM_TIMER, ID_TIMER_TRAY_RETRY);
    assert(arms == 1 && timer); /* No retry limit, no timer reset/starvation. */
    cache.hIcon = (void*)200;
    wcscpy(cache.szTip, L"latest status");
    ready = 1;
    TrayMessages(WM_TIMER, ID_TIMER_TRAY_RETRY);
    assert(g_trayRegistered && !g_trayRetryPending && !timer);
    assert(published.hIcon == (void*)200 && wcscmp(published.szTip, L"latest status") == 0);
    int before = calls;
    TrayMessages(WM_TIMER, ID_TIMER_TRAY_RETRY); /* Queued stale tick. */
    assert(calls == before);
    PublishTrayIcon(); /* Ordinary update never deletes/re-adds a healthy icon. */
    assert(calls == before + 1 && commands[before] == NIM_MODIFY && deletes == 0);
    held = 0;
    TrayMessages(g_WM_TASKBARCREATED, 0); /* Real Explorer restart. */
    assert(held && g_trayRegistered && !timer && deletes == 0);
    before = calls;
    TrayMessages(g_WM_TASKBARCREATED, 0); /* Broadcast with registration retained. */
    assert(calls == before + 2 && commands[before] == NIM_ADD && commands[before+1] == NIM_MODIFY);
    held = 0;
    before = calls;
    PublishTrayIcon(); /* Shell silently lost the icon. */
    assert(calls == before + 2 && commands[before] == NIM_MODIFY && commands[before+1] == NIM_ADD);
    ready = 0; held = 0;
    TrayMessages(g_WM_TASKBARCREATED, 0); /* Broadcast can precede readiness. */
    assert(timer && !g_trayRegistered && g_trayRetryPending);
    ready = 1;
    TrayMessages(WM_TIMER, ID_TIMER_TRAY_RETRY);
    assert(held && !timer && g_trayRegistered);
    ready = 0; held = 0;
    PublishTrayIcon();
    assert(timer);
    StopTrayRegistration();
    before = calls;
    ready = 1;
    TrayMessages(WM_TIMER, ID_TIMER_TRAY_RETRY);
    TrayMessages(g_WM_TASKBARCREATED, 0);
    PublishTrayIcon();
    StopTrayRegistration();
    assert(calls == before && !timer && !g_trayActive && !g_trayRegistered);
    g_trayActive = TRUE; ready = 0; failTimer = 1;
    PublishTrayIcon();
    assert(!timer && !g_trayRetryPending && !g_trayRegistered);
    failTimer = 0;
    TrayMessages(g_WM_TASKBARCREATED, 0);
    assert(timer && g_trayRetryPending); /* Later event can re-arm after timer failure. */
    ready = 1;
    TrayMessages(WM_TIMER, ID_TIMER_TRAY_RETRY);
    assert(g_trayRegistered && !timer);
    StopTrayRegistration();
    puts("tray lifecycle passed");
    return 0;
}
'''
        defines = "\n".join(re.findall(r"^#define (?:ID_TIMER_TRAY_RETRY|TRAY_RETRY_MS) .+$", SOURCE, re.M))
        harness = defines + "\n" + stubs + function("PublishTrayIcon") + "\n" + function("StopTrayRegistration")
        harness += "\nstatic LRESULT TrayMessages(UINT msg, WPARAM wParam) {" + messages + "return -1;\n}\n"
        harness += assertions
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "tray.c"
            exe = Path(directory) / "tray"
            source.write_text(harness)
            subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_broadcast_and_shutdown_wiring(self):
        self.assertNotIn("HWND_MESSAGE", SOURCE)
        register = function("RegisterTaskbarMessage")
        self.assertIn('RegisterWindowMessageW(L"TaskbarCreated")', register)
        self.assertIn('"ChangeWindowMessageFilterEx"', register)
        self.assertIn('allow(hwnd, g_WM_TASKBARCREATED, 1 /* MSGFLT_ALLOW */', register)
        main = SOURCE[SOURCE.index("int WINAPI WinMain("):]
        self.assertLess(main.index("RegisterTaskbarMessage("), main.index("CreateTrayIcon("))
        create = function("CreateTrayIcon")
        self.assertIn("g_trayActive = TRUE;", create)
        self.assertIn("PublishTrayIcon();", create)
        proc = function(PROC)
        self.assertRegex(proc, r"case WM_DESTROY:\s+StopTrayRegistration\(\);")
        ids = re.findall(r"^#define ID_TIMER_\w+\s+(\d+)\s*$", SOURCE, re.M)
        self.assertEqual(len(ids), len(set(ids)), "Retry timer must not collide with another timer")


if __name__ == "__main__":
    unittest.main()

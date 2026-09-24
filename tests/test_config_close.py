"""Exercise the native close gate; the browser suite checks edit decisions."""
import unittest

from test_schedule import function, run_c


class ConfigCloseTests(unittest.TestCase):
    def test_native_close_waits_for_the_ui_and_allows_the_update_handoff(self):
        run_c(r'''
#include <assert.h>
#include <wchar.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
static BOOL g_configViewReady, g_configCloseApproved, g_updateInstallReady;
static void *g_cfgWebView;
static int requests;
static void webview_cfg_execute_script(const wchar_t *script) {
    assert(wcscmp(script, L"window.onCloseRequested()") == 0);
    requests++;
}
''' + function("RequestConfigClose") + r'''
int main(void) {
    assert(!RequestConfigClose());  /* Loading or failed WebView can close. */
    g_configViewReady = TRUE;
    assert(!RequestConfigClose());
    g_configViewReady = FALSE;
    g_cfgWebView = (void *)1;
    assert(!RequestConfigClose());
    g_configViewReady = TRUE;
    assert(RequestConfigClose());
    assert(RequestConfigClose());  /* Repeated X never bypasses the prompt. */
    assert(requests == 2);
    g_configCloseApproved = TRUE;
    assert(!RequestConfigClose());  /* Save, discard or unchanged settings. */
    g_configCloseApproved = FALSE;
    g_updateInstallReady = TRUE;
    assert(!RequestConfigClose());  /* Updater can finish its handoff. */
    g_updateInstallReady = FALSE;
    assert(RequestConfigClose());
    assert(requests == 3);
}
''')

    def test_close_gate_runs_before_teardown_and_resets_when_reopened(self):
        proc = function("CfgWndProc")
        close = proc.split("case WM_CLOSE:", 1)[1].split("case WM_DESTROY:", 1)[0]
        self.assertLess(close.index("if (RequestConfigClose()) return 0;"),
                        close.index("->Close("))
        self.assertLess(close.index("if (RequestConfigClose()) return 0;"),
                        close.index("DestroyWindow("))
        self.assertIn("g_configCloseApproved = FALSE;",
                      proc.split("case WM_DESTROY:", 1)[1])
        self.assertIn("g_configCloseApproved = FALSE;", function("ShowConfigDialog"))
        handler = function("CfgMsgReceived_Invoke")
        save = handler.split('strcmp(action, "saveSettings")', 1)[1].split(
            'strcmp(action, "close")', 1)[0]
        self.assertLess(save.index("SaveConfigToRegistry(&g_config)"),
                        save.index("g_configCloseApproved = TRUE;"))
        self.assertLess(save.index("g_configCloseApproved = TRUE;"),
                        save.index("PostMessageW(g_cfgHwnd, WM_CLOSE"))
        close = handler.split('strcmp(action, "close")', 1)[1].split(
            'strcmp(action, "resize")', 1)[0]
        self.assertLess(close.index("g_configCloseApproved = TRUE;"),
                        close.index("PostMessageW(g_cfgHwnd, WM_CLOSE"))

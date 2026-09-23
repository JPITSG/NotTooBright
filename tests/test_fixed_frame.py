"""Run NotTooBright.c's fixed-size dialog frame against stubbed window calls.

The dialogs keep the standard frame but only the app sizes them: edge hits,
the Size and Maximize commands and the track size are covered here, then the
source is checked for every place that sizes the dialog.
Run: python3 -m unittest discover -s tests -v
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'NotTooBright.c').read_text()


def function(name):
    # Match the definition, not a forward declaration of the same name.
    match = re.search(r'^static [^\n]*\b' + re.escape(name) +
                      r'\([^;]*?\)\s*\{', SOURCE, re.MULTILINE)
    if not match:
        raise AssertionError('Missing production function: ' + name)
    depth = 1
    end = match.end()
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end]


FRAME = 'g_cfgFrameSize'
DIALOG = 'g_cfgHwnd'
PROC = 'CfgWndProc'
CLASS = 'NotTooBrightCfgWnd'


def call(text, start):
    """The argument text of the call whose opening parenthesis is at start."""
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '(') - (text[end] == ')')
        end += 1
    return text[start:end]


class FixedFrameTests(unittest.TestCase):
    def test_frame_refuses_user_sizing(self):
        style = re.search(r'^#define FIXED_FRAME_STYLE\b.*$', SOURCE, re.M).group(0)
        production = '\n'.join(function(name) for name in (
            'FixedFrameMessage', 'FixedFrameInit', 'FixedFrameSetPos'))
        with tempfile.TemporaryDirectory(prefix='nottoobright-frame-') as directory:
            path = Path(directory)
            (path / 'frame.c').write_text(PREFIX + style + '\n' + production + SCENARIO)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path / 'frame.c'), '-o', str(path / 'frame')],
                           check=True)
            subprocess.run([str(path / 'frame')], check=True, timeout=10)

    def test_every_dialog_size_goes_through_the_frame(self):
        proc = function(PROC)
        self.assertRegex(proc, r'^static LRESULT CALLBACK ' + PROC + r'\([^)]*\) \{\n'
                         r'    LRESULT frameResult;\n'
                         r'    if \(FixedFrameMessage\(hwnd, msg, wParam, lParam, &' +
                         FRAME + r', &frameResult\)\) \{')
        self.assertNotIn('SetWindowPos(', proc)
        creations = re.findall(r'(\w+) = CreateWindowExW\([^;]*"' + CLASS + r'"[^;]*;', SOURCE)
        self.assertEqual(creations, [DIALOG])
        creation = re.search(DIALOG + r' = CreateWindowExW\([^;]*;', SOURCE)
        self.assertIn('FIXED_FRAME_STYLE', creation.group(0))
        self.assertNotIn('WS_OVERLAPPEDWINDOW', creation.group(0))
        self.assertIn('FixedFrameInit(' + DIALOG + ', &' + FRAME + ');',
                      SOURCE[creation.end():creation.end() + 400])
        for match in re.finditer(r'\bSetWindowPos\(\s*' + DIALOG + r'\b', SOURCE):
            self.assertIn('SWP_NOSIZE', call(SOURCE, SOURCE.index('(', match.start())))
        self.assertGreaterEqual(SOURCE.count('FixedFrameSetPos(' + DIALOG + ', &' + FRAME), 1)
        self.assertIn('FixedFrameSetPos(hwnd, &' + FRAME, proc)

    def test_no_text_box_can_grow_the_dialog(self):
        # Content-driven sizing would follow a dragged text box's grip.
        for path in sorted((ROOT / 'assets' / 'src').rglob('*')):
            if path.suffix not in ('.tsx', '.css'):
                continue
            text = path.read_text()
            self.assertNotRegex(text, r'\bresize-[xy]\b|[\s"\'`]resize[\s"\'`]|resize:\s*(?!none)',
                                path.name)
            for match in re.finditer(r'<textarea\b', text):
                self.assertIn('resize-none', text[match.end():match.end() + 400], path.name)


PREFIX = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int BOOL;
typedef unsigned int UINT;
typedef long LONG;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM, LRESULT;
typedef void *HWND, *HMENU;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct {
    POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
} MINMAXINFO;
#define TRUE 1
#define FALSE 0
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_CAPTION 0x00C00000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WM_GETMINMAXINFO 0x0024
#define WM_NCDESTROY 0x0082
#define WM_NCHITTEST 0x0084
#define WM_SYSCOMMAND 0x0112
#define WM_CLOSE 0x0010
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTMINBUTTON 8
#define HTMAXBUTTON 9
#define HTLEFT 10
#define HTRIGHT 11
#define HTTOP 12
#define HTTOPLEFT 13
#define HTTOPRIGHT 14
#define HTBOTTOM 15
#define HTBOTTOMLEFT 16
#define HTBOTTOMRIGHT 17
#define HTBORDER 18
#define HTCLOSE 20
#define SC_SIZE 0xF000
#define SC_MOVE 0xF010
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_CLOSE 0xF060
#define SC_KEYMENU 0xF100
#define SC_RESTORE 0xF120
#define MF_BYCOMMAND 0
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define SWP_SHOWWINDOW 0x0040

static HWND const window = (HWND)0x10;
static HMENU const systemMenu = (HMENU)0x20;
static LRESULT defaultHit;
static int defaultCalls;
static RECT windowRect;
static BOOL windowRectOk = TRUE;
static UINT deleted[4];
static int deletedCount;
static SIZE *pinned;
static SIZE pinnedAtCall;
static int setPosCalls;
static RECT placed;
static UINT placedFlags;

LRESULT DefWindowProcW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    assert(hwnd == window && msg == WM_NCHITTEST && wParam == 0 && lParam == 0x00640032);
    defaultCalls++;
    return defaultHit;
}
BOOL GetWindowRect(HWND hwnd, RECT *rect) {
    assert(hwnd == window);
    *rect = windowRect;
    return windowRectOk;
}
HMENU GetSystemMenu(HWND hwnd, BOOL revert) {
    assert(hwnd == window && !revert);
    return systemMenu;
}
BOOL DeleteMenu(HMENU menu, UINT item, UINT flags) {
    assert(menu == systemMenu && flags == MF_BYCOMMAND && deletedCount < 4);
    deleted[deletedCount++] = item;
    return TRUE;
}
BOOL SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    assert(hwnd == window && after == NULL);
    pinnedAtCall = *pinned;
    placed = (RECT){x, y, cx, cy};
    placedFlags = flags;
    setPosCalls++;
    return TRUE;
}
'''

SCENARIO = r'''
static LRESULT hit(SIZE *size, LRESULT fromDefault) {
    LRESULT result = -99;
    defaultHit = fromDefault;
    assert(FixedFrameMessage(window, WM_NCHITTEST, 0, 0x00640032, size, &result));
    return result;
}

static BOOL command(SIZE *size, WPARAM which) {
    LRESULT result = -99;
    BOOL handled = FixedFrameMessage(window, WM_SYSCOMMAND, which, 0, size, &result);
    if (handled) assert(result == 0);
    else assert(result == -99);
    return handled;
}

static MINMAXINFO track(SIZE *size, LONG minX, LONG minY, BOOL expectHandled) {
    MINMAXINFO info = {{0, 0}, {1920, 1080}, {0, 0}, {minX, minY}, {3000, 2000}};
    LRESULT result = -99;
    assert(FixedFrameMessage(window, WM_GETMINMAXINFO, 0, (LPARAM)&info, size, &result) ==
           expectHandled);
    assert(result == (expectHandled ? 0 : -99));
    return info;
}

int main(void) {
    SIZE size = {0, 0};
    pinned = &size;

    /* The standard frame (so the normal caption height) without Maximize. */
    assert((FIXED_FRAME_STYLE & WS_CAPTION) == WS_CAPTION);
    assert(FIXED_FRAME_STYLE & WS_SYSMENU);
    assert(FIXED_FRAME_STYLE & WS_THICKFRAME);
    assert(FIXED_FRAME_STYLE & WS_MINIMIZEBOX);
    assert(!(FIXED_FRAME_STYLE & WS_MAXIMIZEBOX));

    /* Edges and corners cannot be dragged; the top edge moves the window. */
    assert(hit(&size, HTTOP) == HTCAPTION);
    assert(hit(&size, HTTOPLEFT) == HTCAPTION);
    assert(hit(&size, HTTOPRIGHT) == HTCAPTION);
    assert(hit(&size, HTLEFT) == HTBORDER);
    assert(hit(&size, HTRIGHT) == HTBORDER);
    assert(hit(&size, HTBOTTOM) == HTBORDER);
    assert(hit(&size, HTBOTTOMLEFT) == HTBORDER);
    assert(hit(&size, HTBOTTOMRIGHT) == HTBORDER);
    LRESULT untouched[] = {HTNOWHERE, HTCLIENT, HTCAPTION, HTSYSMENU, HTMINBUTTON,
                           HTMAXBUTTON, HTBORDER, HTCLOSE};
    for (size_t i = 0; i < sizeof(untouched) / sizeof(untouched[0]); i++) {
        assert(hit(&size, untouched[i]) == untouched[i]);
    }
    assert(defaultCalls == 16);

    /* Size (from the menu, the keyboard or a border drag) and Maximize are
     * refused; every other system command is left to DefWindowProc. */
    assert(command(&size, SC_SIZE));
    assert(command(&size, SC_SIZE | 8));
    assert(command(&size, SC_MAXIMIZE));
    assert(command(&size, SC_MAXIMIZE | HTCAPTION));
    assert(!command(&size, SC_MOVE));
    assert(!command(&size, SC_MOVE | HTCAPTION));
    assert(!command(&size, SC_MINIMIZE));
    assert(!command(&size, SC_RESTORE));
    assert(!command(&size, SC_CLOSE));
    assert(!command(&size, SC_KEYMENU));

    /* Nothing is pinned while CreateWindowExW is still running. */
    MINMAXINFO info = track(&size, 136, 39, FALSE);
    assert(info.ptMinTrackSize.x == 136 && info.ptMaxTrackSize.x == 3000);

    /* Creation pins the size it gave and trims the system menu. */
    windowRect = (RECT){100, 50, 660, 570};
    FixedFrameInit(window, &size);
    assert(size.cx == 560 && size.cy == 520);
    assert(deletedCount == 2 && deleted[0] == SC_SIZE && deleted[1] == SC_MAXIMIZE);
    info = track(&size, 136, 39, TRUE);
    assert(info.ptMinTrackSize.x == 560 && info.ptMinTrackSize.y == 520);
    assert(info.ptMaxTrackSize.x == 560 && info.ptMaxTrackSize.y == 520);
    assert(info.ptMaxSize.x == 1920 && info.ptMaxPosition.x == 0);
    /* Windows' own minimum still wins over a smaller pinned size. */
    info = track(&size, 600, 39, TRUE);
    assert(info.ptMinTrackSize.x == 600 && info.ptMaxTrackSize.x == 600);
    assert(info.ptMinTrackSize.y == 520 && info.ptMaxTrackSize.y == 520);

    /* The app's own sizes are pinned before Windows applies them. */
    assert(FixedFrameSetPos(window, &size, 10, 20, 900, 700, SWP_NOZORDER | SWP_SHOWWINDOW));
    assert(setPosCalls == 1 && pinnedAtCall.cx == 900 && pinnedAtCall.cy == 700);
    assert(placed.left == 10 && placed.top == 20 && placed.right == 900 && placed.bottom == 700);
    assert(placedFlags == (SWP_NOZORDER | SWP_SHOWWINDOW));
    info = track(&size, 136, 39, TRUE);
    assert(info.ptMinTrackSize.x == 900 && info.ptMaxTrackSize.y == 700);
    /* A move or z-order change keeps the pinned size. */
    FixedFrameSetPos(window, &size, 5, 5, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    assert(setPosCalls == 2 && size.cx == 900 && size.cy == 700);

    /* A failed rectangle read leaves the previous pin rather than zero. */
    windowRectOk = FALSE;
    FixedFrameInit(window, &size);
    assert(size.cx == 900 && size.cy == 700);

    /* Destruction releases the pin for the next dialog and still reaches the
     * window procedure. */
    LRESULT result = -99;
    assert(!FixedFrameMessage(window, WM_NCDESTROY, 0, 0, &size, &result));
    assert(result == -99 && size.cx == 0 && size.cy == 0);
    track(&size, 136, 39, FALSE);
    assert(!FixedFrameMessage(window, WM_CLOSE, 0, 0, &size, &result));
    return 0;
}
'''


if __name__ == '__main__':
    unittest.main()

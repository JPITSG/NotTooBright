"""Start with Windows: the per-user Run entry, ported from ../c_APIMonitor.

The registry is replaced by a small in-memory table so the production
functions run unchanged.
"""
import unittest

from test_idle import defines
from test_schedule import function, run_c


class StartWithWindowsTests(unittest.TestCase):
    def test_run_entry_follows_the_checkbox_and_task_manager(self):
        run_c(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_BAD_PATHNAME 161
#define ERROR_MORE_DATA 234
#define ERROR_UNSUPPORTED_TYPE 1630
#define RRF_RT_REG_SZ 0x2
#define RRF_RT_REG_BINARY 0x8
#define REG_SZ 1
#define REG_BINARY 3
#define REG_OPTION_NON_VOLATILE 0
#define KEY_SET_VALUE 2
#define STARTUP_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_APPROVED_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define DebugPrint(...) (warnings++)
#define _wcsicmp wcscasecmp
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef long LONG;
typedef void* HKEY;
#define HKEY_CURRENT_USER ((HKEY)1)
''' + defines("APP_NAME") + r'''
int warnings;
/* One value per key: the Run entry and the StartupApproved marker. */
typedef struct { const wchar_t* key; BOOL present; DWORD type; BYTE data[600]; DWORD size; } Value;
Value values[2] = { { STARTUP_RUN_KEY }, { STARTUP_APPROVED_RUN_KEY } };
wchar_t exePath[400] = L"C:\\Program Files\\NotTooBright\\NotTooBright.exe";
LONG createResult = ERROR_SUCCESS;
int writes, deletes;
static Value* Find(const wchar_t* key, const wchar_t* name) {
    assert(wcscmp(name, L"NotTooBright") == 0);
    for (int i = 0; i < 2; i++) if (wcscmp(values[i].key, key) == 0) return &values[i];
    assert(!"unexpected key");
    return NULL;
}
static void Put(const wchar_t* key, DWORD type, const void* data, DWORD size) {
    Value* v = Find(key, L"NotTooBright");
    v->present = TRUE; v->type = type; v->size = size; memcpy(v->data, data, size);
}
static void PutRun(const wchar_t* command) {
    Put(STARTUP_RUN_KEY, REG_SZ, command, (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
}
static void PutMarker(BYTE first) {
    BYTE marker[12] = { first };
    Put(STARTUP_APPROVED_RUN_KEY, REG_BINARY, marker, sizeof(marker));
}
static const wchar_t* RunCommand(void) {
    return values[0].present ? (const wchar_t*)values[0].data : NULL;
}
DWORD GetModuleFileNameW(void* module, wchar_t* path, DWORD count) {
    (void)module;
    size_t length = wcslen(exePath);
    if (length >= count) { wcsncpy(path, exePath, count - 1); path[count - 1] = 0; return count; }
    wcscpy(path, exePath);
    return (DWORD)length;
}
int swprintf_s(wchar_t* out, size_t count, const wchar_t* format, ...) {
    wchar_t fixed[64]; size_t n = 0;
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
LONG RegGetValueW(HKEY root, const wchar_t* key, const wchar_t* name, DWORD flags,
                  DWORD* type, void* data, DWORD* size) {
    assert(root == HKEY_CURRENT_USER && !type);
    Value* v = Find(key, name);
    if (!v->present) return ERROR_FILE_NOT_FOUND;
    if ((flags == RRF_RT_REG_SZ) != (v->type == REG_SZ)) return ERROR_UNSUPPORTED_TYPE;
    if (*size < v->size) { *size = v->size; return ERROR_MORE_DATA; }
    memcpy(data, v->data, v->size);
    *size = v->size;
    return ERROR_SUCCESS;
}
LONG RegCreateKeyExW(HKEY root, const wchar_t* key, DWORD reserved, void* cls, DWORD options,
                     DWORD access, void* security, HKEY* out, DWORD* disposition) {
    (void)reserved; (void)cls; (void)options; (void)security; (void)disposition;
    assert(root == HKEY_CURRENT_USER && access == KEY_SET_VALUE && wcscmp(key, STARTUP_RUN_KEY) == 0);
    *out = (HKEY)2;
    return createResult;
}
LONG RegSetValueExW(HKEY key, const wchar_t* name, DWORD reserved, DWORD type,
                    const BYTE* data, DWORD size) {
    (void)reserved; assert(key == (HKEY)2);
    Value* v = Find(STARTUP_RUN_KEY, name);
    v->present = TRUE; v->type = type; v->size = size; memcpy(v->data, data, size);
    writes++;
    return ERROR_SUCCESS;
}
LONG RegCloseKey(HKEY key) { assert(key == (HKEY)2); return ERROR_SUCCESS; }
LONG RegDeleteKeyValueW(HKEY root, const wchar_t* key, const wchar_t* name) {
    assert(root == HKEY_CURRENT_USER);
    Value* v = Find(key, name);
    if (!v->present) return ERROR_FILE_NOT_FOUND;
    v->present = FALSE;
    deletes++;
    return ERROR_SUCCESS;
}
''' + function("json_get_bool") + function("GetStartupCommand") + function("IsStartWithWindowsEnabled") +
              function("SetStartWithWindows") + function("SaveStartWithWindows") + r'''
int main(void) {
    const wchar_t* quoted = L"\"C:\\Program Files\\NotTooBright\\NotTooBright.exe\"";
    /* Nothing registered: off. Turning it on writes this copy's quoted path. */
    assert(!IsStartWithWindowsEnabled());
    assert(SetStartWithWindows(TRUE) == ERROR_SUCCESS);
    assert(wcscmp(RunCommand(), quoted) == 0 && values[0].type == REG_SZ);
    assert(IsStartWithWindowsEnabled());

    /* Disabled in Task Manager (odd first byte): off; turning it on again
     * clears the marker. An even first byte means enabled. */
    PutMarker(0x03);
    assert(!IsStartWithWindowsEnabled());
    PutMarker(0x02);
    assert(IsStartWithWindowsEnabled());
    PutMarker(0x03);
    SaveStartWithWindows("{\"action\":\"saveSettings\",\"startWithWindows\":true}");
    assert(!values[1].present && IsStartWithWindowsEnabled());

    /* The path is compared without regard to case. */
    PutRun(L"\"c:\\program files\\nottoobright\\NOTTOOBRIGHT.EXE\"");
    assert(IsStartWithWindowsEnabled());

    /* An entry for another copy is off here, and saving with the checkbox
     * unchanged leaves it alone; ticking the box points it at this copy. */
    PutRun(L"\"\\\\server\\share\\NotTooBright.exe\"");
    assert(!IsStartWithWindowsEnabled());
    writes = deletes = 0;
    SaveStartWithWindows("{\"action\":\"saveSettings\",\"startWithWindows\":false}");
    assert(writes == 0 && deletes == 0 && wcscmp(RunCommand(), L"\"\\\\server\\share\\NotTooBright.exe\"") == 0);
    SaveStartWithWindows("{\"action\":\"saveSettings\",\"startWithWindows\":true}");
    assert(writes == 1 && wcscmp(RunCommand(), quoted) == 0);

    /* A message without the checkbox changes nothing either way. */
    writes = deletes = 0;
    SaveStartWithWindows("{\"action\":\"saveSettings\",\"debugLog\":false}");
    assert(writes == 0 && deletes == 0 && IsStartWithWindowsEnabled());

    /* Unticking removes the entry and any marker; nothing left is fine. */
    PutMarker(0x02);
    SaveStartWithWindows("{\"action\":\"saveSettings\",\"startWithWindows\":false}");
    assert(!values[0].present && !values[1].present && !IsStartWithWindowsEnabled());
    assert(SetStartWithWindows(FALSE) == ERROR_SUCCESS);

    /* Failures are reported, never half applied as success. */
    createResult = ERROR_ACCESS_DENIED;
    warnings = 0;
    assert(SetStartWithWindows(TRUE) == ERROR_ACCESS_DENIED && !values[0].present);
    SaveStartWithWindows("{\"action\":\"saveSettings\",\"startWithWindows\":true}");
    assert(warnings == 1 && !IsStartWithWindowsEnabled());
    createResult = ERROR_SUCCESS;
    /* A path too long to quote is never registered. */
    for (int i = 0; i < 300; i++) exePath[i] = L'a';
    exePath[300] = 0;
    assert(SetStartWithWindows(TRUE) == ERROR_BAD_PATHNAME && !values[0].present);
    assert(!IsStartWithWindowsEnabled());
}
''')


if __name__ == "__main__":
    unittest.main()

"""Detect from IP: parsing the four services' answers and the lookup state.

WinHTTP and threads are stubbed; the production parser and state machine
run unchanged.
"""
import re
import unittest

from test_idle import defines
from test_schedule import ROOT, function, run_c, structure


def services():
    source = (ROOT / "NotTooBright.c").read_text()
    table = re.search(r"static const LocationService kLocationServices\[\] = \{.*?\n\};\n", source, re.S)[0]
    count = re.search(r"#define LOCATION_SERVICE_COUNT .*\n", source)[0]
    return table + count


PRELUDE = r'''
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define TRUE 1
#define FALSE 0
#define CP_UTF8 65001
#define DebugPrint(...) ((void)0)
typedef int BOOL;
typedef long LONG;
typedef unsigned long DWORD;
typedef unsigned long long ULONGLONG;
''' + defines("LOCATION_PLACE_CHARS", "LOCATION_CACHE_MINUTES", "LOCATION_CACHE_FILETIME",
              "LOCATION_TIMEOUT_MS", "ID_TIMER_LOCATION")

# A small UTF-8 decoder standing in for the Windows conversion.
UTF8 = r'''
int MultiByteToWideChar(unsigned page, DWORD flags, const char* in, int inLength, wchar_t* out, int outCount) {
    (void)page; (void)flags; (void)inLength;
    int n = 0;
    const unsigned char* p = (const unsigned char*)in;
    while (n < outCount) {
        unsigned c = *p++;
        if (c >= 0xF0) { c = (c & 0x07) << 18; c |= (*p++ & 0x3F) << 12; c |= (*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        else if (c >= 0xE0) { c = (c & 0x0F) << 12; c |= (*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        else if (c >= 0xC0) { c = (c & 0x1F) << 6; c |= *p++ & 0x3F; }
        out[n++] = (wchar_t)c;
        if (!c) return n;
    }
    return 0;
}
'''


class LocationTests(unittest.TestCase):
    def test_each_service_answer_gives_a_position_and_place(self):
        run_c(PRELUDE + structure("LocationService") + services() + UTF8 +
              function("json_get_string") + function("json_get_number") +
              function("ParseLocationAnswer") + r'''
static int Parse(int service, const char* json, double* lat, double* lon, wchar_t* place) {
    return ParseLocationAnswer(&kLocationServices[service], json, lat, lon, place, LOCATION_PLACE_CHARS);
}
int main(void) {
    double lat = 0, lon = 0;
    wchar_t place[LOCATION_PLACE_CHARS];
    assert(LOCATION_SERVICE_COUNT == 4);
    /* ipapi.co: numbers, pretty-printed. */
    assert(Parse(0, "{\n  \"city\": \"Warsaw\",\n  \"country\": \"PL\",\n  \"country_code\": \"PL\",\n"
                    "  \"latitude\": 52.2297,\n  \"longitude\": 21.0122\n}", &lat, &lon, place));
    assert(lat == 52.2297 && lon == 21.0122 && wcscmp(place, L"Warsaw, PL") == 0);
    /* GeoJS: numbers in strings, raw UTF-8 city. */
    assert(Parse(1, "{\"city\":\"\xC5\x81" "azy\",\"country\":\"Poland\",\"country_code\":\"PL\","
                    "\"latitude\":\"52.1\",\"longitude\":\"-20.9\"}", &lat, &lon, place));
    assert(lat == 52.1 && lon == -20.9 && wcscmp(place, L"\x0141" L"azy, PL") == 0);
    /* ipinfo.io: "lat,lon" in one string; country is the code there. */
    assert(Parse(2, "{\"city\": \"Magdalenka\",\"country\": \"PL\",\"loc\": \"52.1170,20.9500\"}",
                 &lat, &lon, place));
    assert(lat == 52.117 && lon == 20.95 && wcscmp(place, L"Magdalenka, PL") == 0);
    /* ip-api.com: lat/lon; an escaped city decodes too. */
    assert(Parse(3, "{\"status\":\"success\",\"country\":\"Poland\",\"countryCode\":\"PL\","
                    "\"city\":\"\\u0141azy\",\"lat\":52.1,\"lon\":20.9}", &lat, &lon, place));
    assert(wcscmp(place, L"\x0141" L"azy, PL") == 0);
    /* No city or country: the position alone still counts. */
    assert(Parse(3, "{\"lat\":-33.8688,\"lon\":151.2093}", &lat, &lon, place) && place[0] == 0);
    assert(lat == -33.8688 && lon == 151.2093);

    /* In-band failures, unknown positions and nonsense are not answers. */
    lat = lon = 99;
    assert(!Parse(3, "{\"status\":\"fail\",\"message\":\"reserved range\"}", &lat, &lon, place));
    assert(!Parse(0, "{\"error\": true, \"reason\": \"RateLimited\"}", &lat, &lon, place));
    assert(!Parse(0, "{\"latitude\": 0, \"longitude\": 0}", &lat, &lon, place));
    assert(!Parse(0, "{\"latitude\": 95.1, \"longitude\": 10}", &lat, &lon, place));
    assert(!Parse(3, "{\"lat\":10,\"lon\":181}", &lat, &lon, place));
    assert(!Parse(2, "{\"loc\": \"52.1\"}", &lat, &lon, place));
    assert(!Parse(2, "{\"loc\": \"north,south\"}", &lat, &lon, place));
    assert(!Parse(1, "<html>blocked</html>", &lat, &lon, place));
    assert(lat == 99 && lon == 99);   /* untouched on failure */
}
''')

    def test_json_strings_decode_unicode_escapes(self):
        run_c(PRELUDE + function("json_get_string") + r'''
int main(void) {
    char out[64];
    assert(json_get_string("{\"a\":\"\\u0141\\u00f3d\\u017a\"}", "a", out, sizeof(out)));
    assert(strcmp(out, "\xC5\x81\xC3\xB3" "d\xC5\xBA") == 0);                /* Łódź */
    assert(json_get_string("{\"a\":\"x\\ud83d\\ude00y\"}", "a", out, sizeof(out)));
    assert(strcmp(out, "x\xF0\x9F\x98\x80y") == 0);                           /* a surrogate pair */
    assert(json_get_string("{\"a\":\"\\u0041\\\"\\n\"}", "a", out, sizeof(out)));
    assert(strcmp(out, "A\"\n") == 0);
    /* A short escape is kept as text rather than read past. */
    assert(json_get_string("{\"a\":\"\\u12\"}", "a", out, sizeof(out)) && strcmp(out, "u12") == 0);
    /* No room for the whole character: it is left out, never cut. */
    char tiny[3];
    assert(json_get_string("{\"a\":\"a\\u20ac\"}", "a", tiny, sizeof(tiny)) && strcmp(tiny, "a") == 0);
}
''')

    def test_first_answer_wins_and_is_reused_for_an_hour(self):
        run_c(PRELUDE + r'''
typedef void* HANDLE;
typedef void* HWND;
typedef unsigned UINT;
typedef DWORD (*ThreadProc)(void*);
#define WINAPI
#define LPVOID void*
''' + structure("LocationService") + services() + structure("LocationResult") +
              structure("LocationCache") + r'''
LocationCache g_locationCache;
LONG g_locationRequest;
BOOL g_locationBusy;
int g_locationFailures;
HWND g_hwnd = (HWND)1;
ULONGLONG clockNow = 1000000000000ULL;
LocationResult* started[16];
int startedCount, failStart, timerArmed, saves, pushes, pushedCached;
wchar_t pushedStatus[16];
#define MINUTE (60ULL * 10000000)
ULONGLONG NowFileTime(void) { return clockNow; }
DWORD LocationThread(LPVOID param) { (void)param; return 0; }
HANDLE CreateThread(void* a, size_t b, ThreadProc proc, void* param, DWORD c, DWORD* d) {
    (void)a; (void)b; (void)c; (void)d; (void)proc;
    if (failStart) return NULL;
    started[startedCount++] = (LocationResult*)param;
    return (HANDLE)1;
}
BOOL CloseHandle(HANDLE h) { (void)h; return TRUE; }
UINT SetTimer(HWND hwnd, UINT id, UINT ms, void* cb) {
    (void)hwnd; (void)cb; assert(id == ID_TIMER_LOCATION && ms == LOCATION_TIMEOUT_MS); timerArmed = 1; return 1;
}
BOOL KillTimer(HWND hwnd, UINT id) { (void)hwnd; assert(id == ID_TIMER_LOCATION); timerArmed = 0; return TRUE; }
void SaveLocationCache(void) { saves++; }
void PushLocationResult(const wchar_t* status, BOOL cached) {
    pushes++; pushedCached = cached; wcscpy(pushedStatus, status);
}
void wcscpy_s(wchar_t* out, size_t count, const wchar_t* in) { (void)count; wcscpy(out, in); }
''' + function("LocationCacheFresh") + function("DetectLocation") + function("EndLocationLookup") +
              function("HandleLocationResult") + r'''
static LocationResult* Answer(int service, BOOL ok, double lat, double lon) {
    for (int i = 0; i < startedCount; i++) {
        LocationResult* r = started[i];
        if (r && r->service == service) {
            started[i] = NULL;
            r->ok = ok; r->latitude = lat; r->longitude = lon;
            wcscpy(r->place, ok ? L"Warsaw, PL" : L"");
            return r;
        }
    }
    assert(!"no such request");
    return NULL;
}
static void Reset(void) { startedCount = 0; pushes = 0; }
int main(void) {
    /* All four at once, with a 10 second limit. */
    DetectLocation();
    assert(startedCount == 4 && g_locationBusy && timerArmed && pushes == 0);
    DetectLocation();                              /* a second press while busy does nothing */
    assert(startedCount == 4);
    /* A failure is only counted; the first real answer wins. */
    HandleLocationResult(Answer(0, FALSE, 0, 0));
    assert(g_locationBusy && pushes == 0);
    HandleLocationResult(Answer(2, TRUE, 52.2, 21.0));
    assert(!g_locationBusy && !timerArmed && pushes == 1 && wcscmp(pushedStatus, L"ok") == 0 && !pushedCached);
    assert(g_locationCache.valid && g_locationCache.latitude == 52.2 && saves == 1);
    assert(wcscmp(g_locationCache.source, L"ipinfo.io") == 0 && wcscmp(g_locationCache.place, L"Warsaw, PL") == 0);
    /* Later answers of that lookup are dropped. */
    HandleLocationResult(Answer(1, TRUE, 10, 10));
    HandleLocationResult(Answer(3, FALSE, 0, 0));
    assert(pushes == 1 && g_locationCache.latitude == 52.2 && saves == 1);

    /* For the next hour the button answers from the cache, asking no one. */
    Reset();
    clockNow += 59 * MINUTE;
    DetectLocation();
    assert(startedCount == 0 && pushes == 1 && pushedCached && wcscmp(pushedStatus, L"ok") == 0);
    clockNow -= 59 * MINUTE + 30 * MINUTE;         /* clock moved back: 30 minutes before the answer */
    DetectLocation();
    assert(startedCount == 0 && pushes == 2 && pushedCached);
    clockNow -= 31 * MINUTE;                       /* more than an hour either side: ask again */
    DetectLocation();
    assert(startedCount == 4 && g_locationBusy && pushes == 2);
    EndLocationLookup(L"cancelled");
    for (int i = 0; i < 4; i++) HandleLocationResult(Answer(i, FALSE, 0, 0));
    clockNow += 61 * MINUTE + 61 * MINUTE;         /* 61 minutes after the answer */
    Reset();
    DetectLocation();
    assert(startedCount == 4 && g_locationBusy && pushes == 0);

    /* Every service failing ends the lookup; nothing is cached. */
    ULONGLONG cachedAt = g_locationCache.time;
    for (int i = 0; i < 4; i++) HandleLocationResult(Answer(i, FALSE, 0, 0));
    assert(!g_locationBusy && !timerArmed && pushes == 1 && wcscmp(pushedStatus, L"failed") == 0);
    assert(g_locationCache.time == cachedAt && saves == 1);

    /* The time limit, and cancelling: answers arriving after are dropped. */
    Reset();
    DetectLocation();
    EndLocationLookup(L"timeout");
    assert(!g_locationBusy && !timerArmed && pushes == 1 && wcscmp(pushedStatus, L"timeout") == 0);
    HandleLocationResult(Answer(0, TRUE, 1, 1));
    assert(pushes == 1 && g_locationCache.latitude == 52.2);
    EndLocationLookup(L"cancelled");               /* nothing running: nothing to report */
    assert(pushes == 1);
    for (int i = 1; i < 4; i++) HandleLocationResult(Answer(i, FALSE, 0, 0));
    Reset();
    DetectLocation();
    EndLocationLookup(L"cancelled");
    assert(pushes == 1 && wcscmp(pushedStatus, L"cancelled") == 0 && !g_locationBusy);
    for (int i = 0; i < 4; i++) HandleLocationResult(Answer(i, TRUE, 3, 3));
    assert(pushes == 1 && g_locationCache.latitude == 52.2);

    /* No thread could start: reported at once, not left waiting. */
    Reset();
    failStart = 1;
    DetectLocation();
    assert(!g_locationBusy && !timerArmed && pushes == 1 && wcscmp(pushedStatus, L"failed") == 0);
}
''')


if __name__ == "__main__":
    unittest.main()

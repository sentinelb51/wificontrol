/* perf_win32.c -- the Performance switch's backend.  See perf_win32.h. */
#include "perf_win32.h"
#include "tune.h"
#include "wlan_win32.h"

#include <powrprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION L"held"
#define KEY_MAX (16 + 40 + PERF_NAME_MAX)

static void widen(const char *s, wchar_t *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = L'\0';
}

static void narrow(const wchar_t *s, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, nullptr, nullptr)) out[0] = '\0';
}

static unsigned long absent_or(unsigned long e)
{
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_NOT_FOUND)
         ? PERF_ABSENT : e;
}

/* "{subgroup}\{setting}\ac" or "...\dc" */
static bool power_name(const char *name, GUID *sub, GUID *set, bool *dc)
{
    wchar_t w[PERF_NAME_MAX];
    widen(name, w, PERF_NAME_MAX);
    wchar_t *a = wcschr(w, L'\\'), *b = a ? wcschr(a + 1, L'\\') : nullptr;
    if (!b) return false;
    *a = *b = L'\0';

    wc_guid gs, gt;
    if (!wcw_guid_from_string(w, &gs) || !wcw_guid_from_string(a + 1, &gt)) return false;
    memcpy(sub, gs.b, sizeof *sub);
    memcpy(set, gt.b, sizeof *set);
    *dc = _wcsicmp(b + 1, L"dc") == 0;
    return *dc || _wcsicmp(b + 1, L"ac") == 0;
}

static unsigned long b_read([[maybe_unused]] void *ctx, perf_kind k, const wc_guid *owner,
                            const char *name, const char *target, char *live, int cap,
                            int *has_target)
{
    *has_target = 0;
    if (k == PERF_DEVICE) {
        char state[PERF_VALUE_MAX];
        unsigned long e = tune_wfd_get(owner, state, sizeof state);
        if (e) return absent_or(e);
        if (!state[0]) return PERF_ABSENT;
        /* However many there are, all of them off is the one target. */
        *has_target = 1;
        snprintf(live, (size_t)cap, "%s", strchr(state, '1') ? state : target);
        return WC_OK;
    }
    if (k == PERF_DRIVER) {
        wchar_t kw[PERF_NAME_MAX], want[PERF_VALUE_MAX], got[PERF_VALUE_MAX];
        widen(name, kw, PERF_NAME_MAX);
        widen(target, want, PERF_VALUE_MAX);
        bool has = false;
        unsigned long e = tune_driver_get(owner, kw, want, got, PERF_VALUE_MAX, &has);
        if (e) return absent_or(e);
        *has_target = has;
        narrow(got, live, cap);
        return WC_OK;
    }

    GUID scheme, sub, set;
    bool dc;
    if (!power_name(name, &sub, &set, &dc)) return PERF_ABSENT;
    memcpy(&scheme, owner->b, sizeof scheme);

    DWORD idx = 0;
    DWORD e = dc ? PowerReadDCValueIndex(nullptr, &scheme, &sub, &set, &idx)
                 : PowerReadACValueIndex(nullptr, &scheme, &sub, &set, &idx);
    if (e) return absent_or(e);

    /* Only whether the choice exists matters; tune_power.c explains the size. */
    ULONG type = 0;
    BYTE value[64];
    DWORD bytes = sizeof value;
    e = PowerReadPossibleValue(nullptr, &sub, &set, &type, (ULONG)strtoul(target, nullptr, 10),
                               value, &bytes);
    *has_target = (e == ERROR_SUCCESS || e == ERROR_MORE_DATA);
    snprintf(live, (size_t)cap, "%lu", (unsigned long)idx);
    return WC_OK;
}

static unsigned long b_write([[maybe_unused]] void *ctx, perf_kind k, const wc_guid *owner,
                             const char *name, const char *value)
{
    if (k == PERF_DEVICE) {
        /* Recorded by Windows either way, and read back as done. */
        const unsigned long e = tune_wfd_set(owner, value);
        return e == ERROR_SUCCESS_REBOOT_REQUIRED ? WC_OK : e;
    }
    if (k == PERF_DRIVER) {
        wchar_t kw[PERF_NAME_MAX], raw[PERF_VALUE_MAX];
        widen(name, kw, PERF_NAME_MAX);
        widen(value, raw, PERF_VALUE_MAX);
        return tune_driver_set(owner, kw, raw);
    }

    GUID scheme, sub, set;
    bool dc;
    if (!power_name(name, &sub, &set, &dc)) return PERF_ABSENT;
    memcpy(&scheme, owner->b, sizeof scheme);
    const DWORD idx = (DWORD)strtoul(value, nullptr, 10);
    return dc ? PowerWriteDCValueIndex(nullptr, &scheme, &sub, &set, idx)
              : PowerWriteACValueIndex(nullptr, &scheme, &sub, &set, idx);
}

static unsigned long b_scheme([[maybe_unused]] void *ctx, wc_guid *active)
{
    GUID *g = nullptr;
    DWORD e = PowerGetActiveScheme(nullptr, &g);
    if (e || !g) return e ? e : ERROR_NOT_FOUND;
    memcpy(active->b, g, sizeof active->b);
    LocalFree(g);
    return WC_OK;
}

static unsigned long b_commit([[maybe_unused]] void *ctx)
{
    return tune_power_commit();
}

static unsigned long b_restart([[maybe_unused]] void *ctx, const wc_guid *adapter)
{
    return tune_driver_restart(adapter);
}

/* ---------------------------------------------------------------- journal */

static const wchar_t *const KIND[] = { [PERF_DRIVER] = L"driver", [PERF_POWER] = L"power",
                                        [PERF_DEVICE] = L"device" };

/* "driver\{owner}\keyword", "power\{owner}\{subgroup}\{setting}\ac",
 * "device\{owner}\WiFiDirect" */
static void key_of(const perf_entry *e, wchar_t *out, int cap)
{
    wchar_t owner[40], name[PERF_NAME_MAX];
    wcw_guid_to_string(&e->owner, owner, 40);
    widen(e->name, name, PERF_NAME_MAX);
    _snwprintf(out, (size_t)cap, L"%s\\%s\\%s", KIND[e->kind], owner, name);
    out[cap - 1] = L'\0';
}

static int b_held(void *ctx, perf_entry *out, int cap)
{
    enum { BUF = 32767 };
    wchar_t *buf = malloc(BUF * sizeof *buf);
    if (!buf) return 0;
    const DWORD len = GetPrivateProfileSectionW(SECTION, buf, BUF, (const wchar_t *)ctx);

    int n = 0;
    for (const wchar_t *p = buf; len && *p && n < cap; p += wcslen(p) + 1) {
        const wchar_t *eq = wcschr(p, L'=');
        const wchar_t *s1 = wcschr(p, L'\\');
        const wchar_t *s2 = s1 ? wcschr(s1 + 1, L'\\') : nullptr;
        if (!eq || !s2 || s2 > eq) continue;

        perf_entry *e = &out[n];
        memset(e, 0, sizeof *e);
        const size_t klen = (size_t)(s1 - p), glen = (size_t)(s2 - s1 - 1),
                     nlen = (size_t)(eq - s2 - 1);
        int kind = (int)(sizeof KIND / sizeof KIND[0]) - 1;
        while (kind >= 0 && !(wcslen(KIND[kind]) == klen && !wcsncmp(p, KIND[kind], klen))) kind--;
        if (kind < 0 || glen >= 40 || nlen >= PERF_NAME_MAX) continue;
        e->kind = (perf_kind)kind;

        wchar_t owner[40], name[PERF_NAME_MAX];
        wmemcpy(owner, s1 + 1, glen);
        owner[glen] = L'\0';
        wmemcpy(name, s2 + 1, nlen);
        name[nlen] = L'\0';
        if (!wcw_guid_from_string(owner, &e->owner)) continue;
        narrow(name, e->name, PERF_NAME_MAX);
        narrow(eq + 1, e->value, PERF_VALUE_MAX);
        n++;
    }
    free(buf);
    return n;
}

static unsigned long b_keep(void *ctx, const perf_entry *e)
{
    wchar_t key[KEY_MAX], value[PERF_VALUE_MAX];
    key_of(e, key, KEY_MAX);
    widen(e->value, value, PERF_VALUE_MAX);
    return WritePrivateProfileStringW(SECTION, key, value, (const wchar_t *)ctx)
         ? WC_OK : GetLastError();
}

static unsigned long b_forget(void *ctx, const perf_entry *e)
{
    wchar_t key[KEY_MAX];
    key_of(e, key, KEY_MAX);
    if (WritePrivateProfileStringW(SECTION, key, nullptr, (const wchar_t *)ctx)) return WC_OK;
    const DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND ? WC_OK : err;
}

/* Whether the journal can be written at all.  Nothing is created: an existing
 * record is opened for writing and closed again, and for a first run it is the
 * folder that is tested, with a file that deletes itself on close.  Without
 * this the switch would hold values it could not put back. */
unsigned long perf_win32_journal_check(const wchar_t *journal)
{
    if (!journal || !journal[0]) return ERROR_PATH_NOT_FOUND;

    if (GetFileAttributesW(journal) != INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(journal, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return GetLastError();
        CloseHandle(h);
        return WC_OK;
    }

    wchar_t probe[MAX_PATH];
    _snwprintf(probe, MAX_PATH, L"%s.probe", journal);
    probe[MAX_PATH - 1] = L'\0';
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(h);
    return WC_OK;
}

/* Whether there is an active power plan to hold anything in. */
unsigned long perf_win32_power_check(void)
{
    GUID *g = nullptr;
    DWORD e = PowerGetActiveScheme(nullptr, &g);
    if (e) return e;
    if (!g) return ERROR_NOT_FOUND;
    LocalFree(g);
    return WC_OK;
}

void perf_win32_backend(perf_backend *be, const wchar_t *journal)
{
    *be = (perf_backend){
        .ctx = (void *)journal,
        .read = b_read, .write = b_write, .scheme = b_scheme,
        .commit = b_commit, .restart = b_restart,
        .held = b_held, .keep = b_keep, .forget = b_forget,
    };
}

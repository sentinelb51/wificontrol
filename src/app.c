/* app.c -- WiFi Control's process: config, the icon, the worker thread and
 * startup.  The window itself is app_ui.c.
 *
 * Threading: the UI thread owns the window, the tray icon and the config file.
 * A worker thread owns the WLAN client handle and the policy core, because
 * WlanSetInterface blocks for roughly a second per call and must never run on
 * the UI thread.  They talk only by posted messages, so there are no locks.
 */
#define WIN32_LEAN_AND_MEAN
#include "app.h"
#include "perf_win32.h"
#include "tune.h"
#include "ui.h"

#include <commctrl.h>
#include <process.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#define T_DEBOUNCE 1
#define T_WATCHDOG 2

#define DEBOUNCE_MS  250
#define MIN_POLL_MS 1000
#define WATCHDOG_MS 60000

typedef struct { wc_guid g; bool managed; } cfg_entry;
typedef struct {
    bool bgscan_off, streaming_on, dark, metered, perf;
    int n;
    cfg_entry e[WC_MAX_ADAPTERS];
} cfg;

/* ----------------------------------------------------------------- config */

static wchar_t g_cfg_path[MAX_PATH];
static wchar_t g_journal_path[MAX_PATH];

/* The Performance switch's record of what it changed, beside the config. */
static void journal_resolve_path(void)
{
    wcscpy(g_journal_path, g_cfg_path);
    wchar_t *slash = wcsrchr(g_journal_path, L'\\');
    if (slash) *slash = L'\0';
    wcsncat(g_journal_path, L"\\wificontrol-restore.ini",
            MAX_PATH - wcslen(g_journal_path) - 1);
}

/* Portable when an ini already sits next to the exe, per-user otherwise. */
static void cfg_resolve_path(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = L'\0';
            _snwprintf(g_cfg_path, MAX_PATH, L"%s\\wificontrol.ini", exe);
            g_cfg_path[MAX_PATH - 1] = L'\0';
            if (GetFileAttributesW(g_cfg_path) != INVALID_FILE_ATTRIBUTES) return;
        }
    }
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        _snwprintf(g_cfg_path, MAX_PATH, L"%s\\WifiControl", base);
        g_cfg_path[MAX_PATH - 1] = L'\0';
        CreateDirectoryW(g_cfg_path, NULL);
        wcsncat(g_cfg_path, L"\\wificontrol.ini", MAX_PATH - wcslen(g_cfg_path) - 1);
    }
}

static void cfg_load(cfg *c)
{
    memset(c, 0, sizeof *c);
    /* The first two used to be the one Optimise switch, saved as "enabled". */
    const UINT both = GetPrivateProfileIntW(L"general", L"enabled", 1, g_cfg_path);
    c->bgscan_off   = GetPrivateProfileIntW(L"general", L"no_background_scan", both, g_cfg_path) != 0;
    c->streaming_on = GetPrivateProfileIntW(L"general", L"streaming_mode", both, g_cfg_path) != 0;
    c->metered      = GetPrivateProfileIntW(L"general", L"metered", 1, g_cfg_path) != 0;
    c->perf         = GetPrivateProfileIntW(L"general", L"performance", 0, g_cfg_path) != 0;
    c->dark         = GetPrivateProfileIntW(L"general", L"dark", 1, g_cfg_path) != 0;

    /* Adapters absent from the file default to managed. */
    wchar_t buf[4096];
    DWORD n = GetPrivateProfileSectionW(L"adapters", buf, 4096, g_cfg_path);
    if (!n || n >= 4094) return;

    for (const wchar_t *p = buf; *p && c->n < WC_MAX_ADAPTERS; p += wcslen(p) + 1) {
        const wchar_t *eq = wcschr(p, L'=');
        if (!eq) continue;
        wchar_t key[64];
        size_t klen = (size_t)(eq - p);
        if (klen >= 64) continue;
        memcpy(key, p, klen * sizeof(wchar_t));
        key[klen] = L'\0';
        if (!wcw_guid_from_string(key, &c->e[c->n].g)) continue;
        c->e[c->n].managed = (_wtoi(eq + 1) != 0);
        c->n++;
    }
}

void cfg_save_bgscan_off(bool on)
{
    WritePrivateProfileStringW(L"general", L"no_background_scan", on ? L"1" : L"0", g_cfg_path);
}

void cfg_save_streaming_on(bool on)
{
    WritePrivateProfileStringW(L"general", L"streaming_mode", on ? L"1" : L"0", g_cfg_path);
}

void cfg_save_dark(bool on)
{
    WritePrivateProfileStringW(L"general", L"dark", on ? L"1" : L"0", g_cfg_path);
}

void cfg_save_metered(bool on)
{
    WritePrivateProfileStringW(L"general", L"metered", on ? L"1" : L"0", g_cfg_path);
}

void cfg_save_perf(bool on)
{
    WritePrivateProfileStringW(L"general", L"performance", on ? L"1" : L"0", g_cfg_path);
}

void cfg_save_managed(const wc_guid *g, bool on)
{
    wchar_t key[64];
    wcw_guid_to_string(g, key, 64);
    WritePrivateProfileStringW(L"adapters", key, on ? L"1" : L"0", g_cfg_path);
}

/* ------------------------------------------------------------------- icon */

/* Drawn into a DIB rather than shipped as a .ico: it costs less code than a
 * binary asset, comes out crisp at whatever size the shell asks for, and lets
 * the tray colour carry the state.
 *
 * The glyph is a dot in the bottom-left corner and two quarter rings around
 * that corner, cut flat by its edges, on a transparent background. */
static void raster(unsigned char *px, int size, COLORREF c, double fill)
{
    /* Whole pixels throughout: the corner sits on a pixel boundary, so each
     * cut end of a ring is crisp at 16 px rather than half-covered. */
    const int g = (int)(size * fill + 0.5), m = (size - g) / 2;   /* glyph, margin */
    const int t = (int)(g * 0.115 + 0.5), r1 = (int)(g * 0.66 + 0.5);
    const double left = m, bottom = m + g;
    const double dot = t;   /* radius: twice a ring's width across, touching both edges */
    const double ring[2][2] = { { r1 - t, r1 }, { g - t, g } };
    constexpr int S = 8;  /* supersampling factor */

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < S; ++sy) {
                for (int sx = 0; sx < S; ++sx) {
                    double gx = x + (sx + 0.5) / S - left;
                    double gy = bottom - (y + (sy + 0.5) / S);
                    if (gx < 0 || gy < 0) continue;
                    double d = sqrt(gx * gx + gy * gy);
                    double ex = gx - dot, ey = gy - dot;
                    if (ex * ex + ey * ey <= dot * dot ||
                        (d >= ring[0][0] && d <= ring[0][1]) ||
                        (d >= ring[1][0] && d <= ring[1][1]))
                        hits++;
                }
            }
            double a = (double)hits / (S * S);

            unsigned char *p = px + ((size_t)y * (size_t)size + (size_t)x) * 4;
            p[0] = (unsigned char)(GetBValue(c) * a + 0.5);   /* premultiplied BGRA */
            p[1] = (unsigned char)(GetGValue(c) * a + 0.5);
            p[2] = (unsigned char)(GetRValue(c) * a + 0.5);
            p[3] = (unsigned char)(a * 255.0 + 0.5);
        }
    }
}

HICON make_icon(int size, COLORREF c, double fill)
{
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.bV5Size        = sizeof bi;
    bi.bV5Width       = size;
    bi.bV5Height      = -size;              /* top-down */
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) { if (color) DeleteObject(color); return NULL; }

    raster(bits, size, c, fill);

    size_t mask_bytes = (size_t)(((size + 31) / 32) * 4) * (size_t)size;
    void *zero = calloc(1, mask_bytes);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zero);
    free(zero);

    ICONINFO ii;
    ii.fIcon    = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask  = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

/* ----------------------------------------------------------------- worker */

HWND             g_ui;
HWND             g_worker;
UINT             g_msg_show;
static HANDLE    g_worker_ready;
static HANDLE    g_worker_thread;
static wc_win32  g_win32;
static wc_state  g_core;
static cfg       g_boot_cfg;
static DWORD     g_last_poll;
static unsigned long g_open_err;

static perf_backend g_perf_be;
static unsigned long (*g_perf_restart)(void *ctx, const wc_guid *adapter);
static bool         g_perf;      /* the switch, as last sent by the window */
static bool         g_perf_busy;
static unsigned     g_seq;       /* the window's newest switch change handled */

/* Everything that has failed since the last check, and what this machine can
 * do at all.  Both are the worker's, and the window gets a copy of each with
 * every snapshot -- so there is still nothing to lock. */
static diag_log     g_diag;
static wc_cap_state g_cap[WC_CAP_COUNT];

static void note_core([[maybe_unused]] void *ctx, diag_op op, diag_step step,
                      const char *subject, unsigned long code)
{
    diag_note(&g_diag, op, step, subject, code, GetTickCount());
}

/* The Performance switch reports a setting and the adapter or plan it sits on;
 * the window shows those in words, never a GUID. */
static void note_perf([[maybe_unused]] void *ctx, diag_op op, diag_step step,
                      const wc_guid *owner, const char *name, unsigned long code)
{
    char subject[DIAG_SUBJ_MAX] = "";
    const char *label = name ? perf_label(name) : nullptr;
    const int i = owner ? wc_find(&g_core, owner) : -1;

    if (label && i >= 0) snprintf(subject, sizeof subject, "%s on %s", label, g_core.ad[i].name);
    else if (label)      snprintf(subject, sizeof subject, "%s", label);
    else if (i >= 0)     snprintf(subject, sizeof subject, "%s", g_core.ad[i].name);
    diag_note(&g_diag, op, step, subject, code, GetTickCount());
}

static void cap_set(wc_cap c, unsigned long err)
{
    g_cap[c] = (wc_cap_state){ .ok = (err == WC_OK), .checked = true, .err = err };
}

/* What can be done on this machine at all, so a switch whose service is not
 * here is greyed out with a reason instead of failing when it is flipped.
 * Runs at startup and on every Refresh, never on the poll: each check costs a
 * registry walk or a file open. */
static void worker_probe_caps(void)
{
    cap_set(WC_CAP_WLAN, g_open_err);

    /* The three opcodes: only meaningful once there is a handle to ask about,
     * and advisory even then -- see wc_probe_access. */
    if (g_open_err == WC_OK) {
        static const wc_cap by_opt[WC_OPT_COUNT] = {
            [WC_OPT_STREAMING] = WC_CAP_STREAMING,
            [WC_OPT_BGSCAN]    = WC_CAP_BGSCAN,
            [WC_OPT_AUTOCONF]  = WC_CAP_AUTOCONF,
        };
        for (int o = 0; o < WC_OPT_COUNT; ++o)
            cap_set(by_opt[o], g_core.can_write[o] ? WC_OK : WC_E_ACCESS_DENIED);
    }

    cap_set(WC_CAP_COST, wcw_cost_available());
    cap_set(WC_CAP_JOURNAL, perf_win32_journal_check(g_journal_path));
    cap_set(WC_CAP_POWER, perf_win32_power_check());

    /* One present adapter with a writable driver key is enough; with no
     * adapter at all there is nothing to say yet. */
    unsigned long driver = ERROR_FILE_NOT_FOUND;
    for (int i = 0; i < g_core.n; ++i) {
        if (!g_core.ad[i].present) continue;
        driver = tune_driver_check(&g_core.ad[i].guid);
        if (driver == WC_OK) break;
    }
    cap_set(WC_CAP_DRIVER, driver);
}

static void worker_send_snapshot(void)
{
    snapshot *s = calloc(1, sizeof *s);
    if (!s) return;

    s->n          = g_core.n;
    s->bgscan_off   = g_core.bgscan_off;
    s->streaming_on = g_core.streaming_on;
    s->nuclear    = g_core.nuclear;
    s->metered    = g_core.metered;
    s->perf       = g_perf;
    s->perf_busy  = g_perf_busy;
    s->recovered  = g_core.recovered;
    s->write_denied = wc_write_denied(&g_core);
    s->enum_err   = g_core.enum_err;
    s->open_err   = g_open_err;
    s->seq        = g_seq;
    s->diag       = g_diag;
    memcpy(s->cap, g_cap, sizeof s->cap);

    for (int i = 0; i < g_core.n; ++i) {
        const wc_adapter *a = &g_core.ad[i];
        snap_row *r = &s->row[i];
        r->guid      = a->guid;
        r->state     = a->state;
        r->present   = a->present;
        r->managed   = a->managed;
        r->pending   = a->pending;
        r->streaming = a->streaming;
        r->bgscan    = a->bgscan;
        r->autoconf  = a->autoconf;
        r->profiles  = a->profiles;
        r->metered   = a->metered;
        r->last_err  = a->last_err;
        MultiByteToWideChar(CP_UTF8, 0, a->name, -1, r->name, WC_NAME_MAX);
        r->name[WC_NAME_MAX - 1] = L'\0';
    }
    if (!PostMessageW(g_ui, WM_U_SNAPSHOT, 0, (LPARAM)s)) free(s);
}

/* A restart takes seconds and drops the link, so the window says so first. */
static unsigned long worker_restart(void *ctx, const wc_guid *adapter)
{
    g_perf_busy = true;
    worker_send_snapshot();
    unsigned long e = g_perf_restart(ctx, adapter);
    g_perf_busy = false;
    return e;
}

/* In force: the switch on, and per adapter, managed and present.  Run on
 * transitions only, never from the poll. */
static void worker_perf(bool restart)
{
    /* Nothing is held without somewhere to record what it was.  Whatever was
     * recorded by an earlier run is still put back, which is the one thing
     * left worth doing. */
    if (g_perf && !wc_cap_ok(g_cap, WC_CAP_JOURNAL)) {
        diag_note(&g_diag, DIAG_PERF_JOURNAL, DIAG_RECORD, nullptr, WC_E_NO_JOURNAL,
                  GetTickCount());
        g_perf = false;
    }

    perf_adapter ad[WC_MAX_ADAPTERS];
    for (int i = 0; i < g_core.n; ++i) {
        const wc_adapter *a = &g_core.ad[i];
        ad[i] = (perf_adapter){ a->guid, g_perf && a->managed && a->present, a->present };
    }
    /* The counts it returns are shown nowhere: what the window needs from a
     * pass is the log the note sink fills as it goes. */
    (void)perf_sync(&g_perf_be, g_perf, ad, g_core.n, restart);
}

static void worker_poll(void)
{
    g_last_poll = GetTickCount();
    if (g_win32.h) wc_poll(&g_core);
    worker_send_snapshot();
}

/* Coalesce bursts, and never poll more than once a second no matter how
 * chatty the notification stream gets. */
static void worker_schedule(HWND hwnd)
{
    DWORD since = GetTickCount() - g_last_poll;
    UINT delay = DEBOUNCE_MS;
    if (since < MIN_POLL_MS && MIN_POLL_MS - since > DEBOUNCE_MS)
        delay = MIN_POLL_MS - since;
    SetTimer(hwnd, T_DEBOUNCE, delay, NULL);
}

static void CALLBACK acm_callback([[maybe_unused]] PWLAN_NOTIFICATION_DATA data,
                                  [[maybe_unused]] PVOID ctx)
{
    /* Runs on a wlanapi thread: post and return, never call back into the API
     * and never block. */
    if (g_worker) PostMessageW(g_worker, WM_W_POLL, 0, 0);
}

/* Open the client handle and bring the core up on it: the saved switches
 * first, then the first apply pass, then the notifications.  Called again by
 * Refresh, so a service that was not running at startup is picked up without
 * restarting the app. */
static void worker_start_wlan(void)
{
    if (g_win32.h) return;

    wc_backend be;
    memset(&be, 0, sizeof be);
    g_open_err = wcw_open(&g_win32, &be);
    if (g_open_err != WC_OK) return;

    /* Whatever the switches are set to now: on a retry the user may have
     * moved them while the service was away. */
    const bool bgscan = g_core.bgscan_off, streaming = g_core.streaming_on,
               metered = g_core.metered;
    wc_init(&g_core, &be);
    wc_set_note(&g_core, note_core, nullptr);
    wc_probe_access(&g_core);

    /* Enumerate and apply the saved choices *before* the first apply pass,
     * so an adapter the user unchecked is never briefly optimised. */
    wc_refresh(&g_core);
    g_core.bgscan_off   = bgscan;
    g_core.streaming_on = streaming;
    g_core.metered      = metered;
    for (int i = 0; i < g_boot_cfg.n; ++i) {
        int j = wc_find(&g_core, &g_boot_cfg.e[i].g);
        if (j >= 0) g_core.ad[j].managed = g_boot_cfg.e[i].managed;
    }
    wc_apply_all(&g_core);
    g_last_poll = GetTickCount();

    wcw_register(&g_win32, acm_callback, NULL);
    SetTimer(g_worker, T_WATCHDOG, WATCHDOG_MS, NULL);
}

static LRESULT CALLBACK worker_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_W_POLL:
        worker_schedule(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == T_DEBOUNCE) { KillTimer(hwnd, T_DEBOUNCE); worker_poll(); }
        else if (wp == T_WATCHDOG) worker_poll();
        return 0;

    case WM_W_BGSCAN:
        g_seq = (unsigned)lp;
        wc_set_bgscan_off(&g_core, wp != 0);
        worker_send_snapshot();
        return 0;

    case WM_W_STREAMING:
        g_seq = (unsigned)lp;
        wc_set_streaming_on(&g_core, wp != 0);
        worker_send_snapshot();
        return 0;

    /* The count goes first: a restart sends a snapshot part way through. */
    case WM_W_PERF:
        g_seq = (unsigned)lp;
        g_perf = wp != 0;
        worker_perf(true);
        worker_send_snapshot();
        return 0;

    case WM_W_PLAN:
        /* Another plan is active: hold it instead, and give the old one back. */
        (void)perf_sync_power(&g_perf_be, g_perf);
        worker_send_snapshot();
        return 0;

    case WM_W_PERF_END:
        /* The reboot applies the driver values, so no restart; the next start
         * holds them again from the saved switch.  Off in memory only, so a
         * late plan change cannot hold anything again on the way down. */
        g_perf = false;
        (void)perf_sync(&g_perf_be, false, nullptr, 0, false);
        return 0;

    case WM_W_NUCLEAR:
        wc_set_nuclear(&g_core, (int)wp);
        worker_send_snapshot();
        return 0;

    case WM_W_METERED: {
        g_seq = (unsigned)lp;
        /* A cost cannot be written on this machine at all: say so rather than
         * showing the switch on while every network is left alone. */
        const bool on = wp != 0 && wc_cap_ok(g_cap, WC_CAP_COST);
        if (wp && !on)
            diag_note(&g_diag, DIAG_METERED, DIAG_WRITE, nullptr, g_cap[WC_CAP_COST].err,
                      GetTickCount());
        wc_set_metered(&g_core, on);
        worker_send_snapshot();
        return 0;
    }

    /* Refresh: everything is looked at again, including what is available,
     * and the failure log starts over so it describes this check and not one
     * from ten minutes ago. */
    case WM_W_RECHECK:
        diag_clear(&g_diag);
        worker_start_wlan();
        if (g_win32.h) wc_probe_access(&g_core);
        worker_probe_caps();
        worker_poll();
        return 0;

    case WM_W_MANAGE: {
        manage_cmd *c = (manage_cmd *)lp;
        if (c) {
            g_seq = c->seq;
            int i = wc_find(&g_core, &c->g);
            if (i >= 0) wc_set_managed(&g_core, i, c->on);
            free(c);
        }
        worker_perf(true);
        worker_send_snapshot();
        return 0;
    }

    case WM_W_QUIT:
        KillTimer(hwnd, T_DEBOUNCE);
        KillTimer(hwnd, T_WATCHDOG);
        /* Auto config and a metered cost do not come back on their own, so
         * switch both off and re-apply while the handle is still open.  The
         * other two settings need no such help: closing the handle hands them
         * straight back. */
        if (g_win32.h) {
            g_core.nuclear = false;
            g_core.metered = false;
            wc_poll(&g_core);
        }
        /* After auto config is back, so the restarted adapter reconnects. */
        g_perf = false;
        worker_perf(true);
        wcw_unregister(&g_win32);
        wcw_close(&g_win32);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static unsigned __stdcall worker_main([[maybe_unused]] void *param)
{

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = worker_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"WifiControlWorker";
    RegisterClassExW(&wc);

    g_worker = CreateWindowExW(0, L"WifiControlWorker", NULL, 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!g_worker) { SetEvent(g_worker_ready); return 1; }

    /* The core holds the saved switches whether or not a handle can be opened,
     * so the window shows what is asked for rather than nothing, and a later
     * Refresh applies it if the service turns up.  With no adapters it makes
     * no backend calls. */
    wc_backend none;
    memset(&none, 0, sizeof none);
    wc_init(&g_core, &none);
    wc_set_note(&g_core, note_core, nullptr);
    g_core.bgscan_off   = g_boot_cfg.bgscan_off;
    g_core.streaming_on = g_boot_cfg.streaming_on;
    /* Checked before the first apply pass rather than after it: a saved
     * switch whose service is not here never comes on, so the pass does not
     * spend itself failing on every saved network.  The window greys the
     * switch out and the details window says why. */
    cap_set(WC_CAP_COST, wcw_cost_available());
    g_core.metered      = g_boot_cfg.metered && wc_cap_ok(g_cap, WC_CAP_COST);

    perf_win32_backend(&g_perf_be, g_journal_path);
    g_perf_restart = g_perf_be.restart;
    g_perf_be.restart = worker_restart;
    g_perf_be.note    = note_perf;
    g_perf = g_boot_cfg.perf; /* so the first snapshot shows the saved switch */

    worker_start_wlan();
    worker_probe_caps();
    worker_send_snapshot();

    /* Hold the saved switch, or repair a run that was killed while holding.
     * Queued rather than run here: a restart must not delay the window.  Queued
     * before the window can show, so it lands ahead of anything the user does. */
    PostMessageW(g_worker, WM_W_PERF, (WPARAM)g_boot_cfg.perf, 0);
    SetEvent(g_worker_ready);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(g_worker);
    g_worker = NULL;
    return 0;
}

/* ------------------------------------------------------------------- main */

int WINAPI wWinMain(HINSTANCE inst, [[maybe_unused]] HINSTANCE prev,
                    LPWSTR cmdline, [[maybe_unused]] int show)
{

    g_msg_show = RegisterWindowMessageW(L"WifiControl.Show");

    HANDLE once = CreateMutexW(NULL, FALSE, L"Local\\WifiControl.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, g_msg_show, 0, 0);
        CloseHandle(once);
        return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    cfg_resolve_path();
    journal_resolve_path();
    cfg_load(&g_boot_cfg);
    ui_set_theme(g_boot_cfg.dark);

    HWND dlg = main_window_create(inst);
    if (!dlg) { if (once) CloseHandle(once); return 1; }

    g_worker_ready  = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_worker_thread = (HANDLE)_beginthreadex(NULL, 0, worker_main, NULL, 0, NULL);
    if (g_worker_ready) WaitForSingleObject(g_worker_ready, 10000);

    /* Start hidden when asked for on the command line, otherwise show. */
    main_window_start(dlg, wcsstr(cmdline ? cmdline : L"", L"/tray") != nullptr);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_worker) PostMessageW(g_worker, WM_W_QUIT, 0, 0);
    if (g_worker_thread) {
        /* Long enough to restart an adapter: returning early would end the
         * process between disabling it and enabling it again. */
        WaitForSingleObject(g_worker_thread, 30000);
        CloseHandle(g_worker_thread);
    }
    if (g_worker_ready) CloseHandle(g_worker_ready);
    main_window_cleanup();
    if (once) CloseHandle(once);
    return 0;
}

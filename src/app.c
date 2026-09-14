/* app.c -- Win32 front end for WiFi Control.
 *
 * Threading: the UI thread owns the window, the tray icon and the config file.
 * A worker thread owns the WLAN client handle and the policy core, because
 * WlanSetInterface blocks for roughly a second per call and must never run on
 * the UI thread.  They talk only by posted messages, so there are no locks.
 */
#define WIN32_LEAN_AND_MEAN
#include "wlan_win32.h"
#include "tune.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <process.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef LVS_EX_DOUBLEBUFFER
#define LVS_EX_DOUBLEBUFFER 0x00010000
#endif

/* UI thread messages */
#define WM_U_SNAPSHOT (WM_APP + 1)
#define WM_U_TRAY     (WM_APP + 2)

/* worker thread messages */
#define WM_W_POLL     (WM_APP + 10)
#define WM_W_ENABLE   (WM_APP + 11)
#define WM_W_MANAGE   (WM_APP + 12)
#define WM_W_QUIT     (WM_APP + 13)
#define WM_W_NUCLEAR  (WM_APP + 14)

#define T_DEBOUNCE 1
#define T_WATCHDOG 2
#define T_DEADMAN  3

#define DEBOUNCE_MS  250
#define MIN_POLL_MS 1000
#define WATCHDOG_MS 60000

/* ------------------------------------------------------------------ types */

typedef struct {
    wc_guid       guid;
    wchar_t       name[WC_NAME_MAX];
    wc_ifstate    state;
    bool          present, managed, pending;
    wc_val        streaming, bgscan, autoconf;
    unsigned long last_err;
} snap_row;

typedef struct {
    int           n;
    bool          enabled, write_denied, nuclear;
    int           recovered;
    unsigned long enum_err, open_err;
    snap_row      row[WC_MAX_ADAPTERS];
} snapshot;

typedef struct { wc_guid g; bool managed; } cfg_entry;
typedef struct { bool enabled; int n; cfg_entry e[WC_MAX_ADAPTERS]; } cfg;

typedef struct { wc_guid g; bool on; } manage_cmd;

/* ----------------------------------------------------------------- config */

static wchar_t g_cfg_path[MAX_PATH];

static void guid_to_str(const wc_guid *g, wchar_t *buf, int cap)
{
    wcw_guid_to_string(g, buf, cap);
}

static bool str_to_guid(const wchar_t *s, wc_guid *g)
{
    unsigned int d1, d2, d3, b[8];
    if (swscanf(s, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    GUID out;
    out.Data1 = (unsigned long)d1;
    out.Data2 = (unsigned short)d2;
    out.Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; ++i) out.Data4[i] = (unsigned char)b[i];
    memcpy(g->b, &out, sizeof g->b);
    return true;
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
    c->enabled = GetPrivateProfileIntW(L"general", L"enabled", 1, g_cfg_path) != 0;

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
        if (!str_to_guid(key, &c->e[c->n].g)) continue;
        c->e[c->n].managed = (_wtoi(eq + 1) != 0);
        c->n++;
    }
}

static void cfg_save_enabled(bool on)
{
    WritePrivateProfileStringW(L"general", L"enabled", on ? L"1" : L"0", g_cfg_path);
}

static void cfg_save_managed(const wc_guid *g, bool on)
{
    wchar_t key[64];
    guid_to_str(g, key, 64);
    WritePrivateProfileStringW(L"adapters", key, on ? L"1" : L"0", g_cfg_path);
}

/* ------------------------------------------------------------------- icon */

/* Drawn into a DIB rather than shipped as a .ico: it costs less code than a
 * binary asset, comes out crisp at whatever size the shell asks for, and lets
 * the tray colour carry the state. */
static void raster(unsigned char *px, int size, COLORREF c)
{
    const double R = size * 0.5 - 0.5, cx = size * 0.5, cy = size * 0.5;
    const double ox = cx, oy = cy + size * 0.26;
    const double dotr = size * 0.085, halfw = size * 0.055;
    const double rad[3] = { size * 0.20, size * 0.33, size * 0.46 };
    constexpr int S = 4;  /* supersampling factor */
    const double cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int disc = 0, glyph = 0;
            for (int sy = 0; sy < S; ++sy) {
                for (int sx = 0; sx < S; ++sx) {
                    double fx = x + (sx + 0.5) / S, fy = y + (sy + 0.5) / S;
                    double dx = fx - cx, dy = fy - cy;
                    if (dx * dx + dy * dy <= R * R) disc++;

                    double gx = fx - ox, gy = fy - oy;
                    double d = sqrt(gx * gx + gy * gy);
                    int hit = (d <= dotr);
                    if (!hit && gy < 0) {
                        double ang = atan2(-gy, gx);           /* 0..pi upward */
                        if (ang > 0.60 && ang < 2.54) {
                            for (int k = 0; k < 3; ++k)
                                if (fabs(d - rad[k]) <= halfw) { hit = 1; break; }
                        }
                    }
                    if (hit) glyph++;
                }
            }
            double da = (double)disc / (S * S);
            double ga = (double)glyph / (S * S);
            if (ga > da) ga = da;
            double k = (da > 0.0) ? ga / da : 0.0;

            unsigned char *p = px + ((size_t)y * (size_t)size + (size_t)x) * 4;
            double b = cb + (255.0 - cb) * k;
            double g = cg + (255.0 - cg) * k;
            double r = cr + (255.0 - cr) * k;
            p[0] = (unsigned char)(b * da + 0.5);   /* premultiplied BGRA */
            p[1] = (unsigned char)(g * da + 0.5);
            p[2] = (unsigned char)(r * da + 0.5);
            p[3] = (unsigned char)(da * 255.0 + 0.5);
        }
    }
}

static HICON make_icon(int size, COLORREF c)
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

    raster(bits, size, c);

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

static HWND      g_ui;
static HWND      g_worker;
static HANDLE    g_worker_ready;
static HANDLE    g_worker_thread;
static wc_win32  g_win32;
static wc_state  g_core;
static cfg       g_boot_cfg;
static DWORD     g_last_poll;
static unsigned long g_open_err;

static void worker_send_snapshot(void)
{
    snapshot *s = calloc(1, sizeof *s);
    if (!s) return;

    s->n          = g_core.n;
    s->enabled    = g_core.enabled;
    s->nuclear    = g_core.nuclear;
    s->recovered  = g_core.recovered;
    s->write_denied = wc_write_denied(&g_core);
    s->enum_err   = g_core.enum_err;
    s->open_err   = g_open_err;

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
        r->last_err  = a->last_err;
        MultiByteToWideChar(CP_UTF8, 0, a->name, -1, r->name, WC_NAME_MAX);
        r->name[WC_NAME_MAX - 1] = L'\0';
    }
    if (!PostMessageW(g_ui, WM_U_SNAPSHOT, 0, (LPARAM)s)) free(s);
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

    case WM_W_ENABLE:
        wc_set_enabled(&g_core, (int)wp);
        worker_send_snapshot();
        return 0;

    case WM_W_NUCLEAR:
        wc_set_nuclear(&g_core, (int)wp);
        worker_send_snapshot();
        return 0;

    case WM_W_MANAGE: {
        manage_cmd *c = (manage_cmd *)lp;
        if (c) {
            int i = wc_find(&g_core, &c->g);
            if (i >= 0) wc_set_managed(&g_core, i, c->on);
            free(c);
        }
        worker_send_snapshot();
        return 0;
    }

    case WM_W_QUIT:
        KillTimer(hwnd, T_DEBOUNCE);
        KillTimer(hwnd, T_WATCHDOG);
        /* Auto config does not come back on its own, so disarm and re-apply
         * while the handle is still open.  The other two settings need no
         * such help: closing the handle hands them straight back. */
        if (g_win32.h) {
            g_core.nuclear = false;
            wc_poll(&g_core);
        }
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

    wc_backend be;
    memset(&be, 0, sizeof be);
    g_open_err = wcw_open(&g_win32, &be);

    if (g_open_err == WC_OK) {
        wc_init(&g_core, &be);
        wc_probe_access(&g_core);

        /* Enumerate and apply the saved choices *before* the first apply pass,
         * so an adapter the user unchecked is never briefly optimized. */
        wc_refresh(&g_core);
        g_core.enabled = g_boot_cfg.enabled;
        for (int i = 0; i < g_boot_cfg.n; ++i) {
            int j = wc_find(&g_core, &g_boot_cfg.e[i].g);
            if (j >= 0) g_core.ad[j].managed = g_boot_cfg.e[i].managed;
        }
        wc_apply_all(&g_core);
        g_last_poll = GetTickCount();

        wcw_register(&g_win32, acm_callback, NULL);
        SetTimer(g_worker, T_WATCHDOG, WATCHDOG_MS, NULL);
    }
    worker_send_snapshot();
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

/* --------------------------------------------------------------------- UI */

static snapshot *g_snap;
static HICON     g_icon_small, g_icon_big;
static COLORREF  g_icon_color = 0;
static int       g_tray_added;
static int       g_filling;
static bool      g_list_stale;
static bool      g_nuke_confirmed;   /* asked once per session, not persisted */
static bool      g_expect_repair;    /* we just disarmed: the next repair is ours */

/* Never written to the config file.  The app always starts disarmed, which is
 * what makes the first poll repair a previous run that was killed while armed:
 * there is no stored flag to be wrong about. */
typedef struct { const wchar_t *label; UINT minutes; } nuke_span;
static const nuke_span NUKE_FOR[] = {
    { L"for 15 minutes",      15 },
    { L"for 5 minutes",        5 },
    { L"for 30 minutes",      30 },
    { L"for 1 hour",          60 },
    { L"until I turn it off",  0 },
};
static int       g_warned_tray;
static UINT      g_msg_show;
static UINT      g_msg_taskbar;
static HFONT     g_font;
static int       g_dpi = 96;
static int       g_base_dpi = 96;
static RECT      g_base[16];
static HWND      g_base_hwnd[16];
static int       g_base_n;

static COLORREF state_color(const snapshot *s)
{
    if (!s || s->open_err || s->enum_err) return RGB(200, 60, 60);
    if (s->write_denied)                  return RGB(214, 152, 32);
    if (!s->enabled)                      return RGB(128, 132, 138);
    if (s->nuclear)                       return RGB(198, 86, 26); /* armed: unmistakable */
    for (int i = 0; i < s->n; ++i)
        if (s->row[i].present && s->row[i].last_err) return RGB(214, 152, 32);
    for (int i = 0; i < s->n; ++i)
        if (s->row[i].present && s->row[i].managed &&
            (s->row[i].bgscan == WC_VAL_OFF || s->row[i].streaming == WC_VAL_ON))
            return RGB(43, 145, 72);
    return RGB(128, 132, 138);
}

static void tray_update(HWND hwnd, const wchar_t *tip)
{
    static wchar_t last_tip[128];
    COLORREF c = state_color(g_snap);

    if (g_tray_added && c == g_icon_color && wcsncmp(last_tip, tip, 127) == 0)
        return; /* nothing the shell would render differently */

    if (!g_tray_added || c != g_icon_color) {
        HICON fresh = make_icon(GetSystemMetrics(SM_CXSMICON), c);
        if (fresh) {
            HICON old = g_icon_small;
            g_icon_small = fresh;
            g_icon_color = c;
            if (old) DestroyIcon(old);
        }
    }

    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize           = sizeof nid;
    nid.hWnd             = hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_U_TRAY;
    nid.hIcon            = g_icon_small;
    wcsncpy(nid.szTip, tip, sizeof nid.szTip / sizeof nid.szTip[0] - 1);

    if (!g_tray_added) g_tray_added = Shell_NotifyIconW(NIM_ADD, &nid) ? 1 : 0;
    else               Shell_NotifyIconW(NIM_MODIFY, &nid);

    wcsncpy(last_tip, tip, 127);
    last_tip[127] = L'\0';
}

static void tray_remove(HWND hwnd)
{
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = 0;
}

static void tray_balloon(HWND hwnd, const wchar_t *title, const wchar_t *text)
{
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_INFO;
    wcsncpy(nid.szInfoTitle, title, sizeof nid.szInfoTitle / sizeof nid.szInfoTitle[0] - 1);
    wcsncpy(nid.szInfo, text, sizeof nid.szInfo / sizeof nid.szInfo[0] - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static const wchar_t *val_text(wc_val v)
{
    return v == WC_VAL_ON ? L"on" : v == WC_VAL_OFF ? L"off" : L"–";
}

static void row_status(const snap_row *r, bool enabled, wchar_t *out, int cap)
{
    if (!r->present)          { wcsncpy(out, L"not present", (size_t)cap - 1); out[cap-1]=0; return; }
    if (r->last_err)          { wcw_format_error(r->last_err, out, cap); return; }
    if (!r->managed)          { wcsncpy(out, L"not managed", (size_t)cap - 1); out[cap-1]=0; return; }
    if (!enabled)             { wcsncpy(out, L"off", (size_t)cap - 1); out[cap-1]=0; return; }
    if (r->pending)           { wcsncpy(out, L"waiting for a connection", (size_t)cap - 1); out[cap-1]=0; return; }
    wcsncpy(out, L"optimized", (size_t)cap - 1);
    out[cap - 1] = L'\0';
}

static void fill_list(HWND hwnd)
{
    HWND lv = GetDlgItem(hwnd, IDC_LIST);
    g_filling = 1;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);

    if (g_snap) {
        for (int i = 0; i < g_snap->n; ++i) {
            const snap_row *r = &g_snap->row[i];
            LVITEMW it;
            memset(&it, 0, sizeof it);
            it.mask     = LVIF_TEXT | LVIF_PARAM;
            it.iItem    = i;
            it.pszText  = (LPWSTR)r->name;
            it.lParam   = i;
            int row = ListView_InsertItem(lv, &it);
            if (row < 0) continue;

            wchar_t buf[256];
            MultiByteToWideChar(CP_UTF8, 0, wc_state_name(r->state), -1, buf, 256);
            ListView_SetItemText(lv, row, 1, buf);
            ListView_SetItemText(lv, row, 2, (LPWSTR)val_text(r->bgscan));
            ListView_SetItemText(lv, row, 3, (LPWSTR)val_text(r->streaming));
            ListView_SetItemText(lv, row, 4, (LPWSTR)val_text(r->autoconf));
            row_status(r, g_snap->enabled, buf, 256);
            ListView_SetItemText(lv, row, 5, buf);
            ListView_SetCheckState(lv, row, r->managed ? TRUE : FALSE);
        }
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    g_filling = 0;
}

static void update_status(HWND hwnd)
{
    wchar_t text[512], err[256];

    if (!g_snap) {
        wcscpy(text, L"Starting…");
    } else if (g_snap->open_err) {
        wcw_format_error(g_snap->open_err, err, 256);
        _snwprintf(text, 512, L"Cannot reach the WLAN service: %s\r\n"
                              L"Check that the WLAN AutoConfig service is running.", err);
    } else if (g_snap->enum_err) {
        wcw_format_error(g_snap->enum_err, err, 256);
        _snwprintf(text, 512, L"Cannot list adapters: %s", err);
    } else if (g_snap->write_denied) {
        wcscpy(text, L"Windows is refusing these settings even with administrator rights.\r\n"
                     L"A group policy or a Native Wifi permission change is blocking them.");
    } else {
        int active = 0, waiting = 0;
        for (int i = 0; i < g_snap->n; ++i) {
            const snap_row *r = &g_snap->row[i];
            if (!r->present || !r->managed) continue;
            if (r->pending) waiting++;
            else if (r->bgscan == WC_VAL_OFF || r->streaming == WC_VAL_ON) active++;
        }
        if (!g_snap->enabled)
            wcscpy(text, L"Off. Windows defaults are in effect.");
        else if (g_snap->nuclear)
            _snwprintf(text, 512, L"Scanning stopped on %d adapter%s. No roaming and no "
                                  L"automatic reconnect while this is on.",
                       active, active == 1 ? L"" : L"s");
        else if (active || waiting)
            _snwprintf(text, 512, L"Optimizing %d adapter%s%s. Settings are released "
                                  L"automatically when this app exits.",
                       active, active == 1 ? L"" : L"s",
                       waiting ? L", waiting on a connection for others" : L"");
        else
            wcscpy(text, L"No connected Wi-Fi adapter to optimize yet.");
    }
    /* A repair while armed is the expected reaction to a dropped link, and one
     * straight after disarming is our own doing.  Anything else means we found
     * auto config switched off by a run that never got to clean up. */
    bool foreign_repair = g_snap && g_snap->recovered > 0 && !g_snap->nuclear &&
                          !g_expect_repair;
    if (g_snap && g_snap->recovered > 0) g_expect_repair = false;

    wchar_t note[256] = L"";
    if (foreign_repair) {
        _snwprintf(note, 256, L"Re-enabled Wi-Fi auto configuration on %d adapter%s "
                              L"that had been left disabled.",
                   g_snap->recovered, g_snap->recovered == 1 ? L"" : L"s");
        note[255] = L'\0';
        wcsncpy(text, note, 511);
    }
    text[511] = L'\0';
    SetDlgItemTextW(hwnd, IDC_STATUS, text);
    tray_update(hwnd, text);
    /* After tray_update, so the icon exists to hang it on at startup. */
    if (note[0]) tray_balloon(hwnd, L"WiFi Control", note);
}

static void arm_deadman(HWND hwnd)
{
    KillTimer(hwnd, T_DEADMAN);
    int k = (int)SendMessageW(GetDlgItem(hwnd, IDC_NUKE_FOR), CB_GETCURSEL, 0, 0);
    if (k < 0 || k >= (int)(sizeof NUKE_FOR / sizeof NUKE_FOR[0])) k = 0;
    UINT minutes = NUKE_FOR[k].minutes;
    if (minutes) SetTimer(hwnd, T_DEADMAN, minutes * 60u * 1000u, nullptr);
}

static void set_nuclear(HWND hwnd, bool on)
{
    CheckDlgButton(hwnd, IDC_NUKE, on ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(hwnd, IDC_NUKE_FOR), !on);
    if (on) {
        arm_deadman(hwnd);
    } else {
        KillTimer(hwnd, T_DEADMAN);
        /* The repair this triggers is expected, so do not report it as having
         * cleaned up after something. */
        g_expect_repair = true;
    }
    PostMessageW(g_worker, WM_W_NUCLEAR, (WPARAM)on, 0);
}

static bool confirm_nuclear(HWND hwnd)
{
    if (g_nuke_confirmed) return true;
    int r = MessageBoxW(hwnd,
        L"Disable Wi-Fi auto configuration on the checked adapters?\n\n"
        L"This stops scanning completely, which is the point \u2014 but it also stops "
        L"roaming to a better access point, and stops Windows reconnecting on its "
        L"own if the link drops.\n\n"
        L"It is put back automatically when the link drops, when you turn this off, "
        L"when the timer expires, and when this app exits or next starts. If the app "
        L"is killed outright, it stays off until you run it again.",
        L"Stop all scanning", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    g_nuke_confirmed = (r == IDYES);
    return g_nuke_confirmed;
}

static void send_manage(const wc_guid *g, bool on)
{
    manage_cmd *c = malloc(sizeof *c);
    if (!c) return;
    c->g  = *g;
    c->on = on;
    if (!PostMessageW(g_worker, WM_W_MANAGE, 0, (LPARAM)c)) free(c);
}

/* ------------------------------------------------------------------- DPI */

static int dpi_of(HWND hwnd)
{
    typedef UINT (WINAPI *pfn)(HWND);
    static pfn get;
    static int probed;
    if (!probed) {
        probed = 1;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) get = (pfn)(void *)GetProcAddress(u, "GetDpiForWindow");
    }
    if (get) {
        UINT d = get(hwnd);
        if (d) return (int)d;
    }
    HDC dc = GetDC(NULL);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d ? d : 96;
}

static BOOL CALLBACK cache_child(HWND child, [[maybe_unused]] LPARAM lp)
{
    if (g_base_n >= 16) return FALSE;
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(NULL, GetParent(child), (POINT *)&r, 2);
    g_base_hwnd[g_base_n] = child;
    g_base[g_base_n] = r;
    g_base_n++;
    return TRUE;
}

static void apply_font(HWND hwnd, int dpi)
{
    HFONT old = g_font;
    g_font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!g_font) { g_font = old; return; }
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (int i = 0; i < g_base_n; ++i)
        SendMessageW(g_base_hwnd[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    if (old) DeleteObject(old);
}

/* Column widths are authored for 96 dpi; everything else is already sized by
 * the dialog manager at whatever DPI the dialog was created on. */
static void layout_columns(HWND hwnd, int dpi)
{
    HWND lv = GetDlgItem(hwnd, IDC_LIST);
    static constexpr int w[6] = { 150, 74, 52, 52, 52, 150 };
    for (int i = 0; i < 6; ++i) ListView_SetColumnWidth(lv, i, MulDiv(w[i], dpi, 96));
}

/* Children were cached at g_base_dpi, not at 96: a dialog template is laid out
 * in dialog units against the font of the monitor it was created on, so the
 * ratio to apply is new/cached, not new/96. */
static void rescale(HWND hwnd, int dpi, const RECT *suggest)
{
    if (suggest)
        SetWindowPos(hwnd, NULL, suggest->left, suggest->top,
                     suggest->right - suggest->left, suggest->bottom - suggest->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    for (int i = 0; i < g_base_n; ++i) {
        RECT r = g_base[i];
        SetWindowPos(g_base_hwnd[i], NULL,
                     MulDiv(r.left, dpi, g_base_dpi), MulDiv(r.top, dpi, g_base_dpi),
                     MulDiv(r.right - r.left, dpi, g_base_dpi),
                     MulDiv(r.bottom - r.top, dpi, g_base_dpi),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    apply_font(hwnd, dpi);
    layout_columns(hwnd, dpi);
    g_dpi = dpi;
}

/* --------------------------------------------------------------- dlg proc */

static void show_main(HWND hwnd)
{
    if (g_list_stale) { fill_list(hwnd); g_list_stale = false; }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

static void tray_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"&Show WiFi Control");
    AppendMenuW(m, MF_STRING | ((g_snap && g_snap->enabled) ? MF_CHECKED : 0),
                IDM_TOGGLE, L"&Optimize");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); /* so the menu dismisses on click-away */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static INT_PTR CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msg_taskbar && g_msg_taskbar) { g_tray_added = 0; update_status(hwnd); return TRUE; }
    if (msg == g_msg_show && g_msg_show)       { show_main(hwnd); return TRUE; }

    switch (msg) {
    case WM_INITDIALOG: {
        g_ui = hwnd;
        EnumChildWindows(hwnd, cache_child, 0);

        HWND lv = GetDlgItem(hwnd, IDC_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                              LVS_EX_DOUBLEBUFFER);
        static const wchar_t *cols[6] = { L"Adapter", L"State", L"Bkg scan",
                                          L"Streaming", L"Auto cfg", L"Status" };
        for (int i = 0; i < 6; ++i) {
            LVCOLUMNW c;
            memset(&c, 0, sizeof c);
            c.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            c.iSubItem = i;
            c.pszText  = (LPWSTR)cols[i];
            c.cx       = 100;
            ListView_InsertColumn(lv, i, &c);
        }

        HWND nf = GetDlgItem(hwnd, IDC_NUKE_FOR);
        for (size_t k = 0; k < sizeof NUKE_FOR / sizeof NUKE_FOR[0]; ++k)
            SendMessageW(nf, CB_ADDSTRING, 0, (LPARAM)NUKE_FOR[k].label);
        SendMessageW(nf, CB_SETCURSEL, 0, 0);

        g_icon_big = make_icon(GetSystemMetrics(SM_CXICON), RGB(43, 145, 72));
        if (g_icon_big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);

        g_dpi = g_base_dpi = dpi_of(hwnd);
        layout_columns(hwnd, g_dpi);
        update_status(hwnd);
        return TRUE;
    }

    case WM_U_SNAPSHOT: {
        snapshot *fresh = (snapshot *)lp;
        free(g_snap);
        g_snap = fresh;
        CheckDlgButton(hwnd, IDC_ENABLE, g_snap && g_snap->enabled ? BST_CHECKED : BST_UNCHECKED);
        /* Rebuilding the list costs a teardown plus five text sets per row.
         * While we are in the tray nobody can see it, so defer to the reveal. */
        if (IsWindowVisible(hwnd)) { fill_list(hwnd); g_list_stale = false; }
        else                         g_list_stale = true;
        update_status(hwnd);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_LIST && nh->code == LVN_ITEMCHANGED && !g_filling) {
            NMLISTVIEW *nv = (NMLISTVIEW *)lp;
            if (nv->uChanged & LVIF_STATE) {
                int was = (int)((nv->uOldState & LVIS_STATEIMAGEMASK) >> 12);
                int now = (int)((nv->uNewState & LVIS_STATEIMAGEMASK) >> 12);
                if (was && now && was != now && g_snap &&
                    nv->lParam >= 0 && nv->lParam < g_snap->n) {
                    bool on = (now == 2);
                    const wc_guid *g = &g_snap->row[nv->lParam].guid;
                    g_snap->row[nv->lParam].managed = on;
                    cfg_save_managed(g, on);
                    send_manage(g, on);
                }
            }
        }
        return FALSE;
    }

    case WM_U_TRAY:
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) show_main(hwnd);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) tray_menu(hwnd);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ENABLE: {
            bool on = IsDlgButtonChecked(hwnd, IDC_ENABLE) == BST_CHECKED;
            cfg_save_enabled(on);
            if (!on) set_nuclear(hwnd, false); /* the core ignores it anyway; say so */
            PostMessageW(g_worker, WM_W_ENABLE, (WPARAM)on, 0);
            return TRUE;
        }
        case IDM_TOGGLE: {
            bool on = !(g_snap && g_snap->enabled);
            CheckDlgButton(hwnd, IDC_ENABLE, on ? BST_CHECKED : BST_UNCHECKED);
            cfg_save_enabled(on);
            PostMessageW(g_worker, WM_W_ENABLE, (WPARAM)on, 0);
            return TRUE;
        }
        case IDC_NUKE: {
            bool on = IsDlgButtonChecked(hwnd, IDC_NUKE) == BST_CHECKED;
            if (on && !confirm_nuclear(hwnd)) { set_nuclear(hwnd, false); return TRUE; }
            set_nuclear(hwnd, on);
            return TRUE;
        }
        case IDC_REFRESH:
            PostMessageW(g_worker, WM_W_POLL, 0, 0);
            return TRUE;
        case IDC_TUNE: {
            if (!g_snap || g_snap->n == 0) {
                MessageBoxW(hwnd, L"No Wi-Fi adapter to tune.", L"Tuning", MB_ICONINFORMATION);
                return TRUE;
            }
            HWND lv = GetDlgItem(hwnd, IDC_LIST);
            int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
            int i = -1;
            if (row >= 0) {
                LVITEMW it = { .mask = LVIF_PARAM, .iItem = row };
                if (ListView_GetItem(lv, &it) && it.lParam >= 0 && it.lParam < g_snap->n)
                    i = (int)it.lParam;
            }
            if (i < 0)
                for (int k = 0; k < g_snap->n; ++k)
                    if (g_snap->row[k].present) { i = k; break; }
            if (i < 0) i = 0;
            tune_dialog(hwnd, &g_snap->row[i].guid, g_snap->row[i].name);
            return TRUE;
        }
        case IDM_SHOW:
            show_main(hwnd);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            ShowWindow(hwnd, SW_HIDE);
            if (!g_warned_tray) {
                g_warned_tray = 1;
                tray_balloon(hwnd, L"WiFi Control",
                             L"Still running here. The settings only last while it runs.");
            }
            return TRUE;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return TRUE;
        }
        return FALSE;

    case WM_TIMER:
        if (wp == T_DEADMAN) {
            KillTimer(hwnd, T_DEADMAN);
            set_nuclear(hwnd, false);
            tray_balloon(hwnd, L"WiFi Control",
                         L"Timer expired \u2014 scanning and auto configuration are back on.");
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        /* Logging off or shutting down: WM_DESTROY is not guaranteed to run,
         * and auto config would survive the reboot still disabled.  Send, not
         * post, so the worker has actually written it before we return. */
        if (wp && g_worker) SendMessageW(g_worker, WM_W_NUCLEAR, 0, 0);
        return TRUE;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { ShowWindow(hwnd, SW_HIDE); return TRUE; }
        return FALSE;

    case WM_POWERBROADCAST:
        /* Notifications can be missed across a suspend; re-apply on resume. */
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)
            PostMessageW(g_worker, WM_W_POLL, 0, 0);
        return TRUE;

    case WM_DPICHANGED:
        rescale(hwnd, (int)HIWORD(wp), (const RECT *)lp);
        return TRUE;

    case WM_DESTROY:
        tray_remove(hwnd);
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------- main */

int WINAPI wWinMain(HINSTANCE inst, [[maybe_unused]] HINSTANCE prev,
                    LPWSTR cmdline, [[maybe_unused]] int show)
{

    g_msg_show    = RegisterWindowMessageW(L"WifiControl.Show");
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");

    HANDLE once = CreateMutexW(NULL, FALSE, L"Local\\WifiControl.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, g_msg_show, 0, 0);
        CloseHandle(once);
        return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    cfg_resolve_path();
    cfg_load(&g_boot_cfg);

    HWND dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, dlg_proc, 0);
    if (!dlg) { if (once) CloseHandle(once); return 1; }

    g_worker_ready  = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_worker_thread = (HANDLE)_beginthreadex(NULL, 0, worker_main, NULL, 0, NULL);
    if (g_worker_ready) WaitForSingleObject(g_worker_ready, 10000);

    /* Start hidden when asked for on the command line, otherwise show. */
    if (!wcsstr(cmdline ? cmdline : L"", L"/tray")) ShowWindow(dlg, SW_SHOW);
    else tray_update(dlg, L"WiFi Control");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_worker) PostMessageW(g_worker, WM_W_QUIT, 0, 0);
    if (g_worker_thread) {
        WaitForSingleObject(g_worker_thread, 5000);
        CloseHandle(g_worker_thread);
    }
    if (g_worker_ready) CloseHandle(g_worker_ready);
    free(g_snap);
    if (g_icon_small) DestroyIcon(g_icon_small);
    if (g_icon_big)   DestroyIcon(g_icon_big);
    if (g_font)       DeleteObject(g_font);
    if (once)         CloseHandle(once);
    return 0;
}

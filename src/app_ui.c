/* app_ui.c -- the main window: master switches, one card per adapter, tray.
 *
 * Cards are painted straight from the latest worker snapshot.  The only child
 * windows are the things you operate -- switches, the timer dropdown and the
 * buttons -- so focus and keyboard navigation still come from the dialog
 * manager.
 */
#define WIN32_LEAN_AND_MEAN
#include "app.h"
#include "ui.h"
#include "tune.h"
#include "resource.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define T_DEADMAN 3

/* Layout, in pixels at 96 dpi. */
enum {
    CLIENT_W = 480, PAD = 16, ROW_H = 28, SWITCH_W = 44,
    OPT_SWITCH_X = 80, NUKE_LABEL_X = 148, NUKE_SWITCH_X = 240, NUKE_FOR_X = 292,
    STATUS_TOP = 52, STATUS_H = 36, CARDS_TOP = 96,
    CARD_H = 146, CARD_GAP = 10, CARD_EMPTY = 64, CARDS_VISIBLE = 3,
    CARD_FOOT = 110,
};
#define P(v) ui_px((v), g_f.dpi)

static snapshot *g_snap;
static HICON     g_icon_small, g_icon_big;
static COLORREF  g_icon_color;
static bool      g_tray_added;
static bool      g_warned_tray;
static bool      g_nuke_confirmed;   /* asked once per session, not persisted */
static bool      g_expect_repair;    /* we just disarmed: the next repair is ours */
static UINT      g_msg_taskbar;
static ui_fonts  g_f;
static HWND      g_cards;
static HWND      g_manage[WC_MAX_ADAPTERS];
static int       g_manage_n;
static wchar_t   g_status[512];
static enum { SEV_INFO, SEV_WARN, SEV_ERR } g_status_sev;

/* Never written to the config file.  The app always starts disarmed, which is
 * what makes the first poll repair a previous run that was killed while armed:
 * there is no stored flag to be wrong about. */
typedef struct { const wchar_t *label; UINT minutes; } nuke_span;
static const nuke_span NUKE_FOR[] = {
    { L"for 5 minutes",   5 },
    { L"for 15 minutes", 15 },
    { L"for 30 minutes", 30 },
    { L"for 1 hour",     60 },
    { L"no time limit",   0 },
};
enum { NUKE_DEFAULT = 1 };

static void layout(HWND hwnd, const RECT *at);

/* ------------------------------------------------------------------- tray */

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

    NOTIFYICONDATAW nid = { .cbSize = sizeof nid, .hWnd = hwnd, .uID = 1,
                            .uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP,
                            .uCallbackMessage = WM_U_TRAY, .hIcon = g_icon_small };
    wcsncpy(nid.szTip, tip, sizeof nid.szTip / sizeof nid.szTip[0] - 1);

    if (!g_tray_added) g_tray_added = Shell_NotifyIconW(NIM_ADD, &nid);
    else               Shell_NotifyIconW(NIM_MODIFY, &nid);

    wcsncpy(last_tip, tip, 127);
    last_tip[127] = L'\0';
}

static void tray_remove(HWND hwnd)
{
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid = { .cbSize = sizeof nid, .hWnd = hwnd, .uID = 1 };
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = false;
}

static void tray_balloon(HWND hwnd, const wchar_t *title, const wchar_t *text)
{
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid = { .cbSize = sizeof nid, .hWnd = hwnd, .uID = 1, .uFlags = NIF_INFO };
    wcsncpy(nid.szInfoTitle, title, sizeof nid.szInfoTitle / sizeof nid.szInfoTitle[0] - 1);
    wcsncpy(nid.szInfo, text, sizeof nid.szInfo / sizeof nid.szInfo[0] - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"&Show WiFi Control");
    AppendMenuW(m, MF_STRING | ((g_snap && g_snap->enabled) ? MF_CHECKED : 0),
                IDM_TOGGLE, L"&Optimize");
    AppendMenuW(m, MF_STRING | (ui_dark ? MF_CHECKED : 0), IDM_DARK, L"&Dark theme");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); /* so the menu dismisses on click-away */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ------------------------------------------------------------------ cards */

static const wchar_t *val_text(wc_val v)
{
    return v == WC_VAL_ON ? L"on" : v == WC_VAL_OFF ? L"off" : L"–";
}

static bool row_optimized(const snap_row *r)
{
    return r->present && !r->last_err && r->managed && g_snap->enabled && !r->pending;
}

static void row_status(const snap_row *r, bool enabled, wchar_t *out, int cap)
{
    const wchar_t *s = !r->present ? L"Not present"
                     : r->last_err ? nullptr
                     : !r->managed ? L"Not managed"
                     : !enabled    ? L"Off"
                     : r->pending  ? L"Waiting for a connection"
                                   : L"Optimized";
    if (!s) { wcw_format_error(r->last_err, out, cap); return; }
    wcsncpy(out, s, (size_t)cap - 1);
    out[cap - 1] = L'\0';
}

static int text_w(ui_canvas *c, HFONT f, const wchar_t *s)
{
    HGDIOBJ old = SelectObject(c->dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(c->dc, s, (int)wcslen(s), &sz);
    SelectObject(c->dc, old);
    return sz.cx;
}

static COLORREF state_ink(const snap_row *r)
{
    if (!r->present) return ui_pal.text2;
    switch (r->state) {
    case WC_IF_CONNECTED:      return ui_pal.ok;
    case WC_IF_ASSOCIATING:
    case WC_IF_DISCOVERING:
    case WC_IF_AUTHENTICATING: return ui_pal.warn;
    default:                   return ui_pal.text2;
    }
}

static void check_mark(ui_canvas *c, double x, double y)
{
    const double s = g_f.dpi / 96.0, w = 1.6 * s;
    ui_line(c, x, y, x + 3.5 * s, y + 3.5 * s, w, ui_pal.ok);
    ui_line(c, x + 3.5 * s, y + 3.5 * s, x + 10 * s, y - 4 * s, w, ui_pal.ok);
}

static void paint_card(ui_canvas *c, const snap_row *r, int y)
{
    const double s = g_f.dpi / 96.0;
    const int x0 = P(PAD), x1 = c->w - P(PAD), in = x0 + P(14), right = x1 - P(14);
    const UINT one = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;

    ui_frame(c, x0, y, x1, y + P(CARD_H), 6 * s, 1 * s, ui_pal.border, ui_pal.card);

    /* Header: name on the left, a coloured dot and the link state on the right. */
    wchar_t state[64] = L"Not present";
    if (r->present) MultiByteToWideChar(CP_UTF8, 0, wc_state_name(r->state), -1, state, 64);
    const int sw = text_w(c, g_f.body, state), dot = P(8);
    ui_text(c, g_f.body, ui_pal.text2, state, right - sw, y + P(12), right, y + P(36), one);
    const double dx = right - sw - P(8) - dot / 2.0, dy = y + P(24);
    ui_rrect(c, dx - dot / 2.0, dy - dot / 2.0, dx + dot / 2.0, dy + dot / 2.0, dot / 2.0,
             state_ink(r));
    ui_text(c, g_f.bold, ui_pal.text, r->name, in, y + P(12), right - sw - P(28), y + P(36), one);

    /* What the driver reported back, ticked where it matches what we asked. */
    const bool active = r->present && r->managed && g_snap->enabled;
    const struct { const wchar_t *label; wc_val v; bool good; } rows[3] = {
        { L"Background scan", r->bgscan,    active && r->bgscan == WC_VAL_OFF },
        { L"Streaming mode",  r->streaming, active && r->streaming == WC_VAL_ON },
        { L"Auto config",     r->autoconf,  false },
    };
    for (int k = 0; k < 3; ++k) {
        const int ry = y + P(42 + 22 * k);
        COLORREF ink = (k == 2 && r->autoconf == WC_VAL_OFF) ? ui_pal.warn : ui_pal.text;
        ui_text(c, g_f.body, ui_pal.text2, rows[k].label, in, ry, in + P(140), ry + P(22), one);
        ui_text(c, g_f.body, ink, val_text(rows[k].v), in + P(150), ry, in + P(190), ry + P(22), one);
        if (rows[k].good) check_mark(c, in + P(178), ry + P(11));
    }

    /* Footer: this adapter's status, and its Manage switch (a child window). */
    wchar_t status[256];
    row_status(r, g_snap->enabled, status, 256);
    const COLORREF ink = r->last_err ? ui_pal.err : row_optimized(r) ? ui_pal.ok : ui_pal.text2;
    const int fy = y + P(CARD_FOOT), sx = right - P(SWITCH_W);
    const int lw = text_w(c, g_f.body, L"Manage");
    ui_fill(c, in, fy - P(4), right, fy - P(4) + (P(1) > 0 ? P(1) : 1), ui_pal.border);
    ui_text(c, g_f.body, ui_pal.text2, L"Manage", sx - P(6) - lw, fy, sx - P(6), fy + P(ROW_H), one);
    ui_text(c, g_f.body, ink, status, in, fy, sx - lw - P(20), fy + P(ROW_H), one);
}

static void paint_cards([[maybe_unused]] HWND panel, ui_canvas *c, int scroll,
                        [[maybe_unused]] void *ctx)
{
    if (!g_snap || g_snap->n == 0) {
        const double s = g_f.dpi / 96.0;
        ui_frame(c, P(PAD), 0, c->w - P(PAD), P(CARD_EMPTY), 6 * s, 1 * s,
                 ui_pal.border, ui_pal.card);
        ui_text(c, g_f.body, ui_pal.text2,
                g_snap ? L"No Wi-Fi adapters found." : L"Looking for Wi-Fi adapters…",
                P(PAD), 0, c->w - P(PAD), P(CARD_EMPTY),
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }
    for (int i = 0; i < g_snap->n; ++i) {
        const int y = i * P(CARD_H + CARD_GAP) - scroll;
        if (y >= c->h || y + P(CARD_H) <= 0) continue;
        paint_card(c, &g_snap->row[i], y);
    }
}

static void paint_main(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    ui_canvas c;
    if (ui_canvas_begin(&c, dc, rc.right, rc.bottom)) {
        const UINT one = DT_SINGLELINE | DT_VCENTER;
        ui_fill(&c, 0, 0, c.w, c.h, ui_pal.bg);
        ui_text(&c, g_f.body, ui_pal.text, L"Optimize",
                P(PAD), P(PAD), P(OPT_SWITCH_X), P(PAD + ROW_H), one);
        ui_text(&c, g_f.body, ui_pal.text, L"Stop scanning",
                P(NUKE_LABEL_X), P(PAD), P(NUKE_SWITCH_X), P(PAD + ROW_H), one);

        COLORREF ink = g_status_sev == SEV_ERR  ? ui_pal.err
                     : g_status_sev == SEV_WARN ? ui_pal.warn : ui_pal.text2;
        ui_text(&c, g_f.body, ink, g_status, P(PAD), P(STATUS_TOP), c.w - P(PAD),
                P(STATUS_TOP + STATUS_H), DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
        ui_canvas_end(&c, dc, 0, 0);
    }
    EndPaint(hwnd, &ps);
}

/* ----------------------------------------------------------------- status */

static void update_status(HWND hwnd)
{
    wchar_t text[512], err[256];
    g_status_sev = SEV_INFO;

    if (!g_snap) {
        wcscpy(text, L"Starting…");
    } else if (g_snap->open_err) {
        wcw_format_error(g_snap->open_err, err, 256);
        _snwprintf(text, 512, L"Cannot reach the WLAN service: %s "
                              L"Check that the WLAN AutoConfig service is running.", err);
        g_status_sev = SEV_ERR;
    } else if (g_snap->enum_err) {
        wcw_format_error(g_snap->enum_err, err, 256);
        _snwprintf(text, 512, L"Cannot list adapters: %s", err);
        g_status_sev = SEV_ERR;
    } else if (g_snap->write_denied) {
        wcscpy(text, L"Windows is refusing these settings even with administrator rights. "
                     L"A group policy or a Native Wifi permission change is blocking them.");
        g_status_sev = SEV_WARN;
    } else {
        int active = 0, waiting = 0;
        for (int i = 0; i < g_snap->n; ++i) {
            const snap_row *r = &g_snap->row[i];
            if (!r->present || !r->managed) continue;
            if (r->pending) waiting++;
            else if (r->bgscan == WC_VAL_OFF || r->streaming == WC_VAL_ON) active++;
        }
        if (!g_snap->enabled) {
            wcscpy(text, L"Off. Windows defaults are in effect.");
        } else if (g_snap->nuclear) {
            _snwprintf(text, 512, L"Scanning stopped on %d adapter%s. No roaming and no "
                                  L"automatic reconnect while this is on.",
                       active, active == 1 ? L"" : L"s");
            g_status_sev = SEV_WARN;
        } else if (active || waiting) {
            _snwprintf(text, 512, L"Optimizing %d adapter%s%s. Settings are released "
                                  L"automatically when this app exits.",
                       active, active == 1 ? L"" : L"s",
                       waiting ? L", waiting on a connection for others" : L"");
        } else {
            wcscpy(text, L"No connected Wi-Fi adapter to optimize yet.");
        }
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
        g_status_sev = SEV_WARN;
    }
    text[511] = L'\0';

    if (wcscmp(text, g_status) != 0) {
        wcscpy(g_status, text);
        RECT r = { 0, P(STATUS_TOP), P(CLIENT_W), P(STATUS_TOP + STATUS_H) };
        InvalidateRect(hwnd, &r, FALSE);
    }
    tray_update(hwnd, text);
    /* After tray_update, so the icon exists to hang it on at startup. */
    if (note[0]) tray_balloon(hwnd, L"WiFi Control", note);
}

/* ------------------------------------------------------------- scanning */

static void arm_deadman(HWND hwnd)
{
    KillTimer(hwnd, T_DEADMAN);
    int k = (int)SendMessageW(GetDlgItem(hwnd, IDC_NUKE_FOR), CB_GETCURSEL, 0, 0);
    if (k < 0 || k >= (int)(sizeof NUKE_FOR / sizeof NUKE_FOR[0])) k = NUKE_DEFAULT;
    UINT minutes = NUKE_FOR[k].minutes;
    if (minutes) SetTimer(hwnd, T_DEADMAN, minutes * 60u * 1000u, nullptr);
}

static void set_nuclear(HWND hwnd, bool on)
{
    ui_switch_set(GetDlgItem(hwnd, IDC_NUKE), on);
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
        L"Disable Wi-Fi auto configuration on the managed adapters?\n\n"
        L"This stops scanning completely, which is the point — but it also stops "
        L"roaming to a better access point, and stops Windows reconnecting on its "
        L"own if the link drops.\n\n"
        L"It is put back automatically when the link drops, when you turn this off, "
        L"when the timer expires, and when this app exits or next starts. If the app "
        L"is killed outright, it stays off until you run it again.",
        L"Stop all scanning", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    g_nuke_confirmed = (r == IDYES);
    return g_nuke_confirmed;
}

static void set_enabled(HWND hwnd, bool on)
{
    ui_switch_set(GetDlgItem(hwnd, IDC_ENABLE), on);
    cfg_save_enabled(on);
    if (!on) set_nuclear(hwnd, false); /* the core ignores it anyway; say so */
    PostMessageW(g_worker, WM_W_ENABLE, (WPARAM)on, 0);
}

static void send_manage(const wc_guid *g, bool on)
{
    manage_cmd *c = malloc(sizeof *c);
    if (!c) return;
    c->g  = *g;
    c->on = on;
    if (!PostMessageW(g_worker, WM_W_MANAGE, 0, (LPARAM)c)) free(c);
}

/* ----------------------------------------------------------------- layout */

static void place_manage(void)
{
    RECT rc;
    GetClientRect(g_cards, &rc);
    const int scroll = ui_panel_scroll(g_cards);
    for (int i = 0; i < g_manage_n; ++i)
        SetWindowPos(g_manage[i], nullptr,
                     rc.right - P(PAD) - P(14) - P(SWITCH_W),
                     i * P(CARD_H + CARD_GAP) - scroll + P(CARD_FOOT),
                     P(SWITCH_W), P(ROW_H), SWP_NOZORDER | SWP_NOACTIVATE);
}

/* One Manage switch per adapter.  They are only rebuilt when the number of
 * adapters changes; otherwise a snapshot just updates their state. */
static void sync_manage(HWND hwnd)
{
    const int n = g_snap ? g_snap->n : 0;
    if (n != g_manage_n) {
        for (int i = 0; i < g_manage_n; ++i) DestroyWindow(g_manage[i]);
        g_manage_n = 0;
        for (int i = 0; i < n; ++i) {
            g_manage[i] = ui_control(g_cards, IDC_MANAGE + i, UI_SWITCH, L"Manage", true);
            if (!g_manage[i]) break;
            g_manage_n++;
        }
        layout(hwnd, nullptr);
    }
    for (int i = 0; i < g_manage_n; ++i) {
        SetWindowTextW(g_manage[i], g_snap->row[i].name); /* the name a screen reader reads */
        ui_switch_set(g_manage[i], g_snap->row[i].managed);
        EnableWindow(g_manage[i], g_snap->row[i].present);
    }
}

static void apply_fonts(HWND hwnd)
{
    static const int ids[] = { IDC_ENABLE, IDC_NUKE, IDC_NUKE_FOR, IDC_TUNE, IDC_REFRESH, IDOK };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; ++i)
        SendMessageW(GetDlgItem(hwnd, ids[i]), WM_SETFONT, (WPARAM)g_f.body, FALSE);
    HWND cb = GetDlgItem(hwnd, IDC_NUKE_FOR);
    SendMessageW(cb, CB_SETITEMHEIGHT, (WPARAM)-1, P(22));
    SendMessageW(cb, CB_SETITEMHEIGHT, 0, P(22));
}

static void layout(HWND hwnd, const RECT *at)
{
    const int n       = g_snap ? g_snap->n : 0;
    const int content = n ? n * P(CARD_H + CARD_GAP) - P(CARD_GAP) : P(CARD_EMPTY);
    const int most    = CARDS_VISIBLE * P(CARD_H + CARD_GAP) - P(CARD_GAP);
    const int view    = content < most ? content : most;
    const int cw      = P(CLIENT_W);
    const int ch      = P(CARDS_TOP) + view + P(PAD) + P(ROW_H) + P(PAD);
    const UINT fl     = SWP_NOZORDER | SWP_NOACTIVATE;

    ui_size_window(hwnd, cw, ch, g_f.dpi, at);

    SetWindowPos(GetDlgItem(hwnd, IDC_ENABLE), nullptr, P(OPT_SWITCH_X), P(PAD),
                 P(SWITCH_W), P(ROW_H), fl);
    SetWindowPos(GetDlgItem(hwnd, IDC_NUKE), nullptr, P(NUKE_SWITCH_X), P(PAD),
                 P(SWITCH_W), P(ROW_H), fl);
    HWND cb = GetDlgItem(hwnd, IDC_NUKE_FOR);
    SetWindowPos(cb, nullptr, P(NUKE_FOR_X), P(PAD), cw - P(NUKE_FOR_X) - P(PAD), P(200), fl);
    RECT r;
    GetWindowRect(cb, &r);
    SetWindowPos(cb, nullptr, P(NUKE_FOR_X), P(PAD) + (P(ROW_H) - (r.bottom - r.top)) / 2,
                 0, 0, fl | SWP_NOSIZE);

    SetWindowPos(g_cards, nullptr, 0, P(CARDS_TOP), cw, view, fl);
    ui_panel_set_content(g_cards, content);
    place_manage();

    const int by = ch - P(PAD) - P(ROW_H);
    SetWindowPos(GetDlgItem(hwnd, IDC_TUNE),    nullptr, P(PAD - 8),      by, P(84), P(ROW_H), fl);
    SetWindowPos(GetDlgItem(hwnd, IDC_REFRESH), nullptr, P(PAD - 8 + 88), by, P(72), P(ROW_H), fl);
    SetWindowPos(GetDlgItem(hwnd, IDOK), nullptr, cw - P(PAD) - P(88), by, P(88), P(ROW_H), fl);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void retheme(HWND hwnd)
{
    ui_theme_window(hwnd);
    ui_theme_combo(GetDlgItem(hwnd, IDC_NUKE_FOR));
    ui_theme_scroll(g_cards);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

/* ----------------------------------------------------------------- tuning */

static void open_tuning(HWND hwnd)
{
    int present[WC_MAX_ADAPTERS], n = 0;
    for (int i = 0; g_snap && i < g_snap->n; ++i)
        if (g_snap->row[i].present) present[n++] = i;
    if (n == 0) {
        MessageBoxW(hwnd, L"No Wi-Fi adapter to tune.", L"Tuning", MB_ICONINFORMATION);
        return;
    }

    int pick = present[0];
    if (n > 1) {
        HMENU m = CreatePopupMenu();
        if (!m) return;
        for (int k = 0; k < n; ++k)
            AppendMenuW(m, MF_STRING, (UINT_PTR)(IDM_TUNE_AD + present[k]),
                        g_snap->row[present[k]].name);
        RECT r;
        GetWindowRect(GetDlgItem(hwnd, IDC_TUNE), &r);
        int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                                      r.left, r.top, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd < IDM_TUNE_AD) return;
        pick = cmd - IDM_TUNE_AD;
    }
    /* A snapshot can land while the menu was open. */
    if (!g_snap || pick >= g_snap->n) return;

    wc_guid g = g_snap->row[pick].guid;
    wchar_t name[WC_NAME_MAX];
    wcscpy(name, g_snap->row[pick].name);
    tune_dialog(hwnd, &g, name);
}

/* --------------------------------------------------------------- dlg proc */

static void show_main(HWND hwnd)
{
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

static INT_PTR CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msg_taskbar && g_msg_taskbar) { g_tray_added = false; update_status(hwnd); return TRUE; }
    if (msg == g_msg_show && g_msg_show)       { show_main(hwnd); return TRUE; }

    switch (msg) {
    case WM_INITDIALOG: {
        g_ui = hwnd;
        ui_fonts_make(&g_f, ui_dpi(hwnd));
        ui_theme_window(hwnd);

        ui_control(hwnd, IDC_ENABLE, UI_SWITCH, L"Optimize", false);
        ui_control(hwnd, IDC_NUKE, UI_SWITCH, L"Stop all scanning", false);
        HWND nf = ui_combo(hwnd, IDC_NUKE_FOR, g_f.body, P(22));
        for (size_t k = 0; k < sizeof NUKE_FOR / sizeof NUKE_FOR[0]; ++k)
            SendMessageW(nf, CB_ADDSTRING, 0, (LPARAM)NUKE_FOR[k].label);
        SendMessageW(nf, CB_SETCURSEL, NUKE_DEFAULT, 0);
        g_cards = ui_panel(hwnd, IDC_CARDS, paint_cards, nullptr);
        ui_control(hwnd, IDC_TUNE, UI_LINK, L"Tuning  ›", false);
        ui_control(hwnd, IDC_REFRESH, UI_LINK, L"Refresh", false);
        ui_control(hwnd, IDOK, UI_BUTTON, L"Hide", false);
        apply_fonts(hwnd);

        g_icon_big = make_icon(GetSystemMetrics(SM_CXICON), RGB(43, 145, 72));
        if (g_icon_big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);

        layout(hwnd, nullptr);
        ui_center(hwnd, nullptr);
        update_status(hwnd);
        return TRUE;
    }

    case WM_ERASEBKGND:
        SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, 1);
        return TRUE;

    case WM_PAINT:
        paint_main(hwnd);
        return TRUE;

    case WM_DRAWITEM:
        return ui_draw_item((const DRAWITEMSTRUCT *)lp);

    case WM_CTLCOLORLISTBOX:
        return (INT_PTR)ui_ctlcolor((HDC)wp);

    case WM_U_SNAPSHOT: {
        free(g_snap);
        g_snap = (snapshot *)lp;
        ui_switch_set(GetDlgItem(hwnd, IDC_ENABLE), g_snap && g_snap->enabled);
        sync_manage(hwnd);
        InvalidateRect(g_cards, nullptr, FALSE);
        update_status(hwnd);
        return TRUE;
    }

    case WM_U_TRAY:
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) show_main(hwnd);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) tray_menu(hwnd);
        return TRUE;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        /* Owner-drawn buttons carry BS_NOTIFY, so focus changes arrive under
         * the same IDs; only a click acts.  Menus and Esc send a click. */
        const bool click = HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED;

        if (id >= IDC_MANAGE && id < IDC_MANAGE + g_manage_n) {
            const int i = id - IDC_MANAGE;
            if (!click || !g_snap || i >= g_snap->n) return TRUE;
            const bool on = !ui_switch_get(g_manage[i]);
            ui_switch_set(g_manage[i], on);
            g_snap->row[i].managed = on;
            cfg_save_managed(&g_snap->row[i].guid, on);
            send_manage(&g_snap->row[i].guid, on);
            InvalidateRect(g_cards, nullptr, FALSE);
            return TRUE;
        }

        switch (id) {
        case IDC_ENABLE:
            if (click) set_enabled(hwnd, !ui_switch_get(GetDlgItem(hwnd, IDC_ENABLE)));
            return TRUE;
        case IDM_TOGGLE:
            set_enabled(hwnd, !(g_snap && g_snap->enabled));
            return TRUE;
        case IDC_NUKE: {
            if (!click) return TRUE;
            const bool on = !ui_switch_get(GetDlgItem(hwnd, IDC_NUKE));
            if (on && !confirm_nuclear(hwnd)) return TRUE;
            set_nuclear(hwnd, on);
            return TRUE;
        }
        case IDC_REFRESH:
            if (click) PostMessageW(g_worker, WM_W_POLL, 0, 0);
            return TRUE;
        case IDC_TUNE:
            if (click) open_tuning(hwnd);
            return TRUE;
        case IDM_DARK:
            ui_set_theme(!ui_dark);
            cfg_save_dark(ui_dark);
            retheme(hwnd);
            return TRUE;
        case IDM_SHOW:
            show_main(hwnd);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            if (!click) return TRUE;
            ShowWindow(hwnd, SW_HIDE);
            if (!g_warned_tray) {
                g_warned_tray = true;
                tray_balloon(hwnd, L"WiFi Control",
                             L"Still running here. The settings only last while it runs.");
            }
            return TRUE;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return TRUE;
        }
        return FALSE;
    }

    case WM_TIMER:
        if (wp == T_DEADMAN) {
            KillTimer(hwnd, T_DEADMAN);
            set_nuclear(hwnd, false);
            tray_balloon(hwnd, L"WiFi Control",
                         L"Timer expired — scanning and auto configuration are back on.");
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
        ui_fonts_free(&g_f);
        ui_fonts_make(&g_f, HIWORD(wp));
        apply_fonts(hwnd);
        layout(hwnd, (const RECT *)lp);
        return TRUE;

    case WM_DESTROY:
        tray_remove(hwnd);
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ entry */

HWND main_window_create(HINSTANCE inst)
{
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    return CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dlg_proc, 0);
}

void main_window_start(HWND w, bool to_tray)
{
    if (to_tray) tray_update(w, L"WiFi Control");
    else         ShowWindow(w, SW_SHOW);
}

void main_window_cleanup(void)
{
    free(g_snap);
    g_snap = nullptr;
    if (g_icon_small) DestroyIcon(g_icon_small);
    if (g_icon_big)   DestroyIcon(g_icon_big);
    ui_fonts_free(&g_f);
}

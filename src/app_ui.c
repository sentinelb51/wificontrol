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
#define ICON_HELD RGB(128, 222, 234)   /* pastel cyan: the settings are in force */
/* The glyph's share of its icon.  The title bar's is smaller, so it sits level
 * with the caption instead of towering over it. */
#define ICON_FILL       0.875
#define ICON_FILL_TITLE 0.66

/* The switches, top to bottom: a label, the switch, then a hint.  The row
 * without a hint has the timer dropdown in its place.  Each also names what it
 * needs, so a switch whose service is missing is greyed out with the reason in
 * place of its hint rather than failing when it is flipped. */
typedef struct { int id; wc_switch sw; const wchar_t *label, *hint; } switch_row;
static const switch_row SWITCHES[] = {
    { IDC_BGSCAN,  WC_SW_BGSCAN,    L"No background scans",
      L"Stops the once-a-minute search for other networks" },
    { IDC_STREAM,  WC_SW_STREAMING, L"Streaming mode",
      L"Tells the driver latency matters more than power" },
    { IDC_NUKE,    WC_SW_NUCLEAR,   L"Block all scans",     nullptr },
    { IDC_METERED, WC_SW_METERED,   L"Metered",
      L"Windows holds back updates and background sync" },
    { IDC_PERF,    WC_SW_PERF,      L"Performance",
      L"No power saving or Wi-Fi Direct; restarts the adapter" },
};
enum { N_SWITCHES = sizeof SWITCHES / sizeof SWITCHES[0] };

/* Layout, in pixels at 96 dpi. */
enum {
    CLIENT_W = 500, PAD = 16, ROW_H = 28, ROW_STEP = 36, SWITCH_W = 44,
    SWITCH_X = 150, HINT_X = 210,
    STATUS_TOP = PAD + N_SWITCHES * ROW_STEP, STATUS_H = 36, CARDS_TOP = STATUS_TOP + 44,
    CARD_H = 168, CARD_GAP = 10, CARD_EMPTY = 64, CARDS_VISIBLE = 3,
    CARD_FOOT = 132,
};
#define P(v) ui_px((v), g_f.dpi)
#define ROW_TOP(k) P(PAD + (k) * ROW_STEP)

static snapshot *g_snap;
static HICON     g_icon_small, g_icon_big, g_icon_title;
static COLORREF  g_icon_color;
static bool      g_tray_added;
static bool      g_warned_tray;
static bool      g_nuke_confirmed;   /* asked once per session, not persisted */
static bool      g_perf_confirmed;   /* likewise */
static HPOWERNOTIFY g_plan_note;     /* the active power plan changing */
static bool      g_expect_repair;    /* we just disarmed: the next repair is ours */
static UINT      g_msg_taskbar;
static ui_fonts  g_f;
static HWND      g_cards;
static HWND      g_manage[WC_MAX_ADAPTERS];
static int       g_manage_n;
static wchar_t   g_status[512];
static enum { SEV_INFO, SEV_WARN, SEV_ERR } g_status_sev;
static unsigned  g_seq;              /* switch changes posted to the worker */
static wchar_t   g_why[N_SWITCHES][320]; /* why a switch is greyed out, or empty */

/* A switch the user has just flipped, and the moment they flipped it: when the
 * worker comes back having failed at it, the details window opens by itself.
 * Anything that fails on a poll is left to the status line and Details, since
 * nobody asked for it just then. */
static int       g_report_sw = -1;
static bool      g_report_on;
static bool      g_modal;            /* a window of ours is already modal here */
static unsigned  g_report_seq;
static unsigned long g_report_at;

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

static bool any_on(const snapshot *s)
{
    return s->bgscan_off || s->streaming_on || s->nuclear || s->metered || s->perf;
}

/* One of the two opcodes reads back as asked for. */
static bool opcode_held(const snapshot *s, const snap_row *r)
{
    return (s->bgscan_off && r->bgscan == WC_VAL_OFF) ||
           (s->streaming_on && r->streaming == WC_VAL_ON);
}

static COLORREF state_color(const snapshot *s)
{
    if (!s || s->open_err || s->enum_err) return RGB(200, 60, 60);
    const diag_sev worst = diag_worst(&s->diag);
    if (worst == DIAG_ERR)                return RGB(200, 60, 60);
    if (s->write_denied)                  return RGB(214, 152, 32);
    if (s->nuclear)                       return RGB(198, 86, 26); /* armed: unmistakable */
    if (worst == DIAG_WARN)               return RGB(214, 152, 32);
    for (int i = 0; i < s->n; ++i)
        if (s->row[i].present && s->row[i].last_err) return RGB(214, 152, 32);
    for (int i = 0; i < s->n; ++i)
        if (s->row[i].present && s->row[i].managed &&
            (opcode_held(s, &s->row[i]) || s->metered || s->perf))
            return ICON_HELD;
    return RGB(128, 132, 138);
}

static void tray_update(HWND hwnd, const wchar_t *tip)
{
    static wchar_t last_tip[128];
    COLORREF c = state_color(g_snap);

    if (g_tray_added && c == g_icon_color && wcsncmp(last_tip, tip, 127) == 0)
        return; /* nothing the shell would render differently */

    if (!g_tray_added || c != g_icon_color) {
        HICON fresh = make_icon(GetSystemMetrics(SM_CXSMICON), c, ICON_FILL);
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

/* Drawn at the window's DPI, rather than the title bar scaling the big one
 * down. */
static void title_icon(HWND hwnd)
{
    HICON fresh = make_icon(P(16), ICON_HELD, ICON_FILL_TITLE);
    if (!fresh) return;
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)fresh);
    if (g_icon_title) DestroyIcon(g_icon_title);
    g_icon_title = fresh;
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

/* Whether that switch is greyed out, as the last snapshot left it. */
static bool sw_blocked(wc_switch sw)
{
    for (int k = 0; k < N_SWITCHES; ++k)
        if (SWITCHES[k].sw == sw) return g_why[k][0] != L'\0';
    return false;
}

/* The state of a switch, and whether the menu can offer it at all. */
static UINT menu_flags(HWND hwnd, int id, wc_switch sw)
{
    return MF_STRING | (ui_switch_get(GetDlgItem(hwnd, id)) ? MF_CHECKED : 0)
                     | (sw_blocked(sw) ? MF_GRAYED : 0);
}

static void tray_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"&Show WiFi Control");
    AppendMenuW(m, menu_flags(hwnd, IDC_BGSCAN, WC_SW_BGSCAN), IDM_BGSCAN,
                L"No &background scans");
    AppendMenuW(m, menu_flags(hwnd, IDC_STREAM, WC_SW_STREAMING), IDM_STREAM,
                L"Streaming &mode");
    AppendMenuW(m, menu_flags(hwnd, IDC_PERF, WC_SW_PERF), IDM_PERF, L"&Performance");
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

static bool row_optimised(const snap_row *r)
{
    return r->present && !r->last_err && r->managed && any_on(g_snap) && !r->pending;
}

static void row_status(const snap_row *r, wchar_t *out, int cap)
{
    const wchar_t *s = !r->present      ? L"Not present"
                     : r->last_err      ? nullptr
                     : !r->managed      ? L"Not managed"
                     : !any_on(g_snap)  ? L"Off"
                     : r->pending       ? L"Waiting for a connection"
                                        : L"Optimised";
    if (!s) { wcw_format_error(r->last_err, out, cap); return; }
    wcsncpy(out, s, (size_t)cap - 1);
    out[cap - 1] = L'\0';
}

/* The card's left edge: green with a link, amber while getting one. */
static bool link_ink(const snap_row *r, COLORREF *ink)
{
    if (!r->present) return false;
    switch (r->state) {
    case WC_IF_CONNECTED:      *ink = ui_pal.ok;   return true;
    case WC_IF_ASSOCIATING:
    case WC_IF_DISCOVERING:
    case WC_IF_AUTHENTICATING: *ink = ui_pal.warn; return true;
    default:                   return false;
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
    COLORREF link;
    if (link_ink(r, &link))
        ui_rrect_band(c, x0, y, x1, y + P(CARD_H), 6 * s, x0, x0 + 4 * s, link);

    /* Header: name on the left, the link state on the right. */
    wchar_t state[64] = L"Not present";
    if (r->present) MultiByteToWideChar(CP_UTF8, 0, wc_state_name(r->state), -1, state, 64);
    const int sw = ui_text_w(c, g_f.body, state);
    ui_text(c, g_f.body, ui_pal.text2, state, right - sw, y + P(12), right, y + P(36), one);
    ui_text(c, g_f.bold, ui_pal.text, r->name, in, y + P(12), right - sw - P(12), y + P(36), one);

    /* What the driver reported back, ticked where it matches what we asked. */
    const bool mine = r->present && r->managed;
    const struct { const wchar_t *label; wc_val v; bool good; } rows[3] = {
        { L"Background scan", r->bgscan,    mine && g_snap->bgscan_off && r->bgscan == WC_VAL_OFF },
        { L"Streaming mode",  r->streaming, mine && g_snap->streaming_on && r->streaming == WC_VAL_ON },
        { L"Auto config",     r->autoconf,  false },
    };
    for (int k = 0; k < 3; ++k) {
        const int ry = y + P(42 + 22 * k);
        COLORREF ink = (k == 2 && r->autoconf == WC_VAL_OFF) ? ui_pal.warn : ui_pal.text;
        ui_text(c, g_f.body, ui_pal.text2, rows[k].label, in, ry, in + P(140), ry + P(22), one);
        ui_text(c, g_f.body, ink, val_text(rows[k].v), in + P(150), ry, in + P(190), ry + P(22), one);
        if (rows[k].good) check_mark(c, in + P(178), ry + P(11));
    }

    /* Metered belongs to each saved network, not the adapter, so it is a count. */
    const int my = y + P(42 + 22 * 3);
    const bool metering = mine && g_snap->metered;
    wchar_t met[48];
    if (r->metered < 0)                wcscpy(met, L"–");
    else if (!metering && !r->metered) wcscpy(met, L"off");
    else _snwprintf(met, 48, L"%d of %d networks", r->metered, r->profiles);
    met[47] = L'\0';
    ui_text(c, g_f.body, ui_pal.text2, L"Metered", in, my, in + P(140), my + P(22), one);
    ui_text(c, g_f.body, ui_pal.text, met, in + P(150), my, right, my + P(22), one);
    if (metering && r->profiles > 0 && r->metered == r->profiles)
        check_mark(c, in + P(150) + ui_text_w(c, g_f.body, met) + P(10), my + P(11));

    /* Footer: this adapter's status, and its Manage switch (a child window). */
    wchar_t status[256];
    row_status(r, status, 256);
    const COLORREF ink = r->last_err ? ui_pal.err : row_optimised(r) ? ui_pal.ok : ui_pal.text2;
    const int fy = y + P(CARD_FOOT), sx = right - P(SWITCH_W);
    const int lw = ui_text_w(c, g_f.body, L"Manage");
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
        const UINT one = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
        ui_fill(&c, 0, 0, c.w, c.h, ui_pal.bg);
        for (int k = 0; k < N_SWITCHES; ++k) {
            const int y = ROW_TOP(k);
            const bool blocked = g_why[k][0] != L'\0';
            ui_text(&c, g_f.body, blocked ? ui_pal.text2 : ui_pal.text, SWITCHES[k].label,
                    P(PAD), y, P(SWITCH_X - 8), y + P(ROW_H), one);
            /* The reason takes the hint's place: a dead switch with no
             * explanation beside it is the thing to avoid. */
            if (blocked)
                ui_text(&c, g_f.small, ui_pal.warn, g_why[k],
                        P(HINT_X), y, c.w - P(PAD), y + P(ROW_H), one);
            else if (SWITCHES[k].hint)
                ui_text(&c, g_f.small, ui_pal.text2, SWITCHES[k].hint,
                        P(HINT_X), y, c.w - P(PAD), y + P(ROW_H), one);
        }

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
                     L"A group policy or a Native Wifi permission change is blocking them. "
                     L"Details says which ones.");
        g_status_sev = SEV_WARN;
    } else if (g_snap->perf_busy) {
        wcscpy(text, L"Restarting the Wi-Fi adapter so its power settings take effect. "
                     L"The connection is back in a few seconds.");
    } else if (diag_count(&g_snap->diag, DIAG_WARN) > 0) {
        /* One line for the worst of it; the rest is a click away. */
        const diag_entry *e = diag_worst_entry(&g_snap->diag);
        const int n = diag_count(&g_snap->diag, DIAG_WARN);
        wchar_t what[128], subject[DIAG_SUBJ_MAX];
        MultiByteToWideChar(CP_UTF8, 0, diag_op_name(e->op), -1, what, 128);
        MultiByteToWideChar(CP_UTF8, 0, e->subject, -1, subject, DIAG_SUBJ_MAX);
        wcw_format_error(e->code, err, 256);
        if (n > 1)
            _snwprintf(text, 512, L"%d problems since the last check. %s: %s. Details has "
                                  L"all of them and what to do about them.", n, what, err);
        else if (subject[0])
            _snwprintf(text, 512, L"%s (%s): %s. Details says what to do about it.",
                       what, subject, err);
        else
            _snwprintf(text, 512, L"%s: %s. Details says what to do about it.", what, err);
        g_status_sev = diag_worst(&g_snap->diag) == DIAG_ERR ? SEV_ERR : SEV_WARN;
    } else {
        const snapshot *s = g_snap;
        int active = 0, waiting = 0, blocked = 0;
        for (int i = 0; i < s->n; ++i) {
            const snap_row *r = &s->row[i];
            if (!r->present || !r->managed) continue;
            if (r->pending) waiting++;
            else if (opcode_held(s, r) || s->metered || s->perf) active++;
            if (r->autoconf == WC_VAL_OFF) blocked++;
        }
        if (!any_on(s)) {
            wcscpy(text, L"Off. Windows defaults are in effect.");
        } else if (s->nuclear) {
            _snwprintf(text, 512, L"All scans blocked on %d adapter%s. No roaming and no "
                                  L"automatic reconnect while this is on.",
                       blocked, blocked == 1 ? L"" : L"s");
            g_status_sev = SEV_WARN;
        } else if (active || waiting) {
            _snwprintf(text, 512, L"Optimising %d adapter%s%s%s. %s",
                       active, active == 1 ? L"" : L"s",
                       s->perf ? L" with power saving and Wi-Fi Direct off" : L"",
                       waiting ? L", waiting on a connection for others" : L"",
                       s->perf ? L"All of it is put back when this app exits."
                               : L"Settings are released automatically when this app exits.");
        } else {
            wcscpy(text, L"No connected Wi-Fi adapter to optimise yet.");
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

/* ------------------------------------------------------------- switches */

/* Grey out every switch whose service is not on this machine, and keep the
 * sentence that says why for the row to paint.  Run on every snapshot, so a
 * Refresh that finds the WLAN service back brings them all to life. */
static void sync_availability(HWND hwnd)
{
    for (int k = 0; k < N_SWITCHES; ++k) {
        const char *why = g_snap ? wc_switch_blocked(g_snap->cap, SWITCHES[k].sw) : nullptr;
        wchar_t was[320];
        wcscpy(was, g_why[k]);
        if (!why) g_why[k][0] = L'\0';
        /* A reason that would not fit must still leave the switch dead: an
         * empty one reads as "nothing wrong with this switch". */
        else if (!MultiByteToWideChar(CP_UTF8, 0, why, -1, g_why[k], 320))
            wcscpy(g_why[k], L"Not available on this machine.");
        g_why[k][319] = L'\0';

        EnableWindow(GetDlgItem(hwnd, SWITCHES[k].id), why == nullptr);
        if (wcscmp(was, g_why[k]) != 0) {
            RECT r = { 0, ROW_TOP(k), P(CLIENT_W), ROW_TOP(k) + P(ROW_H) };
            InvalidateRect(hwnd, &r, FALSE);
        }
        /* The timer belongs to Block all scans: dead with it, and locked while
         * it is armed. */
        if (SWITCHES[k].sw == WC_SW_NUCLEAR)
            EnableWindow(GetDlgItem(hwnd, IDC_NUKE_FOR),
                         !why && !(g_snap && g_snap->nuclear));
    }

    /* Details carries the count, so the number of problems is visible without
     * reading the status line. */
    const int n = g_snap ? diag_count(&g_snap->diag, DIAG_WARN) : 0;
    wchar_t label[32];
    if (n > 0) _snwprintf(label, 32, L"Details (%d)", n);
    else       wcscpy(label, L"Details");
    HWND details = GetDlgItem(hwnd, IDC_DETAILS);
    wchar_t now[32] = L"";
    if (details) GetWindowTextW(details, now, 32);
    if (details && wcscmp(now, label) != 0) {
        SetWindowTextW(details, label);
        InvalidateRect(details, nullptr, FALSE);
    }
}

/* The one line that says what just went wrong, for the window that opens by
 * itself after a switch the user flipped could not be applied. */
static void report_headline(int sw, bool on, wchar_t *out, int cap)
{
    const wchar_t *text;
    switch (sw) {
    case WC_SW_BGSCAN:
        text = on ? L"Background scanning could not be turned off everywhere."
                  : L"Background scanning could not be handed back everywhere.";
        break;
    case WC_SW_STREAMING:
        text = on ? L"Streaming mode could not be set everywhere."
                  : L"Streaming mode could not be handed back everywhere.";
        break;
    case WC_SW_NUCLEAR:
        text = on ? L"Scanning could not be blocked everywhere."
                  : L"Scanning could not be given back everywhere. Anything still blocked "
                    L"is retried every minute, and when this app exits.";
        break;
    case WC_SW_METERED:
        text = on ? L"Not all of your saved networks could be marked as metered."
                  : L"Not all of your saved networks could be handed back to the Windows "
                    L"default.";
        break;
    default:
        text = on ? L"Performance was not applied in full: some of the settings below "
                    L"would not move."
                  : L"Not everything Performance changed could be put back. What is left "
                    L"is recorded, and is tried again when you switch it on and off, or "
                    L"the next time the app starts.";
        break;
    }
    wcsncpy(out, text, (size_t)cap - 1);
    out[cap - 1] = L'\0';
}

/* Remember that this switch is what the user just asked for, so the details
 * window opens by itself if the worker comes back having failed at it. */
static void expect_report(wc_switch sw, bool on)
{
    g_report_sw  = (int)sw;
    g_report_on  = on;
    g_report_seq = g_seq;
    g_report_at  = GetTickCount();
}

/* The worker can be busy for seconds, and a snapshot it sends before it gets
 * to a change still shows the old value.  So every change is numbered, each
 * snapshot says which it has seen, and one that is behind keeps what the
 * switches say instead of flipping them back. */
static void post_switch(UINT msg, bool on)
{
    if (PostMessageW(g_worker, msg, (WPARAM)on, (LPARAM)(g_seq + 1))) g_seq++;
}

static void keep_switches(HWND hwnd, snapshot *s)
{
    s->bgscan_off   = ui_switch_get(GetDlgItem(hwnd, IDC_BGSCAN));
    s->streaming_on = ui_switch_get(GetDlgItem(hwnd, IDC_STREAM));
    s->metered      = ui_switch_get(GetDlgItem(hwnd, IDC_METERED));
    s->perf         = ui_switch_get(GetDlgItem(hwnd, IDC_PERF));
    /* A Manage switch belongs to the adapter the last snapshot had in its row. */
    for (int i = 0; g_snap && i < s->n && i < g_snap->n && i < g_manage_n; ++i)
        if (memcmp(&s->row[i].guid, &g_snap->row[i].guid, sizeof s->row[i].guid) == 0)
            s->row[i].managed = ui_switch_get(g_manage[i]);
}

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
    expect_report(WC_SW_NUCLEAR, on);
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
        L"This blocks every scan, including the ones you or other apps ask for, which "
        L"is the point — but it also stops roaming to a better access point, and stops "
        L"Windows reconnecting on its own if the link drops.\n\n"
        L"It is put back automatically when the link drops, when you turn this off, "
        L"when the timer expires, and when this app exits or next starts. If the app "
        L"is killed outright, it stays off until you run it again.",
        L"Block all scans", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    g_nuke_confirmed = (r == IDYES);
    return g_nuke_confirmed;
}

/* The switches that are saved, unlike the arm above.  None of them can leave
 * anything stuck: the two opcodes are handed back when the app exits, and what
 * Metered and Performance change is recognised by its value or recorded. */
static void set_saved(HWND hwnd, int id, wc_switch sw, void (*save)(bool), UINT msg, bool on)
{
    ui_switch_set(GetDlgItem(hwnd, id), on);
    save(on);
    post_switch(msg, on);
    expect_report(sw, on);
}

static bool flipped(HWND hwnd, int id)
{
    return !ui_switch_get(GetDlgItem(hwnd, id));
}

static bool confirm_perf(HWND hwnd)
{
    if (g_perf_confirmed) return true;
    int r = MessageBoxW(hwnd,
        L"Turn off Wi-Fi power saving and Wi-Fi Direct?\n\n"
        L"On the managed adapters: MIMO power save to No SMPS, U-APSD and selective "
        L"suspend off, transmit power to highest. In the active power plan, plugged in "
        L"and on battery: wireless power saving to Maximum Performance, and PCI Express "
        L"link state power management off. That last one covers every PCIe device, so "
        L"expect shorter battery life.\n\n"
        L"The Wi-Fi Direct adapters are disabled too, so Miracast, Mobile Hotspot and "
        L"Wi-Fi Direct stop working.\n\n"
        L"The adapter restarts to pick up its settings, which drops Wi-Fi for a few "
        L"seconds. Every value is put back when you switch this off or exit the app, "
        L"with another restart, and before Windows shuts down.",
        L"Performance", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
    g_perf_confirmed = (r == IDYES);
    return g_perf_confirmed;
}

static void send_manage(const wc_guid *g, bool on)
{
    manage_cmd *c = malloc(sizeof *c);
    if (!c) return;
    c->g   = *g;
    c->on  = on;
    c->seq = g_seq + 1;
    if (PostMessageW(g_worker, WM_W_MANAGE, 0, (LPARAM)c)) g_seq++;
    else free(c);
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
    static const int ids[] = { IDC_NUKE_FOR, IDC_TUNE, IDC_REFRESH, IDC_DETAILS };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; ++i)
        SendMessageW(GetDlgItem(hwnd, ids[i]), WM_SETFONT, (WPARAM)g_f.body, FALSE);
    for (int k = 0; k < N_SWITCHES; ++k)
        SendMessageW(GetDlgItem(hwnd, SWITCHES[k].id), WM_SETFONT, (WPARAM)g_f.body, FALSE);
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

    int nuke_top = ROW_TOP(0);
    for (int k = 0; k < N_SWITCHES; ++k) {
        SetWindowPos(GetDlgItem(hwnd, SWITCHES[k].id), nullptr, P(SWITCH_X), ROW_TOP(k),
                     P(SWITCH_W), P(ROW_H), fl);
        if (SWITCHES[k].id == IDC_NUKE) nuke_top = ROW_TOP(k);
    }
    HWND cb = GetDlgItem(hwnd, IDC_NUKE_FOR);
    SetWindowPos(cb, nullptr, P(HINT_X), nuke_top, cw - P(HINT_X) - P(PAD), P(200), fl);
    RECT r;
    GetWindowRect(cb, &r);
    SetWindowPos(cb, nullptr, P(HINT_X), nuke_top + (P(ROW_H) - (r.bottom - r.top)) / 2,
                 0, 0, fl | SWP_NOSIZE);

    SetWindowPos(g_cards, nullptr, 0, P(CARDS_TOP), cw, view, fl);
    ui_panel_set_content(g_cards, content);
    place_manage();

    const int by = ch - P(PAD) - P(ROW_H);
    SetWindowPos(GetDlgItem(hwnd, IDC_TUNE),    nullptr, P(PAD - 8),       by, P(84), P(ROW_H), fl);
    SetWindowPos(GetDlgItem(hwnd, IDC_REFRESH), nullptr, P(PAD - 8 + 88),  by, P(72), P(ROW_H), fl);
    SetWindowPos(GetDlgItem(hwnd, IDC_DETAILS), nullptr, P(PAD - 8 + 164), by, P(92), P(ROW_H), fl);
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
    /* Snapshots still arrive while this is up; nothing else may open a window
     * over it. */
    g_modal = true;
    tune_dialog(hwnd, &g, name, g_snap->perf);
    g_modal = false;
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

        /* In screen order, which makes it the tab order. */
        for (int k = 0; k < N_SWITCHES; ++k) {
            ui_control(hwnd, SWITCHES[k].id, UI_SWITCH, SWITCHES[k].label, false);
            if (SWITCHES[k].id != IDC_NUKE) continue;
            HWND nf = ui_combo(hwnd, IDC_NUKE_FOR, g_f.body, P(22));
            for (size_t i = 0; i < sizeof NUKE_FOR / sizeof NUKE_FOR[0]; ++i)
                SendMessageW(nf, CB_ADDSTRING, 0, (LPARAM)NUKE_FOR[i].label);
            SendMessageW(nf, CB_SETCURSEL, NUKE_DEFAULT, 0);
        }
        g_cards = ui_panel(hwnd, IDC_CARDS, paint_cards, nullptr);
        g_plan_note = RegisterPowerSettingNotification(hwnd, &GUID_ACTIVE_POWERSCHEME,
                                                       DEVICE_NOTIFY_WINDOW_HANDLE);
        ui_control(hwnd, IDC_TUNE, UI_LINK, L"Tuning  ›", false);
        ui_control(hwnd, IDC_REFRESH, UI_LINK, L"Refresh", false);
        ui_control(hwnd, IDC_DETAILS, UI_LINK, L"Details", false);
        apply_fonts(hwnd);

        g_icon_big = make_icon(GetSystemMetrics(SM_CXICON), ICON_HELD, ICON_FILL);
        if (g_icon_big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);
        title_icon(hwnd);

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

    case WM_MOUSEWHEEL:
        /* From a focused switch or the timer dropdown: the cards are the only
         * thing here that scrolls. */
        SendMessageW(g_cards, msg, wp, lp);
        return TRUE;

    case WM_U_SNAPSHOT: {
        snapshot *s = (snapshot *)lp;
        if (s && s->seq != g_seq) keep_switches(hwnd, s);
        free(g_snap);
        g_snap = s;
        ui_switch_set(GetDlgItem(hwnd, IDC_BGSCAN), g_snap && g_snap->bgscan_off);
        ui_switch_set(GetDlgItem(hwnd, IDC_STREAM), g_snap && g_snap->streaming_on);
        ui_switch_set(GetDlgItem(hwnd, IDC_METERED), g_snap && g_snap->metered);
        ui_switch_set(GetDlgItem(hwnd, IDC_PERF), g_snap && g_snap->perf);
        sync_manage(hwnd);
        sync_availability(hwnd);
        InvalidateRect(g_cards, nullptr, FALSE);
        update_status(hwnd);

        /* The worker has caught up with the switch the user flipped -- and a
         * restart sends a snapshot part way through, which is not the answer.
         * If anything failed since they flipped it, say so in full rather than
         * in one line they may not be looking at. */
        if (g_report_sw >= 0 && g_snap && !g_modal && !g_snap->perf_busy &&
            g_snap->seq >= g_report_seq) {
            const int sw = g_report_sw;
            const bool on = g_report_on;
            g_report_sw = -1;
            if (diag_count_since(&g_snap->diag, DIAG_WARN, g_report_at) > 0) {
                wchar_t headline[512];
                report_headline(sw, on, headline, 512);
                g_modal = true;
                diag_dialog(hwnd, g_snap, headline);
                g_modal = false;
            }
        }
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
        case IDC_BGSCAN:
        case IDM_BGSCAN:
            if (click)
                set_saved(hwnd, IDC_BGSCAN, WC_SW_BGSCAN, cfg_save_bgscan_off, WM_W_BGSCAN,
                          flipped(hwnd, IDC_BGSCAN));
            return TRUE;
        case IDC_STREAM:
        case IDM_STREAM:
            if (click)
                set_saved(hwnd, IDC_STREAM, WC_SW_STREAMING, cfg_save_streaming_on,
                          WM_W_STREAMING, flipped(hwnd, IDC_STREAM));
            return TRUE;
        case IDC_NUKE: {
            if (!click) return TRUE;
            const bool on = flipped(hwnd, IDC_NUKE);
            if (on && !confirm_nuclear(hwnd)) return TRUE;
            set_nuclear(hwnd, on);
            return TRUE;
        }
        case IDC_METERED:
            if (click)
                set_saved(hwnd, IDC_METERED, WC_SW_METERED, cfg_save_metered, WM_W_METERED,
                          flipped(hwnd, IDC_METERED));
            return TRUE;
        case IDC_PERF:
        case IDM_PERF: {
            if (!click) return TRUE;
            const bool on = flipped(hwnd, IDC_PERF);
            if (on && !confirm_perf(hwnd)) return TRUE;
            set_saved(hwnd, IDC_PERF, WC_SW_PERF, cfg_save_perf, WM_W_PERF, on);
            return TRUE;
        }
        case IDC_REFRESH:
            /* Not just another poll: what is available is probed again, and the
             * failure log starts over, so what is shown describes now. */
            if (click) PostMessageW(g_worker, WM_W_RECHECK, 0, 0);
            return TRUE;
        case IDC_DETAILS:
            if (click) diag_dialog(hwnd, g_snap, nullptr);
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
        case IDCANCEL: /* the X, and Esc */
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
         * post, so the worker has actually written it before we return.  A
         * metered cost is left in place on purpose: the next start applies or
         * repairs it from the saved switch, and Windows Update gets no
         * unmetered window before then.  Performance values go back, without
         * a restart: the reboot is what applies them. */
        if (wp && g_worker) {
            SendMessageW(g_worker, WM_W_NUCLEAR, 0, 0);
            SendMessageW(g_worker, WM_W_PERF_END, 0, 0);
        }
        return TRUE;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { ShowWindow(hwnd, SW_HIDE); return TRUE; }
        return FALSE;

    case WM_POWERBROADCAST:
        /* Notifications can be missed across a suspend; re-apply on resume. */
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)
            PostMessageW(g_worker, WM_W_POLL, 0, 0);
        /* The only setting registered for is the active plan. */
        else if (wp == PBT_POWERSETTINGCHANGE && g_worker)
            PostMessageW(g_worker, WM_W_PLAN, 0, 0);
        return TRUE;

    case WM_DPICHANGED:
        ui_fonts_free(&g_f);
        ui_fonts_make(&g_f, HIWORD(wp));
        title_icon(hwnd);
        apply_fonts(hwnd);
        layout(hwnd, (const RECT *)lp);
        return TRUE;

    case WM_DESTROY:
        tray_remove(hwnd);
        if (g_plan_note) UnregisterPowerSettingNotification(g_plan_note);
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
    if (g_icon_title) DestroyIcon(g_icon_title);
    ui_fonts_free(&g_f);
}

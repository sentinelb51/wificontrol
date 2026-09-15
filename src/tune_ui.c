/* tune_ui.c -- the tuning window.
 *
 * One row per setting the system says has a fixed set of choices, each with
 * its own dropdown of exactly those choices, and an Apply button.  The app
 * never proposes a value and never remembers a previous one; to undo something
 * you pick the other entry in the same dropdown.
 */
#define WIN32_LEAN_AND_MEAN
#include "tune.h"
#include "ui.h"
#include "wlan_win32.h"
#include "resource.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* Layout, in pixels at 96 dpi. */
enum {
    CLIENT_W = 600, PAD = 16, TITLE_H = 48, FOOT_H = 86, VIEW_MAX = 460,
    SECTION_H = 36, ROW_H = 44, HINT_H = 40, COMBO_W = 240,
    HELP_ROW_H = 62, HEAD_H = 42, HELP_Y = 36, HELP_H = 18,
    POWER_ROW_H = 84, POWER_COMBOS_Y = 48, POWER_LABEL_W = 74, POWER_COMBO_W = 170,
    BUTTON_W = 88, BUTTON_H = 30, RESTART_W = 124,
};
#define P(v) ui_px((v), t->f.dpi)

/* A row is one setting; a power row pairs its plugged-in and on-battery
 * entries.  Either index is -1 when that half is absent. */
typedef struct { int ac, dc; } trow;

typedef struct {
    wc_guid    adapter;
    wchar_t    adapter_name[WC_NAME_MAX];
    tune_list *list;
    ui_fonts   f;
    HWND       dlg, panel;
    trow       driver[TUNE_MAX_SETTINGS], power[TUNE_MAX_SETTINGS];
    int        n_driver, n_power;
    int        wfd;                             /* the Wi-Fi Direct setting, or -1 */
    int        driver_y[TUNE_MAX_SETTINGS + 1]; /* each adapter row's offset below driver_top */
    int        driver_top, driver_hint, wfd_top, wfd_hint, power_top, power_hint, content;
    wchar_t    status[256];
    bool       status_err;
} tune_ctx;

static void collect(tune_ctx *t)
{
    memset(t->list, 0, sizeof *t->list);
    tune_collect_driver(t->list, &t->adapter);
    tune_collect_wfd(t->list);
    tune_collect_power(t->list);
}

static bool any_pending(const tune_list *l)
{
    for (int i = 0; i < l->n; ++i)
        if (l->s[i].sel >= 0 && l->s[i].sel != l->s[i].cur) return true;
    return false;
}

static bool row_pending(const tune_list *l, trow r)
{
    for (int k = 0; k < 2; ++k) {
        int i = k ? r.dc : r.ac;
        if (i >= 0 && l->s[i].sel >= 0 && l->s[i].sel != l->s[i].cur) return true;
    }
    return false;
}

/* An adapter row with a description keeps its name and dropdown in a band at
 * the top and gives the description its own line underneath. */
static int row_h (const tune_ctx *t, const tune_setting *s) { return P(s->help ? HELP_ROW_H : ROW_H); }
static int head_h(const tune_ctx *t, const tune_setting *s) { return P(s->help ? HEAD_H : ROW_H); }

/* ------------------------------------------------------------------ paint */

static void paint_card_frame(tune_ctx *t, ui_canvas *cv, int y0, int y1)
{
    const double s = t->f.dpi / 96.0;
    ui_frame(cv, P(PAD), y0, cv->w - P(PAD), y1, 6 * s, 1 * s, ui_pal.border, ui_pal.card);
}

static void paint_rule(tune_ctx *t, ui_canvas *cv, int y)
{
    int h = P(1) > 0 ? P(1) : 1;
    ui_fill(cv, P(PAD) + P(14), y, cv->w - P(PAD) - P(14), y + h, ui_pal.border);
}

static void paint_marker(tune_ctx *t, ui_canvas *cv, int y0, int y1)
{
    const double s = t->f.dpi / 96.0;
    ui_rrect(cv, P(PAD) + 4 * s, y0 + P(12), P(PAD) + 7 * s, y1 - P(12), 1.5 * s, ui_pal.accent);
}

static void paint_content([[maybe_unused]] HWND panel, ui_canvas *cv, int scroll, void *ctx)
{
    tune_ctx *t = ctx;
    const tune_list *l = t->list;
    const int in = P(PAD) + P(14), right = cv->w - P(PAD) - P(14);
    const UINT one = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;

    /* Adapter properties */
    int y = t->driver_top - scroll;
    ui_text(cv, t->f.bold, ui_pal.text, L"Adapter properties",
            P(PAD) + P(2), y - P(SECTION_H), right, y - P(8), DT_SINGLELINE | DT_BOTTOM);
    paint_card_frame(t, cv, y, t->driver_hint - scroll);
    if (t->n_driver == 0) {
        ui_text(cv, t->f.body, ui_pal.text2,
                l->have_instance ? L"The driver declares no settings with a fixed list of choices."
                                 : L"Could not locate this adapter's driver key.",
                in, y, right, y + P(ROW_H), one);
    }
    for (int k = 0; k < t->n_driver; ++k) {
        const int ry = y + t->driver_y[k];
        const tune_setting *s = &l->s[t->driver[k].ac];
        if (k) paint_rule(t, cv, ry);
        if (row_pending(l, t->driver[k])) paint_marker(t, cv, ry, y + t->driver_y[k + 1]);
        ui_text(cv, t->f.body, ui_pal.text, s->name, in, ry, right - P(COMBO_W) - P(12),
                ry + head_h(t, s), one);
        if (s->help)
            ui_text(cv, t->f.small, ui_pal.text2, s->help, in, ry + P(HELP_Y), right,
                    ry + P(HELP_Y + HELP_H), one);
    }
    y = t->driver_hint - scroll;
    ui_text(cv, t->f.small, ui_pal.text2,
            L"Read by the driver when it starts: restart the adapter to use a change.",
            P(PAD) + P(2), y, cv->w - P(PAD) - P(RESTART_W) - P(12), y + P(HINT_H), one);

    /* Wi-Fi Direct, only when the card has the virtual adapters at all */
    if (t->wfd >= 0) {
        const tune_setting *s = &l->s[t->wfd];
        y = t->wfd_top - scroll;
        ui_text(cv, t->f.bold, ui_pal.text, L"Wi-Fi Direct",
                P(PAD) + P(2), y - P(SECTION_H), right, y - P(8), DT_SINGLELINE | DT_BOTTOM);
        paint_card_frame(t, cv, y, t->wfd_hint - scroll);
        if (row_pending(l, (trow){ t->wfd, -1 })) paint_marker(t, cv, y, t->wfd_hint - scroll);
        ui_text(cv, t->f.body, ui_pal.text, s->name, in, y, right - P(COMBO_W) - P(12),
                y + head_h(t, s), one);
        ui_text(cv, t->f.small, ui_pal.text2, s->help, in, y + P(HELP_Y), right,
                y + P(HELP_Y + HELP_H), one);
        y = t->wfd_hint - scroll;
        ui_text(cv, t->f.small, ui_pal.text2,
                L"Applied to the devices when you press Apply, and kept across restarts.",
                P(PAD) + P(2), y, cv->w - P(PAD), y + P(HINT_H), one);
    }

    /* Power */
    y = t->power_top - scroll;
    ui_text(cv, t->f.bold, ui_pal.text, L"Power",
            P(PAD) + P(2), y - P(SECTION_H), right, y - P(8), DT_SINGLELINE | DT_BOTTOM);
    paint_card_frame(t, cv, y, t->power_hint - scroll);

    if (t->n_power == 0)
        ui_text(cv, t->f.body, ui_pal.text2, L"Neither setting exists on this machine.",
                in, y, right, y + P(ROW_H), one);
    for (int k = 0; k < t->n_power; ++k) {
        const int ry = y + k * P(POWER_ROW_H);
        const tune_setting *s = &l->s[t->power[k].ac >= 0 ? t->power[k].ac : t->power[k].dc];
        if (k) paint_rule(t, cv, ry);
        if (row_pending(l, t->power[k])) paint_marker(t, cv, ry, ry + P(POWER_ROW_H));
        /* The group goes at the right of the name, leaving the second line to
         * the description. */
        int gw = ui_text_w(cv, t->f.small, s->group);
        if (gw > (right - in) / 2) gw = (right - in) / 2;
        ui_text(cv, t->f.body, ui_pal.text, s->name, in, ry + P(10), right - gw - P(12),
                ry + P(30), one);
        ui_text(cv, t->f.small, ui_pal.text2, s->group, right - gw, ry + P(10), right,
                ry + P(30), one | DT_RIGHT);
        if (s->help)
            ui_text(cv, t->f.small, ui_pal.text2, s->help, in, ry + P(28), right, ry + P(46), one);

        /* Two dropdowns, because Windows keeps the two values separately. */
        const int cy = ry + P(POWER_COMBOS_Y);
        const int battery_x = in + P(POWER_LABEL_W + POWER_COMBO_W + 20);
        ui_text(cv, t->f.small, ui_pal.text2, L"Plugged in", in, cy,
                in + P(POWER_LABEL_W), cy + P(28), one);
        ui_text(cv, t->f.small, ui_pal.text2, L"On battery", battery_x, cy,
                battery_x + P(POWER_LABEL_W), cy + P(28), one);
    }
    y = t->power_hint - scroll;
    ui_text(cv, t->f.small, ui_pal.text2,
            L"Written to the active power plan, and in effect as soon as you press Apply.",
            P(PAD) + P(2), y, cv->w - P(PAD), y + P(HINT_H), one);
}

static void paint_frame(tune_ctx *t)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(t->dlg, &ps);
    RECT rc;
    GetClientRect(t->dlg, &rc);
    ui_canvas cv;
    if (ui_canvas_begin(&cv, dc, rc.right, rc.bottom)) {
        const int line = P(1) > 0 ? P(1) : 1;
        ui_fill(&cv, 0, 0, cv.w, cv.h, ui_pal.bg);
        ui_text(&cv, t->f.title, ui_pal.text, t->adapter_name[0] ? t->adapter_name : L"Tuning",
                P(PAD), 0, cv.w - P(PAD), P(TITLE_H), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        ui_fill(&cv, 0, P(TITLE_H) - line, cv.w, P(TITLE_H), ui_pal.border);

        const int foot = cv.h - P(FOOT_H);
        ui_fill(&cv, 0, foot, cv.w, foot + line, ui_pal.border);
        ui_text(&cv, t->f.body, t->status_err ? ui_pal.err : ui_pal.text2, t->status,
                P(PAD), foot + P(8), cv.w - P(PAD), foot + P(34),
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        ui_canvas_end(&cv, dc, 0, 0);
    }
    EndPaint(t->dlg, &ps);
}

/* ----------------------------------------------------------------- layout */

static void build(tune_ctx *t)
{
    for (int i = 0; i < TUNE_MAX_SETTINGS; ++i) {
        HWND old = GetDlgItem(t->panel, IDC_TUNE_VALUE + i);
        if (old) DestroyWindow(old);
    }

    const tune_list *l = t->list;
    t->n_driver = t->n_power = 0;
    t->wfd = -1;
    for (int i = 0; i < l->n; ++i) {
        const tune_setting *s = &l->s[i];
        if (s->src == TUNE_DRIVER) {
            t->driver[t->n_driver++] = (trow){ i, -1 };
            continue;
        }
        if (s->src == TUNE_DEVICE) {
            t->wfd = i;
            continue;
        }
        /* tune_collect_power adds plugged in, then on battery. */
        trow *last = t->n_power ? &t->power[t->n_power - 1] : nullptr;
        if (s->on_battery && last && last->ac >= 0 && last->dc < 0 &&
            memcmp(&l->s[last->ac].setting, &s->setting, sizeof(GUID)) == 0)
            last->dc = i;
        else
            t->power[t->n_power++] = s->on_battery ? (trow){ -1, i } : (trow){ i, -1 };
    }

    for (int i = 0; i < l->n; ++i) {
        const tune_setting *s = &l->s[i];
        HWND cb = ui_combo(t->panel, IDC_TUNE_VALUE + i, t->f.body, P(22));
        if (!cb) continue;
        for (int k = 0; k < s->n_opt; ++k)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s->opt[k].label);
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)s->sel, 0);
        if (s->sel < 0) SendMessageW(cb, CB_SETCUEBANNER, 0, (LPARAM)L"Not one of the choices");
        SetWindowTextW(cb, s->name); /* the name a screen reader reads */
    }

    /* Recreated dropdowns land after Restart in the tab order; put it back
     * straight after the adapter's own dropdowns, where it sits on screen. */
    HWND restart = GetDlgItem(t->panel, IDC_TUNE_RESTART);
    HWND after = t->n_driver ? GetDlgItem(t->panel, IDC_TUNE_VALUE + t->driver[t->n_driver - 1].ac)
                             : nullptr;
    if (restart)
        SetWindowPos(restart, after ? after : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void place(HWND w, int x, int y, int cw, int ch)
{
    if (w) SetWindowPos(w, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void layout(tune_ctx *t, const RECT *at)
{
    t->driver_y[0] = 0;
    for (int k = 0; k < t->n_driver; ++k)
        t->driver_y[k + 1] = t->driver_y[k] + row_h(t, &t->list->s[t->driver[k].ac]);
    t->driver_top  = P(SECTION_H);
    t->driver_hint = t->driver_top + (t->n_driver ? t->driver_y[t->n_driver] : P(ROW_H));
    int next       = t->driver_hint + P(HINT_H) + P(SECTION_H);
    if (t->wfd >= 0) {
        t->wfd_top  = next;
        t->wfd_hint = t->wfd_top + row_h(t, &t->list->s[t->wfd]);
        next        = t->wfd_hint + P(HINT_H) + P(SECTION_H);
    }
    t->power_top   = next;
    t->power_hint  = t->power_top + (t->n_power ? t->n_power * P(POWER_ROW_H) : P(ROW_H));
    t->content     = t->power_hint + P(HINT_H);

    const int view = t->content < P(VIEW_MAX) ? t->content : P(VIEW_MAX);
    const int cw = P(CLIENT_W), ch = P(TITLE_H) + view + P(FOOT_H);
    ui_size_window(t->dlg, cw, ch, t->f.dpi, at);

    place(t->panel, 0, P(TITLE_H), cw, view);
    ui_panel_set_content(t->panel, t->content);

    RECT pr;
    GetClientRect(t->panel, &pr);
    const int scroll = ui_panel_scroll(t->panel);
    const int right = pr.right - P(PAD) - P(12);
    const tune_list *l = t->list;

    /* A dropdown list's window height is its closed height, whatever is asked. */
    int combo_h = P(28);
    HWND first = l->n ? GetDlgItem(t->panel, IDC_TUNE_VALUE) : nullptr;
    if (first) {
        place(first, 0, 0, P(COMBO_W), P(300));
        RECT r;
        GetWindowRect(first, &r);
        combo_h = r.bottom - r.top;
    }

    for (int k = 0; k < t->n_driver; ++k) {
        const int ry = t->driver_top + t->driver_y[k] - scroll;
        place(GetDlgItem(t->panel, IDC_TUNE_VALUE + t->driver[k].ac), right - P(COMBO_W),
              ry + (head_h(t, &l->s[t->driver[k].ac]) - combo_h) / 2, P(COMBO_W), P(300));
    }
    if (t->wfd >= 0)
        place(GetDlgItem(t->panel, IDC_TUNE_VALUE + t->wfd), right - P(COMBO_W),
              t->wfd_top - scroll + (head_h(t, &l->s[t->wfd]) - combo_h) / 2,
              P(COMBO_W), P(300));
    const int in   = P(PAD) + P(14);
    const int ac_x = in + P(POWER_LABEL_W);
    const int dc_x = in + P(POWER_LABEL_W + POWER_COMBO_W + 20 + POWER_LABEL_W);
    for (int k = 0; k < t->n_power; ++k) {
        const int ry = t->power_top + k * P(POWER_ROW_H) - scroll;
        const int cy = ry + P(POWER_COMBOS_Y) + (P(28) - combo_h) / 2;
        if (t->power[k].ac >= 0)
            place(GetDlgItem(t->panel, IDC_TUNE_VALUE + t->power[k].ac), ac_x, cy,
                  P(POWER_COMBO_W), P(300));
        if (t->power[k].dc >= 0)
            place(GetDlgItem(t->panel, IDC_TUNE_VALUE + t->power[k].dc), dc_x, cy,
                  P(POWER_COMBO_W), P(300));
    }

    place(GetDlgItem(t->panel, IDC_TUNE_RESTART), pr.right - P(PAD) - P(RESTART_W),
          t->driver_hint + (P(HINT_H) - P(BUTTON_H - 2)) / 2 - scroll,
          P(RESTART_W), P(BUTTON_H - 2));

    const int by = ch - P(PAD) - P(BUTTON_H);
    place(GetDlgItem(t->dlg, IDOK), cw - P(PAD) - P(BUTTON_W), by, P(BUTTON_W), P(BUTTON_H));
    place(GetDlgItem(t->dlg, IDC_TUNE_APPLY), cw - P(PAD) - 2 * P(BUTTON_W) - P(8), by,
          P(BUTTON_W), P(BUTTON_H));
    InvalidateRect(t->dlg, nullptr, FALSE);
}

static void apply_fonts(tune_ctx *t)
{
    HWND ctl[] = { GetDlgItem(t->dlg, IDOK), GetDlgItem(t->dlg, IDC_TUNE_APPLY),
                   GetDlgItem(t->panel, IDC_TUNE_RESTART) };
    for (size_t i = 0; i < sizeof ctl / sizeof ctl[0]; ++i)
        if (ctl[i]) SendMessageW(ctl[i], WM_SETFONT, (WPARAM)t->f.body, FALSE);
    for (int i = 0; i < t->list->n; ++i) {
        HWND cb = GetDlgItem(t->panel, IDC_TUNE_VALUE + i);
        if (!cb) continue;
        SendMessageW(cb, WM_SETFONT, (WPARAM)t->f.body, FALSE);
        SendMessageW(cb, CB_SETITEMHEIGHT, (WPARAM)-1, P(22));
        SendMessageW(cb, CB_SETITEMHEIGHT, 0, P(22));
    }
}

static void refresh_state(tune_ctx *t)
{
    EnableWindow(GetDlgItem(t->dlg, IDC_TUNE_APPLY), any_pending(t->list));
    InvalidateRect(t->panel, nullptr, FALSE);
    InvalidateRect(t->dlg, nullptr, FALSE);
}

/* Re-read so every row shows what the system actually holds now. */
static void reload(tune_ctx *t)
{
    collect(t);
    build(t);
    layout(t, nullptr);
    ShowWindow(GetDlgItem(t->panel, IDC_TUNE_RESTART), t->list->have_instance ? SW_SHOW : SW_HIDE);
    refresh_state(t);
    SendMessageW(t->dlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(t->dlg, IDOK), TRUE);
}

/* ---------------------------------------------------------------- actions */

static void do_apply(tune_ctx *t)
{
    unsigned long err = 0;
    int n = tune_apply(t->list, &err);

    if (err) {
        wchar_t detail[200];
        wcw_format_error(err, detail, 200);
        _snwprintf(t->status, 256, L"Wrote %d setting%s. At least one failed: %s",
                   n, n == 1 ? L"" : L"s", detail);
    } else if (n == 0) {
        wcscpy(t->status, L"Nothing to write: no dropdown was changed.");
    } else {
        _snwprintf(t->status, 256, L"Wrote %d setting%s.", n, n == 1 ? L"" : L"s");
    }
    t->status[255] = L'\0';
    t->status_err  = err != 0;
    reload(t);
}

static void do_restart(tune_ctx *t)
{
    if (MessageBoxW(t->dlg,
            L"Restart the Wi-Fi adapter now?\n\n"
            L"This disables and re-enables the device so the driver re-reads its "
            L"properties. The connection drops for a few seconds.",
            L"Restart adapter", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;

    HWND restart = GetDlgItem(t->panel, IDC_TUNE_RESTART), apply = GetDlgItem(t->dlg, IDC_TUNE_APPLY);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    EnableWindow(restart, FALSE);
    EnableWindow(apply, FALSE);

    unsigned long e = tune_restart_adapter(t->list);

    EnableWindow(restart, TRUE);
    SetCursor(old);

    if (e != ERROR_SUCCESS) {
        wchar_t detail[200], msg[400];
        wcw_format_error(e, detail, 200);
        _snwprintf(msg, 400, L"Could not restart the adapter: %s\n\n"
                             L"If it is now disabled, re-enable it in Device Manager.", detail);
        msg[399] = L'\0';
        MessageBoxW(t->dlg, msg, L"Restart adapter", MB_ICONWARNING);
        _snwprintf(t->status, 256, L"Restart failed: %s", detail);
        t->status_err = true;
    } else {
        wcscpy(t->status, L"Adapter restarted.");
        t->status_err = false;
    }
    t->status[255] = L'\0';
    reload(t);
}

/* ---------------------------------------------------------------- dialog */

static INT_PTR CALLBACK tune_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    tune_ctx *t = (tune_ctx *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        t = (tune_ctx *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)t);
        t->dlg = dlg;
        ui_fonts_make(&t->f, ui_dpi(dlg));
        ui_theme_window(dlg);

        t->panel = ui_panel(dlg, IDC_TUNE_PANEL, paint_content, t);
        ui_control(t->panel, IDC_TUNE_RESTART, UI_BUTTON, L"Restart adapter", false);
        ui_control(dlg, IDC_TUNE_APPLY, UI_PRIMARY, L"Apply", false);
        ui_control(dlg, IDOK, UI_BUTTON, L"Close", false);

        wcscpy(t->status, L"Nothing is written until you press Apply.");
        collect(t);
        build(t);
        apply_fonts(t);
        layout(t, nullptr);
        ui_center(dlg, GetParent(dlg));
        ShowWindow(GetDlgItem(t->panel, IDC_TUNE_RESTART), t->list->have_instance ? SW_SHOW : SW_HIDE);
        refresh_state(t);
        /* Not the first tab stop: that sits in the scrolling list, and focusing
         * it would scroll the list away from its top. */
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dlg, IDOK), TRUE);
        return FALSE;

    case WM_ERASEBKGND:
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
        return TRUE;

    case WM_PAINT:
        if (t) paint_frame(t);
        return TRUE;

    case WM_DRAWITEM:
        return ui_draw_item((const DRAWITEMSTRUCT *)lp);

    case WM_CTLCOLORLISTBOX:
        return (INT_PTR)ui_ctlcolor((HDC)wp);

    case WM_COMMAND: {
        if (!t) return FALSE;
        const int id = LOWORD(wp);
        /* Owner-drawn buttons carry BS_NOTIFY, so focus changes arrive under
         * the same IDs; only a click acts.  Esc and the close box send one. */
        const bool click = HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED;

        if (id >= IDC_TUNE_VALUE && id < IDC_TUNE_VALUE + t->list->n) {
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int k = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (k >= 0) t->list->s[id - IDC_TUNE_VALUE].sel = k;
                refresh_state(t);
            }
            return TRUE;
        }
        switch (id) {
        case IDC_TUNE_APPLY:   if (click) do_apply(t);   return TRUE;
        case IDC_TUNE_RESTART: if (click) do_restart(t); return TRUE;
        case IDOK:
        case IDCANCEL:         if (click) EndDialog(dlg, 0); return TRUE;
        }
        return FALSE;
    }

    case WM_DPICHANGED:
        if (!t) return FALSE;
        ui_fonts_free(&t->f);
        ui_fonts_make(&t->f, HIWORD(wp));
        apply_fonts(t);
        layout(t, (const RECT *)lp);
        return TRUE;

    case WM_DESTROY:
        if (t) ui_fonts_free(&t->f);
        return FALSE;
    }
    return FALSE;
}

void tune_dialog(HWND parent, const wc_guid *adapter, const wchar_t *adapter_name)
{
    /* Big enough (two row tables) that it is not a stack object either. */
    tune_ctx *t = calloc(1, sizeof *t);
    if (!t) return;
    t->adapter = *adapter;
    wcsncpy(t->adapter_name, adapter_name ? adapter_name : L"", WC_NAME_MAX - 1);

    /* Well over a megabyte with the option tables: not a stack object. */
    t->list = malloc(sizeof *t->list);
    if (t->list)
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TUNE),
                        parent, tune_proc, (LPARAM)t);
    free(t->list);
    free(t);
}

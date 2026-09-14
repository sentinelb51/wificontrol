/* ui_ctl.c -- owner-drawn switches and buttons, dropdowns, scrolling panel. */
#include "ui.h"

#include <commctrl.h>
#include <stdlib.h>

/* Owner-drawn BUTTONs keep their kind and live state in GWLP_USERDATA. */
#define F_KIND  0x00FF
#define F_ON    0x0100
#define F_HOVER 0x0200
#define F_CARD  0x0400

static LONG_PTR flags(HWND w) { return GetWindowLongPtrW(w, GWLP_USERDATA); }

static void set_flags(HWND w, LONG_PTR f)
{
    if (f == flags(w)) return;
    SetWindowLongPtrW(w, GWLP_USERDATA, f);
    InvalidateRect(w, nullptr, FALSE);
}

static LRESULT CALLBACK control_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, [[maybe_unused]] DWORD_PTR ref)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!(flags(w) & F_HOVER)) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, w, 0 };
            TrackMouseEvent(&t);
            set_flags(w, flags(w) | F_HOVER);
        }
        break;
    case WM_MOUSELEAVE:
        set_flags(w, flags(w) & ~F_HOVER);
        break;
    case WM_ERASEBKGND:
        return 1; /* WM_DRAWITEM paints every pixel */
    case WM_NCDESTROY:
        RemoveWindowSubclass(w, control_proc, id);
        break;
    }
    return DefSubclassProc(w, msg, wp, lp);
}

HWND ui_control(HWND parent, int id, ui_kind kind, const wchar_t *text, bool on_card)
{
    HWND w = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (!w) return nullptr;
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)kind | (on_card ? F_CARD : 0));
    SetWindowSubclass(w, control_proc, 1, 0);
    return w;
}

bool ui_switch_get(HWND sw) { return (flags(sw) & F_ON) != 0; }

void ui_switch_set(HWND sw, bool on)
{
    set_flags(sw, on ? (flags(sw) | F_ON) : (flags(sw) & ~F_ON));
}

static void draw_switch(ui_canvas *c, LONG_PTR f, UINT state, double s, COLORREF under)
{
    const bool on    = (f & F_ON) != 0;
    const bool hover = (f & F_HOVER) != 0;
    const bool off   = (state & ODS_DISABLED) != 0;
    const double tw = 40 * s, th = 20 * s;
    const double x0 = (c->w - tw) / 2, y0 = (c->h - th) / 2;

    if ((state & ODS_FOCUS) && !(state & ODS_NOFOCUSRECT))
        ui_ring(c, x0 - 3 * s, y0 - 3 * s, x0 + tw + 3 * s, y0 + th + 3 * s,
                th / 2 + 3 * s, 1.5 * s, ui_pal.text);

    if (on) {
        COLORREF fill = off   ? ui_mix(ui_pal.accent, under, 0.5)
                      : hover ? ui_mix(ui_pal.accent, ui_pal.text, 0.12)
                              : ui_pal.accent;
        ui_rrect(c, x0, y0, x0 + tw, y0 + th, th / 2, fill);
    } else {
        ui_frame(c, x0, y0, x0 + tw, y0 + th, th / 2, 1 * s,
                 off ? ui_pal.control_edge : ui_pal.text2,
                 hover && !off ? ui_pal.control_hover : under);
    }

    const double kr = (hover && !off ? 7 : 6) * s;
    const double cx = on ? x0 + tw - th / 2 : x0 + th / 2, cy = y0 + th / 2;
    COLORREF knob = on ? ui_pal.on_accent : off ? ui_pal.control_edge : ui_pal.text2;
    ui_rrect(c, cx - kr, cy - kr, cx + kr, cy + kr, kr, knob);
}

static void draw_button(ui_canvas *c, HWND w, ui_kind kind, LONG_PTR f, UINT state,
                        double s, COLORREF under)
{
    const bool hover   = (f & F_HOVER) != 0;
    const bool pressed = (state & ODS_SELECTED) != 0;
    const bool off     = (state & ODS_DISABLED) != 0;
    const double r = 4 * s;
    COLORREF ink = off ? ui_pal.text2 : ui_pal.text;

    if (kind == UI_LINK) {
        if (!off && (hover || pressed))
            ui_rrect(c, 0, 0, c->w, c->h, r, pressed ? ui_pal.control : ui_pal.control_hover);
        if (!off) ink = ui_pal.accent;
    } else if (kind == UI_PRIMARY && !off) {
        COLORREF fill = pressed ? ui_mix(ui_pal.accent, under, 0.25)
                      : hover   ? ui_mix(ui_pal.accent, ui_pal.text, 0.1)
                                : ui_pal.accent;
        ui_rrect(c, 0, 0, c->w, c->h, r, fill);
        ink = ui_pal.on_accent;
    } else {
        COLORREF fill = pressed ? under : hover && !off ? ui_pal.control_hover : ui_pal.control;
        ui_frame(c, 0, 0, c->w, c->h, r, 1 * s, ui_pal.control_edge, fill);
    }

    if ((state & ODS_FOCUS) && !(state & ODS_NOFOCUSRECT))
        ui_ring(c, 0, 0, c->w, c->h, r, 1.5 * s, ui_pal.text);

    wchar_t label[64];
    GetWindowTextW(w, label, 64);
    HFONT font = (HFONT)SendMessageW(w, WM_GETFONT, 0, 0);
    ui_text(c, font, ink, label, 0, 0, c->w, c->h, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

bool ui_draw_item(const DRAWITEMSTRUCT *d)
{
    if (d->CtlType != ODT_BUTTON) return false;
    LONG_PTR f = flags(d->hwndItem);
    ui_kind kind = (ui_kind)(f & F_KIND);
    if (kind < UI_SWITCH || kind > UI_LINK) return false;

    ui_canvas c;
    if (!ui_canvas_begin(&c, d->hDC, d->rcItem.right - d->rcItem.left,
                         d->rcItem.bottom - d->rcItem.top))
        return true;

    const double s = ui_dpi(d->hwndItem) / 96.0;
    const COLORREF under = (f & F_CARD) ? ui_pal.card : ui_pal.bg;
    ui_fill(&c, 0, 0, c.w, c.h, under);

    if (kind == UI_SWITCH) draw_switch(&c, f, d->itemState, s, under);
    else                   draw_button(&c, d->hwndItem, kind, f, d->itemState, s, under);

    ui_canvas_end(&c, d->hDC, d->rcItem.left, d->rcItem.top);
    return true;
}

/* -------------------------------------------------------------- dropdown */

static LRESULT CALLBACK combo_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, [[maybe_unused]] DWORD_PTR ref)
{
    if (msg == WM_MOUSEWHEEL && !SendMessageW(w, CB_GETDROPPEDSTATE, 0, 0))
        return SendMessageW(GetParent(w), msg, wp, lp);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(w, combo_proc, id);
    return DefSubclassProc(w, msg, wp, lp);
}

HWND ui_combo(HWND parent, int id, HFONT f, int item_h)
{
    HWND cb = CreateWindowExW(0, WC_COMBOBOXW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                              0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                              GetModuleHandleW(nullptr), nullptr);
    if (!cb) return nullptr;
    SendMessageW(cb, WM_SETFONT, (WPARAM)f, FALSE);
    SendMessageW(cb, CB_SETITEMHEIGHT, (WPARAM)-1, item_h);
    SendMessageW(cb, CB_SETITEMHEIGHT, 0, item_h);
    ui_theme_combo(cb);
    SetWindowSubclass(cb, combo_proc, 1, 0);
    return cb;
}

/* ----------------------------------------------------------------- panel */

typedef struct { ui_paint_fn paint; void *ctx; int content, scroll; } panel_data;

static panel_data *pd(HWND w) { return (panel_data *)GetWindowLongPtrW(w, GWLP_USERDATA); }

static void scroll_to(HWND w, panel_data *p, int y)
{
    RECT rc;
    GetClientRect(w, &rc);
    int max = p->content > rc.bottom ? p->content - rc.bottom : 0;
    if (y > max) y = max;
    if (y < 0)   y = 0;
    if (y == p->scroll) return;

    int dy = p->scroll - y;
    p->scroll = y;
    ScrollWindowEx(w, 0, dy, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE);
    SetScrollPos(w, SB_VERT, y, TRUE);
    UpdateWindow(w);
}

static void sync(HWND w, panel_data *p)
{
    RECT rc;
    GetClientRect(w, &rc);
    scroll_to(w, p, p->scroll); /* re-clamps against the new size */
    SCROLLINFO si = { .cbSize = sizeof si, .fMask = SIF_RANGE | SIF_PAGE | SIF_POS,
                      .nMax = p->content > 0 ? p->content - 1 : 0,
                      .nPage = (UINT)rc.bottom, .nPos = p->scroll };
    SetScrollInfo(w, SB_VERT, &si, TRUE);
}

static void reveal(HWND w, panel_data *p, HWND child)
{
    RECT r, rc;
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, w, (POINT *)&r, 2);
    GetClientRect(w, &rc);
    int pad = ui_px(12, ui_dpi(w));
    if (r.top < pad)
        scroll_to(w, p, p->scroll + r.top - pad);
    else if (r.bottom > rc.bottom - pad)
        scroll_to(w, p, p->scroll + r.bottom - rc.bottom + pad);
}

static LRESULT CALLBACK panel_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    panel_data *p = pd(w);

    switch (msg) {
    case WM_NCCREATE: {
        /* Allocated here rather than by the caller, so a failed create can
         * never leave it half-owned. */
        panel_data *fresh = malloc(sizeof *fresh);
        if (!fresh) return FALSE;
        *fresh = *(panel_data *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)fresh);
        break;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        free(p);
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        ui_canvas c;
        if (p && ui_canvas_begin(&c, dc, rc.right, rc.bottom)) {
            ui_fill(&c, 0, 0, c.w, c.h, ui_pal.bg);
            if (p->paint) p->paint(w, &c, p->scroll, p->ctx);
            ui_canvas_end(&c, dc, 0, 0);
        }
        EndPaint(w, &ps);
        return 0;
    }

    case WM_SIZE:
        if (p) sync(w, p);
        break;

    case WM_VSCROLL:
        if (p) {
            RECT rc;
            GetClientRect(w, &rc);
            int line = ui_px(40, ui_dpi(w)), y = p->scroll;
            switch (LOWORD(wp)) {
            case SB_LINEUP:   y -= line;      break;
            case SB_LINEDOWN: y += line;      break;
            case SB_PAGEUP:   y -= rc.bottom; break;
            case SB_PAGEDOWN: y += rc.bottom; break;
            case SB_TOP:      y = 0;          break;
            case SB_BOTTOM:   y = p->content; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si = { .cbSize = sizeof si, .fMask = SIF_TRACKPOS };
                GetScrollInfo(w, SB_VERT, &si);
                y = si.nTrackPos;
                break;
            }
            }
            scroll_to(w, p, y);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (p) scroll_to(w, p, p->scroll - GET_WHEEL_DELTA_WPARAM(wp) *
                                           ui_px(48, ui_dpi(w)) / WHEEL_DELTA);
        return 0;

    case WM_COMMAND:
        if (p && (HIWORD(wp) == CBN_SETFOCUS || HIWORD(wp) == BN_SETFOCUS))
            reveal(w, p, (HWND)lp);
        return SendMessageW(GetParent(w), msg, wp, lp);

    case WM_DRAWITEM:
    case WM_CTLCOLORLISTBOX:
        return SendMessageW(GetParent(w), msg, wp, lp);
    }
    return DefWindowProcW(w, msg, wp, lp);
}

HWND ui_panel(HWND parent, int id, ui_paint_fn paint, void *ctx)
{
    static ATOM cls;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!cls) {
        WNDCLASSEXW wc = { .cbSize = sizeof wc, .lpfnWndProc = panel_proc, .hInstance = inst,
                           .hCursor = LoadCursorW(nullptr, IDC_ARROW),
                           .lpszClassName = L"WifiControlPanel" };
        cls = RegisterClassExW(&wc);
    }
    panel_data init = { .paint = paint, .ctx = ctx };
    HWND w = CreateWindowExW(WS_EX_CONTROLPARENT, L"WifiControlPanel", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, inst, &init);
    if (w) ui_theme_scroll(w);
    return w;
}

void ui_panel_set_content(HWND panel, int height)
{
    panel_data *p = pd(panel);
    if (!p) return;
    p->content = height;
    sync(panel, p);
    InvalidateRect(panel, nullptr, FALSE);
}

int ui_panel_scroll(HWND panel)
{
    panel_data *p = pd(panel);
    return p ? p->scroll : 0;
}

/* ---------------------------------------------------------------- frames */

void ui_size_window(HWND w, int cw, int ch, int dpi, const RECT *at)
{
    typedef BOOL (WINAPI *adjust_fn)(RECT *, DWORD, BOOL, DWORD, UINT);
    static adjust_fn adjust;
    static bool probed;
    if (!probed) {
        probed = true;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) adjust = (adjust_fn)(void *)GetProcAddress(u, "AdjustWindowRectExForDpi");
    }

    RECT r = { 0, 0, cw, ch };
    DWORD style = (DWORD)GetWindowLongPtrW(w, GWL_STYLE);
    DWORD ex    = (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (adjust) adjust(&r, style, FALSE, ex, (UINT)dpi);
    else        AdjustWindowRectEx(&r, style, FALSE, ex);

    UINT fl = SWP_NOZORDER | SWP_NOACTIVATE | (at ? 0 : SWP_NOMOVE);
    SetWindowPos(w, nullptr, at ? at->left : 0, at ? at->top : 0,
                 r.right - r.left, r.bottom - r.top, fl);
}

void ui_center(HWND w, HWND owner)
{
    RECT me, area;
    GetWindowRect(w, &me);
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(owner ? owner : w, MONITOR_DEFAULTTONEAREST), &mi);

    if (owner && IsWindowVisible(owner)) GetWindowRect(owner, &area);
    else                                 area = mi.rcWork;

    int wd = me.right - me.left, ht = me.bottom - me.top;
    int x = area.left + (area.right - area.left - wd) / 2;
    int y = area.top + (area.bottom - area.top - ht) / 2;
    if (x + wd > mi.rcWork.right)  x = mi.rcWork.right - wd;
    if (y + ht > mi.rcWork.bottom) y = mi.rcWork.bottom - ht;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;
    SetWindowPos(w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

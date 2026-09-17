/* diag_ui.c -- the details window: what this machine cannot do and why, and
 * every failure since the last check, each with the setting it happened on,
 * the error Windows gave and what to do about it.
 *
 * The status line in the main window has room for one sentence; this is where
 * the rest of it goes.  It opens from Details, and by itself when a switch the
 * user has just flipped could not be applied -- a failure nobody sees is the
 * thing this app was written to avoid.
 *
 * The snapshot is copied in: the dialog is modal, but its message loop still
 * delivers snapshots to the main window, which frees the one it was holding.
 */
#define WIN32_LEAN_AND_MEAN
#include "app.h"
#include "ui.h"
#include "resource.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* Layout, in pixels at 96 dpi. */
enum {
    CLIENT_W = 620, PAD = 16, TITLE_H = 44, FOOT_H = 60, VIEW_MAX = 440,
    SECTION_H = 34, LINE_H = 20, SMALL_H = 17, ROW_PAD = 8, CARD_PAD = 10,
    BUTTON_W = 92, BUTTON_H = 30, STRIPE_W = 4,
};
#define P(v) ui_px((v), d->f.dpi)

typedef struct {
    snapshot *s;
    wchar_t   headline[512];
    ui_fonts  f;
    HWND      dlg, panel;

    int       head_top, head_h;
    int       cap_top, cap_y[WC_CAP_COUNT + 1];
    int       prob_top, prob_y[DIAG_MAX + 1];
    int       order[DIAG_MAX], n_prob;
    int       content;
} diag_ctx;

static void wide(const char *s, wchar_t *out, int cap)
{
    if (!s || !MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = L'\0';
    out[cap - 1] = L'\0';
}

/* ------------------------------------------------------------------ words */

static diag_sev sev_of(const diag_entry *e) { return diag_sev_of(e->op, e->code); }

static COLORREF sev_ink(diag_sev s)
{
    return s == DIAG_ERR ? ui_pal.err : s == DIAG_WARN ? ui_pal.warn : ui_pal.text2;
}

/* "Metered — Home network", or just the op when there is nothing to name. */
static void entry_title(const diag_entry *e, wchar_t *out, int cap)
{
    wchar_t op[96], subject[DIAG_SUBJ_MAX];
    wide(diag_op_name(e->op), op, 96);
    wide(e->subject, subject, DIAG_SUBJ_MAX);
    if (subject[0]) _snwprintf(out, (size_t)cap, L"%s — %s", op, subject);
    else            _snwprintf(out, (size_t)cap, L"%s", op);
    out[cap - 1] = L'\0';
}

/* How long ago, at the coarseness anyone actually reads. */
static void when_text(unsigned long last, wchar_t *out, int cap)
{
    const unsigned long ms = GetTickCount() - last;
    if (ms < 45000)         wcsncpy(out, L"just now", (size_t)cap - 1);
    else if (ms < 3600000)  _snwprintf(out, (size_t)cap, L"%lu min ago", ms / 60000);
    else                    _snwprintf(out, (size_t)cap, L"%lu h ago", ms / 3600000);
    out[cap - 1] = L'\0';
}

/* "Changing it failed: Access is denied (5), seen 4 times, just now" */
static void entry_line(const diag_entry *e, wchar_t *out, int cap)
{
    wchar_t step[48], err[220], seen[32] = L"", ago[32];
    wide(diag_step_name(e->step), step, 48);
    wcw_format_error(e->code, err, 220);
    when_text(e->last, ago, 32);
    if (e->count > 1) _snwprintf(seen, 32, L", seen %u times", e->count);
    _snwprintf(out, (size_t)cap, L"%s failed: %s%s, %s", step, err, seen, ago);
    out[cap - 1] = L'\0';
}

/* Availability: what is missing without this one and what Windows said about
 * it, or nullptr when it is there. */
static const wchar_t *cap_line(const diag_ctx *d, wc_cap c, wchar_t *buf, int cap)
{
    const char *why = wc_cap_why(d->s->cap, c);
    if (!why) return nullptr;

    wchar_t text[320], err[200];
    wide(why, text, 320);
    wcw_format_error(d->s->cap[c].err, err, 200);
    if (err[0]) _snwprintf(buf, (size_t)cap, L"%s (%s)", text, err);
    else        _snwprintf(buf, (size_t)cap, L"%s", text);
    buf[cap - 1] = L'\0';
    return buf;
}

/* ----------------------------------------------------------------- layout */

/* Worst first, and within a severity the one seen most recently. */
static int cmp_entries(const diag_log *l, int a, int b)
{
    const diag_sev sa = sev_of(&l->e[a]), sb = sev_of(&l->e[b]);
    if (sa != sb) return sa > sb ? -1 : 1;
    if (l->e[a].last != l->e[b].last) return l->e[a].last > l->e[b].last ? -1 : 1;
    return a < b ? -1 : 1;
}

static void order_entries(diag_ctx *d)
{
    const diag_log *l = &d->s->diag;
    d->n_prob = l->n < DIAG_MAX ? l->n : DIAG_MAX;
    for (int i = 0; i < d->n_prob; ++i) d->order[i] = i;
    for (int i = 1; i < d->n_prob; ++i) {
        const int v = d->order[i];
        int k = i;
        while (k > 0 && cmp_entries(l, v, d->order[k - 1]) < 0) {
            d->order[k] = d->order[k - 1];
            k--;
        }
        d->order[k] = v;
    }
}

static int text_h(const diag_ctx *d, HFONT f, const wchar_t *s, int w)
{
    return s && s[0] ? ui_text_h(d->dlg, f, s, w) : 0;
}

static void layout(diag_ctx *d, const RECT *at)
{
    const int cw = P(CLIENT_W);
    const int inner = cw - 2 * P(PAD) - 2 * P(14);   /* inside a card */

    order_entries(d);

    int y = 0;
    d->head_top = 0;
    d->head_h   = d->headline[0] ? text_h(d, d->f.body, d->headline, cw - 2 * P(PAD)) + P(12) : 0;
    y += d->head_h;

    /* Availability: one row per capability, with the reason underneath the
     * ones that are missing. */
    d->cap_top  = y + P(SECTION_H);
    d->cap_y[0] = 0;
    for (int c = 0; c < WC_CAP_COUNT; ++c) {
        wchar_t buf[448];
        const wchar_t *why = cap_line(d, (wc_cap)c, buf, 448);
        int h = P(LINE_H) + P(ROW_PAD);
        if (why) h += text_h(d, d->f.small, why, inner);
        d->cap_y[c + 1] = d->cap_y[c] + h;
    }
    y = d->cap_top + d->cap_y[WC_CAP_COUNT] + P(CARD_PAD);

    /* Problems: one card each. */
    d->prob_top  = y + P(SECTION_H);
    d->prob_y[0] = 0;
    for (int i = 0; i < d->n_prob; ++i) {
        const diag_entry *e = &d->s->diag.e[d->order[i]];
        wchar_t line[512], advice[512];
        entry_line(e, line, 512);
        wide(diag_advice(e->op, e->step, e->code), advice, 512);

        int h = P(CARD_PAD) + P(LINE_H);
        h += text_h(d, d->f.small, line, inner);
        h += text_h(d, d->f.small, advice, inner);
        h += P(CARD_PAD);
        d->prob_y[i + 1] = d->prob_y[i] + h + P(ROW_PAD);
    }
    d->content = d->prob_top + (d->n_prob ? d->prob_y[d->n_prob] : P(LINE_H) + 2 * P(CARD_PAD))
               + P(PAD);

    const int view = d->content < P(VIEW_MAX) ? d->content : P(VIEW_MAX);
    const int ch = P(TITLE_H) + view + P(FOOT_H);
    ui_size_window(d->dlg, cw, ch, d->f.dpi, at);

    SetWindowPos(d->panel, nullptr, 0, P(TITLE_H), cw, view, SWP_NOZORDER | SWP_NOACTIVATE);
    ui_panel_set_content(d->panel, d->content);

    const int by = ch - P(PAD) - P(BUTTON_H);
    SetWindowPos(GetDlgItem(d->dlg, IDOK), nullptr, cw - P(PAD) - P(BUTTON_W), by,
                 P(BUTTON_W), P(BUTTON_H), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(GetDlgItem(d->dlg, IDC_DIAG_COPY), nullptr,
                 cw - P(PAD) - 2 * P(BUTTON_W) - P(8), by, P(BUTTON_W), P(BUTTON_H),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(d->dlg, nullptr, FALSE);
}

/* ------------------------------------------------------------------ paint */

static void paint_heading(diag_ctx *d, ui_canvas *cv, const wchar_t *title, int y)
{
    ui_text(cv, d->f.bold, ui_pal.text, title, P(PAD) + P(2), y - P(SECTION_H),
            cv->w - P(PAD), y - P(8), DT_SINGLELINE | DT_BOTTOM);
}

static void paint_caps(diag_ctx *d, ui_canvas *cv, int scroll)
{
    const double s = d->f.dpi / 96.0;
    const int y0 = d->cap_top - scroll, in = P(PAD) + P(14), right = cv->w - P(PAD) - P(14);
    const UINT one = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;

    ui_frame(cv, P(PAD), y0, cv->w - P(PAD), y0 + d->cap_y[WC_CAP_COUNT], 6 * s, 1 * s,
             ui_pal.border, ui_pal.card);

    for (int c = 0; c < WC_CAP_COUNT; ++c) {
        const int ry = y0 + d->cap_y[c];
        wchar_t name[96], buf[448];
        wide(wc_cap_name((wc_cap)c), name, 96);
        const wchar_t *why = cap_line(d, (wc_cap)c, buf, 448);
        const wchar_t *state = !d->s->cap[c].checked ? L"Not checked"
                             : why                   ? L"Unavailable"
                                                     : L"Available";
        const COLORREF ink = why ? ui_pal.warn : !d->s->cap[c].checked ? ui_pal.text2 : ui_pal.ok;
        const int sw = ui_text_w(cv, d->f.body, state);

        ui_text(cv, d->f.body, ui_pal.text, name, in, ry, right - sw - P(12), ry + P(LINE_H), one);
        ui_text(cv, d->f.body, ink, state, right - sw, ry, right, ry + P(LINE_H), one);
        if (why)
            ui_text(cv, d->f.small, ui_pal.text2, why, in, ry + P(LINE_H), right,
                    ry + d->cap_y[c + 1] - d->cap_y[c],
                    DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    }
}

static void paint_problems(diag_ctx *d, ui_canvas *cv, int scroll)
{
    const double s = d->f.dpi / 96.0;
    const int y0 = d->prob_top - scroll, in = P(PAD) + P(14), right = cv->w - P(PAD) - P(14);
    const UINT one = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    const UINT wrap = DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX;

    if (!d->n_prob) {
        const int h = P(LINE_H) + 2 * P(CARD_PAD);
        ui_frame(cv, P(PAD), y0, cv->w - P(PAD), y0 + h, 6 * s, 1 * s, ui_pal.border, ui_pal.card);
        ui_text(cv, d->f.body, ui_pal.ok, L"Nothing has failed since the last check.",
                in, y0, right, y0 + h, one);
        return;
    }

    for (int i = 0; i < d->n_prob; ++i) {
        const diag_entry *e = &d->s->diag.e[d->order[i]];
        const int top = y0 + d->prob_y[i], bottom = y0 + d->prob_y[i + 1] - P(ROW_PAD);
        if (top >= cv->h || bottom <= 0) continue;

        const COLORREF ink = sev_ink(sev_of(e));
        ui_frame(cv, P(PAD), top, cv->w - P(PAD), bottom, 6 * s, 1 * s, ui_pal.border,
                 ui_pal.card);
        ui_rrect_band(cv, P(PAD), top, cv->w - P(PAD), bottom, 6 * s,
                      P(PAD), P(PAD) + STRIPE_W * s, ink);

        wchar_t title[512], line[512], advice[512];
        entry_title(e, title, 512);
        entry_line(e, line, 512);
        wide(diag_advice(e->op, e->step, e->code), advice, 512);

        int y = top + P(CARD_PAD);
        ui_text(cv, d->f.bold, ui_pal.text, title, in, y, right, y + P(LINE_H), one);
        y += P(LINE_H);
        const int lh = text_h(d, d->f.small, line, right - in);
        ui_text(cv, d->f.small, ink, line, in, y, right, y + lh, wrap);
        y += lh;
        if (advice[0])
            ui_text(cv, d->f.small, ui_pal.text2, advice, in, y, right,
                    y + text_h(d, d->f.small, advice, right - in), wrap);
    }
}

static void paint_content([[maybe_unused]] HWND panel, ui_canvas *cv, int scroll, void *ctx)
{
    diag_ctx *d = ctx;

    if (d->head_h) {
        const diag_sev worst = diag_worst(&d->s->diag);
        ui_text(cv, d->f.body, sev_ink(worst == DIAG_INFO ? DIAG_WARN : worst), d->headline,
                P(PAD), d->head_top - scroll, cv->w - P(PAD),
                d->head_top - scroll + d->head_h,
                DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    }

    paint_heading(d, cv, L"What this machine can do", d->cap_top - scroll);
    paint_caps(d, cv, scroll);
    paint_heading(d, cv, L"Problems since the last check", d->prob_top - scroll);
    paint_problems(d, cv, scroll);
}

static void paint_frame(diag_ctx *d)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(d->dlg, &ps);
    RECT rc;
    GetClientRect(d->dlg, &rc);
    ui_canvas cv;
    if (ui_canvas_begin(&cv, dc, rc.right, rc.bottom)) {
        const int line = P(1) > 0 ? P(1) : 1;
        ui_fill(&cv, 0, 0, cv.w, cv.h, ui_pal.bg);
        ui_text(&cv, d->f.title, ui_pal.text, L"Details", P(PAD), 0, cv.w - P(PAD), P(TITLE_H),
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        ui_fill(&cv, 0, P(TITLE_H) - line, cv.w, P(TITLE_H), ui_pal.border);

        const int foot = cv.h - P(FOOT_H);
        ui_fill(&cv, 0, foot, cv.w, foot + line, ui_pal.border);
        ui_text(&cv, d->f.small, ui_pal.text2,
                L"Refresh in the main window checks all of this again.",
                P(PAD), foot + P(8), cv.w - P(PAD) - 2 * P(BUTTON_W) - P(24),
                foot + P(8) + P(SMALL_H * 2), DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        ui_canvas_end(&cv, dc, 0, 0);
    }
    EndPaint(d->dlg, &ps);
}

/* ------------------------------------------------------------- clipboard */

/* Appends and stops at the end of the buffer: _snwprintf reports truncation
 * as -1 rather than as the length it wanted, so the position has to be taken
 * from the buffer rather than from the return value. */
static int add(wchar_t *buf, int at, int cap, const wchar_t *fmt, ...)
{
    if (at >= cap - 1) return cap - 1;
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnwprintf(buf + at, (size_t)(cap - 1 - at), fmt, ap);
    va_end(ap);
    buf[cap - 1] = L'\0';
    return n < 0 ? cap - 1 : at + n;
}

/* The same thing in plain text, so it can go into a bug report. */
static void copy_report(diag_ctx *d)
{
    enum { CAP = 32768 };
    wchar_t *buf = malloc(CAP * sizeof *buf);
    if (!buf) return;

    SYSTEMTIME now;
    GetLocalTime(&now);
    int at = add(buf, 0, CAP,
                 L"WiFi Control details, %04u-%02u-%02u %02u:%02u\r\n\r\n"
                 L"What this machine can do\r\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);

    for (int c = 0; c < WC_CAP_COUNT; ++c) {
        wchar_t name[96], buf2[448];
        wide(wc_cap_name((wc_cap)c), name, 96);
        const wchar_t *why = cap_line(d, (wc_cap)c, buf2, 448);
        at = add(buf, at, CAP, L"  %-28s %s\r\n", name,
                 !d->s->cap[c].checked ? L"not checked" : why ? why : L"available");
    }

    at = add(buf, at, CAP, L"\r\nProblems since the last check\r\n");
    if (!d->n_prob) at = add(buf, at, CAP, L"  none\r\n");

    for (int i = 0; i < d->n_prob; ++i) {
        const diag_entry *e = &d->s->diag.e[d->order[i]];
        wchar_t title[512], line[512], advice[512];
        entry_title(e, title, 512);
        entry_line(e, line, 512);
        wide(diag_advice(e->op, e->step, e->code), advice, 512);
        at = add(buf, at, CAP, L"  %s\r\n    %s\r\n", title, line);
        if (advice[0]) at = add(buf, at, CAP, L"    %s\r\n", advice);
    }
    if (d->s->diag.dropped)
        add(buf, at, CAP, L"  (%u more kinds of failure were dropped)\r\n",
            d->s->diag.dropped);

    const size_t bytes = (wcslen(buf) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void *dst = mem ? GlobalLock(mem) : nullptr;
    if (dst) {
        memcpy(dst, buf, bytes);
        GlobalUnlock(mem);
        if (OpenClipboard(d->dlg)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, mem)) mem = nullptr; /* the clipboard owns it */
            CloseClipboard();
        }
    }
    if (mem) GlobalFree(mem);
    free(buf);
}

/* ---------------------------------------------------------------- dialog */

static INT_PTR CALLBACK diag_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    diag_ctx *d = (diag_ctx *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        d = (diag_ctx *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
        d->dlg = dlg;
        ui_fonts_make(&d->f, ui_dpi(dlg));
        ui_theme_window(dlg);

        d->panel = ui_panel(dlg, IDC_DIAG_PANEL, paint_content, d);
        ui_control(dlg, IDC_DIAG_COPY, UI_BUTTON, L"Copy", false);
        ui_control(dlg, IDOK, UI_PRIMARY, L"Close", false);
        SendMessageW(GetDlgItem(dlg, IDC_DIAG_COPY), WM_SETFONT, (WPARAM)d->f.body, FALSE);
        SendMessageW(GetDlgItem(dlg, IDOK), WM_SETFONT, (WPARAM)d->f.body, FALSE);

        layout(d, nullptr);
        ui_center(dlg, GetParent(dlg));
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dlg, IDOK), TRUE);
        return FALSE;

    case WM_ERASEBKGND:
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
        return TRUE;

    case WM_PAINT:
        if (d) paint_frame(d);
        return TRUE;

    case WM_DRAWITEM:
        return ui_draw_item((const DRAWITEMSTRUCT *)lp);

    case WM_MOUSEWHEEL:
        /* The focus starts on Close, which is outside the list. */
        if (!d) return FALSE;
        SendMessageW(d->panel, msg, wp, lp);
        return TRUE;

    case WM_COMMAND: {
        if (!d) return FALSE;
        const bool click = HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED;
        switch (LOWORD(wp)) {
        case IDC_DIAG_COPY: if (click) copy_report(d); return TRUE;
        case IDOK:
        case IDCANCEL:      if (click) EndDialog(dlg, 0); return TRUE;
        }
        return FALSE;
    }

    case WM_DPICHANGED:
        if (!d) return FALSE;
        ui_fonts_free(&d->f);
        ui_fonts_make(&d->f, HIWORD(wp));
        SendMessageW(GetDlgItem(dlg, IDC_DIAG_COPY), WM_SETFONT, (WPARAM)d->f.body, FALSE);
        SendMessageW(GetDlgItem(dlg, IDOK), WM_SETFONT, (WPARAM)d->f.body, FALSE);
        layout(d, (const RECT *)lp);
        return TRUE;

    case WM_DESTROY:
        if (d) ui_fonts_free(&d->f);
        return FALSE;
    }
    return FALSE;
}

void diag_dialog(HWND parent, const snapshot *s, const wchar_t *headline)
{
    if (!s) return;

    diag_ctx *d = calloc(1, sizeof *d);
    if (!d) return;
    /* Its own copy: a snapshot arriving while this is open replaces the
     * window's. */
    d->s = malloc(sizeof *d->s);
    if (d->s) {
        *d->s = *s;
        if (headline) {
            wcsncpy(d->headline, headline, 511);
            d->headline[511] = L'\0';
        }
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DIAG), parent,
                        diag_proc, (LPARAM)d);
    }
    free(d->s);
    free(d);
}

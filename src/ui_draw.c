/* ui_draw.c -- palette, fonts, window theming and the anti-aliased canvas. */
#include "ui.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <math.h>
#include <string.h>

bool       ui_dark;
ui_palette ui_pal;
static HBRUSH g_list_brush;

/* Windows 11's own dark and light values, so the app sits next to Settings
 * without looking foreign. */
static const ui_palette DARK = {
    .bg = RGB(32, 32, 32),      .card = RGB(43, 43, 43),     .border = RGB(58, 58, 58),
    .text = RGB(255, 255, 255), .text2 = RGB(163, 163, 163),
    .accent = RGB(76, 194, 255), .on_accent = RGB(0, 0, 0),
    .ok = RGB(108, 203, 95),    .warn = RGB(252, 196, 25),   .err = RGB(255, 153, 164),
    .control = RGB(55, 55, 55), .control_hover = RGB(64, 64, 64), .control_edge = RGB(74, 74, 74),
};
static const ui_palette LIGHT = {
    .bg = RGB(243, 243, 243),   .card = RGB(251, 251, 251),  .border = RGB(229, 229, 229),
    .text = RGB(26, 26, 26),    .text2 = RGB(96, 96, 96),
    .accent = RGB(0, 95, 184),  .on_accent = RGB(255, 255, 255),
    .ok = RGB(16, 124, 16),     .warn = RGB(157, 93, 0),     .err = RGB(196, 43, 28),
    .control = RGB(254, 254, 254), .control_hover = RGB(246, 246, 246), .control_edge = RGB(208, 208, 208),
};

/* ---------------------------------------------------------------- theming */

static DWORD os_build(void)
{
    typedef LONG (WINAPI *rtl_fn)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    rtl_fn get = nt ? (rtl_fn)(void *)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    OSVERSIONINFOW v = { .dwOSVersionInfoSize = sizeof v };
    return (get && get(&v) == 0 && v.dwMajorVersion >= 10) ? v.dwBuildNumber : 0;
}

/* Popup menus have no documented dark mode.  From Windows 10 1903 (build
 * 18362) uxtheme ordinal 135 is SetPreferredAppMode and 136 FlushMenuThemes;
 * before that build 135 means something else, so it is left alone.  If either
 * is missing the menus simply stay light. */
static void menu_mode(bool dark)
{
    static DWORD build = (DWORD)-1;
    if (build == (DWORD)-1) build = os_build();
    if (build < 18362) return;

    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    typedef int  (WINAPI *mode_fn)(int);
    typedef void (WINAPI *flush_fn)(void);
    mode_fn  mode  = (mode_fn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    flush_fn flush = (flush_fn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (mode)  mode(dark ? 2 : 3); /* ForceDark : ForceLight */
    if (flush) flush();
}

void ui_set_theme(bool dark)
{
    ui_dark = dark;
    ui_pal  = dark ? DARK : LIGHT;
    if (g_list_brush) DeleteObject(g_list_brush);
    g_list_brush = CreateSolidBrush(ui_pal.card);
    menu_mode(dark);
}

void ui_theme_window(HWND top)
{
    BOOL on = ui_dark;
    /* Attribute 20 from Windows 10 2004; the builds just before it used 19. */
    if (FAILED(DwmSetWindowAttribute(top, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof on)))
        DwmSetWindowAttribute(top, 19, &on, sizeof on);
}

void ui_theme_combo(HWND cb)
{
    SetWindowTheme(cb, ui_dark ? L"DarkMode_CFD" : nullptr, nullptr);
}

void ui_theme_scroll(HWND w)
{
    SetWindowTheme(w, ui_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

LRESULT ui_ctlcolor(HDC dc)
{
    SetTextColor(dc, ui_pal.text);
    SetBkColor(dc, ui_pal.card);
    return (LRESULT)g_list_brush;
}

/* ------------------------------------------------------------ fonts, DPI */

static HFONT font(int tenths_pt, int weight, int dpi)
{
    return CreateFontW(-MulDiv(tenths_pt, dpi, 720), 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void ui_fonts_make(ui_fonts *f, int dpi)
{
    f->dpi   = dpi;
    f->body  = font(90,  FW_NORMAL,   dpi);
    f->bold  = font(90,  FW_SEMIBOLD, dpi);
    f->small = font(80,  FW_NORMAL,   dpi);
    f->title = font(110, FW_SEMIBOLD, dpi);
}

void ui_fonts_free(ui_fonts *f)
{
    HFONT *all[] = { &f->body, &f->bold, &f->small, &f->title };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
        if (*all[i]) { DeleteObject(*all[i]); *all[i] = nullptr; }
}

int ui_dpi(HWND w)
{
    typedef UINT (WINAPI *pfn)(HWND);
    static pfn get;
    static bool probed;
    if (!probed) {
        probed = true;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) get = (pfn)(void *)GetProcAddress(u, "GetDpiForWindow");
    }
    if (get) {
        UINT d = get(w);
        if (d) return (int)d;
    }
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

/* ---------------------------------------------------------------- canvas */

bool ui_canvas_begin(ui_canvas *c, HDC ref, int w, int h)
{
    memset(c, 0, sizeof *c);
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bi = { .bmiHeader = { .biSize = sizeof(BITMAPINFOHEADER), .biWidth = w,
                                     .biHeight = -h, .biPlanes = 1, .biBitCount = 32,
                                     .biCompression = BI_RGB } };
    void *bits = nullptr;
    c->dc  = CreateCompatibleDC(ref);
    c->bmp = c->dc ? CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!c->bmp || !bits) {
        if (c->bmp) DeleteObject(c->bmp);
        if (c->dc)  DeleteDC(c->dc);
        memset(c, 0, sizeof *c);
        return false;
    }
    c->old = (HBITMAP)SelectObject(c->dc, c->bmp);
    c->px  = bits;
    c->w   = w;
    c->h   = h;
    SetBkMode(c->dc, TRANSPARENT);
    return true;
}

void ui_canvas_end(ui_canvas *c, HDC dst, int x, int y)
{
    if (!c->dc) return;
    BitBlt(dst, x, y, c->w, c->h, c->dc, 0, 0, SRCCOPY);
    SelectObject(c->dc, c->old);
    DeleteObject(c->bmp);
    DeleteDC(c->dc);
    memset(c, 0, sizeof *c);
}

COLORREF ui_mix(COLORREF a, COLORREF b, double t)
{
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5));
}

static inline void put(ui_canvas *c, int x, int y, COLORREF col, double a)
{
    unsigned char *p = c->px + ((size_t)y * (size_t)c->w + (size_t)x) * 4;
    p[0] = (unsigned char)(p[0] + (GetBValue(col) - p[0]) * a + 0.5);
    p[1] = (unsigned char)(p[1] + (GetGValue(col) - p[1]) * a + 0.5);
    p[2] = (unsigned char)(p[2] + (GetRValue(col) - p[2]) * a + 0.5);
    p[3] = 255;
}

void ui_fill(ui_canvas *c, int x0, int y0, int x1, int y1, COLORREF col)
{
    GdiFlush();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) put(c, x, y, col, 1.0);
}

/* Every shape is a signed distance function; coverage is how far the pixel
 * centre sits inside the edge, clamped to one pixel of falloff. */
typedef double (*sdf_fn)(double x, double y, const double *k);

static void shade(ui_canvas *c, double x0, double y0, double x1, double y1,
                  sdf_fn f, const double *k, COLORREF col)
{
    GdiFlush();
    int ix0 = (int)floor(x0) - 1, iy0 = (int)floor(y0) - 1;
    int ix1 = (int)ceil(x1) + 1,  iy1 = (int)ceil(y1) + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > c->w) ix1 = c->w;
    if (iy1 > c->h) iy1 = c->h;

    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            double a = 0.5 - f(x + 0.5, y + 0.5, k);
            if (a <= 0.0) continue;
            put(c, x, y, col, a > 1.0 ? 1.0 : a);
        }
    }
}

/* k: centre x, centre y, half width, half height, radius */
static double sd_rrect(double x, double y, const double *k)
{
    double qx = fabs(x - k[0]) - (k[2] - k[4]);
    double qy = fabs(y - k[1]) - (k[3] - k[4]);
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    double inside = qx > qy ? qx : qy;
    return sqrt(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - k[4];
}

/* k: as sd_rrect, then stroke width */
static double sd_ring(double x, double y, const double *k)
{
    double d = sd_rrect(x, y, k);
    double in = -(d + k[5]);
    return d > in ? d : in;
}

/* k: as sd_rrect, then the band's left and right edge */
static double sd_band(double x, double y, const double *k)
{
    double d = sd_rrect(x, y, k);
    double b = k[5] - x > x - k[6] ? k[5] - x : x - k[6];
    return d > b ? d : b;
}

/* k: x0, y0, x1, y1, half width */
static double sd_line(double x, double y, const double *k)
{
    double px = x - k[0], py = y - k[1], bx = k[2] - k[0], by = k[3] - k[1];
    double len2 = bx * bx + by * by;
    double t = len2 > 0 ? (px * bx + py * by) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double dx = px - bx * t, dy = py - by * t;
    return sqrt(dx * dx + dy * dy) - k[4];
}

static void rrect_k(double *k, double x0, double y0, double x1, double y1, double r)
{
    k[0] = (x0 + x1) / 2;
    k[1] = (y0 + y1) / 2;
    k[2] = (x1 - x0) / 2;
    k[3] = (y1 - y0) / 2;
    double lim = k[2] < k[3] ? k[2] : k[3];
    k[4] = r < 0 ? 0 : r > lim ? lim : r;
}

void ui_rrect(ui_canvas *c, double x0, double y0, double x1, double y1, double r, COLORREF col)
{
    double k[5];
    rrect_k(k, x0, y0, x1, y1, r);
    shade(c, x0, y0, x1, y1, sd_rrect, k, col);
}

void ui_rrect_band(ui_canvas *c, double x0, double y0, double x1, double y1, double r,
                   double from, double to, COLORREF col)
{
    double k[7];
    rrect_k(k, x0, y0, x1, y1, r);
    k[5] = from;
    k[6] = to;
    shade(c, from, y0, to, y1, sd_band, k, col);
}

void ui_ring(ui_canvas *c, double x0, double y0, double x1, double y1, double r,
             double t, COLORREF col)
{
    double k[6];
    rrect_k(k, x0, y0, x1, y1, r);
    k[5] = t;
    shade(c, x0, y0, x1, y1, sd_ring, k, col);
}

void ui_frame(ui_canvas *c, double x0, double y0, double x1, double y1, double r,
              double t, COLORREF edge, COLORREF fill)
{
    ui_rrect(c, x0, y0, x1, y1, r, edge);
    ui_rrect(c, x0 + t, y0 + t, x1 - t, y1 - t, r - t, fill);
}

void ui_line(ui_canvas *c, double x0, double y0, double x1, double y1, double w, COLORREF col)
{
    double k[5] = { x0, y0, x1, y1, w / 2 };
    shade(c, (x0 < x1 ? x0 : x1) - w, (y0 < y1 ? y0 : y1) - w,
             (x0 > x1 ? x0 : x1) + w, (y0 > y1 ? y0 : y1) + w, sd_line, k, col);
}

void ui_text(ui_canvas *c, HFONT f, COLORREF col, const wchar_t *s,
             int x0, int y0, int x1, int y1, UINT fmt)
{
    RECT r = { x0, y0, x1, y1 };
    HGDIOBJ old = SelectObject(c->dc, f);
    SetTextColor(c->dc, col);
    DrawTextW(c->dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(c->dc, old);
}

int ui_text_w(ui_canvas *c, HFONT f, const wchar_t *s)
{
    HGDIOBJ old = SelectObject(c->dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(c->dc, s, (int)wcslen(s), &sz);
    SelectObject(c->dc, old);
    return sz.cx;
}

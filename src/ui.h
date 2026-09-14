/* ui.h -- the drawing and control layer shared by both windows.
 *
 * Plain GDI and owner-drawn standard controls: no extra runtime, no framework.
 * Shapes are anti-aliased by writing coverage straight into a 32-bit back
 * buffer, so rounded corners and switch knobs come out smooth without GDI+.
 */
#ifndef WIFICONTROL_UI_H
#define WIFICONTROL_UI_H

#include <windows.h>

typedef struct {
    COLORREF bg, card, border, text, text2, accent, on_accent;
    COLORREF ok, warn, err, control, control_hover, control_edge;
} ui_palette;

extern bool       ui_dark;
extern ui_palette ui_pal;

/* Palette plus the app-wide menu mode.  Windows already open need
 * ui_theme_window() and a repaint afterwards. */
void ui_set_theme(bool dark);
void ui_theme_window(HWND top);
void ui_theme_combo (HWND cb);
void ui_theme_scroll(HWND w);

/* For WM_CTLCOLORLISTBOX: the dropdown half of a combobox. */
LRESULT ui_ctlcolor(HDC dc);

typedef struct { HFONT body, bold, small, title; int dpi; } ui_fonts;
void ui_fonts_make(ui_fonts *f, int dpi);
void ui_fonts_free(ui_fonts *f);
int  ui_dpi(HWND w);

/* Size a top-level window so its client area is cw x ch at dpi, keeping its
 * position unless `at` (the rect WM_DPICHANGED suggests) is given. */
void ui_size_window(HWND w, int cw, int ch, int dpi, const RECT *at);
/* Centre over a visible owner, else within the work area of its monitor. */
void ui_center(HWND w, HWND owner);
static inline int ui_px(int v, int dpi) { return MulDiv(v, dpi, 96); }

typedef struct { HDC dc; HBITMAP bmp, old; unsigned char *px; int w, h; } ui_canvas;
bool ui_canvas_begin(ui_canvas *c, HDC ref, int w, int h);
void ui_canvas_end  (ui_canvas *c, HDC dst, int x, int y);
void ui_fill (ui_canvas *c, int x0, int y0, int x1, int y1, COLORREF col);
void ui_rrect(ui_canvas *c, double x0, double y0, double x1, double y1, double r, COLORREF col);
void ui_frame(ui_canvas *c, double x0, double y0, double x1, double y1, double r,
              double t, COLORREF edge, COLORREF fill);
void ui_ring (ui_canvas *c, double x0, double y0, double x1, double y1, double r,
              double t, COLORREF col);
void ui_line(ui_canvas *c, double x0, double y0, double x1, double y1, double w, COLORREF col);
void ui_text (ui_canvas *c, HFONT f, COLORREF col, const wchar_t *s,
              int x0, int y0, int x1, int y1, UINT fmt);
int  ui_text_w(ui_canvas *c, HFONT f, const wchar_t *s);
COLORREF ui_mix(COLORREF a, COLORREF b, double t);

/* Owner-drawn buttons.  The parent forwards WM_DRAWITEM to ui_draw_item and
 * flips a switch itself on BN_CLICKED, so it can refuse (a declined confirm). */
typedef enum { UI_SWITCH = 1, UI_BUTTON, UI_PRIMARY, UI_LINK } ui_kind;
HWND ui_control(HWND parent, int id, ui_kind kind, const wchar_t *text, bool on_card);
bool ui_switch_get(HWND sw);
void ui_switch_set(HWND sw, bool on);
bool ui_draw_item(const DRAWITEMSTRUCT *d);

/* A dropdown that passes the mouse wheel on unless it is open, so scrolling a
 * list never silently changes a value under the pointer. */
HWND ui_combo(HWND parent, int id, HFONT f, int item_h);

/* A vertically scrolling child with painted content.  It forwards WM_COMMAND,
 * WM_DRAWITEM and WM_CTLCOLORLISTBOX to its parent, and scrolls a focused
 * dropdown into view. */
typedef void (*ui_paint_fn)(HWND panel, ui_canvas *c, int scroll, void *ctx);
HWND ui_panel(HWND parent, int id, ui_paint_fn paint, void *ctx);
void ui_panel_set_content(HWND panel, int height);
int  ui_panel_scroll(HWND panel);

#endif

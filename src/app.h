/* app.h -- what the main window (app_ui.c) and the rest of the app (app.c)
 * share: the messages between threads, the snapshot the worker publishes, and
 * the few config and icon helpers the window calls back into.
 */
#ifndef WIFICONTROL_APP_H
#define WIFICONTROL_APP_H

#include "wlan_win32.h"

/* UI thread messages */
#define WM_U_SNAPSHOT (WM_APP + 1)
#define WM_U_TRAY     (WM_APP + 2)

/* worker thread messages.  Those that carry a switch the snapshot shows also
 * carry the window's count of such changes, in lp (WM_W_MANAGE: in its
 * command). */
#define WM_W_POLL      (WM_APP + 10)
#define WM_W_BGSCAN    (WM_APP + 11) /* wp: the No background scans switch */
#define WM_W_MANAGE    (WM_APP + 12)
#define WM_W_QUIT      (WM_APP + 13)
#define WM_W_NUCLEAR   (WM_APP + 14) /* wp: the Block all scans switch */
#define WM_W_METERED   (WM_APP + 15) /* wp: the Metered switch */
#define WM_W_PERF      (WM_APP + 16) /* wp: the Performance switch */
#define WM_W_PLAN      (WM_APP + 17) /* the active power plan changed */
#define WM_W_PERF_END  (WM_APP + 18) /* session ending: put everything back, no restart */
#define WM_W_STREAMING (WM_APP + 19) /* wp: the Streaming mode switch */

typedef struct {
    wc_guid       guid;
    wchar_t       name[WC_NAME_MAX];
    wc_ifstate    state;
    bool          present, managed, pending;
    wc_val        streaming, bgscan, autoconf;
    int           profiles, metered;
    unsigned long last_err;
} snap_row;

typedef struct {
    int           n;
    bool          bgscan_off, streaming_on, nuclear, metered, write_denied;
    bool          perf, perf_busy;   /* the switch; an adapter is restarting */
    unsigned long perf_err;
    int           recovered;
    unsigned long enum_err, open_err;
    unsigned      seq;           /* the newest switch change it reflects */
    snap_row      row[WC_MAX_ADAPTERS];
} snapshot;

typedef struct { wc_guid g; bool on; unsigned seq; } manage_cmd;

extern HWND g_ui;        /* the main window; the worker posts snapshots to it */
extern HWND g_worker;    /* the worker's message window */
extern UINT g_msg_show;  /* broadcast by a second instance to reveal the first */

void  cfg_save_bgscan_off(bool on);
void  cfg_save_streaming_on(bool on);
void  cfg_save_managed(const wc_guid *g, bool on);
void  cfg_save_metered(bool on);
void  cfg_save_perf(bool on);
void  cfg_save_dark(bool on);
HICON make_icon(int size, COLORREF c, double fill); /* fill: the glyph's share of the icon */

/* Created hidden.  ui_set_theme() must already have run. */
HWND  main_window_create(HINSTANCE inst);
void  main_window_start(HWND w, bool to_tray);
void  main_window_cleanup(void);

#endif

/* wlan.h -- platform-independent core for WiFi Control.
 *
 * The core owns all policy (which adapter gets which setting, when to retry,
 * how to interpret errors).  It never touches windows.h, so it builds and is
 * unit-tested on any host against a fake backend.  wlan_win32.c supplies the
 * real wlanapi backend.
 */
#ifndef WIFICONTROL_WLAN_H
#define WIFICONTROL_WLAN_H

#define WC_MAX_ADAPTERS 8
#define WC_NAME_MAX     128 /* UTF-8 bytes */

/* Status codes are Win32 DWORDs so the backend can pass them through
 * untranslated; the UI decodes them with FormatMessage.  Codes at or above
 * WC_E_APP are ours and are decoded from wc_strerror(). */
#define WC_OK                0UL
#define WC_E_ACCESS_DENIED   5UL    /* ERROR_ACCESS_DENIED */
#define WC_E_INVALID_STATE   5023UL /* ERROR_INVALID_STATE: adapter not connected */
#define WC_E_APP             0xE0000000UL
#define WC_E_VERIFY          0xE0000001UL /* wrote the value, read back something else */
#define WC_E_BADDATA         0xE0000002UL /* driver returned a short/absent buffer */

typedef struct { unsigned char b[16]; } wc_guid;

typedef enum {
    WC_IF_UNKNOWN = 0,
    WC_IF_NOT_READY,
    WC_IF_CONNECTED,
    WC_IF_AD_HOC,
    WC_IF_DISCONNECTING,
    WC_IF_DISCONNECTED,
    WC_IF_ASSOCIATING,
    WC_IF_DISCOVERING,
    WC_IF_AUTHENTICATING
} wc_ifstate;

/* The two settings WLAN Optimizer manipulates.  Both are reference-counted by
 * the OS across client handles and are reset automatically when the adapter
 * disconnects, so "restoring" them means writing the opposite value (which
 * withdraws this process's vote) or closing the handle. */
typedef enum { WC_OPT_STREAMING = 0, WC_OPT_BGSCAN = 1, WC_OPT_COUNT = 2 } wc_opt;

typedef enum { WC_VAL_UNKNOWN = -1, WC_VAL_OFF = 0, WC_VAL_ON = 1 } wc_val;

typedef struct {
    wc_guid    guid;
    char       name[WC_NAME_MAX];
    wc_ifstate state;
} wc_ifinfo;

/* Every backend call returns WC_OK or a Win32 error code. */
typedef struct wc_backend {
    void *ctx;
    unsigned long (*enum_ifaces)  (void *ctx, wc_ifinfo *out, int cap, int *count);
    unsigned long (*query_bool)   (void *ctx, const wc_guid *g, wc_opt o, int *value);
    unsigned long (*set_bool)     (void *ctx, const wc_guid *g, wc_opt o, int value);
    unsigned long (*granted_write)(void *ctx, wc_opt o, int *can_write);
} wc_backend;

typedef struct {
    wc_guid       guid;
    char          name[WC_NAME_MAX];
    wc_ifstate    state;
    int           present;      /* seen in the most recent enumeration */
    int           managed;      /* user wants this adapter optimized */
    int           touched;      /* we have an outstanding request on it */
    int           pending;      /* wants optimizing but is not connected yet */
    wc_val        streaming;    /* last value read back */
    wc_val        bgscan;
    unsigned long last_err;     /* WC_OK when the last pass succeeded */
    unsigned      consec_fail;  /* consecutive failed passes; never fatal */
} wc_adapter;

typedef struct {
    wc_backend    be;
    int           enabled;                 /* master switch */
    wc_adapter    ad[WC_MAX_ADAPTERS];
    int           n;
    int           can_write[WC_OPT_COUNT]; /* advisory: from WlanGetSecuritySettings */
    unsigned long enum_err;
} wc_state;

void          wc_init        (wc_state *s, const wc_backend *be);
void          wc_probe_access(wc_state *s);
unsigned long wc_refresh     (wc_state *s);          /* enumerate, merge, keep user flags */
void          wc_apply_all   (wc_state *s);
void          wc_apply_one   (wc_state *s, int i);
unsigned long wc_poll        (wc_state *s);          /* refresh + apply_all */
void          wc_set_enabled (wc_state *s, int on);
void          wc_set_managed (wc_state *s, int i, int on);
int           wc_find        (const wc_state *s, const wc_guid *g); /* -1 if absent */

/* True once any adapter has reported ERROR_ACCESS_DENIED, or the advisory
 * probe says we lack write access.  Drives the "needs elevation" hint. */
int           wc_needs_elevation(const wc_state *s);

const char   *wc_state_name(wc_ifstate st);
const char   *wc_strerror   (unsigned long code); /* only for WC_E_APP codes; else NULL */

#endif /* WIFICONTROL_WLAN_H */

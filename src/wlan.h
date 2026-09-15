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
#define WC_MAX_PROFILES 64
#define WC_PROFILE_MAX  769 /* UTF-8 bytes: a 256-character profile name and its terminator */

/* Status codes are Win32 DWORDs so the backend can pass them through
 * untranslated; the UI decodes them with FormatMessage.  Codes at or above
 * WC_E_APP are ours and are decoded from wc_strerror(). */
#define WC_OK                0UL
#define WC_E_ACCESS_DENIED   5UL    /* ERROR_ACCESS_DENIED */
#define WC_E_INVALID_STATE   5023UL /* ERROR_INVALID_STATE: adapter not connected */
#define WC_E_APP             0xE0000000UL
#define WC_E_VERIFY          0xE0000001UL /* wrote the value, read back something else */
#define WC_E_BADDATA         0xE0000002UL /* driver returned a short/absent buffer */
#define WC_E_NETSH           0xE0000003UL /* netsh refused to change a profile's cost */

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

/* STREAMING and BGSCAN are reference-counted by the OS across client handles
 * and are reset automatically when the adapter disconnects, so "restoring"
 * them means writing the opposite value (which withdraws this process's vote)
 * or closing the handle.
 *
 * AUTOCONF is not like that at all.  Nothing refcounts it, nothing resets it
 * on disconnect, and it survives the process -- the docs call it equivalent to
 * `netsh wlan setautoconfig`.  Turning it off stops scanning completely, which
 * also stops roaming and stops Windows reconnecting on its own.  Left off by
 * a process that died, it stays off.  Every recovery path in this file exists
 * because of that one asymmetry. */
typedef enum {
    WC_OPT_STREAMING = 0,
    WC_OPT_BGSCAN    = 1,
    WC_OPT_AUTOCONF  = 2,
    WC_OPT_COUNT     = 3
} wc_opt;

typedef enum { WC_VAL_UNKNOWN = -1, WC_VAL_OFF = 0, WC_VAL_ON = 1 } wc_val;

/* A saved profile's connection cost is shaped like AUTOCONF, not like the
 * other two: Windows keeps it through disconnects, exits and reboots until
 * something changes it.  It is also per profile rather than per adapter.
 *
 * The app writes exactly one value, Variable set by the user, which the
 * Settings app never produces -- its metered switch writes Fixed.  A profile
 * carrying that value is ours, and it is reset to the Windows default whenever
 * metering is not in force.  So a run that was killed is repaired by the next
 * one, with no record of what it did. */
#define WC_COST_VARIABLE 0x4UL /* WCM_CONNECTION_COST_VARIABLE */
#define WC_COST_SRC_USER 2     /* wcm_connection_cost_source_user */

typedef struct { char name[WC_PROFILE_MAX]; } wc_profile;

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
    unsigned long (*list_profiles)(void *ctx, const wc_guid *g, wc_profile *out, int cap, int *count);
    unsigned long (*query_cost)   (void *ctx, const wc_guid *g, const char *profile,
                                   unsigned long *cost, int *source);
    unsigned long (*set_metered)  (void *ctx, const wc_guid *g, const char *profile, int metered);
} wc_backend;

typedef struct {
    wc_guid       guid;
    char          name[WC_NAME_MAX];
    wc_ifstate    state;
    bool          present;      /* seen in the most recent enumeration */
    bool          managed;      /* user wants this adapter optimized */
    bool          touched;      /* we have an outstanding request on it */
    bool          pending;      /* wants optimizing but is not connected yet */
    wc_val        streaming;    /* last value read back */
    wc_val        bgscan;
    wc_val        autoconf;
    int           profiles;     /* saved profiles seen by the last cost pass */
    int           metered;      /* of those, carrying our cost; -1 until known */
    unsigned long last_err;     /* WC_OK when the last pass succeeded */
    unsigned      consec_fail;  /* consecutive failed passes; never fatal */
} wc_adapter;

typedef struct {
    wc_backend    be;
    bool          enabled;                 /* master switch */
    bool          nuclear;                 /* also disable WLAN auto config */
    bool          metered;                 /* give every saved profile a metered cost */
    int           recovered;               /* adapters we forced auto config back on */
    wc_adapter    ad[WC_MAX_ADAPTERS];
    int           n;
    bool          can_write[WC_OPT_COUNT]; /* advisory: from WlanGetSecuritySettings */
    unsigned long enum_err;
} wc_state;

void          wc_init        (wc_state *s, const wc_backend *be);
void          wc_probe_access(wc_state *s);
unsigned long wc_refresh     (wc_state *s);          /* enumerate, merge, keep user flags */
void          wc_apply_all   (wc_state *s);
void          wc_apply_one   (wc_state *s, int i);
unsigned long wc_poll        (wc_state *s);          /* refresh + apply_all */
void          wc_set_enabled (wc_state *s, bool on);
void          wc_set_nuclear (wc_state *s, bool on);
void          wc_set_metered (wc_state *s, bool on);
void          wc_set_managed (wc_state *s, int i, bool on);
int           wc_find        (const wc_state *s, const wc_guid *g); /* -1 if absent */

/* True once any adapter has reported ERROR_ACCESS_DENIED, or the advisory
 * probe says we lack write access.  The app manifest already demands
 * administrator, so this means the Native Wifi securable object's DACL or a
 * group policy is refusing the write -- not that we need to elevate. */
bool          wc_write_denied(const wc_state *s);

const char   *wc_state_name(wc_ifstate st);
const char   *wc_strerror   (unsigned long code); /* only for WC_E_APP codes; else NULL */

#endif /* WIFICONTROL_WLAN_H */

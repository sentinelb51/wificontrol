#ifndef WIFICONTROL_WLAN_WIN32_H
#define WIFICONTROL_WLAN_WIN32_H

#include "wlan.h"
#include <windows.h>
#include <wlanapi.h>

typedef struct { HANDLE h; } wc_win32;

/* Opens the client handle and fills in a backend bound to it.  The handle is
 * held open for the lifetime of the process on purpose: both settings are
 * scoped to the client handle, and closing it hands them straight back to the
 * OS.  That is also our safety net -- if this process dies for any reason,
 * Windows restores the defaults with no cleanup on our side. */
unsigned long wcw_open (wc_win32 *w, wc_backend *out);
void          wcw_close(wc_win32 *w);

/* ACM notifications only.  WLAN_NOTIFICATION_SOURCE_MSM additionally requires
 * the wiFiControl device capability, which is gated behind precise-location
 * consent since the 2024 Wi-Fi/location changes and fails with
 * ERROR_ACCESS_DENIED for an ordinary desktop app.  ACM already reports every
 * transition we act on. */
unsigned long wcw_register  (wc_win32 *w, WLAN_NOTIFICATION_CALLBACK cb, void *ctx);
void          wcw_unregister(wc_win32 *w);

/* Deliberately no per-notification-code filter.  The ACM enumeration is based
 * at L2_NOTIFICATION_CODE_V2_BEGIN, which the mingw-w64 headers do not define
 * -- they number the enumeration from 0 instead.  Comparing against those
 * constants would silently match nothing if the real base is nonzero, leaving
 * the app on its watchdog with no visible symptom.  Every ACM notification
 * therefore schedules one debounced, rate-limited poll; a poll that finds
 * nothing to change costs two WlanQueryInterface calls per adapter. */

/* Whether a saved network's cost can be read and written at all on this
 * machine: the WCM API, which Windows 7 does not have, and netsh, which is
 * what writes it.  WC_OK, or the reason it cannot.  Safe to call before
 * wcw_open. */
unsigned long wcw_cost_available(void);

void wcw_format_error(unsigned long code, wchar_t *buf, int cap);
void wcw_guid_to_string(const wc_guid *g, wchar_t *buf, int cap);
bool wcw_guid_from_string(const wchar_t *s, wc_guid *g);

#endif

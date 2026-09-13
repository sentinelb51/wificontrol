/* wlan_win32.c -- the only file that talks to wlanapi. */
#include "wlan_win32.h"
#include <stdio.h>

/* Not present in the mingw-w64 headers; values follow the Windows SDK. */
#ifndef WLAN_READ_ACCESS
#define WLAN_READ_ACCESS    (STANDARD_RIGHTS_READ | FILE_READ_DATA)
#define WLAN_EXECUTE_ACCESS (WLAN_READ_ACCESS | STANDARD_RIGHTS_EXECUTE | FILE_EXECUTE)
#define WLAN_WRITE_ACCESS   (WLAN_READ_ACCESS | WLAN_EXECUTE_ACCESS | \
                             STANDARD_RIGHTS_WRITE | FILE_WRITE_DATA)
#endif

static WLAN_INTF_OPCODE opcode_of(wc_opt o)
{
    return (o == WC_OPT_BGSCAN) ? wlan_intf_opcode_background_scan_enabled
                                : wlan_intf_opcode_media_streaming_mode;
}

static WLAN_SECURABLE_OBJECT securable_of(wc_opt o)
{
    return (o == WC_OPT_BGSCAN) ? wlan_secure_bc_scan_enabled
                                : wlan_secure_media_streaming_mode_enabled;
}

static wc_ifstate state_of(WLAN_INTERFACE_STATE s)
{
    switch (s) {
    case wlan_interface_state_not_ready:            return WC_IF_NOT_READY;
    case wlan_interface_state_connected:            return WC_IF_CONNECTED;
    case wlan_interface_state_ad_hoc_network_formed:return WC_IF_AD_HOC;
    case wlan_interface_state_disconnecting:        return WC_IF_DISCONNECTING;
    case wlan_interface_state_disconnected:         return WC_IF_DISCONNECTED;
    case wlan_interface_state_associating:          return WC_IF_ASSOCIATING;
    case wlan_interface_state_discovering:          return WC_IF_DISCOVERING;
    case wlan_interface_state_authenticating:       return WC_IF_AUTHENTICATING;
    default:                                        return WC_IF_UNKNOWN;
    }
}

static unsigned long be_enum(void *ctx, wc_ifinfo *out, int cap, int *count)
{
    wc_win32 *w = ctx;
    PWLAN_INTERFACE_INFO_LIST list = NULL;

    *count = 0;
    DWORD e = WlanEnumInterfaces(w->h, NULL, &list);
    if (e != ERROR_SUCCESS) return e;
    if (!list) return WC_E_BADDATA;

    int n = 0;
    for (DWORD i = 0; i < list->dwNumberOfItems && n < cap; ++i) {
        const WLAN_INTERFACE_INFO *in = &list->InterfaceInfo[i];
        memcpy(out[n].guid.b, &in->InterfaceGuid, sizeof out[n].guid.b);
        out[n].state = state_of(in->isState);
        if (!WideCharToMultiByte(CP_UTF8, 0, in->strInterfaceDescription, -1,
                                 out[n].name, WC_NAME_MAX, NULL, NULL))
            out[n].name[0] = '\0';
        out[n].name[WC_NAME_MAX - 1] = '\0';
        n++;
    }
    *count = n;
    WlanFreeMemory(list); /* the original leaks every query result; we do not */
    return ERROR_SUCCESS;
}

static unsigned long be_query(void *ctx, const wc_guid *g, wc_opt o, int *value)
{
    wc_win32 *w = ctx;
    DWORD size = 0;
    PVOID data = NULL;
    WLAN_OPCODE_VALUE_TYPE type = wlan_opcode_value_type_invalid;

    GUID id;
    memcpy(&id, g->b, sizeof id); /* wc_guid is byte-aligned; GUID is not */

    DWORD e = WlanQueryInterface(w->h, &id, opcode_of(o), NULL,
                                 &size, &data, &type);
    if (e != ERROR_SUCCESS) return e;
    if (!data) return WC_E_BADDATA;

    /* The payload is a BOOL, so demand all four bytes -- the original accepted
     * anything >= 1 byte and then dereferenced a BOOL through it. */
    if (size < sizeof(BOOL)) { WlanFreeMemory(data); return WC_E_BADDATA; }

    *value = (*(const BOOL *)data) != 0;
    WlanFreeMemory(data);
    return ERROR_SUCCESS;
}

static unsigned long be_set(void *ctx, const wc_guid *g, wc_opt o, int value)
{
    wc_win32 *w = ctx;
    GUID id;
    BOOL v = value ? TRUE : FALSE;
    memcpy(&id, g->b, sizeof id);

    /* Takes roughly a second; the caller must not be the UI thread. */
    return WlanSetInterface(w->h, &id, opcode_of(o), (DWORD)sizeof v, &v, NULL);
}

static unsigned long be_granted(void *ctx, wc_opt o, int *can_write)
{
    wc_win32 *w = ctx;
    WLAN_OPCODE_VALUE_TYPE type = wlan_opcode_value_type_invalid;
    LPWSTR sddl = NULL;
    DWORD granted = 0;

    DWORD e = WlanGetSecuritySettings(w->h, securable_of(o), &type, &sddl, &granted);
    if (sddl) WlanFreeMemory(sddl);
    if (e != ERROR_SUCCESS) return e;

    /* FILE_WRITE_DATA is the bit WLAN_WRITE_ACCESS carries that the read and
     * execute masks do not, so test it directly. */
    *can_write = (granted & FILE_WRITE_DATA) != 0;
    return ERROR_SUCCESS;
}

unsigned long wcw_open(wc_win32 *w, wc_backend *out)
{
    DWORD negotiated = 0;
    w->h = NULL;

    DWORD e = WlanOpenHandle(2, NULL, &negotiated, &w->h);
    if (e != ERROR_SUCCESS) { w->h = NULL; return e; }

    out->ctx           = w;
    out->enum_ifaces   = be_enum;
    out->query_bool    = be_query;
    out->set_bool      = be_set;
    out->granted_write = be_granted;
    return ERROR_SUCCESS;
}

void wcw_close(wc_win32 *w)
{
    if (w->h) { WlanCloseHandle(w->h, NULL); w->h = NULL; }
}

unsigned long wcw_register(wc_win32 *w, WLAN_NOTIFICATION_CALLBACK cb, void *ctx)
{
    return WlanRegisterNotification(w->h, WLAN_NOTIFICATION_SOURCE_ACM, TRUE,
                                    cb, ctx, NULL, NULL);
}

void wcw_unregister(wc_win32 *w)
{
    if (!w->h) return;
    /* Blocks until any in-flight callback returns, so it must never be called
     * from the callback itself. */
    WlanRegisterNotification(w->h, WLAN_NOTIFICATION_SOURCE_NONE, FALSE,
                             NULL, NULL, NULL, NULL);
}

void wcw_format_error(unsigned long code, wchar_t *buf, int cap)
{
    if (code == WC_OK) { buf[0] = L'\0'; return; }

    const char *own = wc_strerror(code);
    if (own) {
        MultiByteToWideChar(CP_UTF8, 0, own, -1, buf, cap);
        return;
    }

    LPWSTR msg = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                             FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)code,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPWSTR)&msg, 0, NULL);
    if (n && msg) {
        while (n && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' || msg[n - 1] == L'.'))
            msg[--n] = L'\0';
        _snwprintf(buf, (size_t)cap, L"%s (%lu)", msg, code);
    } else {
        _snwprintf(buf, (size_t)cap, L"error %lu", code);
    }
    buf[cap - 1] = L'\0';
    if (msg) LocalFree(msg);
}

void wcw_guid_to_string(const wc_guid *g, wchar_t *buf, int cap)
{
    GUID id;
    memcpy(&id, g->b, sizeof id);
    _snwprintf(buf, (size_t)cap,
               L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               id.Data1, id.Data2, id.Data3,
               id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
               id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
    buf[cap - 1] = L'\0';
}

/* wlan_win32.c -- the only file that talks to wlanapi, and to the cost of the
 * profiles it lists. */
#include <winsock2.h> /* before windows.h, which iphlpapi needs */
#include "wlan_win32.h"
#include <iphlpapi.h>
#include <stdio.h>

/* Not present in the mingw-w64 headers; values follow the Windows SDK. */
#ifndef WLAN_READ_ACCESS
#define WLAN_READ_ACCESS    (STANDARD_RIGHTS_READ | FILE_READ_DATA)
#define WLAN_EXECUTE_ACCESS (WLAN_READ_ACCESS | STANDARD_RIGHTS_EXECUTE | FILE_EXECUTE)
#define WLAN_WRITE_ACCESS   (WLAN_READ_ACCESS | WLAN_EXECUTE_ACCESS | \
                             STANDARD_RIGHTS_WRITE | FILE_WRITE_DATA)
#endif

/* Neither is wcmapi.h.  The one call needed is declared from the SDK and bound
 * at runtime, so on Windows 7, which has no WCM API, reading a cost is simply
 * unsupported. */
typedef struct { DWORD cost, source; } wcm_cost_data;  /* WCM_CONNECTION_COST_DATA */
enum { WCM_INTF_PROPERTY_CONNECTION_COST = 4 };        /* wcm_intf_property_connection_cost */
typedef DWORD (WINAPI *wcm_query_fn)(const GUID *, LPCWSTR, int, PVOID, PDWORD, PBYTE *);
typedef VOID  (WINAPI *wcm_free_fn)(PVOID);
static wcm_query_fn wcm_query;
static wcm_free_fn  wcm_free;

static WLAN_INTF_OPCODE opcode_of(wc_opt o)
{
    switch (o) {
    case WC_OPT_BGSCAN:   return wlan_intf_opcode_background_scan_enabled;
    case WC_OPT_AUTOCONF: return wlan_intf_opcode_autoconf_enabled;
    default:              return wlan_intf_opcode_media_streaming_mode;
    }
}

static WLAN_SECURABLE_OBJECT securable_of(wc_opt o)
{
    switch (o) {
    case WC_OPT_BGSCAN:   return wlan_secure_bc_scan_enabled;
    case WC_OPT_AUTOCONF: return wlan_secure_ac_enabled;
    default:              return wlan_secure_media_streaming_mode_enabled;
    }
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

/* ------------------------------------------------------------ profile cost */

/* Neither of these is location-gated: only BSSID-bearing calls are. */
static unsigned long be_profiles(void *ctx, const wc_guid *g, wc_profile *out, int cap, int *count)
{
    wc_win32 *w = ctx;
    PWLAN_PROFILE_INFO_LIST list = NULL;
    GUID id;
    memcpy(&id, g->b, sizeof id);

    *count = 0;
    DWORD e = WlanGetProfileList(w->h, &id, NULL, &list);
    if (e != ERROR_SUCCESS) return e;
    if (!list) return WC_E_BADDATA;

    int n = 0;
    for (DWORD i = 0; i < list->dwNumberOfItems && n < cap; ++i) {
        /* A name that does not convert cleanly is skipped, never written back
         * under a mangled spelling. */
        const WCHAR *src = list->ProfileInfo[i].strProfileName;
        int len = (int)wcsnlen(src, WLAN_MAX_NAME_LENGTH);
        int b = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, len,
                                    out[n].name, WC_PROFILE_MAX - 1, NULL, NULL);
        if (len && b > 0) { out[n].name[b] = '\0'; n++; }
    }
    *count = n;
    WlanFreeMemory(list);
    return ERROR_SUCCESS;
}

static bool profile_w(const char *utf8, wchar_t *out)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                               out, WLAN_MAX_NAME_LENGTH + 1) > 0;
}

/* Bound at runtime, once: on Windows 7 there is no WCM API at all, and a
 * missing export must read as "not supported here", not as a failure. */
static bool load_wcm(void)
{
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryExW(L"wcmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m) {
            /* Through void *: GCC warns on a direct cast between function types. */
            wcm_free  = (wcm_free_fn)(void *)GetProcAddress(m, "WcmFreeMemory");
            wcm_query = wcm_free ? (wcm_query_fn)(void *)GetProcAddress(m, "WcmQueryProperty")
                                 : NULL;
        }
    }
    return wcm_query != NULL;
}

/* netsh, which is what actually writes a cost; see be_set_metered. */
static bool netsh_path(wchar_t *out, size_t cap)
{
    UINT n = GetSystemDirectoryW(out, (UINT)cap - 12);
    if (!n || n >= cap - 12) return false;
    wcscat(out, L"\\netsh.exe");
    return true;
}

unsigned long wcw_cost_available(void)
{
    if (!load_wcm()) return ERROR_NOT_SUPPORTED;

    wchar_t exe[MAX_PATH];
    if (!netsh_path(exe, MAX_PATH)) return ERROR_PATH_NOT_FOUND;
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        return e ? e : ERROR_FILE_NOT_FOUND;
    }
    return ERROR_SUCCESS;
}

static unsigned long be_query_cost([[maybe_unused]] void *ctx, const wc_guid *g,
                                   const char *profile, unsigned long *cost, int *source)
{
    if (!wcm_query) return ERROR_NOT_SUPPORTED;

    wchar_t name[WLAN_MAX_NAME_LENGTH + 1];
    if (!profile_w(profile, name)) return ERROR_INVALID_NAME;
    GUID id;
    memcpy(&id, g->b, sizeof id);

    DWORD size = 0;
    PBYTE data = NULL;
    DWORD e = wcm_query(&id, name, WCM_INTF_PROPERTY_CONNECTION_COST, NULL, &size, &data);
    if (e != ERROR_SUCCESS) return e;
    if (!data) return WC_E_BADDATA;
    if (size < sizeof(wcm_cost_data)) { wcm_free(data); return WC_E_BADDATA; }

    wcm_cost_data d;
    memcpy(&d, data, sizeof d);
    wcm_free(data);
    *cost   = d.cost;
    *source = (int)d.source;
    return ERROR_SUCCESS;
}

/* Written through netsh, not WcmSetProperty.  Measured on Windows 11:
 * WcmSetProperty succeeds on a Wi-Fi profile but only records an operator
 * cost, which Windows ignores for Wi-Fi, so the effective cost never moves.
 * `netsh wlan set profileparameter cost=` records the same user cost the
 * Settings app does, takes effect at once, and is documented. */
static unsigned long be_set_metered([[maybe_unused]] void *ctx, const wc_guid *g,
                                    const char *profile, int metered)
{
    GUID id;
    memcpy(&id, g->b, sizeof id);

    /* netsh names an interface by its alias, not its GUID.  Naming it keeps a
     * profile two adapters share from being written on both. */
    NET_LUID luid;
    wchar_t alias[IF_MAX_STRING_SIZE + 1], name[WLAN_MAX_NAME_LENGTH + 1];
    if (ConvertInterfaceGuidToLuid(&id, &luid) != NO_ERROR ||
        ConvertInterfaceLuidToAlias(&luid, alias, IF_MAX_STRING_SIZE + 1) != NO_ERROR)
        return ERROR_NOT_FOUND;
    if (!profile_w(profile, name)) return ERROR_INVALID_NAME;
    /* Quotes delimit the arguments, so a name containing one cannot be passed. */
    if (wcschr(name, L'"') || wcschr(alias, L'"')) return ERROR_INVALID_NAME;

    wchar_t exe[MAX_PATH], cmd[1024];
    if (!netsh_path(exe, MAX_PATH)) return ERROR_PATH_NOT_FOUND;

    int len = _snwprintf(cmd, 1024,
                         L"\"%s\" wlan set profileparameter name=\"%s\" interface=\"%s\" cost=%s",
                         exe, name, alias, metered ? L"Variable" : L"Default");
    if (len < 0 || len >= 1024) return ERROR_BUFFER_OVERFLOW;

    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return GetLastError();
    CloseHandle(pi.hThread);

    /* About 40 ms.  The bound only stops a wedged netsh from holding up the
     * worker, which the exit path waits on. */
    DWORD code = 1, r = WaitForSingleObject(pi.hProcess, 3000);
    if (r == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else                    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);

    if (r != WAIT_OBJECT_0) return WAIT_TIMEOUT;
    return code ? WC_E_NETSH : ERROR_SUCCESS;
}

/* ------------------------------------------------------------------ handle */

unsigned long wcw_open(wc_win32 *w, wc_backend *out)
{
    DWORD negotiated = 0;
    w->h = NULL;

    DWORD e = WlanOpenHandle(2, NULL, &negotiated, &w->h);
    if (e != ERROR_SUCCESS) { w->h = NULL; return e; }

    load_wcm();

    out->ctx           = w;
    out->enum_ifaces   = be_enum;
    out->query_bool    = be_query;
    out->set_bool      = be_set;
    out->granted_write = be_granted;
    out->list_profiles = be_profiles;
    out->query_cost    = be_query_cost;
    out->set_metered   = be_set_metered;
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

bool wcw_guid_from_string(const wchar_t *s, wc_guid *g)
{
    unsigned int d1, d2, d3, b[8];
    if (swscanf(s, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    GUID out;
    out.Data1 = (unsigned long)d1;
    out.Data2 = (unsigned short)d2;
    out.Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; ++i) out.Data4[i] = (unsigned char)b[i];
    memcpy(g->b, &out, sizeof g->b);
    return true;
}

/* tune_driver.c -- the adapter's own advanced properties.
 *
 * These are the entries on Device Manager's Advanced tab.  The choices are not
 * hardcoded per vendor: the driver publishes them under Ndi\Params in its
 * class-instance key, and we render exactly what it declares.
 *
 * Also the Wi-Fi Direct virtual adapters Windows layers on the same card,
 * which are devices of their own rather than properties, and single-property
 * access by adapter for the Performance switch.
 */
#include "tune.h"
#include "wlan_win32.h"

#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <regstr.h>
#include <stdio.h>
#include <stdlib.h>

static const wchar_t *CLASS_NET =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";

/* One line on what a well-known property does: what changing it does to the
 * radio or the link, never which choice to make.  Keyed by registry keyword,
 * not ParamDesc, which the INF may translate.  Starred keywords are Microsoft's
 * standardised ones and mean the same on any vendor's card; the rest are the
 * names Intel's driver uses.  A property not listed simply has no line. */
typedef struct { const wchar_t *keyword, *help; } known_prop;

static const known_prop KNOWN[] = {
    { L"*PacketCoalescing",
      L"Batches received broadcast and multicast frames into fewer interrupts; they arrive later." },
    { L"*InterruptModeration",
      L"Waits for more packets or a timeout before raising a receive interrupt; packets arrive later." },
    { L"*RscIPv4",
      L"Hands a run of received IPv4 TCP segments to the stack as one, so only one header is processed." },
    { L"*RscIPv6",
      L"Hands a run of received IPv6 TCP segments to the stack as one, so only one header is processed." },
    { L"*RSS",
      L"Spreads receive processing over CPUs, one per connection; off, it all runs on the interrupt's CPU." },
    { L"*SelectiveSuspend",
      L"NDIS suspends the adapter after a few idle seconds; the next packet waits for it to resume." },
    { L"*DeviceSleepOnDisconnect",
      L"With no link, the adapter drops to low power (D3) and returns to full power on reconnect." },

    { L"BgScanGlobalBlocking",
      L"Blocks background scans while connected: Never, only while the signal is good, or Always." },
    { L"RoamAggressiveness",
      L"Signal level at which the adapter starts scanning for a better AP; higher scans sooner." },
    { L"RoamingPreferredBandType",
      L"Biases AP selection and roaming towards the chosen band; other bands stay usable." },
    { L"ChannelWidth24",
      L"Auto follows the AP up to 40 MHz; 20 MHz only never bonds channels, capping peak rate." },
    { L"ChannelWidth52",
      L"Auto follows the AP up to 160 MHz; 20 MHz only never bonds channels, capping peak rate." },
    { L"ChannelWidth6",
      L"Auto follows the AP's channel width; 20 MHz only never bonds channels, capping peak rate." },
    { L"FatChannelIntolerant",
      L"Sets the 40 MHz Intolerant bit, asking nearby 2.4 GHz networks to use 20 MHz channels." },
    { L"CtsToItself",
      L"Airtime reservation with 802.11b nearby: RTS/CTS covers hidden nodes, CTS-to-self costs less." },
    { L"IEEE11nMode",
      L"Newest PHY used: 802.11n (HT), ac (VHT) or ax (HE); Disabled limits the link to a/b/g rates." },
    { L"WirelessMode",
      L"Which 802.11a/b/g modes, and so which bands, are allowed: a is 5 GHz, b and g are 2.4 GHz." },
    { L"Is6GhzBandSupported",
      L"Whether the adapter scans and connects on the 6 GHz band (Wi-Fi 6E) at all." },
    { L"MIMOPowerSaveMode",
      L"Receive chains kept on: No SMPS all, Dynamic one until the AP sends RTS, Static only one." },
    { L"uAPSDSupport",
      L"WMM Power Save: while dozing, the AP holds frames until the adapter sends a trigger frame." },
    { L"ThroughputBoosterEnabled",
      L"Holds the medium longer to burst buffered uplink frames; upload only, at others' airtime." },
    { L"IbssTxPower",
      L"Radio transmit power; lower shrinks range and the signal strength the AP receives." },
};

/* The ones that act only while the PC sleeps, which the dialog keeps apart. */
static const known_prop ASLEEP[] = {
    { L"*PMARPOffload",
      L"While the PC sleeps, the adapter answers IPv4 ARP requests itself instead of waking it." },
    { L"*PMNSOffload",
      L"While the PC sleeps, the adapter answers IPv6 neighbour solicitations itself instead of waking it." },
    { L"*PMWiFiRekeyOffload",
      L"While the PC sleeps, the adapter completes group key (GTK) rekeys itself to stay associated." },
    { L"*WakeOnMagicPacket",
      L"A magic packet (this adapter's MAC address repeated 16 times) wakes the PC from sleep." },
    { L"*ModernStandbyWoLMagicPacket",
      L"A magic packet wakes the PC from modern standby (S0ix); hibernation is not affected." },
    { L"*WakeOnPattern",
      L"Packets matching patterns Windows registers, such as an incoming TCP SYN, wake the PC." },
};

static const known_prop *known(const known_prop *table, size_t n, const wchar_t *keyword)
{
    for (size_t i = 0; i < n; ++i)
        if (_wcsicmp(table[i].keyword, keyword) == 0) return &table[i];
    return nullptr;
}

static bool reg_str(HKEY key, const wchar_t *name, wchar_t *out, DWORD chars)
{
    DWORD type = 0, bytes = chars * sizeof(wchar_t);
    if (RegQueryValueExW(key, name, nullptr, &type, (BYTE *)out, &bytes) != ERROR_SUCCESS)
        return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    out[(bytes / sizeof(wchar_t)) < chars ? (bytes / sizeof(wchar_t)) : chars - 1] = L'\0';
    out[chars - 1] = L'\0';
    return true;
}

/* Most drivers store these as REG_SZ, some as REG_DWORD.  Accept either, so
 * the dropdown opens on the value that is actually in force. */
static bool reg_value(HKEY key, const wchar_t *name, wchar_t *out, DWORD chars)
{
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
        return false;

    if (type == REG_DWORD) {
        DWORD v = 0;
        bytes = sizeof v;
        if (RegQueryValueExW(key, name, nullptr, nullptr, (BYTE *)&v, &bytes) != ERROR_SUCCESS)
            return false;
        _snwprintf(out, chars, L"%lu", (unsigned long)v);
        out[chars - 1] = L'\0';
        return true;
    }
    return reg_str(key, name, out, chars);
}

/* The class key holds one numbered subkey per network device; the one we want
 * is whichever carries this adapter's GUID in NetCfgInstanceId. */
static bool find_instance(const wc_guid *adapter, wchar_t *out, DWORD chars)
{
    wchar_t want[64];
    wcw_guid_to_string(adapter, want, 64);

    HKEY cls;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CLASS_NET, 0, KEY_READ, &cls) != ERROR_SUCCESS)
        return false;

    bool found = false;
    for (DWORD i = 0; !found; ++i) {
        wchar_t sub[64];
        DWORD len = 64;
        if (RegEnumKeyExW(cls, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        HKEY inst;
        if (RegOpenKeyExW(cls, sub, 0, KEY_READ, &inst) != ERROR_SUCCESS) continue;

        wchar_t id[64];
        if (reg_str(inst, L"NetCfgInstanceId", id, 64) && _wcsicmp(id, want) == 0) {
            _snwprintf(out, chars, L"%s\\%s", CLASS_NET, sub);
            out[chars - 1] = L'\0';
            found = true;
        }
        RegCloseKey(inst);
    }
    RegCloseKey(cls);
    return found;
}

/* Ndi\Params\<name>\Enum holds one value per choice: the value's *name* is
 * what gets written back, its data is the label shown to the user. */
static int read_enum_options(HKEY param, tune_option *opt, int cap)
{
    HKEY e;
    if (RegOpenKeyExW(param, L"Enum", 0, KEY_READ, &e) != ERROR_SUCCESS) return 0;

    int n = 0;
    for (DWORD i = 0; n < cap; ++i) {
        wchar_t name[TUNE_KEY_MAX];
        wchar_t data[TUNE_TEXT_MAX];
        DWORD namelen = TUNE_KEY_MAX, type = 0, bytes = sizeof data;

        LONG r = RegEnumValueW(e, i, name, &namelen, nullptr, &type,
                               (BYTE *)data, &bytes);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;

        data[(bytes / sizeof(wchar_t)) < TUNE_TEXT_MAX
             ? (bytes / sizeof(wchar_t)) : TUNE_TEXT_MAX - 1] = L'\0';
        data[TUNE_TEXT_MAX - 1] = L'\0';

        wcsncpy(opt[n].raw,   name, TUNE_KEY_MAX - 1);
        opt[n].raw[TUNE_KEY_MAX - 1] = L'\0';
        wcsncpy(opt[n].label, data[0] ? data : name, TUNE_TEXT_MAX - 1);
        opt[n].label[TUNE_TEXT_MAX - 1] = L'\0';
        opt[n].index = i;
        n++;
    }
    RegCloseKey(e);
    return n;
}

void tune_collect_driver(tune_list *l, const wc_guid *adapter)
{
    l->have_instance = find_instance(adapter, l->instance_key, MAX_PATH);
    if (!l->have_instance) return;

    wchar_t params_path[MAX_PATH];
    _snwprintf(params_path, MAX_PATH, L"%s\\Ndi\\Params", l->instance_key);
    params_path[MAX_PATH - 1] = L'\0';

    HKEY params;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, params_path, 0, KEY_READ, &params) != ERROR_SUCCESS)
        return;

    HKEY inst = nullptr;
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, l->instance_key, 0, KEY_READ, &inst);

    for (DWORD i = 0; l->n < TUNE_MAX_SETTINGS; ++i) {
        wchar_t pname[TUNE_KEY_MAX];
        DWORD len = TUNE_KEY_MAX;
        if (RegEnumKeyExW(params, i, pname, &len, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;

        HKEY param;
        if (RegOpenKeyExW(params, pname, 0, KEY_READ, &param) != ERROR_SUCCESS) continue;

        wchar_t type[32] = L"";
        reg_str(param, L"Type", type, 32);

        tune_setting *s = &l->s[l->n];
        memset(s, 0, sizeof *s);
        s->n_opt = read_enum_options(param, s->opt, TUNE_MAX_OPTIONS);

        /* Only fixed choices belong in a dropdown; ints and free text do not. */
        if (s->n_opt < 2 || (type[0] && _wcsicmp(type, L"enum") != 0)) {
            RegCloseKey(param);
            continue;
        }

        s->src = TUNE_DRIVER;
        wcscpy(s->group, L"Adapter");
        if (!reg_str(param, L"ParamDesc", s->name, TUNE_TEXT_MAX)) {
            wcsncpy(s->name, pname, TUNE_TEXT_MAX - 1);
            s->name[TUNE_TEXT_MAX - 1] = L'\0';
        }
        wcsncpy(s->value_name, pname, TUNE_KEY_MAX - 1);
        s->value_name[TUNE_KEY_MAX - 1] = L'\0';
        const known_prop *asleep = known(ASLEEP, sizeof ASLEEP / sizeof ASLEEP[0], pname);
        const known_prop *kp = asleep ? asleep : known(KNOWN, sizeof KNOWN / sizeof KNOWN[0], pname);
        s->help   = kp ? kp->help : nullptr;
        s->asleep = asleep != nullptr;

        /* Live value, else the driver's declared default. */
        wchar_t live[TUNE_KEY_MAX] = L"";
        if (!(inst && reg_value(inst, pname, live, TUNE_KEY_MAX)))
            reg_value(param, L"Default", live, TUNE_KEY_MAX);

        s->cur = -1;
        for (int k = 0; k < s->n_opt; ++k)
            if (_wcsicmp(s->opt[k].raw, live) == 0) { s->cur = k; break; }
        s->sel = s->cur;

        l->n++;
        RegCloseKey(param);
    }
    if (inst) RegCloseKey(inst);
    RegCloseKey(params);
}

static unsigned long write_value(const wchar_t *instance_key, const wchar_t *name,
                                 const wchar_t *raw)
{
    HKEY inst;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, instance_key, 0,
                           KEY_QUERY_VALUE | KEY_SET_VALUE, &inst);
    if (r != ERROR_SUCCESS) return (unsigned long)r;

    /* Keep whatever type the driver already uses for this value. */
    DWORD existing = REG_SZ, ignored = 0;
    RegQueryValueExW(inst, name, nullptr, &existing, nullptr, &ignored);

    if (existing == REG_DWORD) {
        DWORD v = (DWORD)wcstoul(raw, nullptr, 10);
        r = RegSetValueExW(inst, name, 0, REG_DWORD, (const BYTE *)&v, (DWORD)sizeof v);
    } else {
        r = RegSetValueExW(inst, name, 0, REG_SZ, (const BYTE *)raw,
                           (DWORD)((wcslen(raw) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(inst);
    return (unsigned long)r;
}

unsigned long tune_write_driver(const tune_list *l, const tune_setting *s)
{
    if (!l->have_instance) return ERROR_NOT_FOUND;
    return write_value(l->instance_key, s->value_name, s->opt[s->sel].raw);
}

unsigned long tune_driver_get(const wc_guid *adapter, const wchar_t *keyword,
                              const wchar_t *choice, wchar_t *live, int cap, bool *has_choice)
{
    wchar_t inst_key[MAX_PATH], param_key[MAX_PATH];
    if (!find_instance(adapter, inst_key, MAX_PATH)) return ERROR_FILE_NOT_FOUND;
    _snwprintf(param_key, MAX_PATH, L"%s\\Ndi\\Params\\%s", inst_key, keyword);
    param_key[MAX_PATH - 1] = L'\0';

    HKEY param;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, param_key, 0, KEY_READ, &param);
    if (r != ERROR_SUCCESS) return (unsigned long)r;

    /* Only a property with a list of choices has an Enum key. */
    HKEY choices;
    *has_choice = false;
    if (RegOpenKeyExW(param, L"Enum", 0, KEY_READ, &choices) == ERROR_SUCCESS) {
        *has_choice = RegQueryValueExW(choices, choice, nullptr, nullptr, nullptr, nullptr)
                      == ERROR_SUCCESS;
        RegCloseKey(choices);
    }

    /* Live value, else the driver's declared default. */
    HKEY inst;
    bool found = false;
    live[0] = L'\0';
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, inst_key, 0, KEY_READ, &inst) == ERROR_SUCCESS) {
        found = reg_value(inst, keyword, live, (DWORD)cap);
        RegCloseKey(inst);
    }
    if (!found) reg_value(param, L"Default", live, (DWORD)cap);
    RegCloseKey(param);
    return ERROR_SUCCESS;
}

/* Opened for writing and closed again, so a driver key behind a policy is
 * found now rather than on the first attempt to hold a value. */
unsigned long tune_driver_check(const wc_guid *adapter)
{
    wchar_t inst_key[MAX_PATH];
    if (!find_instance(adapter, inst_key, MAX_PATH)) return ERROR_FILE_NOT_FOUND;

    HKEY inst;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, inst_key, 0,
                           KEY_QUERY_VALUE | KEY_SET_VALUE, &inst);
    if (r != ERROR_SUCCESS) return (unsigned long)r;
    RegCloseKey(inst);
    return ERROR_SUCCESS;
}

unsigned long tune_driver_set(const wc_guid *adapter, const wchar_t *keyword, const wchar_t *raw)
{
    wchar_t inst_key[MAX_PATH];
    if (!find_instance(adapter, inst_key, MAX_PATH)) return ERROR_FILE_NOT_FOUND;
    return write_value(inst_key, keyword, raw);
}

/* ---------------------------------------------------------------- devices */

/* The card itself: the network device whose driver key is this class-instance
 * key. */
static bool find_card(const wchar_t *instance_key, HDEVINFO set, SP_DEVINFO_DATA *out)
{
    /* The instance key path ends in the four digits SPDRP_DRIVER reports. */
    const wchar_t *slash = wcsrchr(instance_key, L'\\');
    if (!slash) return false;
    wchar_t want[64];
    _snwprintf(want, 64, L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\%s", slash + 1);
    want[63] = L'\0';

    out->cbSize = sizeof *out;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, out); ++i) {
        wchar_t drv[64] = L"";
        if (SetupDiGetDeviceRegistryPropertyW(set, out, SPDRP_DRIVER, nullptr,
                                              (BYTE *)drv, sizeof drv, nullptr) &&
            _wcsicmp(drv, want) == 0)
            return true;
    }
    return false;
}

/* DICS_ENABLE or DICS_DISABLE, for every hardware profile: what Device
 * Manager does, and it persists across restarts. */
static bool set_state(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD change)
{
    SP_PROPCHANGE_PARAMS pc = {
        .ClassInstallHeader = { .cbSize = sizeof(SP_CLASSINSTALL_HEADER),
                                .InstallFunction = DIF_PROPERTYCHANGE },
        .StateChange = change,
        .Scope = DICS_FLAG_GLOBAL,
    };
    return SetupDiSetClassInstallParamsW(set, dev, &pc.ClassInstallHeader, sizeof pc) &&
           SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, dev);
}

/* Disable then re-enable the device so the miniport re-reads its parameters.
 * Same thing Device Manager does when you press OK on the Advanced tab. */
static unsigned long restart_card(const wchar_t *instance_key)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return GetLastError();

    unsigned long result = ERROR_NOT_FOUND;
    SP_DEVINFO_DATA dev = { .cbSize = sizeof dev };
    if (find_card(instance_key, set, &dev)) {
        if (!set_state(set, &dev, DICS_DISABLE))     result = GetLastError();
        else if (!set_state(set, &dev, DICS_ENABLE)) result = GetLastError(); /* left disabled: the caller must report this */
        else                                         result = ERROR_SUCCESS;
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

unsigned long tune_restart_adapter(const tune_list *l)
{
    return l->have_instance ? restart_card(l->instance_key) : ERROR_NOT_FOUND;
}

unsigned long tune_driver_restart(const wc_guid *adapter)
{
    wchar_t inst_key[MAX_PATH];
    if (!find_instance(adapter, inst_key, MAX_PATH)) return ERROR_FILE_NOT_FOUND;
    return restart_card(inst_key);
}

/* ----------------------------------------------------------- Wi-Fi Direct */

/* The virtual adapters from vwifimp.inf: one for Wi-Fi Direct and Miracast,
 * one for Mobile Hotspot.  Matched by parent too, so a second card's are
 * never touched. */
static const wchar_t WFD_HWID[] = L"{5d624f94-8850-40c3-a3fa-a4fd2080baf3}\\vwifimp_wfd";
enum { WFD_MAX = 8 };

typedef struct { SP_DEVINFO_DATA dev; wchar_t id[MAX_DEVICE_ID_LEN]; } wfd_dev;

static int by_id(const void *a, const void *b)
{
    return _wcsicmp(((const wfd_dev *)a)->id, ((const wfd_dev *)b)->id);
}

/* Disabled now, or from the next restart: the stored choice is what counts. */
static bool dev_disabled(HDEVINFO set, SP_DEVINFO_DATA *dev)
{
    DWORD flags = 0;
    ULONG status = 0, problem = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_CONFIGFLAGS, nullptr, (BYTE *)&flags,
                                          sizeof flags, nullptr) &&
        (flags & CONFIGFLAG_DISABLED))
        return true;
    return CM_Get_DevNode_Status(&status, &problem, dev->DevInst, 0) == CR_SUCCESS &&
           (status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED;
}

/* This card's Wi-Fi Direct adapters in instance ID order, so a list of their
 * states means the same thing next time.  `state` gets a '1' (enabled) or '0'
 * (disabled) for each, and an empty string when the card has none.  With
 * `want`, each adapter is first moved to its character there, an adapter past
 * the end taking the last one. */
static unsigned long wfd_walk(const wchar_t *instance_key, const char *want, char *state, int cap)
{
    state[0] = '\0';
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return GetLastError();

    SP_DEVINFO_DATA card = { .cbSize = sizeof card }, dev = { .cbSize = sizeof dev };
    unsigned long result = find_card(instance_key, set, &card) ? ERROR_SUCCESS : ERROR_NOT_FOUND;
    wfd_dev found[WFD_MAX];
    int n = 0;

    for (DWORD i = 0; result == ERROR_SUCCESS && n < WFD_MAX && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        /* A multi-string: the first entry is the one to match.  Two characters
         * short, so it always ends in a double terminator. */
        wchar_t hwid[256] = L"";
        DEVINST parent = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, (BYTE *)hwid,
                                               sizeof hwid - 2 * sizeof(wchar_t), nullptr) ||
            _wcsicmp(hwid, WFD_HWID) != 0 ||
            CM_Get_Parent(&parent, dev.DevInst, 0) != CR_SUCCESS || parent != card.DevInst)
            continue;
        found[n].dev = dev;
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, found[n].id, MAX_DEVICE_ID_LEN, nullptr))
            found[n].id[0] = L'\0';
        n++;
    }
    qsort(found, (size_t)n, sizeof *found, by_id);

    const size_t len = want ? strlen(want) : 0;
    bool reboot = false;
    for (int k = 0; result == ERROR_SUCCESS && k < n; ++k) {
        bool off = dev_disabled(set, &found[k].dev);
        if (len && (want[(size_t)k < len ? (size_t)k : len - 1] == '0') != off) {
            if (!set_state(set, &found[k].dev, off ? DICS_ENABLE : DICS_DISABLE)) {
                result = GetLastError();
                break;
            }
            SP_DEVINSTALL_PARAMS_W ip = { .cbSize = sizeof ip };
            if (SetupDiGetDeviceInstallParamsW(set, &found[k].dev, &ip) &&
                (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
                reboot = true;
            off = dev_disabled(set, &found[k].dev);
        }
        if (k < cap - 1) {
            state[k]     = off ? '0' : '1';
            state[k + 1] = '\0';
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return (result == ERROR_SUCCESS && reboot) ? ERROR_SUCCESS_REBOOT_REQUIRED : result;
}

unsigned long tune_wfd_get(const wc_guid *adapter, char *state, int cap)
{
    wchar_t inst_key[MAX_PATH];
    state[0] = '\0';
    if (!find_instance(adapter, inst_key, MAX_PATH)) return ERROR_FILE_NOT_FOUND;
    return wfd_walk(inst_key, nullptr, state, cap);
}

unsigned long tune_wfd_set(const wc_guid *adapter, const char *state)
{
    wchar_t inst_key[MAX_PATH];
    char after[WFD_MAX + 1];
    if (!state[0]) return ERROR_INVALID_PARAMETER;
    if (!find_instance(adapter, inst_key, MAX_PATH)) return ERROR_FILE_NOT_FOUND;
    return wfd_walk(inst_key, state, after, sizeof after);
}

void tune_collect_wfd(tune_list *l)
{
    char state[WFD_MAX + 1];
    if (l->n >= TUNE_MAX_SETTINGS || !l->have_instance ||
        wfd_walk(l->instance_key, nullptr, state, sizeof state) != ERROR_SUCCESS || !state[0])
        return;
    const int total = (int)strlen(state);
    int disabled = 0;
    for (int k = 0; k < total; ++k) disabled += state[k] == '0';

    tune_setting *s = &l->s[l->n];
    memset(s, 0, sizeof *s);
    s->src = TUNE_DEVICE;
    wcscpy(s->group, L"Wi-Fi Direct");
    _snwprintf(s->name, TUNE_TEXT_MAX, L"Wi-Fi Direct adapters (%d)", total);
    s->name[TUNE_TEXT_MAX - 1] = L'\0';
    s->help = L"Miracast, Mobile Hotspot and Wi-Fi Direct share the radio through these; "
              L"disabled, they stop.";

    s->n_opt = 2;
    wcscpy(s->opt[0].label, L"Enabled");
    s->opt[0].index = DICS_ENABLE;
    wcscpy(s->opt[1].label, L"Disabled");
    s->opt[1].index = DICS_DISABLE;

    /* Half and half matches neither choice; either one then sets both. */
    s->cur = disabled == 0 ? 0 : disabled == total ? 1 : -1;
    s->sel = s->cur;
    l->n++;
}

unsigned long tune_write_wfd(const tune_list *l, const tune_setting *s)
{
    char state[WFD_MAX + 1];
    if (!l->have_instance) return ERROR_NOT_FOUND;
    unsigned long e = wfd_walk(l->instance_key, s->opt[s->sel].index == DICS_DISABLE ? "0" : "1",
                               state, sizeof state);
    return (e == ERROR_SUCCESS && !state[0]) ? ERROR_NOT_FOUND : e;
}

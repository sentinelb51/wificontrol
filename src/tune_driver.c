/* tune_driver.c -- the adapter's own advanced properties.
 *
 * These are the entries on Device Manager's Advanced tab.  The choices are not
 * hardcoded per vendor: the driver publishes them under Ndi\Params in its
 * class-instance key, and we render exactly what it declares.
 */
#include "tune.h"
#include "wlan_win32.h"

#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>
#include <stdlib.h>

static const wchar_t *CLASS_NET =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";

/* One line on what a well-known property does: what changing it does to the
 * radio or the link, never which choice to make.  Keyed by registry keyword,
 * not ParamDesc, which the INF may translate.  Starred keywords are Microsoft's
 * standardized ones and mean the same on any vendor's card; the rest are the
 * names Intel's driver uses.  A property not listed simply has no line. */
static const struct { const wchar_t *keyword, *help; } KNOWN[] = {
    { L"*PacketCoalescing",
      L"Batches received broadcast and multicast frames into fewer interrupts; they arrive later." },
    { L"*SelectiveSuspend",
      L"NDIS suspends the adapter after a few idle seconds; the next packet waits for it to resume." },
    { L"*DeviceSleepOnDisconnect",
      L"With no link, the adapter drops to low power (D3) and returns to full power on reconnect." },
    { L"*PMARPOffload",
      L"While the PC sleeps, the adapter answers IPv4 ARP requests itself instead of waking it." },
    { L"*PMNSOffload",
      L"While the PC sleeps, the adapter answers IPv6 neighbor solicitations itself instead of waking it." },
    { L"*PMWiFiRekeyOffload",
      L"While the PC sleeps, the adapter completes group key (GTK) rekeys itself to stay associated." },
    { L"*WakeOnMagicPacket",
      L"A magic packet (this adapter's MAC address repeated 16 times) wakes the PC from sleep." },
    { L"*ModernStandbyWoLMagicPacket",
      L"A magic packet wakes the PC from modern standby (S0ix); hibernation is not affected." },
    { L"*WakeOnPattern",
      L"Packets matching patterns Windows registers, such as an incoming TCP SYN, wake the PC." },

    { L"BgScanGlobalBlocking",
      L"Blocks background scans while connected: Never, only while the signal is good, or Always." },
    { L"RoamAggressiveness",
      L"Signal level at which the adapter starts scanning for a better AP; higher scans sooner." },
    { L"RoamingPreferredBandType",
      L"Biases AP selection and roaming toward the chosen band; other bands stay usable." },
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

static const wchar_t *known_help(const wchar_t *keyword)
{
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; ++i)
        if (_wcsicmp(KNOWN[i].keyword, keyword) == 0) return KNOWN[i].help;
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
        s->help = known_help(pname);

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

unsigned long tune_write_driver(const tune_list *l, const tune_setting *s)
{
    if (!l->have_instance) return ERROR_NOT_FOUND;

    HKEY inst;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, l->instance_key, 0,
                           KEY_QUERY_VALUE | KEY_SET_VALUE, &inst);
    if (r != ERROR_SUCCESS) return (unsigned long)r;

    /* Keep whatever type the driver already uses for this value. */
    DWORD existing = REG_SZ, ignored = 0;
    RegQueryValueExW(inst, s->value_name, nullptr, &existing, nullptr, &ignored);

    const wchar_t *raw = s->opt[s->sel].raw;
    if (existing == REG_DWORD) {
        DWORD v = (DWORD)wcstoul(raw, nullptr, 10);
        r = RegSetValueExW(inst, s->value_name, 0, REG_DWORD,
                           (const BYTE *)&v, (DWORD)sizeof v);
    } else {
        r = RegSetValueExW(inst, s->value_name, 0, REG_SZ, (const BYTE *)raw,
                           (DWORD)((wcslen(raw) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(inst);
    return (unsigned long)r;
}

/* Disable then re-enable the device so the miniport re-reads its parameters.
 * Same thing Device Manager does when you press OK on the Advanced tab. */
unsigned long tune_restart_adapter(const tune_list *l)
{
    if (!l->have_instance) return ERROR_NOT_FOUND;

    /* The instance key path ends in the four digits SPDRP_DRIVER reports. */
    const wchar_t *slash = wcsrchr(l->instance_key, L'\\');
    if (!slash) return ERROR_NOT_FOUND;
    wchar_t want[64];
    _snwprintf(want, 64, L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\%s", slash + 1);
    want[63] = L'\0';

    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return GetLastError();

    unsigned long result = ERROR_NOT_FOUND;
    SP_DEVINFO_DATA dev = { .cbSize = sizeof(SP_DEVINFO_DATA) };

    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t drv[64] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DRIVER, nullptr,
                                               (BYTE *)drv, sizeof drv, nullptr))
            continue;
        if (_wcsicmp(drv, want) != 0) continue;

        SP_PROPCHANGE_PARAMS pc = {
            .ClassInstallHeader = { .cbSize = sizeof(SP_CLASSINSTALL_HEADER),
                                    .InstallFunction = DIF_PROPERTYCHANGE },
            .Scope = DICS_FLAG_GLOBAL,
        };

        pc.StateChange = DICS_DISABLE;
        if (!SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev)) {
            result = GetLastError();
            break;
        }

        pc.StateChange = DICS_ENABLE;
        if (!SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev)) {
            result = GetLastError(); /* left disabled: the caller must report this */
            break;
        }
        result = ERROR_SUCCESS;
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

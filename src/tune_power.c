/* tune_power.c -- the power settings that sit on the Wi-Fi path.
 *
 * A power scheme is mostly display, processor, GPU and battery policy, none of
 * which this app has any business showing.  So the settings are a fixed list;
 * only their names and choices come from Windows.  A setting this machine does
 * not have, or one that is a numeric range rather than a list, is skipped.
 */
#include "tune.h"

#include <powrprof.h>
#include <powersetting.h>
#include <stdio.h>

/* The hidden attribute is deliberately not consulted: Microsoft marks ASPM
 * hidden, and it is precisely the setting that adds wake-up latency to a
 * PCIe Wi-Fi card. */
static const struct { GUID sub, setting; const wchar_t *help; } WIFI_POWER[] = {
    /* Wireless Adapter Settings \ Power Saving Mode */
    { { 0x19cbb8fa, 0x5279, 0x450e, { 0x9f, 0xac, 0x8a, 0x3d, 0x5f, 0xed, 0xd0, 0xc1 } },
      { 0x12bbebe6, 0x58d6, 0x4636, { 0x95, 0xbb, 0x32, 0x17, 0xef, 0x86, 0x7c, 0x1a } },
      L"Power save hint to the driver; saving levels doze the radio between beacons, adding latency." },
    /* PCI Express \ Link State Power Management */
    { { 0x501a4d13, 0x42af, 0x4429, { 0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20 } },
      { 0xee12f906, 0xd277, 0x404b, { 0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5 } },
      L"Idle PCIe link state: Moderate permits L0s, Maximum permits L1, deeper and slower to wake." },
};

static bool friendly(const GUID *scheme, const GUID *sub, const GUID *set,
                     wchar_t *out, DWORD chars)
{
    DWORD bytes = chars * sizeof(wchar_t);
    if (PowerReadFriendlyName(nullptr, scheme, sub, set, (PUCHAR)out, &bytes) != ERROR_SUCCESS)
        return false;
    out[chars - 1] = L'\0';
    return out[0] != L'\0';
}

static int read_options(const GUID *sub, const GUID *set, tune_option *opt, int cap)
{
    int n = 0;
    for (ULONG i = 0; n < cap; ++i) {
        /* Only whether choice i exists matters -- its index is what gets
         * written, never its data.  Wireless power saving declares each value
         * as 16 bytes of REG_BINARY, so a DWORD-sized buffer would read as
         * "no choices" and silently drop the one setting that matters most. */
        ULONG type = 0;
        BYTE  value[64];
        DWORD bytes = sizeof value;
        DWORD e = PowerReadPossibleValue(nullptr, sub, set, &type, i, value, &bytes);
        if (e != ERROR_SUCCESS && e != ERROR_MORE_DATA)
            break;

        wchar_t label[TUNE_TEXT_MAX] = L"";
        DWORD lbytes = sizeof label;
        if (PowerReadPossibleFriendlyName(nullptr, sub, set, i,
                                          (PUCHAR)label, &lbytes) != ERROR_SUCCESS
            || label[0] == L'\0')
            _snwprintf(label, TUNE_TEXT_MAX, L"%lu", (unsigned long)i);
        label[TUNE_TEXT_MAX - 1] = L'\0';

        opt[n].index = i;
        wcsncpy(opt[n].label, label, TUNE_TEXT_MAX - 1);
        opt[n].label[TUNE_TEXT_MAX - 1] = L'\0';
        n++;
    }
    return n;
}

static void add_setting(tune_list *l, const GUID *scheme, const GUID *sub,
                        const GUID *set, const wchar_t *subname, const wchar_t *help,
                        const tune_option *opt, int n_opt, bool dc)
{
    if (l->n >= TUNE_MAX_SETTINGS) return;

    DWORD cur = 0;
    DWORD e = dc ? PowerReadDCValueIndex(nullptr, scheme, sub, set, &cur)
                 : PowerReadACValueIndex(nullptr, scheme, sub, set, &cur);
    if (e != ERROR_SUCCESS) return;

    tune_setting *s = &l->s[l->n];
    memset(s, 0, sizeof *s);
    s->src        = TUNE_POWER;
    s->sub        = *sub;
    s->setting    = *set;
    s->on_battery = dc;
    s->help       = help;
    s->n_opt      = n_opt;
    memcpy(s->opt, opt, (size_t)n_opt * sizeof *opt);

    wcsncpy(s->group, subname, TUNE_TEXT_MAX - 1);
    s->group[TUNE_TEXT_MAX - 1] = L'\0';

    if (!friendly(scheme, sub, set, s->name, TUNE_TEXT_MAX))
        wcscpy(s->name, L"(unnamed setting)");

    s->cur = -1;
    for (int k = 0; k < n_opt; ++k)
        if (opt[k].index == cur) { s->cur = k; break; }
    s->sel = s->cur;

    l->n++;
}

void tune_collect_power(tune_list *l)
{
    GUID *scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return;

    for (size_t i = 0; i < sizeof WIFI_POWER / sizeof WIFI_POWER[0]; ++i) {
        const GUID *sub = &WIFI_POWER[i].sub, *set = &WIFI_POWER[i].setting;

        wchar_t subname[TUNE_TEXT_MAX];
        if (!friendly(scheme, sub, nullptr, subname, TUNE_TEXT_MAX)) continue;

        tune_option opt[TUNE_MAX_OPTIONS];
        int n_opt = read_options(sub, set, opt, TUNE_MAX_OPTIONS);
        if (n_opt < 2) continue; /* absent here, or a range rather than a choice */

        /* Plugged in first, then on battery: the dialog pairs them by order. */
        const wchar_t *help = WIFI_POWER[i].help;
        add_setting(l, scheme, sub, set, subname, help, opt, n_opt, false);
        add_setting(l, scheme, sub, set, subname, help, opt, n_opt, true);
    }
    LocalFree(scheme);
}

unsigned long tune_write_power(const tune_setting *s)
{
    GUID *scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme)
        return ERROR_NOT_FOUND;

    DWORD index = (DWORD)s->opt[s->sel].index;
    DWORD e = s->on_battery
        ? PowerWriteDCValueIndex(nullptr, scheme, &s->sub, &s->setting, index)
        : PowerWriteACValueIndex(nullptr, scheme, &s->sub, &s->setting, index);

    LocalFree(scheme);
    return e;
}

/* Re-selecting the scheme is what makes the writes take effect, and it
 * broadcasts a power-setting change to every process -- so it happens once
 * after a batch of writes, not once per setting. */
unsigned long tune_power_commit(void)
{
    GUID *scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme)
        return ERROR_NOT_FOUND;
    DWORD e = PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);
    return e;
}

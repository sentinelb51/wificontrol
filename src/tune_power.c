/* tune_power.c -- the active power scheme, as declared by Windows.
 *
 * Subgroups, settings and their choices are all enumerated: no GUID is
 * hardcoded, so whatever the machine exposes (wireless adapter power saving
 * included) shows up on its own.  Settings that are a numeric range rather
 * than a list of choices are skipped -- they are not dropdowns.
 */
#include "tune.h"

#include <powrprof.h>
#include <powersetting.h>
#include <stdio.h>

#ifndef POWER_ATTRIBUTE_HIDE
#define POWER_ATTRIBUTE_HIDE 0x00000001
#endif

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
        ULONG type = 0;
        DWORD value = 0, bytes = sizeof value;
        if (PowerReadPossibleValue(nullptr, sub, set, &type, i,
                                   (PUCHAR)&value, &bytes) != ERROR_SUCCESS)
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
                        const GUID *set, const wchar_t *subname,
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
    s->n_opt      = n_opt;
    memcpy(s->opt, opt, (size_t)n_opt * sizeof *opt);

    _snwprintf(s->group, TUNE_TEXT_MAX, L"%s (%s)", subname,
               dc ? L"on battery" : L"plugged in");
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

    for (ULONG i = 0; l->n < TUNE_MAX_SETTINGS; ++i) {
        GUID sub;
        DWORD bytes = sizeof sub;
        if (PowerEnumerate(nullptr, scheme, nullptr, ACCESS_SUBGROUP, i,
                           (UCHAR *)&sub, &bytes) != ERROR_SUCCESS)
            break;

        wchar_t subname[TUNE_TEXT_MAX];
        if (!friendly(scheme, &sub, nullptr, subname, TUNE_TEXT_MAX)) continue;

        for (ULONG j = 0; l->n < TUNE_MAX_SETTINGS; ++j) {
            GUID set;
            DWORD sbytes = sizeof set;
            if (PowerEnumerate(nullptr, scheme, &sub, ACCESS_INDIVIDUAL_SETTING, j,
                               (UCHAR *)&set, &sbytes) != ERROR_SUCCESS)
                break;

            if (PowerReadSettingAttributes(&sub, &set) & POWER_ATTRIBUTE_HIDE) continue;

            tune_option opt[TUNE_MAX_OPTIONS];
            int n_opt = read_options(&sub, &set, opt, TUNE_MAX_OPTIONS);
            if (n_opt < 2) continue; /* a range, not a choice */

            add_setting(l, scheme, &sub, &set, subname, opt, n_opt, false);
            add_setting(l, scheme, &sub, &set, subname, opt, n_opt, true);
        }
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

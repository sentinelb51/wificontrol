/* diag.c -- the failure log, the words for each failure, and which switch is
 * unavailable when.  No Windows headers: see diag.h. */
#include "diag.h"

#include <stdio.h>
#include <string.h>

/* The Win32 codes this file reasons about, from winerror.h.  Named here rather
 * than included, so the policy stays host-buildable. */
enum {
    E_FILE_NOT_FOUND   = 2,
    E_PATH_NOT_FOUND   = 3,
    E_WRITE_PROTECT    = 19,
    E_GEN_FAILURE      = 31,
    E_SHARING          = 32,
    E_NOT_SUPPORTED    = 50,
    E_INVALID_PARAM    = 87,
    E_INVALID_NAME     = 123,
    E_TIMEOUT          = 258,  /* WAIT_TIMEOUT */
    E_NO_SUCH_DEVICE   = 433,
    E_SERVICE_DISABLED = 1058,
    E_SERVICE_STOPPED  = 1062,
    E_NOT_FOUND        = 1168,
    E_DEVICE_GONE      = 1167,
    E_RPC_UNAVAILABLE  = 1722,
    E_RPC_NOT_REG      = 1753,
};

/* Tick counter arithmetic, in the 32 bits GetTickCount wraps in: subtracting
 * and reading the difference as signed survives the wrap, where comparing two
 * values directly would read anything after it as ancient.  The width is
 * pinned to `unsigned` so a host with 64-bit longs behaves the same. */
static int elapsed(unsigned long now, unsigned long then)
{
    return (int)(unsigned)(now - then);
}

const char *wc_strerror(unsigned long code)
{
    switch (code) {
    case WC_E_VERIFY:     return "setting did not stick (driver rejected it)";
    case WC_E_BADDATA:    return "driver returned an unexpected value";
    case WC_E_NETSH:      return "netsh refused to change a network's cost";
    case WC_E_NO_JOURNAL: return "nothing could be recorded, so nothing was changed";
    default:              return (code >= WC_E_APP) ? "unknown internal error" : nullptr;
    }
}

/* ------------------------------------------------------------------- log */

void diag_clear(diag_log *l)
{
    l->n = 0;
    l->dropped = 0;
    /* The generation counter is deliberately kept: the window compares it
     * against what it last saw, and resetting it would look like news. */
}

static bool same(const diag_entry *e, diag_op op, diag_step step, const char *subject,
                 unsigned long code)
{
    return e->op == op && e->step == step && e->code == code &&
           strncmp(e->subject, subject, DIAG_SUBJ_MAX) == 0;
}

void diag_note(diag_log *l, diag_op op, diag_step step, const char *subject,
               unsigned long code, unsigned long now)
{
    if (code == WC_OK) return;
    if (!subject) subject = "";
    l->gen++;

    for (int i = 0; i < l->n; ++i) {
        if (!same(&l->e[i], op, step, subject, code)) continue;
        l->e[i].count++;
        l->e[i].last = now;
        return;
    }

    diag_entry *e;
    if (l->n < DIAG_MAX) {
        e = &l->e[l->n++];
    } else {
        /* Full: the one nothing has repeated for longest goes, since the
         * newest failure is the one the user is looking at. */
        int old = 0;
        for (int i = 1; i < l->n; ++i)
            if (elapsed(now, l->e[i].last) > elapsed(now, l->e[old].last)) old = i;
        e = &l->e[old];
        l->dropped++;
    }

    memset(e, 0, sizeof *e);
    e->op    = op;
    e->step  = step;
    e->code  = code;
    e->count = 1;
    e->first = e->last = now;
    snprintf(e->subject, sizeof e->subject, "%s", subject);
}

diag_sev diag_worst(const diag_log *l)
{
    diag_sev worst = DIAG_INFO;
    for (int i = 0; i < l->n; ++i) {
        diag_sev s = diag_sev_of(l->e[i].op, l->e[i].code);
        if (s > worst) worst = s;
    }
    return worst;
}

int diag_count(const diag_log *l, diag_sev at_least)
{
    int n = 0;
    for (int i = 0; i < l->n; ++i)
        if (diag_sev_of(l->e[i].op, l->e[i].code) >= at_least) n++;
    return n;
}

int diag_count_since(const diag_log *l, diag_sev at_least, unsigned long since)
{
    int n = 0;
    for (int i = 0; i < l->n; ++i)
        if (diag_sev_of(l->e[i].op, l->e[i].code) >= at_least &&
            elapsed(l->e[i].last, since) >= 0)
            n++;
    return n;
}

const diag_entry *diag_worst_entry(const diag_log *l)
{
    const diag_entry *worst = nullptr;
    diag_sev best = DIAG_INFO;
    for (int i = 0; i < l->n; ++i) {
        const diag_sev s = diag_sev_of(l->e[i].op, l->e[i].code);
        /* Ties go to the one seen most recently. */
        if (!worst || s > best || (s == best && elapsed(l->e[i].last, worst->last) >= 0)) {
            worst = &l->e[i];
            best  = s;
        }
    }
    return worst;
}

/* ----------------------------------------------------------------- words */

const char *diag_op_name(diag_op op)
{
    switch (op) {
    case DIAG_WLAN_SERVICE: return "The WLAN service";
    case DIAG_ADAPTERS:     return "The list of Wi-Fi adapters";
    case DIAG_BGSCAN:       return "No background scans";
    case DIAG_STREAMING:    return "Streaming mode";
    case DIAG_AUTOCONF:     return "Wi-Fi auto configuration";
    case DIAG_PROFILES:     return "The saved networks";
    case DIAG_METERED:      return "Metered";
    case DIAG_PERF_DRIVER:  return "Performance: a driver property";
    case DIAG_PERF_POWER:   return "Performance: a power plan setting";
    case DIAG_PERF_DEVICE:  return "Performance: the Wi-Fi Direct adapters";
    case DIAG_PERF_PLAN:    return "Performance: the active power plan";
    case DIAG_PERF_JOURNAL: return "Performance: the restore file";
    case DIAG_RESTART:      return "Restarting the adapter";
    default:                return "Something else";
    }
}

const char *diag_step_name(diag_step s)
{
    switch (s) {
    case DIAG_READ:    return "Reading it";
    case DIAG_WRITE:   return "Changing it";
    case DIAG_RESTORE: return "Putting it back";
    case DIAG_RECORD:  return "Recording what it was";
    default:           return "It";
    }
}

diag_sev diag_sev_of(diag_op op, unsigned long code)
{
    /* Losing a race with a disconnect is the documented contract, not a
     * failure: the next connection applies the setting. */
    if (code == WC_E_INVALID_STATE) return DIAG_INFO;

    switch (op) {
    /* Nothing else works without these. */
    case DIAG_WLAN_SERVICE:
    case DIAG_ADAPTERS:
        return DIAG_ERR;
    /* These can leave something the user has to put right by hand. */
    case DIAG_RESTART:
    case DIAG_PERF_JOURNAL:
        return DIAG_ERR;
    /* Auto config left off is the one setting that survives the process. */
    case DIAG_AUTOCONF:
        return DIAG_ERR;
    default:
        return DIAG_WARN;
    }
}

const char *diag_advice(diag_op op, diag_step step, unsigned long code)
{
    /* Whatever the code, these two say the same thing. */
    if (op == DIAG_RESTART)
        return "The adapter may be left disabled. Check it in Device Manager, "
               "under Network adapters, and enable it there if it is.";
    if (op == DIAG_PERF_JOURNAL)
        return "Without the record, values already changed cannot be put back on their "
               "own. wificontrol-restore.ini sits beside the settings file; if it is "
               "read-only, or in a folder this app cannot write to, fix that and switch "
               "Performance off and on again.";

    switch (code) {
    case WC_E_ACCESS_DENIED:
        switch (op) {
        case DIAG_PERF_DRIVER:
            return "The driver's registry key refused the write even as administrator, "
                   "which usually means a group policy or a permission change on the "
                   "network class key.";
        case DIAG_PERF_POWER:
        case DIAG_PERF_PLAN:
            return "The power plan refused the write, which usually means the plan is "
                   "managed by group policy.";
        case DIAG_PERF_DEVICE:
            return "Enabling or disabling the Wi-Fi Direct devices was refused, which "
                   "usually means a device installation policy is blocking it.";
        default:
            return "Windows refused this even with administrator rights: the Native Wifi "
                   "permissions or a group policy are blocking it, so elevating again "
                   "will not help.";
        }

    case WC_E_INVALID_STATE:
        return "The adapter was not connected any more by the time the value was "
               "written. It is applied again as soon as it reconnects.";

    case WC_E_VERIFY:
        return "The driver accepted the value and then reported a different one. Some "
               "drivers ignore this setting; the card will not honour it.";

    case WC_E_BADDATA:
        return "The driver answered with something other than the value it was asked "
               "for, so it was left alone.";

    case WC_E_NETSH:
        return "netsh refused the change. A profile deployed by group policy cannot "
               "have its cost changed, and neither can one that is no longer saved.";

    case WC_E_NO_JOURNAL:
        return "Nothing was changed, because there would have been no record to put it "
               "back from.";

    case E_TIMEOUT:
        return "netsh did not finish within three seconds, so the network's cost was "
               "left as it was. It is tried again on the next pass.";

    case E_NOT_SUPPORTED:
        return op == DIAG_METERED || op == DIAG_PROFILES
             ? "This version of Windows has no connection cost API, so a network cannot "
               "be marked as metered."
             : "Windows says it does not support this call on this machine.";

    case E_FILE_NOT_FOUND:
    case E_PATH_NOT_FOUND:
    case E_NOT_FOUND:
        return "It is not on this machine, so there was nothing to change. Most cards "
               "lack most of these properties; this is only worth reading as a failure "
               "if the card is supposed to have it.";

    case E_NO_SUCH_DEVICE:
    case E_DEVICE_GONE:
        return "The adapter is gone. Anything it still holds is put back the next time "
               "it appears.";

    case E_SERVICE_STOPPED:
    case E_SERVICE_DISABLED:
    case E_RPC_UNAVAILABLE:
    case E_RPC_NOT_REG:
        return "The WLAN AutoConfig service is not running. Start it from Services, or "
               "run \"net start wlansvc\" as administrator, then press Refresh.";

    case E_WRITE_PROTECT:
    case E_SHARING:
        return "The file could not be written: it is read-only, or open in another "
               "program.";

    case E_GEN_FAILURE:
        return "The driver reported a general failure. This is usually the card itself "
               "refusing; restarting the adapter clears it.";

    case E_INVALID_NAME:
        return "The name could not be passed on unchanged, so nothing was written. A "
               "network name containing a quotation mark cannot be given to netsh.";

    case E_INVALID_PARAM:
        return step == DIAG_WRITE || step == DIAG_RESTORE
             ? "The value was rejected as invalid, so the setting was left alone."
             : nullptr;

    default:
        return nullptr;
    }
}

/* ---------------------------------------------------------- capabilities */

const char *wc_cap_name(wc_cap c)
{
    switch (c) {
    case WC_CAP_WLAN:      return "WLAN service";
    case WC_CAP_BGSCAN:    return "Background scan setting";
    case WC_CAP_STREAMING: return "Streaming mode setting";
    case WC_CAP_AUTOCONF:  return "Auto configuration setting";
    case WC_CAP_COST:      return "Connection cost (metered)";
    case WC_CAP_POWER:     return "Wi-Fi power plan settings";
    case WC_CAP_DRIVER:    return "Driver properties";
    case WC_CAP_JOURNAL:   return "Performance restore file";
    default:               return "Unknown";
    }
}

bool wc_cap_ok(const wc_cap_state *cap, wc_cap c)
{
    /* Before the probe has run, assume it works: an unchecked capability must
     * never be the reason a switch is dead. */
    return !cap[c].checked || cap[c].ok;
}

static bool service_down(unsigned long e)
{
    return e == E_SERVICE_STOPPED || e == E_SERVICE_DISABLED ||
           e == E_RPC_UNAVAILABLE || e == E_RPC_NOT_REG;
}

const char *wc_cap_why(const wc_cap_state *cap, wc_cap c)
{
    if (wc_cap_ok(cap, c)) return nullptr;
    const unsigned long e = cap[c].err;

    switch (c) {
    case WC_CAP_WLAN:
        return service_down(e)
             ? "The WLAN AutoConfig service is not running. Start it, then press Refresh."
             : "The WLAN service could not be reached, so none of these settings can be "
               "written. Details has the error.";

    case WC_CAP_BGSCAN:
    case WC_CAP_STREAMING:
    case WC_CAP_AUTOCONF:
        return "Windows is refusing write access to this setting, so it cannot be "
               "changed even as administrator.";

    case WC_CAP_COST:
        return e == E_NOT_SUPPORTED
             ? "This version of Windows has no connection cost API, so networks cannot "
               "be marked as metered."
             : e == E_FILE_NOT_FOUND || e == E_PATH_NOT_FOUND
             ? "netsh.exe was not found, and it is what writes a network's cost."
             : "A network's cost cannot be read or written on this machine. Details has "
               "the error.";

    case WC_CAP_JOURNAL:
        return "The restore file cannot be written, and without it nothing could be put "
               "back. Details has the error.";

    case WC_CAP_POWER:
        return "The active power plan could not be read, so the power settings it holds "
               "cannot be changed.";

    case WC_CAP_DRIVER:
        return e == E_FILE_NOT_FOUND || e == E_PATH_NOT_FOUND
             ? "No Wi-Fi card's driver properties could be found, so there is nothing to "
               "set on the adapter itself."
             : "This card's driver properties cannot be written, which usually means a "
               "group policy on the network class key.";

    default:
        return "Not available on this machine.";
    }
}

/* The same thing in the few words a switch row has space for. */
static const char *brief(const wc_cap_state *cap, wc_cap c)
{
    if (wc_cap_ok(cap, c)) return nullptr;
    const unsigned long e = cap[c].err;

    switch (c) {
    case WC_CAP_WLAN:
        return service_down(e) ? "The WLAN service is not running"
                               : "The WLAN service cannot be reached";
    case WC_CAP_BGSCAN:
    case WC_CAP_STREAMING:
    case WC_CAP_AUTOCONF:
        return "Windows is refusing this setting";
    case WC_CAP_COST:
        return e == E_NOT_SUPPORTED                          ? "No connection cost API here"
             : e == E_FILE_NOT_FOUND || e == E_PATH_NOT_FOUND ? "netsh.exe is missing"
                                                              : "A network's cost cannot be set";
    case WC_CAP_JOURNAL: return "The restore file cannot be written";
    case WC_CAP_POWER:   return "The power plan cannot be read";
    case WC_CAP_DRIVER:  return "The driver properties are out of reach";
    default:             return "Not available on this machine";
    }
}

const char *wc_switch_blocked(const wc_cap_state *cap, wc_switch sw)
{
    /* Everything but Performance goes through the WLAN service. */
    if (sw != WC_SW_PERF) {
        const char *no_service = brief(cap, WC_CAP_WLAN);
        if (no_service) return no_service;
    }

    switch (sw) {
    case WC_SW_BGSCAN:    return brief(cap, WC_CAP_BGSCAN);
    case WC_SW_STREAMING: return brief(cap, WC_CAP_STREAMING);
    case WC_SW_NUCLEAR:   return brief(cap, WC_CAP_AUTOCONF);
    /* The profiles come from the WLAN service, the cost from elsewhere. */
    case WC_SW_METERED:   return brief(cap, WC_CAP_COST);
    case WC_SW_PERF:
        /* The journal first: holding values with no way back is the one thing
         * this switch must never do. */
        if (!wc_cap_ok(cap, WC_CAP_JOURNAL)) return brief(cap, WC_CAP_JOURNAL);
        /* Either half is worth offering; neither is not. */
        if (!wc_cap_ok(cap, WC_CAP_POWER) && !wc_cap_ok(cap, WC_CAP_DRIVER))
            return "Neither power nor driver settings are reachable";
        return nullptr;
    default:
        return nullptr;
    }
}

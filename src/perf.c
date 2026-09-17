/* perf.c -- policy for the Performance switch: which settings, and when a
 * recorded value goes back.  See perf.h. */
#include "perf.h"

#include <stdio.h>
#include <string.h>

typedef struct { perf_kind kind; const char *name, *target; } perf_item;

#define WIFI_POWER "{19CBB8FA-5279-450E-9FAC-8A3D5FEDD0C1}\\{12BBEBE6-58D6-4636-95BB-3217EF867C1A}"
#define PCIE_ASPM  "{501A4D13-42AF-4429-9FD1-A8218C268E20}\\{EE12F906-D277-404B-B6DA-E5FA1A576DF5}"

/* Each target is the value that saves no power.  Power indexes are Windows'
 * own and the same everywhere.  Driver values are the raw strings Intel's INF
 * declares, and a card that lacks the property, or that choice, is skipped.
 * Packet coalescing is deliberately absent: it only delays broadcast and
 * multicast frames.  The Wi-Fi Direct adapters go too: they share the radio,
 * and looking for peers takes it off the AP's channel. */
static const perf_item PRESET[] = {
    /* Wireless Adapter Settings \ Power Saving Mode: Maximum Performance */
    { PERF_POWER,  WIFI_POWER "\\ac", "0" },
    { PERF_POWER,  WIFI_POWER "\\dc", "0" },
    /* PCI Express \ Link State Power Management: Off, for every PCIe device */
    { PERF_POWER,  PCIE_ASPM "\\ac", "0" },
    { PERF_POWER,  PCIE_ASPM "\\dc", "0" },
    { PERF_DRIVER, "MIMOPowerSaveMode", "3" },   /* No SMPS: every receive chain stays on */
    { PERF_DRIVER, "uAPSDSupport",      "0" },   /* no WMM Power Save */
    { PERF_DRIVER, "IbssTxPower",       "100" }, /* highest transmit power */
    { PERF_DRIVER, "*SelectiveSuspend", "0" },   /* no suspending an idle adapter */
    { PERF_DEVICE, "WiFiDirect",        "0" },   /* every Wi-Fi Direct adapter disabled */
};
enum { PRESET_N = sizeof PRESET / sizeof PRESET[0] };

typedef struct { perf_entry e[PERF_MAX_HELD]; int n; } journal;

static void load(const perf_backend *be, journal *j)
{
    j->n = be->held(be->ctx, j->e, PERF_MAX_HELD);
    if (j->n < 0) j->n = 0;
}

static int count(const perf_backend *be)
{
    journal j;
    load(be, &j);
    return j.n;
}

static bool same(const wc_guid *a, const wc_guid *b)
{
    return memcmp(a->b, b->b, sizeof a->b) == 0;
}

static const perf_item *preset_of(perf_kind k, const char *name)
{
    for (int i = 0; i < PRESET_N; ++i)
        if (PRESET[i].kind == k && strcmp(PRESET[i].name, name) == 0) return &PRESET[i];
    return nullptr;
}

static bool recorded(const journal *j, perf_kind k, const wc_guid *owner, const char *name)
{
    for (int i = 0; i < j->n; ++i)
        if (j->e[i].kind == k && same(&j->e[i].owner, owner) && strcmp(j->e[i].name, name) == 0)
            return true;
    return false;
}

static void fail(perf_result *r, unsigned long e)
{
    if (e && !r->err) r->err = e;
}

/* Move one setting to its target.  True when something was written. */
static bool hold(const perf_backend *be, const journal *j, const perf_item *it,
                 const wc_guid *owner, perf_result *r)
{
    char live[PERF_VALUE_MAX] = "";
    int accepts = 0;
    unsigned long e = be->read(be->ctx, it->kind, owner, it->name, it->target,
                               live, sizeof live, &accepts);
    if (e == PERF_ABSENT || (e == WC_OK && !accepts)) return false;
    if (e) { fail(r, e); return false; }
    if (strcmp(live, it->target) == 0) return false;

    /* Recorded before it is written, so a crash in between still leaves the
     * way back.  A record that already exists holds the original; the value
     * found now is a later change made while the switch was on. */
    perf_entry rec = { .kind = it->kind, .owner = *owner };
    snprintf(rec.name, sizeof rec.name, "%s", it->name);
    snprintf(rec.value, sizeof rec.value, "%s", live);
    const bool fresh = !recorded(j, it->kind, owner, it->name);
    if (fresh && (e = be->keep(be->ctx, &rec))) { fail(r, e); return false; }

    if ((e = be->write(be->ctx, it->kind, owner, it->name, it->target))) {
        fail(r, e);
        if (fresh) be->forget(be->ctx, &rec);
        return false;
    }
    r->changed++;
    return true;
}

/* Put one recorded setting back and drop its record.  True when something was
 * written.  A read failure keeps the record for the next attempt. */
static bool put_back(const perf_backend *be, const perf_entry *rec, perf_result *r)
{
    const perf_item *it = preset_of(rec->kind, rec->name);
    char live[PERF_VALUE_MAX] = "";
    int accepts = 0;
    unsigned long e = it ? be->read(be->ctx, rec->kind, &rec->owner, rec->name, it->target,
                                    live, sizeof live, &accepts)
                         : PERF_ABSENT;
    if (e && e != PERF_ABSENT) { fail(r, e); return false; }

    /* Only a value still at ours goes back.  Anything else was changed since,
     * by someone else, and theirs stands. */
    bool wrote = false;
    if (!e && strcmp(live, it->target) == 0) {
        if ((e = be->write(be->ctx, rec->kind, &rec->owner, rec->name, rec->value))) {
            fail(r, e);
            return false;
        }
        r->changed++;
        wrote = true;
    }
    fail(r, be->forget(be->ctx, rec));
    return wrote;
}

perf_result perf_sync_power(const perf_backend *be, bool want)
{
    perf_result r = { 0 };
    journal j;
    load(be, &j);

    wc_guid active;
    unsigned long e = be->scheme(be->ctx, &active);
    const bool known = (e == WC_OK);
    if (want && !known) fail(&r, e);

    bool dirty = false; /* the active scheme was written */
    if (want && known)
        for (int i = 0; i < PRESET_N; ++i)
            if (PRESET[i].kind == PERF_POWER) dirty |= hold(be, &j, &PRESET[i], &active, &r);

    /* A record against another scheme means the plan was switched while
     * holding: that scheme gets its own values back. */
    for (int k = 0; k < j.n; ++k) {
        const perf_entry *rec = &j.e[k];
        if (rec->kind != PERF_POWER) continue;
        const bool on_active = known && same(&rec->owner, &active);
        if (want && on_active) continue;
        if (put_back(be, rec, &r) && (on_active || !known)) dirty = true;
    }

    if (dirty) fail(&r, be->commit(be->ctx));
    r.held = count(be);
    return r;
}

perf_result perf_sync_adapters(const perf_backend *be, const perf_adapter *ad, int n,
                               bool restart)
{
    perf_result r = { 0 };
    journal j;
    load(be, &j);
    if (n > WC_MAX_ADAPTERS) n = WC_MAX_ADAPTERS;
    bool changed[WC_MAX_ADAPTERS] = { false }; /* a property the driver has yet to read */

    for (int i = 0; i < n; ++i) {
        if (!ad[i].want) continue;
        for (int p = 0; p < PRESET_N; ++p) {
            if (PRESET[p].kind == PERF_POWER) continue;
            const bool wrote = hold(be, &j, &PRESET[p], &ad[i].guid, &r);
            if (PRESET[p].kind == PERF_DRIVER) changed[i] |= wrote;
        }
    }

    /* Adapters missing from the list are put back too: their properties stay
     * in the registry while they are unplugged. */
    for (int k = 0; k < j.n; ++k) {
        const perf_entry *rec = &j.e[k];
        if (rec->kind == PERF_POWER) continue;
        int i = n - 1;
        while (i >= 0 && !same(&ad[i].guid, &rec->owner)) i--;
        if (i >= 0 && ad[i].want) continue;
        if (put_back(be, rec, &r) && i >= 0 && rec->kind == PERF_DRIVER) changed[i] = true;
    }

    for (int i = 0; i < n; ++i) {
        if (!changed[i] || !ad[i].present || !restart) continue;
        unsigned long e = be->restart(be->ctx, &ad[i].guid);
        if (e) fail(&r, e);
        else   r.restarted++;
    }
    r.held = count(be);
    return r;
}

perf_result perf_sync(const perf_backend *be, bool power, const perf_adapter *ad, int n,
                      bool restart)
{
    perf_result p = perf_sync_power(be, power);
    perf_result d = perf_sync_adapters(be, ad, n, restart);
    d.changed += p.changed;
    d.restarted += p.restarted;
    if (p.err) d.err = p.err;
    return d;
}

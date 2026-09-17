/* wlan_core.c -- policy layer.  No Windows headers: see wlan.h. */
#include "wlan.h"
#include <string.h>

static bool guid_eq(const wc_guid *a, const wc_guid *b)
{
    return memcmp(a->b, b->b, sizeof a->b) == 0;
}

int wc_find(const wc_state *s, const wc_guid *g)
{
    for (int i = 0; i < s->n; ++i)
        if (guid_eq(&s->ad[i].guid, g)) return i;
    return -1;
}

void wc_init(wc_state *s, const wc_backend *be)
{
    memset(s, 0, sizeof *s);
    s->be = *be;
    s->bgscan_off = s->streaming_on = true;
    /* Assume we may write until the probe says otherwise; a failed probe must
     * never stop us from trying, because the probe is only advisory. */
    for (int o = 0; o < WC_OPT_COUNT; ++o) s->can_write[o] = true;
}

void wc_set_note(wc_state *s, wc_note_fn note, void *ctx)
{
    s->note     = note;
    s->note_ctx = ctx;
}

/* Report a failure, with the setting and the adapter it happened on.  WC_OK is
 * passed through so callers can hand over whatever the backend returned. */
static void note(const wc_state *s, diag_op op, diag_step step, const char *subject,
                 unsigned long e)
{
    if (e != WC_OK && s->note) s->note(s->note_ctx, op, step, subject, e);
}

static diag_op op_of(wc_opt o)
{
    return o == WC_OPT_STREAMING ? DIAG_STREAMING
         : o == WC_OPT_BGSCAN    ? DIAG_BGSCAN
                                 : DIAG_AUTOCONF;
}

void wc_probe_access(wc_state *s)
{
    if (!s->be.granted_write) return;
    for (int o = 0; o < WC_OPT_COUNT; ++o) {
        int w = 1;
        if (s->be.granted_write(s->be.ctx, (wc_opt)o, &w) == WC_OK) s->can_write[o] = (w != 0);
    }
}

static bool voted(const wc_adapter *a)
{
    return a->voted[WC_OPT_STREAMING] || a->voted[WC_OPT_BGSCAN];
}

/* Reuse the slot of an adapter that is gone, so a machine that cycles through
 * USB dongles cannot wedge the table. */
static int alloc_slot(wc_state *s)
{
    if (s->n < WC_MAX_ADAPTERS) return s->n++;
    for (int i = 0; i < s->n; ++i)
        if (!s->ad[i].present && !voted(&s->ad[i])) return i;
    return -1;
}

unsigned long wc_refresh(wc_state *s)
{
    wc_ifinfo tmp[WC_MAX_ADAPTERS];
    int n = 0;

    /* No backend yet: the app brings the core up before it has a handle, so
     * that it can hold the saved switches until one opens. */
    if (!s->be.enum_ifaces) return WC_E_BADDATA;

    unsigned long e = s->be.enum_ifaces(s->be.ctx, tmp, WC_MAX_ADAPTERS, &n);
    if (e != WC_OK) {
        s->enum_err = e;
        note(s, DIAG_ADAPTERS, DIAG_READ, nullptr, e);
        for (int i = 0; i < s->n; ++i) s->ad[i].present = false;
        return e;
    }
    s->enum_err = WC_OK;

    for (int i = 0; i < s->n; ++i) s->ad[i].present = false;

    for (int i = 0; i < n; ++i) {
        int j = wc_find(s, &tmp[i].guid);
        if (j < 0) {
            j = alloc_slot(s);
            if (j < 0) continue;
            memset(&s->ad[j], 0, sizeof s->ad[j]);
            s->ad[j].guid      = tmp[i].guid;
            s->ad[j].managed   = true; /* new adapters are managed by default */
            s->ad[j].streaming = WC_VAL_UNKNOWN;
            s->ad[j].bgscan    = WC_VAL_UNKNOWN;
            s->ad[j].autoconf  = WC_VAL_UNKNOWN;
            s->ad[j].metered   = -1;   /* its profiles have never been checked */
        }
        memcpy(s->ad[j].name, tmp[i].name, sizeof s->ad[j].name);
        s->ad[j].name[WC_NAME_MAX - 1] = '\0';
        s->ad[j].state   = tmp[i].state;
        s->ad[j].present = true;
    }
    return WC_OK;
}

/* Read, compare, write only on mismatch, then verify.  Both the comparison and
 * the verification normalise to 0/1: the API documents any nonzero as TRUE, so
 * a driver answering 0xFFFFFFFF must not read as a failure.
 *
 * `want` asks for streaming on or background scan off.  Otherwise the default
 * is written, but only to withdraw a request of ours: a value we never asked
 * for is another client's, and it may well read back unchanged. */
static void apply_opcode(wc_state *s, wc_adapter *a, wc_opt o, bool want, wc_val *slot)
{
    const int desired = (o == WC_OPT_STREAMING) == want;
    const diag_op   op   = op_of(o);
    const diag_step step = want ? DIAG_WRITE : DIAG_RESTORE;

    int cur = 0;
    unsigned long e = s->be.query_bool(s->be.ctx, &a->guid, o, &cur);
    if (e != WC_OK) {
        *slot = WC_VAL_UNKNOWN;
        note(s, op, DIAG_READ, a->name, e);
        if (!a->last_err) a->last_err = e;
        return;
    }

    cur   = !!cur;
    *slot = cur ? WC_VAL_ON : WC_VAL_OFF;
    if (!want && !a->voted[o]) return;
    if (cur == desired) { a->voted[o] = want; return; }

    e = s->be.set_bool(s->be.ctx, &a->guid, o, desired);
    if (e != WC_OK) {
        note(s, op, step, a->name, e);
        if (!a->last_err) a->last_err = e;
        return;
    }
    a->voted[o] = want;

    e = s->be.query_bool(s->be.ctx, &a->guid, o, &cur);
    if (e != WC_OK) {
        *slot = WC_VAL_UNKNOWN;
        note(s, op, DIAG_READ, a->name, e);
        if (!a->last_err) a->last_err = e;
        return;
    }

    cur   = !!cur;
    *slot = cur ? WC_VAL_ON : WC_VAL_OFF;
    if (want && cur != desired) {
        note(s, op, step, a->name, WC_E_VERIFY);
        if (!a->last_err) a->last_err = WC_E_VERIFY;
    }
}

/* Auto config is forced back on unless every condition for keeping it off
 * holds right now.  Written as a positive check with no memory of what we did
 * last time: whatever the reason it is off -- the user disarmed, the adapter
 * dropped, a previous run of this program was killed -- the next pass turns it
 * back on.  That is the whole recovery mechanism, and it needs no journal.
 *
 * Unlike the other two opcodes this one is settable while disconnected, which
 * is what makes recovery on a dropped link possible at all. */
static void apply_autoconf(wc_state *s, wc_adapter *a)
{
    const bool keep_off = s->nuclear && a->managed && a->state == WC_IF_CONNECTED;

    int cur = 0;
    unsigned long e = s->be.query_bool(s->be.ctx, &a->guid, WC_OPT_AUTOCONF, &cur);
    if (e != WC_OK) {
        a->autoconf = WC_VAL_UNKNOWN;
        note(s, DIAG_AUTOCONF, DIAG_READ, a->name, e);
        if (!a->last_err) a->last_err = e;
        return;
    }

    cur = !!cur;
    a->autoconf = cur ? WC_VAL_ON : WC_VAL_OFF;

    const int desired = keep_off ? 0 : 1;
    if (cur == desired) return;

    e = s->be.set_bool(s->be.ctx, &a->guid, WC_OPT_AUTOCONF, desired);
    if (e != WC_OK) {
        /* Failing to give scanning back is the worst of the three: it is the
         * one setting that outlives the process. */
        note(s, DIAG_AUTOCONF, desired ? DIAG_RESTORE : DIAG_WRITE, a->name, e);
        if (!a->last_err) a->last_err = e;
        return;
    }

    a->autoconf = desired ? WC_VAL_ON : WC_VAL_OFF;
    /* Count only repairs, so the UI can say it cleaned up after something. */
    if (desired) s->recovered++;
}

/* The same positive check, per saved profile: a profile is ours exactly when
 * it carries the cost we write, and it should be ours exactly while metering is
 * in force.  A cost can be written connected or not, so the network is metered
 * before the first packet of its next connection. */
static void apply_metered(wc_state *s, wc_adapter *a)
{
    const bool want = s->metered && a->managed;

    /* A clean pass found nothing of ours and nothing is wanted: skip the
     * per-profile reads until something changes that. */
    if ((!want && a->metered == 0) || !s->be.list_profiles) return;

    /* Around 50 KB, so not a stack object.  The core runs on one thread. */
    static wc_profile prof[WC_MAX_PROFILES];
    int n = 0;
    unsigned long e = s->be.list_profiles(s->be.ctx, &a->guid, prof, WC_MAX_PROFILES, &n);
    if (e != WC_OK) {
        note(s, DIAG_PROFILES, DIAG_READ, a->name, e);
        if (want && !a->last_err) a->last_err = e;
        return;
    }

    int ours = 0;
    bool clean = true;
    for (int k = 0; k < n; ++k) {
        /* A failed read is an error only for someone who asked for metering.
         * Without the WCM API (Windows 7) every read fails, and that must not
         * paint an error under a switch nobody turned on. */
        unsigned long cost = 0;
        int src = 0;
        e = s->be.query_cost(s->be.ctx, &a->guid, prof[k].name, &cost, &src);
        if (e != WC_OK) {
            note(s, DIAG_METERED, DIAG_READ, prof[k].name, e);
            clean = false;
            if (want && !a->last_err) a->last_err = e;
            continue;
        }

        bool is_ours = (cost & WC_COST_VARIABLE) && src == WC_COST_SRC_USER;
        if (is_ours != want) {
            e = s->be.set_metered(s->be.ctx, &a->guid, prof[k].name, want);
            if (e == WC_OK) is_ours = want;
            else {
                note(s, DIAG_METERED, want ? DIAG_WRITE : DIAG_RESTORE, prof[k].name, e);
                clean = false;
                if (!a->last_err) a->last_err = e;
            }
        }
        if (is_ours) ours++;
    }
    a->profiles = n;
    /* After a failure with metering off, "unknown" makes the next pass look
     * again instead of skipping a profile that may still be ours. */
    a->metered = (clean || want) ? ours : -1;
}

void wc_apply_one(wc_state *s, int i)
{
    if (i < 0 || i >= s->n) return;
    wc_adapter *a = &s->ad[i];
    if (!a->present) return;

    const bool stream = s->streaming_on && a->managed;
    const bool quiet  = s->bgscan_off && a->managed;
    const bool want   = stream || quiet;

    /* Cleared once, here, so that an auto config failure below survives every
     * early return.  apply_opcode keeps the first error rather than the last. */
    a->last_err = WC_OK;

    /* Run before every early return: auto config and a metered cost must be
     * repaired even for an adapter that is disconnected, unmanaged, or
     * switched off. */
    apply_autoconf(s, a);
    apply_metered(s, a);

    /* Nothing to do and nothing to undo. */
    if (!want && !voted(a)) {
        a->pending   = false;
        a->streaming = a->bgscan = WC_VAL_UNKNOWN;
        return;
    }

    /* Both opcodes are settable only while connected; the OS resets them on
     * disconnect anyway.  This is the documented contract, not a failure. */
    if (a->state != WC_IF_CONNECTED) {
        a->pending   = want;
        /* the disconnect already dropped our requests */
        a->voted[WC_OPT_STREAMING] = a->voted[WC_OPT_BGSCAN] = false;
        a->streaming = a->bgscan = WC_VAL_UNKNOWN;
        return;
    }

    apply_opcode(s, a, WC_OPT_STREAMING, stream, &a->streaming);
    apply_opcode(s, a, WC_OPT_BGSCAN,    quiet,  &a->bgscan);

    /* Lost the race with a disconnect between enumerate and set. */
    if (a->last_err == WC_E_INVALID_STATE) {
        a->pending  = want;
        a->last_err = WC_OK;
        return;
    }

    a->pending = false;
    if (a->last_err) a->consec_fail++;
    else             a->consec_fail = 0;
}

void wc_apply_all(wc_state *s)
{
    s->recovered = 0;
    for (int i = 0; i < s->n; ++i) wc_apply_one(s, i);
}

unsigned long wc_poll(wc_state *s)
{
    unsigned long e = wc_refresh(s);
    if (e != WC_OK) return e;
    wc_apply_all(s);
    return WC_OK;
}

void wc_set_bgscan_off(wc_state *s, bool on)
{
    s->bgscan_off = on;
    wc_apply_all(s);
}

void wc_set_streaming_on(wc_state *s, bool on)
{
    s->streaming_on = on;
    wc_apply_all(s);
}

void wc_set_nuclear(wc_state *s, bool on)
{
    s->nuclear = on;
    wc_apply_all(s);
}

void wc_set_metered(wc_state *s, bool on)
{
    s->metered = on;
    wc_apply_all(s);
}

void wc_set_managed(wc_state *s, int i, bool on)
{
    if (i < 0 || i >= s->n) return;
    s->ad[i].managed = on;
    wc_apply_one(s, i);
}

bool wc_write_denied(const wc_state *s)
{
    for (int o = 0; o < WC_OPT_COUNT; ++o)
        if (!s->can_write[o]) return true;
    for (int i = 0; i < s->n; ++i)
        if (s->ad[i].present && s->ad[i].last_err == WC_E_ACCESS_DENIED) return true;
    return false;
}

const char *wc_state_name(wc_ifstate st)
{
    switch (st) {
    case WC_IF_NOT_READY:      return "Not ready";
    case WC_IF_CONNECTED:      return "Connected";
    case WC_IF_AD_HOC:         return "Ad hoc";
    case WC_IF_DISCONNECTING:  return "Disconnecting";
    case WC_IF_DISCONNECTED:   return "Disconnected";
    case WC_IF_ASSOCIATING:    return "Associating";
    case WC_IF_DISCOVERING:    return "Discovering";
    case WC_IF_AUTHENTICATING: return "Authenticating";
    default:                   return "Unknown";
    }
}

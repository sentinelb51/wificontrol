/* wlan_core.c -- policy layer.  No Windows headers: see wlan.h. */
#include "wlan.h"
#include <string.h>

static int guid_eq(const wc_guid *a, const wc_guid *b)
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
    s->enabled = 1;
    /* Assume we may write until the probe says otherwise; a failed probe must
     * never stop us from trying, because the probe is only advisory. */
    for (int o = 0; o < WC_OPT_COUNT; ++o) s->can_write[o] = 1;
}

void wc_probe_access(wc_state *s)
{
    if (!s->be.granted_write) return;
    for (int o = 0; o < WC_OPT_COUNT; ++o) {
        int w = 1;
        if (s->be.granted_write(s->be.ctx, (wc_opt)o, &w) == WC_OK) s->can_write[o] = w;
    }
}

/* Reuse the slot of an adapter that is gone, so a machine that cycles through
 * USB dongles cannot wedge the table. */
static int alloc_slot(wc_state *s)
{
    if (s->n < WC_MAX_ADAPTERS) return s->n++;
    for (int i = 0; i < s->n; ++i)
        if (!s->ad[i].present && !s->ad[i].touched) return i;
    return -1;
}

unsigned long wc_refresh(wc_state *s)
{
    wc_ifinfo tmp[WC_MAX_ADAPTERS];
    int n = 0;

    unsigned long e = s->be.enum_ifaces(s->be.ctx, tmp, WC_MAX_ADAPTERS, &n);
    if (e != WC_OK) {
        s->enum_err = e;
        for (int i = 0; i < s->n; ++i) s->ad[i].present = 0;
        return e;
    }
    s->enum_err = WC_OK;

    for (int i = 0; i < s->n; ++i) s->ad[i].present = 0;

    for (int i = 0; i < n; ++i) {
        int j = wc_find(s, &tmp[i].guid);
        if (j < 0) {
            j = alloc_slot(s);
            if (j < 0) continue;
            memset(&s->ad[j], 0, sizeof s->ad[j]);
            s->ad[j].guid      = tmp[i].guid;
            s->ad[j].managed   = 1; /* new adapters are managed by default */
            s->ad[j].streaming = WC_VAL_UNKNOWN;
            s->ad[j].bgscan    = WC_VAL_UNKNOWN;
        }
        memcpy(s->ad[j].name, tmp[i].name, sizeof s->ad[j].name);
        s->ad[j].name[WC_NAME_MAX - 1] = '\0';
        s->ad[j].state   = tmp[i].state;
        s->ad[j].present = 1;
    }
    return WC_OK;
}

/* Read, compare, write only on mismatch, then verify.  Both the comparison and
 * the verification normalise to 0/1: the API documents any nonzero as TRUE, so
 * a driver answering 0xFFFFFFFF must not read as a failure. */
static void apply_opcode(wc_state *s, wc_adapter *a, wc_opt o, int desired, wc_val *slot)
{
    int cur = 0;
    unsigned long e = s->be.query_bool(s->be.ctx, &a->guid, o, &cur);
    if (e != WC_OK) { *slot = WC_VAL_UNKNOWN; if (!a->last_err) a->last_err = e; return; }

    cur   = !!cur;
    *slot = cur ? WC_VAL_ON : WC_VAL_OFF;
    if (cur == desired) return;

    e = s->be.set_bool(s->be.ctx, &a->guid, o, desired);
    if (e != WC_OK) { if (!a->last_err) a->last_err = e; return; }

    e = s->be.query_bool(s->be.ctx, &a->guid, o, &cur);
    if (e != WC_OK) { *slot = WC_VAL_UNKNOWN; if (!a->last_err) a->last_err = e; return; }

    cur   = !!cur;
    *slot = cur ? WC_VAL_ON : WC_VAL_OFF;
    if (cur != desired && !a->last_err) a->last_err = WC_E_VERIFY;
}

void wc_apply_one(wc_state *s, int i)
{
    if (i < 0 || i >= s->n) return;
    wc_adapter *a = &s->ad[i];
    if (!a->present) return;

    const int want = s->enabled && a->managed;

    /* Nothing to do and nothing to undo. */
    if (!want && !a->touched) {
        a->pending   = 0;
        a->last_err  = WC_OK;
        a->streaming = a->bgscan = WC_VAL_UNKNOWN;
        return;
    }

    /* Both opcodes are settable only while connected; the OS resets them on
     * disconnect anyway.  This is the documented contract, not a failure. */
    if (a->state != WC_IF_CONNECTED) {
        a->pending   = want;
        a->touched   = 0; /* the disconnect already dropped our request */
        a->streaming = a->bgscan = WC_VAL_UNKNOWN;
        a->last_err  = WC_OK;
        return;
    }

    a->last_err = WC_OK;
    apply_opcode(s, a, WC_OPT_STREAMING, want ? 1 : 0, &a->streaming);
    apply_opcode(s, a, WC_OPT_BGSCAN,    want ? 0 : 1, &a->bgscan);

    /* Lost the race with a disconnect between enumerate and set. */
    if (a->last_err == WC_E_INVALID_STATE) {
        a->pending  = want;
        a->last_err = WC_OK;
        return;
    }

    a->pending = 0;
    a->touched = want;
    if (a->last_err) a->consec_fail++;
    else             a->consec_fail = 0;
}

void wc_apply_all(wc_state *s)
{
    for (int i = 0; i < s->n; ++i) wc_apply_one(s, i);
}

unsigned long wc_poll(wc_state *s)
{
    unsigned long e = wc_refresh(s);
    if (e != WC_OK) return e;
    wc_apply_all(s);
    return WC_OK;
}

void wc_set_enabled(wc_state *s, int on)
{
    s->enabled = on ? 1 : 0;
    wc_apply_all(s);
}

void wc_set_managed(wc_state *s, int i, int on)
{
    if (i < 0 || i >= s->n) return;
    s->ad[i].managed = on ? 1 : 0;
    wc_apply_one(s, i);
}

int wc_needs_elevation(const wc_state *s)
{
    for (int o = 0; o < WC_OPT_COUNT; ++o)
        if (!s->can_write[o]) return 1;
    for (int i = 0; i < s->n; ++i)
        if (s->ad[i].present && s->ad[i].last_err == WC_E_ACCESS_DENIED) return 1;
    return 0;
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

const char *wc_strerror(unsigned long code)
{
    switch (code) {
    case WC_E_VERIFY:  return "setting did not stick (driver rejected it)";
    case WC_E_BADDATA: return "driver returned an unexpected value";
    default:           return (code >= WC_E_APP) ? "unknown internal error" : (const char *)0;
    }
}

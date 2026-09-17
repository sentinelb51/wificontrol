/* Unit tests for the policy core.  Builds and runs on any host; each case is
 * a regression test for a specific defect in the original WLANOptimizer. */
#include "../src/wlan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define COST_UNRESTRICTED 0x1UL
#define COST_FIXED        0x2UL

/* ------------------------------------------------------------------ fake */

typedef struct {
    wc_ifinfo     ifs[4];
    int           n;
    long          val[4][WC_OPT_COUNT]; /* raw driver values */
    long          raw_true;             /* what this "driver" calls TRUE */
    unsigned long q_err, s_err;
    bool          stick;                /* false: set succeeds but nothing changes */
    bool          other[4][WC_OPT_COUNT]; /* another client asks for the non-default */
    int           nq, ns;               /* call counts */
    int           nset[4];              /* writes per adapter */
    bool          can_write[WC_OPT_COUNT];

    int           nprof[4];             /* saved profiles per adapter */
    unsigned long cost[4][8];           /* each profile's cost and its source */
    int           csrc[4][8];
    unsigned long c_err;                /* fails every cost read */
    unsigned long m_err;                /* fails every cost write */
    unsigned long e_err;                /* fails the enumeration */
    int           ncq, ncs;             /* cost reads and writes */
    diag_log      log;                  /* what the core reported, in order */
} fake;

static int fake_index(const fake *f, const wc_guid *g)
{
    for (int i = 0; i < f->n; ++i)
        if (memcmp(f->ifs[i].guid.b, g->b, 16) == 0) return i;
    return -1;
}

static unsigned long f_enum(void *ctx, wc_ifinfo *out, int cap, int *count)
{
    fake *f = ctx;
    if (f->e_err) { *count = 0; return f->e_err; }
    int n = f->n < cap ? f->n : cap;
    memcpy(out, f->ifs, (size_t)n * sizeof *out);
    *count = n;
    return WC_OK;
}
static unsigned long f_query(void *ctx, const wc_guid *g, wc_opt o, int *v)
{
    fake *f = ctx;
    f->nq++;
    if (f->q_err) return f->q_err;
    int i = fake_index(f, g);
    if (i < 0) return WC_E_BADDATA;
    *v = (int)f->val[i][o];
    return WC_OK;
}
static unsigned long f_set(void *ctx, const wc_guid *g, wc_opt o, int v)
{
    fake *f = ctx;
    f->ns++;
    if (f->s_err) return f->s_err;
    int i = fake_index(f, g);
    if (i < 0) return WC_E_BADDATA;
    f->nset[i]++;
    /* Streaming defaults to off, background scan to on. */
    const int dflt = (o != WC_OPT_STREAMING);
    if (f->other[i][o] && !!v == dflt) return WC_OK;   /* outvoted */
    if (f->stick) f->val[i][o] = v ? f->raw_true : 0;
    return WC_OK;
}
static unsigned long f_granted(void *ctx, wc_opt o, int *w)
{
    fake *f = ctx;
    *w = f->can_write[o] ? 1 : 0;
    return WC_OK;
}
/* Profiles are named by their index. */
static unsigned long f_profiles(void *ctx, const wc_guid *g, wc_profile *out, int cap, int *count)
{
    fake *f = ctx;
    int i = fake_index(f, g);
    if (i < 0) return WC_E_BADDATA;
    int n = f->nprof[i] < cap ? f->nprof[i] : cap;
    for (int k = 0; k < n; ++k) snprintf(out[k].name, WC_PROFILE_MAX, "%d", k);
    *count = n;
    return WC_OK;
}
static unsigned long f_qcost(void *ctx, const wc_guid *g, const char *p, unsigned long *cost, int *src)
{
    fake *f = ctx;
    f->ncq++;
    if (f->c_err) return f->c_err;
    int i = fake_index(f, g);
    if (i < 0) return WC_E_BADDATA;
    *cost = f->cost[i][atoi(p)];
    *src  = f->csrc[i][atoi(p)];
    return WC_OK;
}
static unsigned long f_setmet(void *ctx, const wc_guid *g, const char *p, int metered)
{
    fake *f = ctx;
    f->ncs++;
    if (f->m_err) return f->m_err;
    int i = fake_index(f, g);
    if (i < 0) return WC_E_BADDATA;
    f->cost[i][atoi(p)] = metered ? WC_COST_VARIABLE : COST_UNRESTRICTED;
    f->csrc[i][atoi(p)] = metered ? WC_COST_SRC_USER : 0;
    return WC_OK;
}

static void fake_init(fake *f, int nifs)
{
    memset(f, 0, sizeof *f);
    f->n = nifs;
    f->raw_true = 1;
    f->stick = true;
    for (int i = 0; i < WC_OPT_COUNT; ++i) f->can_write[i] = true;
    for (int i = 0; i < nifs; ++i) {
        f->ifs[i].guid.b[0] = (unsigned char)(i + 1);
        f->ifs[i].state = WC_IF_CONNECTED;
        snprintf(f->ifs[i].name, WC_NAME_MAX, "Adapter %d", i);
        f->val[i][WC_OPT_STREAMING] = 0;              /* OS defaults */
        f->val[i][WC_OPT_BGSCAN]    = f->raw_true;
        f->val[i][WC_OPT_AUTOCONF]  = f->raw_true;
        for (int k = 0; k < 8; ++k) f->cost[i][k] = COST_UNRESTRICTED;
    }
}

/* The app shows these; here they are just recorded, as the app's log would. */
static void f_note(void *ctx, diag_op op, diag_step step, const char *subject,
                   unsigned long code)
{
    diag_note(&((fake *)ctx)->log, op, step, subject, code, 1);
}

static const diag_entry *reported(fake *f, diag_op op, diag_step step)
{
    for (int i = 0; i < f->log.n; ++i)
        if (f->log.e[i].op == op && f->log.e[i].step == step) return &f->log.e[i];
    return nullptr;
}

static void bind(wc_state *s, fake *f)
{
    wc_backend be = { .ctx = f, .enum_ifaces = f_enum, .query_bool = f_query,
                      .set_bool = f_set, .granted_write = f_granted,
                      .list_profiles = f_profiles, .query_cost = f_qcost,
                      .set_metered = f_setmet };
    wc_init(s, &be);
    wc_set_note(s, f_note, f);
}

/* ----------------------------------------------------------------- cases */

static void t_applies_and_is_idempotent(void)
{
    printf("applies both opcodes, then stops writing\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);

    wc_poll(&s);
    CHECK(f.val[0][WC_OPT_STREAMING] == 1);
    CHECK(f.val[0][WC_OPT_BGSCAN]    == 0);
    CHECK(s.ad[0].streaming == WC_VAL_ON);
    CHECK(s.ad[0].bgscan    == WC_VAL_OFF);
    CHECK(s.ad[0].last_err  == WC_OK);
    CHECK(f.ns == 2);

    /* Steady state: reads only, no writes.  WlanSetInterface costs ~1s. */
    int writes = f.ns;
    wc_poll(&s);
    wc_poll(&s);
    CHECK(f.ns == writes);
}

static void t_nonzero_bool_is_true(void)
{
    /* Original compared the raw BOOL against TRUE, so a driver reporting
     * 0xFFFFFFFF looked like a read failure and provoked a rewrite forever. */
    printf("treats any nonzero BOOL as TRUE\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.raw_true = -1;
    f.val[0][WC_OPT_BGSCAN] = -1;

    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_OK);
    CHECK(s.ad[0].streaming == WC_VAL_ON);
    int writes = f.ns;
    wc_poll(&s);
    CHECK(f.ns == writes);
}

static void t_disconnected_is_pending_not_error(void)
{
    printf("disconnected adapter is pending, not failed\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.ifs[0].state = WC_IF_DISCONNECTED;

    wc_poll(&s);
    CHECK(s.ad[0].pending);
    CHECK(s.ad[0].last_err == WC_OK);
    CHECK(f.ns == 0);

    /* Reconnect: the very next poll applies, which is what an ACM event triggers. */
    f.ifs[0].state = WC_IF_CONNECTED;
    wc_poll(&s);
    CHECK(!s.ad[0].pending);
    CHECK(f.val[0][WC_OPT_STREAMING] == 1);
}

static void t_invalid_state_race(void)
{
    printf("ERROR_INVALID_STATE during set is a retry, not a failure\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.s_err = WC_E_INVALID_STATE;

    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_OK);
    CHECK(s.ad[0].pending);
}

static void t_transient_error_does_not_stop_us(void)
{
    /* WLANOptimizerThread::Loop breaks out of the loop on any unexpected
     * failure, so one hiccup silently disables the optimiser for good. */
    printf("recovers after a transient failure\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);

    f.q_err = WC_E_BADDATA;
    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_E_BADDATA);
    CHECK(s.ad[0].consec_fail == 1);

    f.q_err = WC_OK;
    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_OK);
    CHECK(s.ad[0].consec_fail == 0);
    CHECK(f.val[0][WC_OPT_STREAMING] == 1);
}

static void t_verify_mismatch(void)
{
    printf("reports a setting the driver silently drops\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.stick = false;

    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_E_VERIFY);
    CHECK(wc_strerror(WC_E_VERIFY) != NULL);
}

static void t_disable_withdraws_request(void)
{
    printf("disabling writes the opposite values exactly once\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);

    wc_poll(&s);
    CHECK(s.ad[0].voted[WC_OPT_STREAMING] && s.ad[0].voted[WC_OPT_BGSCAN]);

    wc_set_streaming_on(&s, false);
    CHECK(f.val[0][WC_OPT_STREAMING] == 0);
    CHECK(f.val[0][WC_OPT_BGSCAN]    == 0);   /* the other switch still holds */
    CHECK(!s.ad[0].voted[WC_OPT_STREAMING]);

    wc_set_bgscan_off(&s, false);
    CHECK(f.val[0][WC_OPT_BGSCAN]    == 1);
    CHECK(!s.ad[0].voted[WC_OPT_BGSCAN]);
    CHECK(f.ns == 4);

    int writes = f.ns;
    wc_poll(&s);
    wc_poll(&s);
    CHECK(f.ns == writes); /* nothing left to undo */
}

static void t_switches_are_independent(void)
{
    printf("each switch writes only its own opcode\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    s.streaming_on = false;

    wc_poll(&s);
    CHECK(f.val[0][WC_OPT_BGSCAN] == 0);
    CHECK(f.val[0][WC_OPT_STREAMING] == 0);
    CHECK(s.ad[0].streaming == WC_VAL_OFF);   /* still read, for the card */
    CHECK(f.ns == 1);

    wc_set_bgscan_off(&s, false);
    wc_set_streaming_on(&s, true);
    CHECK(f.val[0][WC_OPT_BGSCAN] == 1);
    CHECK(f.val[0][WC_OPT_STREAMING] == 1);
    CHECK(f.ns == 3);
    CHECK(s.ad[0].last_err == WC_OK);
}

static void t_another_clients_vote_is_theirs(void)
{
    /* Streaming mode stays on while any client asks for it, so neither
     * leaving it alone nor withdrawing our own request may fight that. */
    printf("a value another client holds is neither written nor an error\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.other[0][WC_OPT_STREAMING] = true;
    f.val[0][WC_OPT_STREAMING] = 1;
    s.streaming_on = false;

    wc_poll(&s);
    CHECK(f.ns == 1);                         /* background scan only */
    CHECK(s.ad[0].last_err == WC_OK);
    CHECK(s.ad[0].streaming == WC_VAL_ON);

    /* Asked for and then withdrawn: the write goes out once, reads back on,
     * and is not retried. */
    wc_set_streaming_on(&s, true);
    CHECK(s.ad[0].voted[WC_OPT_STREAMING]);
    wc_set_streaming_on(&s, false);
    CHECK(s.ad[0].last_err == WC_OK);
    CHECK(!s.ad[0].voted[WC_OPT_STREAMING]);
    const int writes = f.ns;
    wc_poll(&s);
    CHECK(f.ns == writes);
}

static void t_unmanaged_is_never_written(void)
{
    printf("unmanaged adapters are left alone\n");
    fake f; wc_state s; fake_init(&f, 2); bind(&s, &f);

    wc_refresh(&s);
    wc_set_managed(&s, 1, false);
    wc_poll(&s);
    wc_poll(&s);
    CHECK(f.nset[1] == 0);                  /* never written to at all */
    CHECK(f.val[1][WC_OPT_STREAMING] == 0);
    CHECK(f.val[1][WC_OPT_BGSCAN]    == 1);
    CHECK(f.nset[0] == 2);
    CHECK(f.val[0][WC_OPT_STREAMING] == 1); /* the other one still works */
}

static void t_managed_flag_survives_unplug(void)
{
    printf("per-adapter choice survives an unplug/replug\n");
    fake f; wc_state s; fake_init(&f, 2); bind(&s, &f);

    wc_refresh(&s);
    wc_set_managed(&s, 1, false);

    f.n = 1;                       /* dongle unplugged */
    wc_poll(&s);
    CHECK(!s.ad[1].present);

    f.n = 2;                       /* back again */
    wc_poll(&s);
    CHECK(s.ad[1].present);
    CHECK(!s.ad[1].managed);
    CHECK(f.val[1][WC_OPT_STREAMING] == 0);
}

static void t_access_denied_flags_elevation(void)
{
    printf("surfaces access denied instead of swallowing it\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.s_err = WC_E_ACCESS_DENIED;

    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_E_ACCESS_DENIED);
    CHECK(wc_write_denied(&s));
}

/* ---- the nuclear option: disabling WLAN auto config ------------------- */

static void t_nuclear_disables_autoconf(void)
{
    printf("arming the nuclear option turns auto config off\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);

    wc_poll(&s);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 1);   /* untouched while disarmed */

    wc_set_nuclear(&s, true);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);
    CHECK(s.ad[0].autoconf == WC_VAL_OFF);
}

static void t_startup_repairs_a_previous_crash(void)
{
    /* Auto config does not refcount and does not reset on disconnect, so a
     * process killed while armed leaves it off for good.  Starting up must
     * repair that without any record of having caused it. */
    printf("startup re-enables auto config left off by a dead process\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.val[0][WC_OPT_AUTOCONF] = 0;           /* the wreckage of a previous run */

    wc_poll(&s);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 1);
    CHECK(s.recovered == 1);
    CHECK(s.ad[0].autoconf == WC_VAL_ON);
}

static void t_disconnect_restores_autoconf(void)
{
    /* With auto config off Windows cannot reconnect on its own, so a dropped
     * link has to put it back immediately or the adapter is stranded. */
    printf("a dropped link re-enables auto config at once\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);

    wc_poll(&s);
    wc_set_nuclear(&s, true);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);

    f.ifs[0].state = WC_IF_DISCONNECTED;
    wc_poll(&s);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 1);
    CHECK(s.recovered == 1);
    CHECK(s.nuclear);                        /* still armed, just not applied */

    f.ifs[0].state = WC_IF_CONNECTED;        /* and it re-arms on reconnect */
    wc_poll(&s);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);
}

static void t_disarm_restores(void)
{
    printf("disarming restores auto config, whatever the other switches say\n");
    fake f; wc_state s; fake_init(&f, 2); bind(&s, &f);

    wc_poll(&s);
    wc_set_nuclear(&s, true);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);
    CHECK(f.val[1][WC_OPT_AUTOCONF] == 0);

    wc_set_nuclear(&s, false);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 1);
    CHECK(f.val[1][WC_OPT_AUTOCONF] == 1);

    /* It needs neither of the other two. */
    wc_set_bgscan_off(&s, false);
    wc_set_streaming_on(&s, false);
    wc_set_nuclear(&s, true);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);
    wc_set_nuclear(&s, false);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 1);
}

static void t_unmanaged_keeps_autoconf(void)
{
    printf("an unmanaged adapter never loses auto config\n");
    fake f; wc_state s; fake_init(&f, 2); bind(&s, &f);

    wc_poll(&s);
    wc_set_managed(&s, 1, false);
    wc_set_nuclear(&s, true);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);
    CHECK(f.val[1][WC_OPT_AUTOCONF] == 1);
}

static void t_autoconf_error_is_not_swallowed(void)
{
    printf("a refused auto config write is reported, not lost\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.ifs[0].state = WC_IF_DISCONNECTED;   /* takes an early return path */
    f.val[0][WC_OPT_AUTOCONF] = 0;
    f.s_err = WC_E_ACCESS_DENIED;

    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_E_ACCESS_DENIED);
    CHECK(wc_write_denied(&s));
}

static void t_probe_is_advisory(void)
{
    printf("a denied probe still lets us try\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.can_write[WC_OPT_BGSCAN] = false;

    wc_probe_access(&s);
    CHECK(wc_write_denied(&s));
    wc_poll(&s);
    CHECK(f.val[0][WC_OPT_BGSCAN] == 0); /* attempted anyway, and it worked */
}

/* ---- metered: a profile cost that outlives the process ---------------- */

static void t_metered_marks_every_profile(void)
{
    printf("metering marks every saved profile, then stops writing\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.nprof[0] = 3;

    wc_poll(&s);
    CHECK(f.ncs == 0);                       /* off by default: nothing written */

    wc_set_metered(&s, true);
    for (int k = 0; k < 3; ++k) CHECK(f.cost[0][k] == WC_COST_VARIABLE);
    CHECK(s.ad[0].metered == 3);
    CHECK(s.ad[0].profiles == 3);
    CHECK(s.ad[0].last_err == WC_OK);

    int writes = f.ncs;
    wc_poll(&s);
    CHECK(f.ncs == writes);
}

static void t_metered_repairs_only_its_own_cost(void)
{
    /* A killed run leaves Variable behind.  A network the user made metered in
     * Settings reads Fixed, and has to survive the repair untouched. */
    printf("startup resets a leftover metered cost, and nothing else\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.nprof[0] = 2;
    f.cost[0][0] = WC_COST_VARIABLE; f.csrc[0][0] = WC_COST_SRC_USER; /* ours */
    f.cost[0][1] = COST_FIXED;       f.csrc[0][1] = WC_COST_SRC_USER; /* the user's */

    wc_poll(&s);
    CHECK(f.cost[0][0] == COST_UNRESTRICTED);
    CHECK(f.cost[0][1] == COST_FIXED);
    CHECK(f.ncs == 1);
    CHECK(s.ad[0].metered == 0);

    /* Clean and switched off: later passes do not even read the costs. */
    int reads = f.ncq;
    wc_poll(&s);
    CHECK(f.ncq == reads);
}

static void t_metered_released_by_every_switch(void)
{
    printf("switching off and unmanaging reset the cost; the other switches do not\n");
    fake f; wc_state s; fake_init(&f, 2); bind(&s, &f);
    f.nprof[0] = f.nprof[1] = 1;

    wc_poll(&s);
    wc_set_metered(&s, true);
    CHECK(f.cost[0][0] == WC_COST_VARIABLE);
    CHECK(f.cost[1][0] == WC_COST_VARIABLE);

    wc_set_metered(&s, false);
    CHECK(f.cost[0][0] == COST_UNRESTRICTED);
    CHECK(f.cost[1][0] == COST_UNRESTRICTED);

    wc_set_metered(&s, true);
    wc_set_bgscan_off(&s, false);
    wc_set_streaming_on(&s, false);
    CHECK(f.cost[0][0] == WC_COST_VARIABLE);
    CHECK(f.cost[1][0] == WC_COST_VARIABLE);

    wc_set_managed(&s, 1, false);
    CHECK(f.cost[0][0] == WC_COST_VARIABLE);
    CHECK(f.cost[1][0] == COST_UNRESTRICTED);

    f.ifs[0].state = WC_IF_DISCONNECTED;     /* a cost holds while disconnected */
    wc_poll(&s);
    CHECK(f.cost[0][0] == WC_COST_VARIABLE);
}

static void t_metered_read_errors_quiet_when_off(void)
{
    /* Windows 7 has no WCM API, so every cost read fails there.  That is an
     * error only for someone who asked for metering. */
    printf("cost read failures are reported only while metering is on\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.nprof[0] = 1;
    f.c_err = 50; /* ERROR_NOT_SUPPORTED */

    wc_poll(&s);
    CHECK(s.ad[0].last_err == WC_OK);

    wc_set_metered(&s, true);
    CHECK(s.ad[0].last_err == 50);
}

static void t_a_refused_write_names_the_switch_and_the_adapter(void)
{
    printf("a refused write says which switch, which adapter and why\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.s_err = WC_E_ACCESS_DENIED;

    wc_poll(&s);
    const diag_entry *e = reported(&f, DIAG_STREAMING, DIAG_WRITE);
    CHECK(e && e->code == WC_E_ACCESS_DENIED && !strcmp(e->subject, "Adapter 0"));
    CHECK(reported(&f, DIAG_BGSCAN, DIAG_WRITE) != nullptr);
    /* Repeated passes are the same failure, not a growing pile of them. */
    const int was = f.log.n;
    wc_poll(&s);
    CHECK(f.log.n == was);
}

static void t_a_cost_that_will_not_move_names_the_network(void)
{
    printf("a network whose cost will not move is named, one entry each\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.nprof[0] = 2;

    wc_poll(&s);
    f.m_err = WC_E_NETSH;
    wc_set_metered(&s, true);
    CHECK(diag_count(&f.log, DIAG_WARN) == 2);   /* the fake names profiles "0" and "1" */
    const diag_entry *e = reported(&f, DIAG_METERED, DIAG_WRITE);
    CHECK(e && e->code == WC_E_NETSH);
    CHECK(e && (!strcmp(e->subject, "0") || !strcmp(e->subject, "1")));
}

static void t_failing_to_give_scanning_back_is_serious(void)
{
    printf("failing to give auto configuration back is reported as serious\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);

    wc_poll(&s);
    wc_set_nuclear(&s, true);
    CHECK(s.ad[0].autoconf == WC_VAL_OFF);

    /* Disarm with the writes refused: scanning stays off, which is the one
     * failure that outlives the process. */
    f.s_err = WC_E_ACCESS_DENIED;
    wc_set_nuclear(&s, false);
    const diag_entry *e = reported(&f, DIAG_AUTOCONF, DIAG_RESTORE);
    CHECK(e && diag_sev_of(e->op, e->code) == DIAG_ERR);
}

static void t_a_disconnect_race_is_not_an_alarm(void)
{
    printf("losing a race with a disconnect is noted, but not as a problem\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.s_err = WC_E_INVALID_STATE;

    wc_poll(&s);
    CHECK(f.log.n > 0);
    CHECK(diag_count(&f.log, DIAG_WARN) == 0);
}

static void t_an_enumeration_failure_is_reported(void)
{
    printf("an adapter list that cannot be read is reported as fatal\n");
    fake f; wc_state s; fake_init(&f, 1); bind(&s, &f);
    f.e_err = WC_E_ACCESS_DENIED;

    CHECK(wc_poll(&s) == WC_E_ACCESS_DENIED);
    const diag_entry *e = reported(&f, DIAG_ADAPTERS, DIAG_READ);
    CHECK(e && diag_sev_of(e->op, e->code) == DIAG_ERR);

    /* A machine with no Wi-Fi at all is not a failure. */
    fake g; wc_state t; fake_init(&g, 0); bind(&t, &g);
    wc_poll(&t);
    CHECK(g.log.n == 0);
}

int main(void)
{
    t_applies_and_is_idempotent();
    t_nonzero_bool_is_true();
    t_disconnected_is_pending_not_error();
    t_invalid_state_race();
    t_transient_error_does_not_stop_us();
    t_verify_mismatch();
    t_disable_withdraws_request();
    t_switches_are_independent();
    t_another_clients_vote_is_theirs();
    t_unmanaged_is_never_written();
    t_managed_flag_survives_unplug();
    t_access_denied_flags_elevation();
    t_probe_is_advisory();
    t_nuclear_disables_autoconf();
    t_startup_repairs_a_previous_crash();
    t_disconnect_restores_autoconf();
    t_disarm_restores();
    t_unmanaged_keeps_autoconf();
    t_autoconf_error_is_not_swallowed();
    t_metered_marks_every_profile();
    t_metered_repairs_only_its_own_cost();
    t_metered_released_by_every_switch();
    t_metered_read_errors_quiet_when_off();
    t_a_refused_write_names_the_switch_and_the_adapter();
    t_a_cost_that_will_not_move_names_the_network();
    t_failing_to_give_scanning_back_is_serious();
    t_a_disconnect_race_is_not_an_alarm();
    t_an_enumeration_failure_is_reported();

    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}

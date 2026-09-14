/* Unit tests for the policy core.  Builds and runs on any host; each case is
 * a regression test for a specific defect in the original WLANOptimizer. */
#include "../src/wlan.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ------------------------------------------------------------------ fake */

typedef struct {
    wc_ifinfo     ifs[4];
    int           n;
    long          val[4][WC_OPT_COUNT]; /* raw driver values */
    long          raw_true;             /* what this "driver" calls TRUE */
    unsigned long q_err, s_err;
    bool          stick;                /* false: set succeeds but nothing changes */
    int           nq, ns;               /* call counts */
    int           nset[4];              /* writes per adapter */
    bool          can_write[WC_OPT_COUNT];
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
    if (f->stick) f->val[i][o] = v ? f->raw_true : 0;
    return WC_OK;
}
static unsigned long f_granted(void *ctx, wc_opt o, int *w)
{
    fake *f = ctx;
    *w = f->can_write[o] ? 1 : 0;
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
    }
}

static void bind(wc_state *s, fake *f)
{
    wc_backend be = { f, f_enum, f_query, f_set, f_granted };
    wc_init(s, &be);
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
     * failure, so one hiccup silently disables the optimizer for good. */
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
    CHECK(s.ad[0].touched);

    wc_set_enabled(&s, false);
    CHECK(f.val[0][WC_OPT_STREAMING] == 0);
    CHECK(f.val[0][WC_OPT_BGSCAN]    == 1);
    CHECK(!s.ad[0].touched);

    int writes = f.ns;
    wc_poll(&s);
    wc_poll(&s);
    CHECK(f.ns == writes); /* nothing left to undo */
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

static void t_disarm_and_master_off_restore(void)
{
    printf("disarming, and the master switch, both restore auto config\n");
    fake f; wc_state s; fake_init(&f, 2); bind(&s, &f);

    wc_poll(&s);
    wc_set_nuclear(&s, true);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);
    CHECK(f.val[1][WC_OPT_AUTOCONF] == 0);

    wc_set_nuclear(&s, false);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 1);
    CHECK(f.val[1][WC_OPT_AUTOCONF] == 1);

    wc_set_nuclear(&s, true);
    CHECK(f.val[0][WC_OPT_AUTOCONF] == 0);
    wc_set_enabled(&s, false);               /* master off overrides the arm */
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

int main(void)
{
    t_applies_and_is_idempotent();
    t_nonzero_bool_is_true();
    t_disconnected_is_pending_not_error();
    t_invalid_state_race();
    t_transient_error_does_not_stop_us();
    t_verify_mismatch();
    t_disable_withdraws_request();
    t_unmanaged_is_never_written();
    t_managed_flag_survives_unplug();
    t_access_denied_flags_elevation();
    t_probe_is_advisory();
    t_nuclear_disables_autoconf();
    t_startup_repairs_a_previous_crash();
    t_disconnect_restores_autoconf();
    t_disarm_and_master_off_restore();
    t_unmanaged_keeps_autoconf();
    t_autoconf_error_is_not_swallowed();

    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}

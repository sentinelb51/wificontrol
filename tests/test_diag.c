/* Unit tests for the failure log, the words it produces and the rules that
 * grey out a switch.  Builds and runs on any host: diag.c knows no Windows. */
#include "../src/diag.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* The Win32 codes used below, by name. */
enum { E_DENIED = 5, E_NOT_SUPPORTED = 50, E_SERVICE_STOPPED = 1062, E_ODD = 4711 };

static void t_a_repeat_is_not_a_new_entry(void)
{
    printf("the same failure twice is one entry with a count\n");
    diag_log l = { 0 };

    diag_note(&l, DIAG_BGSCAN, DIAG_WRITE, "Intel AX201", E_DENIED, 1000);
    diag_note(&l, DIAG_BGSCAN, DIAG_WRITE, "Intel AX201", E_DENIED, 2000);
    diag_note(&l, DIAG_BGSCAN, DIAG_WRITE, "Intel AX201", E_DENIED, 3000);

    CHECK(l.n == 1);
    CHECK(l.e[0].count == 3);
    CHECK(l.e[0].first == 1000 && l.e[0].last == 3000);
    /* Every note is news, so the window can tell that it failed again. */
    CHECK(l.gen == 3);
}

static void t_what_makes_an_entry_distinct(void)
{
    printf("a different setting, adapter, step or code is its own entry\n");
    diag_log l = { 0 };

    diag_note(&l, DIAG_BGSCAN,  DIAG_WRITE, "one", E_DENIED, 1);
    diag_note(&l, DIAG_BGSCAN,  DIAG_WRITE, "two", E_DENIED, 1);   /* other adapter */
    diag_note(&l, DIAG_BGSCAN,  DIAG_READ,  "one", E_DENIED, 1);   /* other step */
    diag_note(&l, DIAG_BGSCAN,  DIAG_WRITE, "one", E_ODD, 1);      /* other code */
    diag_note(&l, DIAG_METERED, DIAG_WRITE, "one", E_DENIED, 1);   /* other setting */

    CHECK(l.n == 5);
}

static void t_ok_is_not_a_failure(void)
{
    printf("a code of WC_OK is ignored, so callers can pass one straight in\n");
    diag_log l = { 0 };
    diag_note(&l, DIAG_METERED, DIAG_WRITE, "home", WC_OK, 1);
    CHECK(l.n == 0 && l.gen == 0);
}

static void t_a_full_log_drops_the_stalest(void)
{
    printf("a full log drops the one nothing has repeated for longest\n");
    diag_log l = { 0 };

    for (int i = 0; i < DIAG_MAX; ++i) {
        char subject[32];
        snprintf(subject, sizeof subject, "net %d", i);
        diag_note(&l, DIAG_METERED, DIAG_WRITE, subject, E_DENIED, (unsigned long)(100 + i));
    }
    /* The first one is seen again, so it is no longer the stalest. */
    diag_note(&l, DIAG_METERED, DIAG_WRITE, "net 0", E_DENIED, 500);
    diag_note(&l, DIAG_RESTART, DIAG_WRITE, "Intel AX201", E_ODD, 600);

    CHECK(l.n == DIAG_MAX);
    CHECK(l.dropped == 1);
    bool kept_first = false, dropped_second = true, kept_new = false;
    for (int i = 0; i < l.n; ++i) {
        if (!strcmp(l.e[i].subject, "net 0")) kept_first = true;
        if (!strcmp(l.e[i].subject, "net 1")) dropped_second = false;
        if (l.e[i].op == DIAG_RESTART)        kept_new = true;
    }
    CHECK(kept_first && dropped_second && kept_new);
}

static void t_clearing_keeps_the_generation(void)
{
    printf("clearing empties the log but not the generation counter\n");
    diag_log l = { 0 };
    diag_note(&l, DIAG_METERED, DIAG_WRITE, "home", E_DENIED, 1);
    const unsigned gen = l.gen;

    diag_clear(&l);
    CHECK(l.n == 0 && l.dropped == 0);
    /* Otherwise a Refresh would look like a fresh failure to the window. */
    CHECK(l.gen == gen);
}

static void t_severity(void)
{
    printf("what counts as serious\n");
    /* Anything that can outlive the process, or leave a disabled adapter. */
    CHECK(diag_sev_of(DIAG_AUTOCONF, E_DENIED) == DIAG_ERR);
    CHECK(diag_sev_of(DIAG_RESTART, E_ODD) == DIAG_ERR);
    CHECK(diag_sev_of(DIAG_PERF_JOURNAL, E_DENIED) == DIAG_ERR);
    CHECK(diag_sev_of(DIAG_WLAN_SERVICE, E_SERVICE_STOPPED) == DIAG_ERR);
    /* A setting that is reapplied by itself is not worth an alarm. */
    CHECK(diag_sev_of(DIAG_BGSCAN, WC_E_INVALID_STATE) == DIAG_INFO);
    CHECK(diag_sev_of(DIAG_AUTOCONF, WC_E_INVALID_STATE) == DIAG_INFO);
    CHECK(diag_sev_of(DIAG_METERED, WC_E_NETSH) == DIAG_WARN);
}

static void t_counting_and_the_worst_of_it(void)
{
    printf("the counts the status line asks for, and the worst entry\n");
    diag_log l = { 0 };
    diag_note(&l, DIAG_STREAMING, DIAG_WRITE, "one", WC_E_VERIFY, 1000);       /* warn */
    diag_note(&l, DIAG_BGSCAN,    DIAG_WRITE, "one", WC_E_INVALID_STATE, 2000);/* info */
    diag_note(&l, DIAG_RESTART,   DIAG_WRITE, "one", E_ODD, 3000);             /* error */

    CHECK(diag_worst(&l) == DIAG_ERR);
    CHECK(diag_count(&l, DIAG_WARN) == 2);
    CHECK(diag_count(&l, DIAG_INFO) == 3);
    CHECK(diag_count(&l, DIAG_ERR) == 1);

    /* Only what happened since the switch was flipped. */
    CHECK(diag_count_since(&l, DIAG_WARN, 2500) == 1);
    CHECK(diag_count_since(&l, DIAG_WARN, 3500) == 0);

    const diag_entry *e = diag_worst_entry(&l);
    CHECK(e && e->op == DIAG_RESTART);
}

static void t_a_wrapped_tick_counter(void)
{
    printf("a tick counter that has wrapped does not hide a failure\n");
    diag_log l = { 0 };
    const unsigned long before = 0xFFFFFF00UL, after = 0x00000100UL;
    diag_note(&l, DIAG_METERED, DIAG_WRITE, "home", WC_E_NETSH, after);
    CHECK(diag_count_since(&l, DIAG_WARN, before) == 1);
}

static void t_every_op_and_step_has_words(void)
{
    printf("every op and step has words, whatever is added later\n");
    for (int op = 0; op < DIAG_OP_COUNT; ++op) {
        const char *name = diag_op_name((diag_op)op);
        CHECK(name && name[0] && strcmp(name, "Something else") != 0);
    }
    for (int st = 0; st < DIAG_STEP_COUNT; ++st) {
        const char *name = diag_step_name((diag_step)st);
        CHECK(name && name[0] && strcmp(name, "It") != 0);
    }
}

static void t_advice(void)
{
    printf("the advice says what to do, and says nothing when it cannot\n");
    /* A restart and the journal are the two the user may have to act on. */
    CHECK(diag_advice(DIAG_RESTART, DIAG_WRITE, E_ODD) != nullptr);
    CHECK(diag_advice(DIAG_PERF_JOURNAL, DIAG_RECORD, E_ODD) != nullptr);
    /* Access denied reads differently for a driver key and for Native Wifi. */
    CHECK(strcmp(diag_advice(DIAG_PERF_DRIVER, DIAG_WRITE, E_DENIED),
                 diag_advice(DIAG_BGSCAN, DIAG_WRITE, E_DENIED)) != 0);
    CHECK(diag_advice(DIAG_METERED, DIAG_READ, E_NOT_SUPPORTED) != nullptr);
    CHECK(diag_advice(DIAG_WLAN_SERVICE, DIAG_READ, E_SERVICE_STOPPED) != nullptr);
    /* Nothing invented for a code nobody has words for. */
    CHECK(diag_advice(DIAG_STREAMING, DIAG_WRITE, E_ODD) == nullptr);
}

static void t_own_codes_are_decoded(void)
{
    printf("our own codes are decoded, and Win32 ones are left to Windows\n");
    CHECK(wc_strerror(WC_E_VERIFY) != nullptr);
    CHECK(wc_strerror(WC_E_NETSH) != nullptr);
    CHECK(wc_strerror(WC_E_NO_JOURNAL) != nullptr);
    CHECK(wc_strerror(E_DENIED) == nullptr);
    CHECK(wc_strerror(WC_OK) == nullptr);
}

/* ---------------------------------------------------------- capabilities */

static void all_ok(wc_cap_state *cap)
{
    for (int c = 0; c < WC_CAP_COUNT; ++c) cap[c] = (wc_cap_state){ .ok = true, .checked = true };
}

static void t_nothing_checked_blocks_nothing(void)
{
    printf("a capability nobody has probed yet never greys a switch out\n");
    wc_cap_state cap[WC_CAP_COUNT] = { 0 };  /* checked = false throughout */
    for (int sw = 0; sw < WC_SW_COUNT; ++sw)
        CHECK(wc_switch_blocked(cap, (wc_switch)sw) == nullptr);
}

static void t_no_wlan_service(void)
{
    printf("without the WLAN service every switch but Performance is dead\n");
    wc_cap_state cap[WC_CAP_COUNT];
    all_ok(cap);
    cap[WC_CAP_WLAN] = (wc_cap_state){ .checked = true, .err = E_SERVICE_STOPPED };

    CHECK(wc_switch_blocked(cap, WC_SW_BGSCAN) != nullptr);
    CHECK(wc_switch_blocked(cap, WC_SW_STREAMING) != nullptr);
    CHECK(wc_switch_blocked(cap, WC_SW_NUCLEAR) != nullptr);
    CHECK(wc_switch_blocked(cap, WC_SW_METERED) != nullptr);
    /* Driver properties and the power plan have nothing to do with wlansvc. */
    CHECK(wc_switch_blocked(cap, WC_SW_PERF) == nullptr);
}

static void t_one_denied_opcode_only_greys_its_own_switch(void)
{
    printf("a setting Windows refuses greys out that switch and no other\n");
    wc_cap_state cap[WC_CAP_COUNT];
    all_ok(cap);
    cap[WC_CAP_BGSCAN] = (wc_cap_state){ .checked = true, .err = WC_E_ACCESS_DENIED };

    CHECK(wc_switch_blocked(cap, WC_SW_BGSCAN) != nullptr);
    CHECK(wc_switch_blocked(cap, WC_SW_STREAMING) == nullptr);
    CHECK(wc_switch_blocked(cap, WC_SW_NUCLEAR) == nullptr);
}

static void t_no_cost_api(void)
{
    printf("with no connection cost API, Metered says so\n");
    wc_cap_state cap[WC_CAP_COUNT];
    all_ok(cap);
    cap[WC_CAP_COST] = (wc_cap_state){ .checked = true, .err = E_NOT_SUPPORTED };

    /* The row beside the switch gets the short form, the details window the
     * sentence; both have to name the thing that is missing. */
    const char *brief = wc_switch_blocked(cap, WC_SW_METERED);
    CHECK(brief != nullptr && strstr(brief, "cost") != nullptr);
    const char *why = wc_cap_why(cap, WC_CAP_COST);
    CHECK(why != nullptr && strstr(why, "metered") != nullptr);
    CHECK(wc_cap_why(cap, WC_CAP_WLAN) == nullptr);
    CHECK(wc_switch_blocked(cap, WC_SW_BGSCAN) == nullptr);
}

static void t_performance_needs_a_way_back(void)
{
    printf("Performance is dead without the restore file, whatever else works\n");
    wc_cap_state cap[WC_CAP_COUNT];
    all_ok(cap);
    cap[WC_CAP_JOURNAL] = (wc_cap_state){ .checked = true, .err = WC_E_ACCESS_DENIED };
    CHECK(wc_switch_blocked(cap, WC_SW_PERF) != nullptr);

    /* One of the two halves is enough to be worth offering. */
    all_ok(cap);
    cap[WC_CAP_DRIVER] = (wc_cap_state){ .checked = true, .err = 2 };
    CHECK(wc_switch_blocked(cap, WC_SW_PERF) == nullptr);
    cap[WC_CAP_POWER] = (wc_cap_state){ .checked = true, .err = 2 };
    CHECK(wc_switch_blocked(cap, WC_SW_PERF) != nullptr);
}

static void t_every_capability_has_a_name(void)
{
    printf("every capability has a name for the details window\n");
    for (int c = 0; c < WC_CAP_COUNT; ++c) {
        const char *name = wc_cap_name((wc_cap)c);
        CHECK(name && name[0] && strcmp(name, "Unknown") != 0);
    }
}

int main(void)
{
    t_a_repeat_is_not_a_new_entry();
    t_what_makes_an_entry_distinct();
    t_ok_is_not_a_failure();
    t_a_full_log_drops_the_stalest();
    t_clearing_keeps_the_generation();
    t_severity();
    t_counting_and_the_worst_of_it();
    t_a_wrapped_tick_counter();
    t_every_op_and_step_has_words();
    t_advice();
    t_own_codes_are_decoded();
    t_nothing_checked_blocks_nothing();
    t_no_wlan_service();
    t_one_denied_opcode_only_greys_its_own_switch();
    t_no_cost_api();
    t_performance_needs_a_way_back();
    t_every_capability_has_a_name();

    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}

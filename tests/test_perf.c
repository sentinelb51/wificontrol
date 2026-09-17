/* Unit tests for the Performance switch's policy.  Builds and runs on any
 * host against a fake registry, power plan and journal. */
#include "../src/perf.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define WIFI_AC "{19CBB8FA-5279-450E-9FAC-8A3D5FEDD0C1}\\{12BBEBE6-58D6-4636-95BB-3217EF867C1A}\\ac"
#define WIFI_DC "{19CBB8FA-5279-450E-9FAC-8A3D5FEDD0C1}\\{12BBEBE6-58D6-4636-95BB-3217EF867C1A}\\dc"
#define ASPM_AC "{501A4D13-42AF-4429-9FD1-A8218C268E20}\\{EE12F906-D277-404B-B6DA-E5FA1A576DF5}\\ac"
#define ASPM_DC "{501A4D13-42AF-4429-9FD1-A8218C268E20}\\{EE12F906-D277-404B-B6DA-E5FA1A576DF5}\\dc"

/* ------------------------------------------------------------------ fake */

typedef struct {
    perf_kind kind;
    wc_guid   owner;
    char      name[PERF_NAME_MAX];
    char      value[PERF_VALUE_MAX];
    char      choices[64];   /* space-separated, every value it accepts */
    unsigned long r_err, w_err;
} setting;

typedef struct {
    setting    s[32];
    int        n;
    wc_guid    active;
    unsigned long scheme_err, keep_err, restart_err;
    perf_entry j[PERF_MAX_HELD];
    int        nj;
    int        writes, commits, restarts[4];
    diag_log   log;                  /* what the switch reported, in order */
} fake;

static wc_guid G(int k) { wc_guid g = { { 0 } }; g.b[0] = (unsigned char)k; return g; }

static setting *find(fake *f, perf_kind k, const wc_guid *o, const char *name)
{
    for (int i = 0; i < f->n; ++i)
        if (f->s[i].kind == k && !memcmp(f->s[i].owner.b, o->b, 16) && !strcmp(f->s[i].name, name))
            return &f->s[i];
    return nullptr;
}

static setting *add(fake *f, perf_kind k, wc_guid o, const char *name, const char *value,
                    const char *choices)
{
    setting *s = &f->s[f->n++];
    memset(s, 0, sizeof *s);
    s->kind = k;
    s->owner = o;
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->value, sizeof s->value, "%s", value);
    snprintf(s->choices, sizeof s->choices, " %s ", choices);
    return s;
}

/* An Intel card as found on the test machine, with *SelectiveSuspend absent
 * and both Wi-Fi Direct adapters enabled. */
static void add_card(fake *f, wc_guid g)
{
    add(f, PERF_DRIVER, g, "MIMOPowerSaveMode", "0", "0 1 2 3");
    add(f, PERF_DRIVER, g, "uAPSDSupport", "1", "0 1");
    add(f, PERF_DRIVER, g, "IbssTxPower", "100", "0 25 50 75 100");
    add(f, PERF_DRIVER, g, "*PacketCoalescing", "1", "0 1");
    add(f, PERF_DEVICE, g, "WiFiDirect", "11", "0");
}

/* Balanced as found: plugged in already at the target, battery saving. */
static void add_scheme(fake *f, wc_guid g)
{
    add(f, PERF_POWER, g, WIFI_AC, "0", "0 1 2 3");
    add(f, PERF_POWER, g, WIFI_DC, "2", "0 1 2 3");
    add(f, PERF_POWER, g, ASPM_AC, "0", "0 1 2");
    add(f, PERF_POWER, g, ASPM_DC, "2", "0 1 2");
}

static const char *val(fake *f, perf_kind k, wc_guid o, const char *name)
{
    setting *s = find(f, k, &o, name);
    return s ? s->value : "(absent)";
}

static unsigned long f_read(void *ctx, perf_kind k, const wc_guid *o, const char *name,
                            const char *target, char *live, int cap, int *has)
{
    fake *f = ctx;
    setting *s = find(f, k, o, name);
    if (!s) return PERF_ABSENT;
    if (s->r_err) return s->r_err;
    char pat[PERF_VALUE_MAX + 2];
    snprintf(pat, sizeof pat, " %s ", target);
    *has = strstr(s->choices, pat) != nullptr;
    snprintf(live, (size_t)cap, "%s", s->value);
    return WC_OK;
}

static unsigned long f_write(void *ctx, perf_kind k, const wc_guid *o, const char *name,
                             const char *value)
{
    fake *f = ctx;
    setting *s = find(f, k, o, name);
    if (!s) return PERF_ABSENT;
    if (s->w_err) return s->w_err;
    snprintf(s->value, sizeof s->value, "%s", value);
    f->writes++;
    return WC_OK;
}

static unsigned long f_scheme(void *ctx, wc_guid *active)
{
    fake *f = ctx;
    *active = f->active;
    return f->scheme_err;
}

static unsigned long f_commit(void *ctx) { ((fake *)ctx)->commits++; return WC_OK; }

static unsigned long f_restart(void *ctx, const wc_guid *g)
{
    fake *f = ctx;
    if (f->restart_err) return f->restart_err;
    f->restarts[g->b[0]]++;
    return WC_OK;
}

/* The app puts these in front of the user; here they are just recorded. */
static void f_note(void *ctx, diag_op op, diag_step step, const wc_guid *owner,
                   const char *name, unsigned long err)
{
    fake *f = ctx;
    char subject[DIAG_SUBJ_MAX];
    snprintf(subject, sizeof subject, "%s/%u", name ? perf_label(name) : "",
             owner ? (unsigned)owner->b[0] : 0u);
    diag_note(&f->log, op, step, subject, err, 1);
}

static const diag_entry *reported(fake *f, diag_op op, diag_step step)
{
    for (int i = 0; i < f->log.n; ++i)
        if (f->log.e[i].op == op && f->log.e[i].step == step) return &f->log.e[i];
    return nullptr;
}

static int f_held(void *ctx, perf_entry *out, int cap)
{
    fake *f = ctx;
    int n = f->nj < cap ? f->nj : cap;
    memcpy(out, f->j, (size_t)n * sizeof *out);
    return n;
}

static int jfind(fake *f, const perf_entry *e)
{
    for (int i = 0; i < f->nj; ++i)
        if (f->j[i].kind == e->kind && !memcmp(f->j[i].owner.b, e->owner.b, 16) &&
            !strcmp(f->j[i].name, e->name))
            return i;
    return -1;
}

static unsigned long f_keep(void *ctx, const perf_entry *e)
{
    fake *f = ctx;
    if (f->keep_err) return f->keep_err;
    int i = jfind(f, e);
    f->j[i < 0 ? f->nj++ : i] = *e;
    return WC_OK;
}

static unsigned long f_forget(void *ctx, const perf_entry *e)
{
    fake *f = ctx;
    int i = jfind(f, e);
    if (i >= 0) f->j[i] = f->j[--f->nj];
    return WC_OK;
}

static const char *recorded(fake *f, perf_kind k, wc_guid o, const char *name)
{
    perf_entry e = { .kind = k, .owner = o };
    snprintf(e.name, sizeof e.name, "%s", name);
    int i = jfind(f, &e);
    return i < 0 ? nullptr : f->j[i].value;
}

static perf_backend backend(fake *f)
{
    return (perf_backend){ .ctx = f, .read = f_read, .write = f_write, .scheme = f_scheme,
                           .commit = f_commit, .restart = f_restart, .held = f_held,
                           .keep = f_keep, .forget = f_forget, .note = f_note };
}

static void setup(fake *f)
{
    memset(f, 0, sizeof *f);
    f->active = G(9);
    add_scheme(f, G(9));
    add_card(f, G(1));
}

/* ------------------------------------------------------------------ tests */

static void t_holds_and_records_only_what_it_changes(void)
{
    printf("switching on records each changed value, then writes the target\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_result r = perf_sync(&be, true, ad, 1, true);
    CHECK(r.err == WC_OK);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "3"));
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "uAPSDSupport"), "0"));
    CHECK(!strcmp(val(&f, PERF_POWER, G(9), WIFI_DC), "0"));
    CHECK(!strcmp(val(&f, PERF_POWER, G(9), ASPM_DC), "0"));
    /* Already at the target, so nothing to record and nothing to put back. */
    CHECK(!recorded(&f, PERF_DRIVER, G(1), "IbssTxPower"));
    CHECK(!recorded(&f, PERF_POWER, G(9), WIFI_AC));
    CHECK(!strcmp(recorded(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "0"));
    CHECK(!strcmp(recorded(&f, PERF_POWER, G(9), ASPM_DC), "2"));
    CHECK(!strcmp(val(&f, PERF_DEVICE, G(1), "WiFiDirect"), "0"));
    CHECK(!strcmp(recorded(&f, PERF_DEVICE, G(1), "WiFiDirect"), "11"));
    /* Packet coalescing is not part of it. */
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "*PacketCoalescing"), "1"));
    CHECK(r.held == 5 && r.changed == 5);
    CHECK(f.restarts[1] == 1 && f.commits == 1);
}

static void t_second_sync_does_nothing(void)
{
    printf("holding again changes nothing and restarts nothing\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_sync(&be, true, ad, 1, true);
    int w = f.writes;
    perf_result r = perf_sync(&be, true, ad, 1, true);
    CHECK(f.writes == w);
    CHECK(r.changed == 0 && r.restarted == 0 && r.held == 5);
    CHECK(f.restarts[1] == 1 && f.commits == 1);
}

static void t_switching_off_puts_everything_back(void)
{
    printf("switching off restores each recorded value and restarts once\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter on[] = { { G(1), true, true } }, off[] = { { G(1), false, true } };

    perf_sync(&be, true, on, 1, true);
    perf_result r = perf_sync(&be, false, off, 1, true);
    CHECK(r.err == WC_OK && r.held == 0 && f.nj == 0);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "0"));
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "uAPSDSupport"), "1"));
    CHECK(!strcmp(val(&f, PERF_POWER, G(9), WIFI_DC), "2"));
    CHECK(!strcmp(val(&f, PERF_POWER, G(9), ASPM_DC), "2"));
    CHECK(!strcmp(val(&f, PERF_POWER, G(9), WIFI_AC), "0"));
    CHECK(!strcmp(val(&f, PERF_DEVICE, G(1), "WiFiDirect"), "11"));
    CHECK(f.restarts[1] == 2 && f.commits == 2);
}

static void t_a_later_change_is_left_alone(void)
{
    printf("a value changed by someone else while held is not overwritten\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter on[] = { { G(1), true, true } }, off[] = { { G(1), false, true } };

    perf_sync(&be, true, on, 1, true);
    snprintf(find(&f, PERF_DRIVER, &(wc_guid){ { 1 } }, "MIMOPowerSaveMode")->value,
             PERF_VALUE_MAX, "2");
    snprintf(find(&f, PERF_POWER, &(wc_guid){ { 9 } }, WIFI_DC)->value, PERF_VALUE_MAX, "3");

    perf_result r = perf_sync(&be, false, off, 1, true);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "2"));
    CHECK(!strcmp(val(&f, PERF_POWER, G(9), WIFI_DC), "3"));
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "uAPSDSupport"), "1"));
    CHECK(r.held == 0);
}

static void t_reholding_keeps_the_original(void)
{
    printf("holding over a later change keeps the first recorded value\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter on[] = { { G(1), true, true } }, off[] = { { G(1), false, true } };

    perf_sync(&be, true, on, 1, true);
    snprintf(find(&f, PERF_DRIVER, &(wc_guid){ { 1 } }, "MIMOPowerSaveMode")->value,
             PERF_VALUE_MAX, "2");
    perf_sync(&be, true, on, 1, true);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "3"));
    CHECK(!strcmp(recorded(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "0"));

    perf_sync(&be, false, off, 1, true);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "0"));
}

static void t_absent_and_unsupported_are_skipped(void)
{
    printf("a card without a property, or without its target, is left alone\n");
    fake f;
    memset(&f, 0, sizeof f);
    f.active = G(9);
    add(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode", "0", "0 1 2"); /* no "3" */
    add(&f, PERF_DRIVER, G(1), "*SelectiveSuspend", "1", "0 1");
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_result r = perf_sync(&be, true, ad, 1, true);
    CHECK(r.err == WC_OK);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "0"));
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "*SelectiveSuspend"), "0"));
    CHECK(r.held == 1 && f.commits == 0);
}

static void t_startup_repairs_a_killed_run(void)
{
    printf("records left by a killed run are put back when the switch is off\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter on[] = { { G(1), true, true } }, off[] = { { G(1), false, true } };
    perf_sync(&be, true, on, 1, true);

    /* A new process: the journal is all that is left. */
    perf_result r = perf_sync(&be, false, off, 1, true);
    CHECK(r.held == 0);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "uAPSDSupport"), "1"));
}

static void t_a_failed_write_leaves_no_record(void)
{
    printf("a write that fails is reported and leaves no record behind\n");
    fake f;
    setup(&f);
    find(&f, PERF_DRIVER, &(wc_guid){ { 1 } }, "uAPSDSupport")->w_err = 5;
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_result r = perf_sync(&be, true, ad, 1, true);
    CHECK(r.err == 5);
    CHECK(!recorded(&f, PERF_DRIVER, G(1), "uAPSDSupport"));
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "3"));
    CHECK(r.held == 4);
}

static void t_no_record_no_write(void)
{
    printf("nothing is written when the journal refuses the record\n");
    fake f;
    setup(&f);
    f.keep_err = 32;
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_result r = perf_sync(&be, true, ad, 1, true);
    CHECK(r.err == 32 && f.writes == 0 && f.restarts[1] == 0 && f.commits == 0);
}

static void t_unreadable_record_is_kept(void)
{
    printf("a record whose setting cannot be read is kept for the next attempt\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter on[] = { { G(1), true, true } }, off[] = { { G(1), false, true } };
    perf_sync(&be, true, on, 1, true);

    find(&f, PERF_DRIVER, &(wc_guid){ { 1 } }, "uAPSDSupport")->r_err = 5;
    perf_result r = perf_sync(&be, false, off, 1, true);
    CHECK(r.err == 5 && r.held == 1);
    CHECK(recorded(&f, PERF_DRIVER, G(1), "uAPSDSupport"));

    find(&f, PERF_DRIVER, &(wc_guid){ { 1 } }, "uAPSDSupport")->r_err = 0;
    r = perf_sync(&be, false, off, 1, true);
    CHECK(r.err == WC_OK && r.held == 0);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "uAPSDSupport"), "1"));
}

static void t_switched_plan_gets_its_values_back(void)
{
    printf("after a plan switch, the old plan is restored and the new one held\n");
    fake f;
    setup(&f);
    add_scheme(&f, G(8));
    perf_backend be = backend(&f);

    perf_sync_power(&be, true);
    f.active = G(8);
    perf_result r = perf_sync_power(&be, true);
    CHECK(r.err == WC_OK);
    CHECK(!strcmp(val(&f, PERF_POWER, G(9), WIFI_DC), "2"));
    CHECK(!strcmp(val(&f, PERF_POWER, G(8), WIFI_DC), "0"));
    CHECK(r.held == 2 && f.commits == 2);

    /* Writing an inactive plan needs no commit. */
    f.active = G(9);
    perf_sync_power(&be, false);
    CHECK(!strcmp(val(&f, PERF_POWER, G(8), ASPM_DC), "2"));
    CHECK(f.commits == 2 && f.nj == 0);
}

static void t_unlisted_adapter_is_put_back_without_restart(void)
{
    printf("an adapter missing from the list is restored, never restarted\n");
    fake f;
    setup(&f);
    add_card(&f, G(2));
    perf_backend be = backend(&f);
    perf_adapter both[] = { { G(1), true, true }, { G(2), true, true } };
    perf_sync_adapters(&be, both, 2, true);
    CHECK(f.restarts[2] == 1);

    perf_adapter one[] = { { G(1), true, true } };
    perf_result r = perf_sync_adapters(&be, one, 1, true);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(2), "MIMOPowerSaveMode"), "0"));
    CHECK(!strcmp(val(&f, PERF_DEVICE, G(2), "WiFiDirect"), "11"));
    CHECK(f.restarts[2] == 1 && f.restarts[1] == 1);
    CHECK(r.held == 3);
}

static void t_wifi_direct_needs_no_restart(void)
{
    printf("the Wi-Fi Direct adapters go and come back without a restart\n");
    fake f;
    memset(&f, 0, sizeof f);
    f.active = G(9);
    add(&f, PERF_DEVICE, G(1), "WiFiDirect", "10", "0");   /* one already disabled */
    perf_backend be = backend(&f);
    perf_adapter on[] = { { G(1), true, true } }, off[] = { { G(1), false, true } };

    perf_result r = perf_sync(&be, true, on, 1, true);
    CHECK(r.err == WC_OK && r.changed == 1 && r.held == 1);
    CHECK(!strcmp(val(&f, PERF_DEVICE, G(1), "WiFiDirect"), "0"));
    CHECK(f.restarts[1] == 0);

    /* Each one gets its own state back, the disabled one included. */
    r = perf_sync(&be, false, off, 1, true);
    CHECK(r.err == WC_OK && r.held == 0);
    CHECK(!strcmp(val(&f, PERF_DEVICE, G(1), "WiFiDirect"), "10"));
    CHECK(f.restarts[1] == 0);

    /* Re-enabled by hand while held: theirs stands. */
    perf_sync(&be, true, on, 1, true);
    snprintf(find(&f, PERF_DEVICE, &(wc_guid){ { 1 } }, "WiFiDirect")->value, PERF_VALUE_MAX, "11");
    perf_sync(&be, false, off, 1, true);
    CHECK(!strcmp(val(&f, PERF_DEVICE, G(1), "WiFiDirect"), "11"));
    CHECK(f.nj == 0);
}

static void t_unmanaged_and_absent_adapters(void)
{
    printf("unmanaged adapters are never touched; absent ones are not restarted\n");
    fake f;
    setup(&f);
    add_card(&f, G(2));
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, false }, { G(2), false, true } };

    perf_sync_adapters(&be, ad, 2, true);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "3"));
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(2), "MIMOPowerSaveMode"), "0"));
    CHECK(f.restarts[1] == 0 && f.restarts[2] == 0);
}

static void t_session_end_does_not_restart(void)
{
    printf("putting back without restart leaves the reboot to apply it\n");
    fake f;
    setup(&f);
    perf_backend be = backend(&f);
    perf_adapter on[] = { { G(1), true, true } }, off[] = { { G(1), false, true } };
    perf_sync(&be, true, on, 1, true);

    perf_result r = perf_sync(&be, false, off, 1, false);
    CHECK(r.held == 0 && r.restarted == 0 && f.restarts[1] == 1);
    CHECK(!strcmp(val(&f, PERF_DRIVER, G(1), "MIMOPowerSaveMode"), "0"));
}

static void t_unknown_scheme(void)
{
    printf("an unreadable active scheme is an error only when holding\n");
    fake f;
    setup(&f);
    f.scheme_err = 1168;
    perf_backend be = backend(&f);

    CHECK(perf_sync_power(&be, true).err == 1168);
    CHECK(f.writes == 0);
    CHECK(perf_sync_power(&be, false).err == WC_OK);
}

static void t_a_failure_names_the_setting(void)
{
    printf("a failure names the setting and the adapter it happened on\n");
    fake f;
    setup(&f);
    find(&f, PERF_DRIVER, &(wc_guid){ { 1 } }, "uAPSDSupport")->w_err = 5;
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_sync(&be, true, ad, 1, true);
    const diag_entry *e = reported(&f, DIAG_PERF_DRIVER, DIAG_WRITE);
    CHECK(e && e->code == 5);
    CHECK(e && !strcmp(e->subject, "U-APSD (WMM power save)/1"));
    /* One property refusing does not report the others. */
    CHECK(diag_count(&f.log, DIAG_WARN) == 1);
}

static void t_a_journal_that_cannot_record_is_reported_as_such(void)
{
    printf("a journal that cannot record is reported, and nothing is changed\n");
    fake f;
    setup(&f);
    f.keep_err = 5;
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_result r = perf_sync(&be, true, ad, 1, true);
    CHECK(r.changed == 0 && r.held == 0);
    CHECK(reported(&f, DIAG_PERF_JOURNAL, DIAG_RECORD) != nullptr);
    /* Not filed against the setting: the setting was never touched. */
    CHECK(reported(&f, DIAG_PERF_DRIVER, DIAG_WRITE) == nullptr);
}

static void t_a_failed_restart_is_reported(void)
{
    printf("a restart that fails is reported as the serious thing it is\n");
    fake f;
    setup(&f);
    f.restart_err = 31;
    perf_backend be = backend(&f);
    perf_adapter ad[] = { { G(1), true, true } };

    perf_result r = perf_sync(&be, true, ad, 1, true);
    CHECK(r.restarted == 0 && r.err == 31);
    const diag_entry *e = reported(&f, DIAG_RESTART, DIAG_WRITE);
    CHECK(e && diag_sev_of(e->op, e->code) == DIAG_ERR);
}

static void t_an_unreadable_plan_is_reported_against_the_plan(void)
{
    printf("an unreadable power plan is reported against the plan itself\n");
    fake f;
    setup(&f);
    f.scheme_err = 1168;
    perf_backend be = backend(&f);

    perf_sync_power(&be, true);
    CHECK(reported(&f, DIAG_PERF_PLAN, DIAG_READ) != nullptr);
}

static void t_labels(void)
{
    printf("every setting in the preset has words for it\n");
    CHECK(!strcmp(perf_label("MIMOPowerSaveMode"), "MIMO power save"));
    CHECK(!strcmp(perf_label(WIFI_DC), "Wireless power saving, on battery"));
    CHECK(!strcmp(perf_label(ASPM_AC), "PCIe link state power management, plugged in"));
    /* Anything unknown is its own label rather than nothing. */
    CHECK(!strcmp(perf_label("SomethingElse"), "SomethingElse"));
}

int main(void)
{
    t_holds_and_records_only_what_it_changes();
    t_second_sync_does_nothing();
    t_switching_off_puts_everything_back();
    t_a_later_change_is_left_alone();
    t_reholding_keeps_the_original();
    t_absent_and_unsupported_are_skipped();
    t_startup_repairs_a_killed_run();
    t_a_failed_write_leaves_no_record();
    t_no_record_no_write();
    t_unreadable_record_is_kept();
    t_switched_plan_gets_its_values_back();
    t_unlisted_adapter_is_put_back_without_restart();
    t_wifi_direct_needs_no_restart();
    t_unmanaged_and_absent_adapters();
    t_session_end_does_not_restart();
    t_unknown_scheme();
    t_a_failure_names_the_setting();
    t_a_journal_that_cannot_record_is_reported_as_such();
    t_a_failed_restart_is_reported();
    t_an_unreadable_plan_is_reported_against_the_plan();
    t_labels();

    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}

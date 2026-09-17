/* diag.h -- status codes, what went wrong in words, and what cannot be done.
 *
 * Three jobs, all of them policy and none of them touching Windows, so they
 * are unit-tested on any host like the rest of the decision-making:
 *
 *  - The codes.  Status codes are Win32 DWORDs so a backend can pass them
 *    through untranslated; the UI decodes them with FormatMessage, and the
 *    codes at or above WC_E_APP are ours and come from wc_strerror().
 *  - The log.  Every failure a pass hits is noted with the setting it
 *    happened on, rather than collapsed into one DWORD: a switch that half
 *    worked can then say which half.  A repeat of the same failure bumps a
 *    count instead of filling the log, so a pass failing once a second stays
 *    one line, and the log is a fixed size with nothing to allocate.
 *  - The capabilities.  A switch whose service is not on this machine cannot
 *    be operated at all, so the window greys it out and says why instead of
 *    letting the user flip something that will only fail.  Which capability
 *    each switch needs, and the sentence shown when one is missing, are here.
 */
#ifndef WIFICONTROL_DIAG_H
#define WIFICONTROL_DIAG_H

/* Codes at or above WC_E_APP are ours; the rest are Win32 and are formatted
 * by the OS.  The few Win32 codes this file reasons about are named where
 * they are used, in diag.c. */
#define WC_OK                0UL
#define WC_E_ACCESS_DENIED   5UL    /* ERROR_ACCESS_DENIED */
#define WC_E_INVALID_STATE   5023UL /* ERROR_INVALID_STATE: adapter not connected */
#define WC_E_APP             0xE0000000UL
#define WC_E_VERIFY          0xE0000001UL /* wrote the value, read back something else */
#define WC_E_BADDATA         0xE0000002UL /* driver returned a short/absent buffer */
#define WC_E_NETSH           0xE0000003UL /* netsh refused to change a profile's cost */
#define WC_E_NO_JOURNAL      0xE0000004UL /* the Performance switch cannot record anything */

const char *wc_strerror(unsigned long code); /* only for WC_E_APP codes; else nullptr */

/* ------------------------------------------------------------------- log */

#define DIAG_MAX      24  /* distinct failures kept; a repeat is not a new one */
#define DIAG_SUBJ_MAX 96  /* UTF-8 bytes: the setting, adapter or network */

/* What was being done.  One per thing the user can point at, so the log reads
 * as "this switch, on this adapter" rather than as an API trace. */
typedef enum {
    DIAG_WLAN_SERVICE = 0, /* the WLAN service and its client handle */
    DIAG_ADAPTERS,         /* listing the Wi-Fi adapters */
    DIAG_BGSCAN,
    DIAG_STREAMING,
    DIAG_AUTOCONF,
    DIAG_PROFILES,         /* listing an adapter's saved networks */
    DIAG_METERED,          /* one saved network's connection cost */
    DIAG_PERF_DRIVER,      /* one driver property */
    DIAG_PERF_POWER,       /* one power plan setting */
    DIAG_PERF_DEVICE,      /* the Wi-Fi Direct adapters */
    DIAG_PERF_PLAN,        /* the active power plan itself */
    DIAG_PERF_JOURNAL,     /* the file the Performance switch puts values back from */
    DIAG_RESTART,          /* disabling and re-enabling an adapter */
    DIAG_OP_COUNT
} diag_op;

/* Which half of it failed.  A read that fails means the value is unknown; a
 * write that fails means it is unchanged; a restore that fails means it is
 * still ours, which is the one the user needs to know about. */
typedef enum {
    DIAG_READ = 0,
    DIAG_WRITE,
    DIAG_RESTORE,
    DIAG_RECORD,
    DIAG_STEP_COUNT
} diag_step;

typedef enum { DIAG_INFO = 0, DIAG_WARN, DIAG_ERR } diag_sev;

typedef struct {
    diag_op       op;
    diag_step     step;
    unsigned long code;
    char          subject[DIAG_SUBJ_MAX]; /* empty when the op says it all */
    unsigned      count;                  /* times seen, including the first */
    unsigned long first, last;            /* the caller's clock, in ms */
} diag_entry;

typedef struct {
    diag_entry e[DIAG_MAX];
    int        n;
    unsigned   dropped; /* distinct failures the log had no room for */
    unsigned   gen;     /* bumped by every note, repeats included */
} diag_log;

void diag_clear(diag_log *l);

/* Note one failure.  A code of WC_OK is ignored, so a caller can hand over
 * whatever a backend returned.  `subject` may be nullptr. */
void diag_note(diag_log *l, diag_op op, diag_step step, const char *subject,
               unsigned long code, unsigned long now);

/* The worst severity in the log, and how many entries are at least that bad. */
diag_sev diag_worst(const diag_log *l);
int      diag_count(const diag_log *l, diag_sev at_least);

/* The same count, but only of what has been seen since `since` on the caller's
 * clock: what just failed, rather than what failed ten minutes ago.  Compared
 * as a difference, so it survives the tick counter wrapping. */
int      diag_count_since(const diag_log *l, diag_sev at_least, unsigned long since);

/* The worst entry, for the one line the status bar has room for, or nullptr
 * when the log is empty. */
const diag_entry *diag_worst_entry(const diag_log *l);

const char *diag_op_name  (diag_op op);   /* "Metered", "Performance: driver property" */
const char *diag_step_name(diag_step s);  /* "Reading", "Changing", "Putting back" */
diag_sev    diag_sev_of   (diag_op op, unsigned long code);

/* One line on the likely cause and what to do about it, or nullptr when the
 * formatted error says everything there is to say. */
const char *diag_advice(diag_op op, diag_step step, unsigned long code);

/* ---------------------------------------------------------- capabilities */

typedef enum {
    WC_CAP_WLAN = 0,  /* the WLAN service, and a client handle on it */
    WC_CAP_BGSCAN,    /* write access to the background scan setting */
    WC_CAP_STREAMING, /* write access to media streaming mode */
    WC_CAP_AUTOCONF,  /* write access to Wi-Fi auto configuration */
    WC_CAP_COST,      /* the connection cost API, and netsh to write one */
    WC_CAP_POWER,     /* the active power plan and its Wi-Fi settings */
    WC_CAP_DRIVER,    /* some adapter's driver properties, writable */
    WC_CAP_JOURNAL,   /* the file the Performance switch puts values back from */
    WC_CAP_COUNT
} wc_cap;

/* Unknown until probed, which is what `checked` says: a capability nobody has
 * looked at yet is treated as present, because refusing to try is worse than
 * trying and reporting the failure. */
typedef struct { bool ok, checked; unsigned long err; } wc_cap_state;

const char *wc_cap_name(wc_cap c);
bool        wc_cap_ok  (const wc_cap_state *cap, wc_cap c);

/* What is missing without it, in one sentence, or nullptr when it is there.
 * The formatted error goes beside this, never instead of it: "error 1062" is
 * not something to act on.  This is the details window's text; the few words
 * that fit on the row beside a greyed-out switch come from
 * wc_switch_blocked(). */
const char *wc_cap_why (const wc_cap_state *cap, wc_cap c);

/* The switches, in the order the window shows them. */
typedef enum {
    WC_SW_BGSCAN = 0,
    WC_SW_STREAMING,
    WC_SW_NUCLEAR,
    WC_SW_METERED,
    WC_SW_PERF,
    WC_SW_COUNT
} wc_switch;

/* Why this switch cannot be operated on this machine, or nullptr when it can:
 * the few words that fit where its description would be, with the sentence
 * and the error left to the details window.  `cap` is WC_CAP_COUNT entries. */
const char *wc_switch_blocked(const wc_cap_state *cap, wc_switch sw);

#endif /* WIFICONTROL_DIAG_H */

/* perf.h -- the Performance switch: a fixed set of power-saving settings
 * turned to their fastest value, the Wi-Fi Direct adapters that share the
 * radio disabled, and all of it put back afterwards.
 *
 * Unlike everything the wlanapi opcodes touch, these settings are plain stored
 * values -- driver properties in the registry, indexes in the power plan and a
 * device's enabled state -- with nothing to mark a value as ours.  So this is
 * the one place with a journal: before a setting is changed, its value is
 * recorded, and the record is what gets it back.  The journal outlives the process, so a run that was
 * killed is repaired by the next one.
 *
 * Settings are only moved on explicit transitions (the switches, startup,
 * exit), never on the poll: a value the user sets in Tuning while the switch is
 * off is theirs, and restarting an adapter behind their back is not an option.
 *
 * Like wlan_core.c, this knows nothing about Windows and is unit-tested
 * against a fake backend.
 */
#ifndef WIFICONTROL_PERF_H
#define WIFICONTROL_PERF_H

#include "wlan.h"

#define PERF_NAME_MAX  96
#define PERF_VALUE_MAX 64
#define PERF_MAX_HELD  64

/* A backend returns this when the setting, adapter or power scheme does not
 * exist, which is not a failure: most cards lack most of the list. */
#define PERF_ABSENT    2UL  /* ERROR_FILE_NOT_FOUND */

typedef enum { PERF_DRIVER = 0, PERF_POWER = 1, PERF_DEVICE = 2 } perf_kind;

/* One journal record.  The owner is the adapter a driver property or device
 * belongs to, or the power scheme a power setting was written to.  A power
 * setting's name is "{subgroup}\{setting}\ac" or "...\dc".  Values are strings
 * whatever the kind: a driver's own raw value, a power value index in decimal,
 * or a device state, one '1' (enabled) or '0' (disabled) per device and just
 * "0" when all of them are disabled. */
typedef struct {
    perf_kind kind;
    wc_guid   owner;
    char      name [PERF_NAME_MAX];
    char      value[PERF_VALUE_MAX]; /* what it was before we changed it */
} perf_entry;

typedef struct perf_backend {
    void *ctx;
    /* The live value, and whether `target` is one of the values it accepts. */
    unsigned long (*read)   (void *ctx, perf_kind k, const wc_guid *owner, const char *name,
                             const char *target, char *live, int cap, int *has_target);
    unsigned long (*write)  (void *ctx, perf_kind k, const wc_guid *owner, const char *name,
                             const char *value);
    unsigned long (*scheme) (void *ctx, wc_guid *active);
    /* Power writes to the active scheme take effect only once committed. */
    unsigned long (*commit) (void *ctx);
    /* Disable and re-enable, so the driver re-reads its properties. */
    unsigned long (*restart)(void *ctx, const wc_guid *adapter);

    int           (*held)   (void *ctx, perf_entry *out, int cap);
    unsigned long (*keep)   (void *ctx, const perf_entry *e);   /* add or replace */
    unsigned long (*forget) (void *ctx, const perf_entry *e);
} perf_backend;

typedef struct {
    wc_guid guid;
    bool    want;    /* hold the preset on this adapter */
    bool    present; /* can be restarted */
} perf_adapter;

typedef struct {
    int           held;      /* journal records left afterwards */
    int           changed;   /* values written, either way */
    int           restarted; /* adapters restarted */
    unsigned long err;       /* the first failure, or WC_OK */
} perf_result;

/* Power: hold the preset on the active scheme when wanted, and put back
 * everything recorded against any other scheme, or all of it when not. */
perf_result perf_sync_power(const perf_backend *be, bool want);

/* Driver properties and devices: hold the preset on the adapters that want it
 * and put back the rest, including adapters missing from the list.  A present
 * adapter whose properties changed is restarted when `restart` is set;
 * otherwise the change waits for the next restart or reboot.  Devices take
 * effect at once, with no restart. */
perf_result perf_sync_adapters(const perf_backend *be, const perf_adapter *ad, int n,
                               bool restart);

/* Both of the above. */
perf_result perf_sync(const perf_backend *be, bool power, const perf_adapter *ad, int n,
                      bool restart);

#endif

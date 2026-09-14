/* tune.h -- user-chosen device and power settings.
 *
 * Everything here is presented as a dropdown of the values the system itself
 * declares, and nothing is written until the user presses Apply.  The app
 * never picks a value, never recommends one, and never records what a setting
 * used to be: the registry and the power scheme are the state.  Undo is
 * choosing the other value from the same dropdown.
 */
#ifndef WIFICONTROL_TUNE_H
#define WIFICONTROL_TUNE_H

#include "wlan.h"
#include <windows.h>

#define TUNE_MAX_SETTINGS 128
#define TUNE_MAX_OPTIONS   32
#define TUNE_TEXT_MAX      96
#define TUNE_KEY_MAX       64

typedef enum { TUNE_DRIVER = 0, TUNE_POWER = 1 } tune_src;

typedef struct {
    wchar_t raw  [TUNE_KEY_MAX];  /* driver: the string written to the registry */
    unsigned long index;          /* power:  the value index                    */
    wchar_t label[TUNE_TEXT_MAX]; /* what the system calls this choice          */
} tune_option;

typedef struct {
    tune_src    src;
    wchar_t     group[TUNE_TEXT_MAX];
    wchar_t     name [TUNE_TEXT_MAX];
    tune_option opt[TUNE_MAX_OPTIONS];
    int         n_opt;
    int         cur;    /* index into opt of the live value, -1 if unrecognised */
    int         sel;    /* index the user has chosen; equals cur until they act */
    const wchar_t *help; /* one line on what the setting does, or nullptr if unknown */

    /* provider-private addressing */
    wchar_t     value_name[TUNE_KEY_MAX]; /* driver: registry value under the instance key */
    GUID        sub, setting;             /* power:  subgroup and setting */
    bool        on_battery;               /* power:  DC rather than AC */
} tune_setting;

typedef struct {
    tune_setting s[TUNE_MAX_SETTINGS];
    int          n;
    wchar_t      instance_key[MAX_PATH];  /* driver: resolved class-instance path */
    bool         have_instance;
} tune_list;

/* Collect every setting that offers a fixed set of choices.  Settings that are
 * free ranges rather than a list are skipped: they are not dropdowns. */
void tune_collect_driver(tune_list *l, const wc_guid *adapter);
void tune_collect_power (tune_list *l);

unsigned long tune_write_driver(const tune_list *l, const tune_setting *s);
unsigned long tune_write_power (const tune_setting *s);
unsigned long tune_power_commit(void);

/* Write only the settings whose dropdown differs from what was read.  Returns
 * the number written; *first_err is the first failure, or 0. */
int  tune_apply(tune_list *l, unsigned long *first_err);

/* Driver properties are read by the miniport at init, so a change sits in the
 * registry until the adapter restarts.  Explicit, never automatic: this drops
 * the Wi-Fi link for a few seconds. */
unsigned long tune_restart_adapter(const tune_list *l);

void tune_dialog(HWND parent, const wc_guid *adapter, const wchar_t *adapter_name);

#endif

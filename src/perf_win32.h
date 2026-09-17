/* perf_win32.h -- the Performance switch's Windows backend. */
#ifndef WIFICONTROL_PERF_WIN32_H
#define WIFICONTROL_PERF_WIN32_H

#include "perf.h"
#include <wchar.h>

/* Driver properties through tune_driver.c, power settings through powrprof,
 * and the journal as the [held] section of an ini file at `journal`, which
 * must outlive the backend. */
void perf_win32_backend(perf_backend *be, const wchar_t *journal);

/* The two things the switch cannot work without, checked before it is offered
 * rather than discovered half way through: the journal it puts values back
 * from, and an active power plan.  WC_OK, or the reason it is unavailable.
 * The driver properties are the third, and are per adapter: tune_driver_check. */
unsigned long perf_win32_journal_check(const wchar_t *journal);
unsigned long perf_win32_power_check(void);

#endif

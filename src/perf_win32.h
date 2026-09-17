/* perf_win32.h -- the Performance switch's Windows backend. */
#ifndef WIFICONTROL_PERF_WIN32_H
#define WIFICONTROL_PERF_WIN32_H

#include "perf.h"
#include <wchar.h>

/* Driver properties through tune_driver.c, power settings through powrprof,
 * and the journal as the [held] section of an ini file at `journal`, which
 * must outlive the backend. */
void perf_win32_backend(perf_backend *be, const wchar_t *journal);

#endif

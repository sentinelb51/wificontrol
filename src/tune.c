/* tune.c -- apply what the user chose, and nothing else. */
#include "tune.h"

int tune_apply(tune_list *l, unsigned long *first_err)
{
    int written = 0;
    bool wrote_power = false;
    *first_err = 0;

    for (int i = 0; i < l->n; ++i) {
        tune_setting *s = &l->s[i];

        /* Untouched dropdowns are left alone: no rewriting a value with
         * itself, and no notion of what it was before. */
        if (s->sel < 0 || s->sel == s->cur) continue;

        unsigned long e = (s->src == TUNE_DRIVER) ? tune_write_driver(l, s)
                                                  : tune_write_power(s);
        if (e != ERROR_SUCCESS) {
            if (!*first_err) *first_err = e;
            continue;
        }
        if (s->src == TUNE_POWER) wrote_power = true;
        s->cur = s->sel;
        written++;
    }

    if (wrote_power) {
        unsigned long e = tune_power_commit();
        if (e != ERROR_SUCCESS && !*first_err) *first_err = e;
    }
    return written;
}

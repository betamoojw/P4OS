/*
 * P4OS - Python: what MicroPython's time module asks of the port, included
 * by extmod/modtime.c (MICROPY_PY_TIME_INCLUDEFILE).
 *
 * The clock is the system's: the network sets it and the time zone is the
 * one in Settings. time.time() counts seconds since 1970; localtime() with
 * no argument is the local time, with one it is MicroPython's (UTC).
 */
#include <time.h>

#include "shared/timeutils/timeutils.h"

static void mp_time_localtime_get(timeutils_struct_time_t *tm)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    tm->tm_year = lt.tm_year + 1900;
    tm->tm_mon = lt.tm_mon + 1;
    tm->tm_mday = lt.tm_mday;
    tm->tm_hour = lt.tm_hour;
    tm->tm_min = lt.tm_min;
    tm->tm_sec = lt.tm_sec;
    tm->tm_wday = (lt.tm_wday + 6) % 7;     /* Python's weeks start on Monday */
    tm->tm_yday = lt.tm_yday + 1;
}

static mp_obj_t mp_time_time_get(void)
{
    return mp_obj_new_int_from_ll((long long)time(NULL));
}

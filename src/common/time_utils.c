/*
 * time_utils.c - Monotone Zeitbasis: aktuelle Zeit, Schlafen bis zu einem
 * absoluten Zeitpunkt und Alter eines Zeitstempels.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#include "time_utils.h"

#include <errno.h>
#include <time.h>

uint64_t time_now_us(void)
{
    struct timespec ts;
    uint64_t now = 0U;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        now = ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
    }
    return now;
}

void time_sleep_until_us(uint64_t deadline_us)
{
    struct timespec ts;
    int32_t rc;

    ts.tv_sec = (time_t)(deadline_us / 1000000ULL);
    ts.tv_nsec = (int32_t)((deadline_us % 1000000ULL) * 1000ULL);

    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (rc == EINTR);
}

uint64_t time_age_us(uint64_t now_us, uint64_t timestamp_us)
{
    uint64_t age = UINT64_MAX;

    if ((now_us != 0U) && (timestamp_us != 0U) && (now_us >= timestamp_us)) {
        age = now_us - timestamp_us;
    }
    return age;
}

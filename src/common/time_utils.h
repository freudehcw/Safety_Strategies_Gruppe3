/*
 * time_utils.h - Monotone Zeitbasis (CLOCK_MONOTONIC) fuer Timeouts,
 * Datenalter und zyklische Threads.
 */
#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <stdint.h>

#define TIME_US_PER_MS ((uint64_t)1000U)

/* Aktuelle monotone Zeit in Mikrosekunden; 0 bei Fehler. */
uint64_t time_now_us(void);

/* Schlaeft bis zum absoluten monotonen Zeitpunkt (Mikrosekunden). */
void time_sleep_until_us(uint64_t deadline_us);

/*
 * Alter eines Zeitstempels. Liegt der Zeitstempel in der Zukunft oder ist
 * eine der Zeiten 0 (Fehler/nie gesetzt), wird UINT64_MAX geliefert, damit
 * der Aufrufer die Daten als veraltet behandelt (sichere Seite).
 */
uint64_t time_age_us(uint64_t now_us, uint64_t timestamp_us);

#endif /* TIME_UTILS_H */

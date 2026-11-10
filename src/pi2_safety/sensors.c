/*
 * sensors.c - Sensor-Threads (HC-SR04) mit den Pruefungen P1 und P2 sowie
 * Fehlereinspeisung fuer den Testmodus.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#include "sensors.h"

#include <stdatomic.h>
#include <stddef.h>

#include "config.h"
#include "hal.h"
#include "pi2_config.h"
#include "protocol.h"
#include "shared.h"
#include "time_utils.h"

static _Atomic uint64_t g_start_us; /* 0 = nicht festgelegt */

#ifdef SSM_TESTMODE
static _Atomic uint32_t g_force_mm_plus1[SENSOR_COUNT]; /* 0 = keine Vorgabe, sonst mm + 1 */
static _Atomic uint32_t g_block_ms[SENSOR_COUNT];
#endif

void sensor_check_init(sensor_check_t *ctx)
{
    if (ctx != NULL) {
        ctx->have_prev = false;
        ctx->prev_mm = 0U;
        ctx->prev_ts_us = 0U;
    }
}

bool sensor_check_sample(sensor_check_t *ctx, bool have_echo, uint32_t distance_mm,
                         uint64_t timestamp_us, uint16_t *out_mm)
{
    bool valid = false;

    if ((ctx != NULL) && (out_mm != NULL)) {
        /* P1: Echo vorhanden und Messbereich 2..400 cm */
        bool in_range = have_echo && (distance_mm >= CFG_DIST_MIN_MM) &&
                        (distance_mm <= CFG_DIST_MAX_MM);

        if (!in_range) {
            ctx->have_prev = false;
        } else {
            if (!ctx->have_prev) {
                /* Kein Bezugswert: P2 wird nicht geprueft, die Messung gilt. */
                valid = true;
            } else if (timestamp_us > ctx->prev_ts_us) {
                /* P2: |d_k - d_(k-1)| / (t_k - t_(k-1)) <= 2 mm/ms */
                uint64_t dt_us = timestamp_us - ctx->prev_ts_us;
                uint64_t delta_mm = (distance_mm >= (uint32_t)ctx->prev_mm)
                                        ? ((uint64_t)distance_mm - (uint64_t)ctx->prev_mm)
                                        : ((uint64_t)ctx->prev_mm - (uint64_t)distance_mm);

                valid = (delta_mm * TIME_US_PER_MS) <= ((uint64_t)CFG_MAX_RATE_MM_PER_MS * dt_us);
            } else {
                /* Zeit nicht fortgeschritten: P2 nicht berechenbar, ungueltig */
            }
            /* Referenz ist immer die letzte Messung im Messbereich; dadurch
             * erholt sich P2 nach einem echten Sprung von selbst. */
            ctx->have_prev = true;
            ctx->prev_mm = (uint16_t)distance_mm;
            ctx->prev_ts_us = timestamp_us;
        }

        *out_mm = valid ? (uint16_t)distance_mm : (uint16_t)DIST_INVALID_MM;
    }
    return valid;
}

void *sensor_thread(void *arg)
{
    const uint32_t *idx_ptr = (const uint32_t *)arg;

    if ((idx_ptr != NULL) && (*idx_ptr < SENSOR_COUNT)) {
        const uint32_t idx = *idx_ptr;
        const uint64_t period_us = (uint64_t)SENSOR_PERIOD_MS * TIME_US_PER_MS;
        const uint64_t window_us = (uint64_t)SENSOR_ECHO_WINDOW_MS * TIME_US_PER_MS;
        sensor_check_t check;
        sensor_data_t data = { (uint16_t)DIST_INVALID_MM, false, 0U, 0U };
        /* Beide Threads rechnen vom selben Startzeitpunkt aus; dadurch ist der
         * Versatz von 30 ms unabhaengig davon, wann die Threads anlaufen. */
        uint64_t start_us = atomic_load(&g_start_us);
        uint64_t next_us = ((start_us != 0U) ? start_us : time_now_us()) +
                           ((uint64_t)idx * (uint64_t)SENSOR_PHASE_OFFSET_MS * TIME_US_PER_MS);

        sensor_check_init(&check);

        while (shared_running()) {
            uint64_t t0_us;
            uint32_t echo_us = 0U;
            uint32_t raw_mm = 0U;
            bool have_echo = false;

            time_sleep_until_us(next_us);

#ifdef SSM_TESTMODE
            {
                uint32_t block_ms = atomic_exchange(&g_block_ms[idx], 0U);
                if (block_ms > 0U) {
                    /* TC-04: Thread haengt, Daten werden nicht aktualisiert */
                    time_sleep_until_us(time_now_us() + ((uint64_t)block_ms * TIME_US_PER_MS));
                }
            }
#endif

            /* Zeitstempel = Beginn der Messung (konservativ fuer P4). */
            t0_us = time_now_us();
            if (hal_sonar_trigger(idx) == 0) {
                time_sleep_until_us(t0_us + window_us);
                have_echo = hal_sonar_result(idx, &echo_us);
            }
            if (have_echo) {
                raw_mm = (uint32_t)(((uint64_t)echo_us * SONAR_MM_NUM) / SONAR_MM_DEN);
            }

#ifdef SSM_TESTMODE
            {
                uint32_t forced_plus1 = atomic_load(&g_force_mm_plus1[idx]);
                if (forced_plus1 > 0U) {
                    raw_mm = forced_plus1 - 1U;
                    have_echo = true;
                }
            }
#endif

            data.valid = sensor_check_sample(&check, have_echo, raw_mm, t0_us, &data.distance_mm);
            data.timestamp_us = t0_us;
            data.seq++;
            if (data.seq == 0U) {
                data.seq = 1U; /* 0 ist fuer "nie gemessen" reserviert */
            }
            shared_sensor_write(idx, &data);

            next_us += period_us;
            {
                uint64_t now_us = time_now_us();
                if (next_us <= now_us) {
                    next_us = now_us + period_us; /* Zyklus verpasst: neu aufsetzen */
                }
            }
        }
    }
    return NULL;
}

void sensors_set_start_time(uint64_t start_us)
{
    atomic_store(&g_start_us, start_us);
}

#ifdef SSM_TESTMODE
void sensors_test_force(uint32_t idx, int32_t mm)
{
    if (idx < SENSOR_COUNT) {
        atomic_store(&g_force_mm_plus1[idx], (mm < 0) ? 0U : ((uint32_t)mm + 1U));
    }
}

void sensors_test_block(uint32_t idx, uint32_t ms)
{
    if (idx < SENSOR_COUNT) {
        atomic_store(&g_block_ms[idx], ms);
    }
}
#endif

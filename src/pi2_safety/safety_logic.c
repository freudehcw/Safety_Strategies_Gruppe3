/*
 * safety_logic.c - Safety-Entscheidung je Zyklus (safety_evaluate) und der
 * Safety-Logik-Thread, der sie alle 50 ms ausfuehrt.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#include "safety_logic.h"

#include <stddef.h>

#include "config.h"
#include "protocol.h"
#include "time_utils.h"
#include "watchdog.h"

void safety_ctx_init(safety_ctx_t *ctx)
{
    if (ctx != NULL) {
        uint32_t i;

        for (i = 0U; i < SENSOR_COUNT; i++) {
            ctx->last_seq[i] = 0U;
        }
        ctx->mismatch_cycles = 0U;
        ctx->shutdown = false;
    }
}

uint16_t safety_distance_mm(uint8_t level)
{
    uint16_t distance;

    switch (level) {
    case LEVEL_LOW:
        distance = (uint16_t)CFG_S_LOW_MM;
        break;
    case LEVEL_MEDIUM:
        distance = (uint16_t)CFG_S_MEDIUM_MM;
        break;
    case LEVEL_HIGH:
        distance = (uint16_t)CFG_S_HIGH_MM;
        break;
    default:
        distance = 0U; /* ungueltige Stufe: kein Default-Betrieb */
        break;
    }
    return distance;
}

/*
 * P3: Quervergleich. Bewertet wird einmal je Messzyklus, d. h. sobald beide
 * Sensoren einen neuen, gueltigen Messwert geliefert haben. Zyklen mit einem
 * ungueltigen Messwert lassen den Zaehler unveraendert (sie fuehren ohnehin
 * zu FEHLER / SENSOR).
 */
static void update_cross_check(safety_ctx_t *ctx, const sensor_data_t sensors[SENSOR_COUNT])
{
    if (sensors[0].valid && sensors[1].valid &&
        (sensors[0].seq != ctx->last_seq[0]) && (sensors[1].seq != ctx->last_seq[1])) {
        uint16_t d0 = sensors[0].distance_mm;
        uint16_t d1 = sensors[1].distance_mm;
        uint16_t diff = (d0 >= d1) ? (uint16_t)(d0 - d1) : (uint16_t)(d1 - d0);

        ctx->last_seq[0] = sensors[0].seq;
        ctx->last_seq[1] = sensors[1].seq;

        if (diff > (uint16_t)CFG_SENSOR_DEV_MAX_MM) {
            if (ctx->mismatch_cycles < (uint8_t)CFG_SENSOR_DEV_CYCLES) {
                ctx->mismatch_cycles++;
            }
        } else {
            ctx->mismatch_cycles = 0U;
        }
    }
}

void safety_evaluate(safety_ctx_t *ctx, const sensor_data_t sensors[SENSOR_COUNT],
                     const comm_data_t *comm, bool local_stop, uint64_t now_us,
                     safety_decision_t *decision)
{
    if (decision != NULL) {
        /* Grundstellung: Fehler, Motor AUS. Freigabe nur im letzten Zweig. */
        decision->state = STATE_FEHLER;
        decision->error = ERR_SENSOR;
        decision->motor_level = LEVEL_OFF;

        if ((ctx != NULL) && (sensors != NULL) && (comm != NULL)) {
            const uint64_t comm_timeout_us = (uint64_t)CFG_COMM_TIMEOUT_MS * TIME_US_PER_MS;
            const uint64_t max_age_us = (uint64_t)CFG_SENSOR_MAX_AGE_MS * TIME_US_PER_MS;
            bool sensors_ok;
            bool comm_ok;
            bool fresh;

            if (comm->stop || local_stop) {
                ctx->shutdown = true;
            }
            update_cross_check(ctx, sensors);

            sensors_ok = sensors[0].valid && sensors[1].valid &&
                         (ctx->mismatch_cycles < (uint8_t)CFG_SENSOR_DEV_CYCLES);
            comm_ok = comm->have_msg && proto_level_valid(comm->level) &&
                      (time_age_us(now_us, comm->last_valid_us) <= comm_timeout_us);
            fresh = (time_age_us(now_us, sensors[0].timestamp_us) <= max_age_us) &&
                    (time_age_us(now_us, sensors[1].timestamp_us) <= max_age_us);

            if (ctx->shutdown) {
                decision->state = STATE_SHUTDOWN;
                decision->error = ERR_NONE;
            } else if (!sensors_ok) {
                decision->error = ERR_SENSOR;
            } else if (!comm_ok) {
                decision->error = ERR_KOMMUNIKATION;
            } else if (!fresh) {
                decision->error = ERR_DATENALTER;
            } else {
                uint16_t s_mm = safety_distance_mm(comm->level);
                uint16_t d_min = (sensors[0].distance_mm <= sensors[1].distance_mm)
                                     ? sensors[0].distance_mm
                                     : sensors[1].distance_mm;

                if (s_mm == 0U) {
                    decision->error = ERR_KOMMUNIKATION;
                } else if (d_min < s_mm) {
                    decision->state = STATE_OBJEKT_STOPP;
                    decision->error = ERR_NONE;
                } else {
                    decision->state = STATE_BETRIEB;
                    decision->error = ERR_NONE;
                    decision->motor_level = comm->level;
                }
            }
        }
    }
}

bool safety_motor_up_to_date(uint32_t last_generation, const motor_status_t *status)
{
    bool up_to_date = false;

    if ((status != NULL) && (status->applied_level != MOTOR_LEVEL_UNKNOWN)) {
        /* uint32-Differenz ist auch beim Ueberlauf des Zaehlers korrekt */
        uint32_t lag = last_generation - status->generation;

        up_to_date = (lag <= WATCHDOG_MAX_MOTOR_LAG);
    }
    return up_to_date;
}

void *safety_thread(void *arg)
{
    const uint64_t period_us = (uint64_t)CFG_SAFETY_PERIOD_MS * TIME_US_PER_MS;
    safety_ctx_t ctx;
    safety_state_t state = { STATE_INIT, ERR_NONE, LEVEL_OFF, 0U, 0U, 0U, 0U };
    uint64_t next_us = time_now_us();

    (void)arg;
    safety_ctx_init(&ctx);

    /* Keine blockierenden Netzwerk-, Logging- oder GPIO-Aufrufe in dieser Schleife;
     * erlaubt sind Mutex-Kopien und das Bedienen des Watchdogs. */
    while (shared_running()) {
        sensor_data_t sensors[SENSOR_COUNT];
        comm_data_t comm;
        safety_decision_t decision;
        uint64_t now_us;
        uint32_t i;

        /* Watchdog nur bedienen, wenn die Motorsteuerung die letzte Freigabe
         * umgesetzt hat. Haengt einer der beiden Threads, setzt der Watchdog
         * den Pi zurueck. Kann er nicht bedient werden: kontrolliert beenden. */
        if (watchdog_is_open()) {
            motor_status_t motor;

            shared_motor_read(&motor);
            if (safety_motor_up_to_date(state.generation, &motor) && (watchdog_kick() != 0)) {
                shared_request_stop();
            }
        }

        for (i = 0U; i < SENSOR_COUNT; i++) {
            shared_sensor_read(i, &sensors[i]);
        }
        shared_comm_read(&comm);
        /* Zeit nach den Snapshots nehmen: Zeitstempel liegen nie in der Zukunft. */
        now_us = time_now_us();

        safety_evaluate(&ctx, sensors, &comm, shared_stop_requested(), now_us, &decision);

        if ((decision.state != state.state) || (decision.error != state.error) ||
            (decision.motor_level != state.motor_level)) {
            state.state_since_us = now_us;
            state.transitions++;
        }
        state.state = decision.state;
        state.error = decision.error;
        state.motor_level = decision.motor_level;
        state.updated_us = now_us;
        state.generation++;
        shared_state_write(&state);

        next_us += period_us;
        now_us = time_now_us();
        if (next_us <= now_us) {
            next_us = now_us + period_us; /* Zyklus ueberschritten: neu aufsetzen */
        }
        time_sleep_until_us(next_us);
    }
    return NULL;
}

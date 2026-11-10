#include "motor.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "hal.h"
#include "pi2_config.h"
#include "protocol.h"
#include "shared.h"
#include "time_utils.h"

static _Atomic uint32_t g_off_failures;

/* Motor AUS anfordern; ein Fehler wird gezaehlt und im naechsten Zyklus
 * erneut versucht (die Abschaltung wird zyklisch wiederholt). */
static void motor_off(void)
{
    if (hal_motor_off() != 0) {
        (void)atomic_fetch_add(&g_off_failures, 1U);
    }
}

uint32_t motor_off_failures(void)
{
    return atomic_load(&g_off_failures);
}

uint32_t motor_duty_permille(uint8_t level)
{
    uint32_t duty;

    switch (level) {
    case LEVEL_LOW:
        duty = MOTOR_DUTY_LOW_PM;
        break;
    case LEVEL_MEDIUM:
        duty = MOTOR_DUTY_MEDIUM_PM;
        break;
    case LEVEL_HIGH:
        duty = MOTOR_DUTY_HIGH_PM;
        break;
    default:
        duty = 0U; /* unbekannte Stufe: kein Default-Betrieb, Motor AUS */
        break;
    }
    return duty;
}

void *motor_thread(void *arg)
{
    const uint64_t refresh_us = (uint64_t)MOTOR_REFRESH_MS * TIME_US_PER_MS;
    const uint64_t alive_us = (uint64_t)SAFETY_ALIVE_TIMEOUT_MS * TIME_US_PER_MS;
    uint32_t generation = 0U;
    uint8_t applied = LEVEL_OFF;

    (void)arg;
    motor_off();

    while (shared_running()) {
        safety_state_t state;
        uint64_t now_us;
        uint32_t duty = 0U;

        /* Wird von der Safety-Logik nach jedem Zyklus geweckt. Der Rueckgabewert
         * (neuer Zustand ja/nein) ist keine Fehlerinformation: bewertet wird
         * in jedem Fall der gelieferte Zustand samt Alter. */
        (void)shared_state_wait(generation, time_now_us() + refresh_us, &state);
        generation = state.generation;
        now_us = time_now_us();

        /* Freigabe nur im Zustand BETRIEB und nur, solange die Safety-Logik
         * ihren Zustand zyklisch aktualisiert. */
        if ((state.state == STATE_BETRIEB) &&
            (time_age_us(now_us, state.updated_us) <= alive_us)) {
            duty = motor_duty_permille(state.motor_level);
        }

        if (duty == 0U) {
            motor_off();
            applied = LEVEL_OFF;
        } else if (state.motor_level != applied) {
            if (hal_motor_run(duty) == 0) {
                applied = state.motor_level;
            } else {
                motor_off();
                applied = LEVEL_OFF;
            }
        } else {
            /* Stufe unveraendert: Ausgang bleibt */
        }
    }

    motor_off();
    return NULL;
}

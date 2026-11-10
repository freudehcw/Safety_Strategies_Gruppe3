/*
 * hal_sim.c - Simulierte Hardware fuer Entwicklung und Tests ohne Raspberry Pi.
 * Der Abstand je Sensor wird ueber hal_test_set_distance_mm() vorgegeben, die
 * Motoransteuerung wird nur protokolliert.
 */
#include "hal.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>

#include "pi2_config.h"
#include "types.h"

#define SIM_DEFAULT_DISTANCE_MM 1000U

static _Atomic uint32_t g_distance_mm[SENSOR_COUNT];
static _Atomic uint32_t g_motor_duty; /* 0 = AUS */

int32_t hal_init(void)
{
    uint32_t i;

    for (i = 0U; i < SENSOR_COUNT; i++) {
        atomic_store(&g_distance_mm[i], SIM_DEFAULT_DISTANCE_MM);
    }
    atomic_store(&g_motor_duty, 0U);
    return 0;
}

int32_t hal_shutdown(void)
{
    atomic_store(&g_motor_duty, 0U);
    return 0;
}

const char_t *hal_name(void)
{
    return "Simulation";
}

int32_t hal_motor_off(void)
{
    if (atomic_exchange(&g_motor_duty, 0U) != 0U) {
        (void)fprintf(stderr, "[sim] Motor AUS (STBY = LOW, PWM = 0)\n");
    }
    return 0;
}

void hal_motor_emergency_off(void)
{
    atomic_store(&g_motor_duty, 0U);
}

int32_t hal_motor_run(uint32_t duty_permille)
{
    int32_t result = -1;

    if ((duty_permille > 0U) && (duty_permille <= 1000U)) {
        if (atomic_exchange(&g_motor_duty, duty_permille) != duty_permille) {
            (void)fprintf(stderr, "[sim] Motor AN, PWM = %" PRIu32 " Promille\n", duty_permille);
        }
        result = 0;
    }
    return result;
}

int32_t hal_sonar_trigger(uint32_t idx)
{
    return (idx < SENSOR_COUNT) ? 0 : -1;
}

bool hal_sonar_result(uint32_t idx, uint32_t *echo_us)
{
    bool have_echo = false;

    if ((idx < SENSOR_COUNT) && (echo_us != NULL)) {
        uint32_t mm = atomic_load(&g_distance_mm[idx]);

        if (mm > 0U) {
            /* aufrunden, damit die Rueckrechnung wieder genau mm ergibt */
            *echo_us = ((mm * SONAR_MM_DEN) + (SONAR_MM_NUM - 1U)) / SONAR_MM_NUM;
            have_echo = true;
        }
    }
    return have_echo;
}

int32_t hal_test_set_distance_mm(uint32_t idx, uint32_t distance_mm)
{
    int32_t result = -1;

    if ((idx < SENSOR_COUNT) && (distance_mm <= 100000U)) {
        atomic_store(&g_distance_mm[idx], distance_mm);
        result = 0;
    }
    return result;
}

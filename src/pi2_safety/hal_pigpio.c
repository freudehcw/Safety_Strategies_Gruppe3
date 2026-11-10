/*
 * hal_pigpio.c - Reale Hardware ueber pigpio (Raspberry Pi 4B, root noetig).
 *
 * HC-SR04: Trigger-Puls ueber gpioTrigger(), Echo-Dauer ueber Flanken-Callbacks
 * mit den Mikrosekunden-Ticks von pigpio (unabhaengig vom Scheduling dieses
 * Programms).
 * TB6612FNG Kanal A: Hardware-PWM an PWMA, Richtung ueber AIN1/AIN2,
 * STBY als Enable.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#include "hal.h"

#include <stdatomic.h>
#include <stddef.h>

#include <pigpio.h>

#include "pi2_config.h"
#include "types.h"

#define PWM_DUTY_PER_PERMILLE 1000U /* pigpio: Tastverhaeltnis 0..1000000 */

static const uint32_t g_trig_pin[SENSOR_COUNT] = { PIN_SONAR1_TRIG, PIN_SONAR2_TRIG };
static const uint32_t g_echo_pin[SENSOR_COUNT] = { PIN_SONAR1_ECHO, PIN_SONAR2_ECHO };
static const uint32_t g_sensor_index[SENSOR_COUNT] = { 0U, 1U };

/* Vom pigpio-Callback-Thread geschrieben, vom Sensor-Thread gelesen. */
static _Atomic uint32_t g_rise_tick[SENSOR_COUNT];
static _Atomic uint32_t g_echo_us[SENSOR_COUNT];
static _Atomic int32_t g_echo_phase[SENSOR_COUNT]; /* 0 = warten, 1 = Echo laeuft, 2 = fertig */

static atomic_bool g_initialised;

static void echo_callback(int gpio, int level, uint32_t tick, void *userdata)
{
    const uint32_t *idx_ptr = (const uint32_t *)userdata;
    (void)gpio;

    if ((idx_ptr != NULL) && (*idx_ptr < SENSOR_COUNT)) {
        uint32_t idx = *idx_ptr;

        if ((level == 1) && (atomic_load(&g_echo_phase[idx]) == 0)) {
            atomic_store(&g_rise_tick[idx], tick);
            atomic_store(&g_echo_phase[idx], 1);
        } else if ((level == 0) && (atomic_load(&g_echo_phase[idx]) == 1)) {
            /* uint32-Differenz ist auch beim Ueberlauf des Ticks korrekt */
            atomic_store(&g_echo_us[idx], tick - atomic_load(&g_rise_tick[idx]));
            atomic_store(&g_echo_phase[idx], 2);
        } else {
            /* Watchdog-Meldung oder Flanke ausserhalb einer Messung: ignorieren */
        }
    }
}

int32_t hal_init(void)
{
    int32_t result = -1;
    uint32_t i;

    /* Eigene Signal-Handler behalten (kontrolliertes Beenden, Motor AUS). */
    if ((gpioCfgSetInternals(gpioCfgGetInternals() | PI_CFG_NOSIGHANDLER) == 0) &&
        (gpioInitialise() >= 0)) {
        int32_t rc = 0;

        /* Zuerst den sicheren Zustand herstellen (SAF-02). */
        rc |= gpioSetMode(PIN_MOTOR_STBY, PI_OUTPUT);
        rc |= gpioWrite(PIN_MOTOR_STBY, 0);
        rc |= gpioSetMode(PIN_MOTOR_AIN1, PI_OUTPUT);
        rc |= gpioWrite(PIN_MOTOR_AIN1, 0);
        rc |= gpioSetMode(PIN_MOTOR_AIN2, PI_OUTPUT);
        rc |= gpioWrite(PIN_MOTOR_AIN2, 0);
        rc |= gpioHardwarePWM(PIN_MOTOR_PWMA, MOTOR_PWM_FREQ_HZ, 0U);

        for (i = 0U; i < SENSOR_COUNT; i++) {
            atomic_store(&g_echo_phase[i], 0);
            rc |= gpioSetMode(g_trig_pin[i], PI_OUTPUT);
            rc |= gpioWrite(g_trig_pin[i], 0);
            rc |= gpioSetMode(g_echo_pin[i], PI_INPUT);
            rc |= gpioSetPullUpDown(g_echo_pin[i], PI_PUD_OFF);
            rc |= gpioSetAlertFuncEx(g_echo_pin[i], echo_callback,
                                     (void *)&g_sensor_index[i]);
        }

        if (rc == 0) {
            atomic_store(&g_initialised, true);
            result = 0;
        } else {
            hal_motor_emergency_off();
            gpioTerminate();
        }
    }
    return result;
}

int32_t hal_shutdown(void)
{
    int32_t rc = 0;

    if (atomic_exchange(&g_initialised, false)) {
        uint32_t i;

        rc |= hal_motor_off();
        for (i = 0U; i < SENSOR_COUNT; i++) {
            rc |= gpioSetAlertFuncEx(g_echo_pin[i], NULL, NULL);
        }
        gpioTerminate();
    }
    return (rc == 0) ? 0 : -1;
}

const char_t *hal_name(void)
{
    return "pigpio";
}

bool hal_is_simulation(void)
{
    return false;
}

void hal_motor_emergency_off(void)
{
    /* Dir 4.7: bewusst ungeprueft - Aufruf aus Signal-Handler bzw. Fehlerpfad,
     * in dem keine weitere Reaktion moeglich ist. */
    (void)gpioWrite(PIN_MOTOR_STBY, 0);
}

int32_t hal_motor_off(void)
{
    int32_t rc = 0;

    /* Zuerst Enable wegnehmen, dann PWM und Richtung zuruecksetzen. Alle
     * Ausgaenge werden auch dann gesetzt, wenn ein vorheriger Aufruf scheitert. */
    rc |= gpioWrite(PIN_MOTOR_STBY, 0);
    rc |= gpioHardwarePWM(PIN_MOTOR_PWMA, MOTOR_PWM_FREQ_HZ, 0U);
    rc |= gpioWrite(PIN_MOTOR_AIN1, 0);
    rc |= gpioWrite(PIN_MOTOR_AIN2, 0);
    return (rc == 0) ? 0 : -1;
}

int32_t hal_motor_run(uint32_t duty_permille)
{
    int32_t result = -1;

    if ((duty_permille > 0U) && (duty_permille <= 1000U)) {
        int32_t rc = 0;

        rc |= gpioWrite(PIN_MOTOR_AIN1, 1);
        rc |= gpioWrite(PIN_MOTOR_AIN2, 0);
        rc |= gpioHardwarePWM(PIN_MOTOR_PWMA, MOTOR_PWM_FREQ_HZ,
                              duty_permille * PWM_DUTY_PER_PERMILLE);
        if (rc == 0) {
            rc = gpioWrite(PIN_MOTOR_STBY, 1);
        }
        if (rc == 0) {
            result = 0;
        } else if (hal_motor_off() != 0) {
            hal_motor_emergency_off();
        } else {
            /* Motor ist AUS, Fehler wird ueber result gemeldet */
        }
    }
    return result;
}

int32_t hal_sonar_trigger(uint32_t idx)
{
    int32_t result = -1;

    if (idx < SENSOR_COUNT) {
        atomic_store(&g_echo_phase[idx], 0);
        if (gpioTrigger(g_trig_pin[idx], SONAR_TRIGGER_PULSE_US, 1U) == 0) {
            result = 0;
        }
    }
    return result;
}

bool hal_sonar_result(uint32_t idx, uint32_t *echo_us)
{
    bool have_echo = false;

    if ((idx < SENSOR_COUNT) && (echo_us != NULL) &&
        (atomic_load(&g_echo_phase[idx]) == 2)) {
        *echo_us = atomic_load(&g_echo_us[idx]);
        have_echo = true;
    }
    return have_echo;
}

int32_t hal_test_set_distance_mm(uint32_t idx, uint32_t distance_mm)
{
    (void)idx;
    (void)distance_mm;
    return -1; /* reale Hardware: Abstand kann nicht vorgegeben werden */
}

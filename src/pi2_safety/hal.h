/*
 * hal.h - Hardware-Abstraktion fuer Pi 2 (HC-SR04 und TB6612FNG).
 *
 * Zwei Implementierungen:
 *   hal_pigpio.c - reale Hardware ueber pigpio (Raspberry Pi 4B)
 *   hal_sim.c    - Simulation fuer Entwicklung und Tests ohne Hardware
 */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

/* Initialisiert die Hardware; der Motor ist danach AUS. 0 = OK. */
int32_t hal_init(void);
/* Gibt die Hardware frei; der Motor ist danach AUS. 0 = OK. */
int32_t hal_shutdown(void);
const char_t *hal_name(void);

/* Sicherer Zustand (SAF-01): STBY = LOW ("Enable = OFF") und PWM = 0.
 * 0 = OK, -1 = mindestens ein Ausgang konnte nicht gesetzt werden. */
int32_t hal_motor_off(void);
/* Nur STBY = LOW; darf aus einem Signal-Handler aufgerufen werden. */
void hal_motor_emergency_off(void);
/* Motor mit Tastverhaeltnis in Promille (1..1000) ansteuern. 0 = OK. */
int32_t hal_motor_run(uint32_t duty_permille);

/* Loest eine Messung an Sensor idx aus. 0 = OK. */
int32_t hal_sonar_trigger(uint32_t idx);
/* Echo-Dauer der letzten Messung; false, wenn (noch) kein Echo vorliegt. */
bool hal_sonar_result(uint32_t idx, uint32_t *echo_us);

/* Nur Simulation: simulierten Abstand setzen (0 = kein Echo). 0 = OK,
 * -1 = von dieser HAL nicht unterstuetzt. */
int32_t hal_test_set_distance_mm(uint32_t idx, uint32_t distance_mm);

#endif /* HAL_H */

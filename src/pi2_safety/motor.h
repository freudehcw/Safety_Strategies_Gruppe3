/*
 * motor.h - Motorsteuerung: setzt ausschliesslich die von der Safety-Logik
 * freigegebene Stufe um. Alles andere fuehrt zu Motor AUS.
 */
#ifndef MOTOR_H
#define MOTOR_H

#include <stdint.h>

/* Tastverhaeltnis in Promille zur Stufe; 0 = Motor AUS / ungueltige Stufe. */
uint32_t motor_duty_permille(uint8_t level);

void *motor_thread(void *arg);

/* Anzahl fehlgeschlagener Abschaltversuche (hal_motor_off) seit Programmstart. */
uint32_t motor_off_failures(void);

#endif /* MOTOR_H */

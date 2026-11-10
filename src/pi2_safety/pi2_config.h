/*
 * pi2_config.h - Pi-2-spezifische Konstanten: Pinbelegung (BCM-Nummerierung,
 * siehe Designdokument Abschnitt "Pinout"), Zeitverhalten der Threads und
 * Motoransteuerung.
 */
#ifndef PI2_CONFIG_H
#define PI2_CONFIG_H

#define SENSOR_COUNT 2U

/* --- Pinbelegung (BCM) -------------------------------------------------- */
#define PIN_SONAR1_TRIG 23U /* Pin 16 */
#define PIN_SONAR1_ECHO 24U /* Pin 18, ueber Spannungsteiler */
#define PIN_SONAR2_TRIG 17U /* Pin 11 */
#define PIN_SONAR2_ECHO 27U /* Pin 13, ueber Spannungsteiler */
#define PIN_MOTOR_PWMA  18U /* Pin 12, Hardware-PWM0 */
#define PIN_MOTOR_STBY  25U /* Pin 22, "Enable", externer Pull-down */
#define PIN_MOTOR_AIN1  22U /* Pin 15 */
#define PIN_MOTOR_AIN2  16U /* Pin 36 */

/* --- Sensor-Threads ------------------------------------------------------ */
/* Beide Sensoren messen mit gleicher Periode, aber um eine halbe Periode
 * versetzt, damit sie sich nicht gegenseitig stoeren. */
#define SENSOR_PERIOD_MS       60U
#define SENSOR_PHASE_OFFSET_MS 30U
/* Festes Messfenster nach dem Trigger. 400 cm entsprechen ca. 23,3 ms Echo. */
#define SENSOR_ECHO_WINDOW_MS  30U
#define SONAR_TRIGGER_PULSE_US 10U
/* Schallgeschwindigkeit 343 m/s: d[mm] = t_echo[us] * 343 / 2000 */
#define SONAR_MM_NUM 343U
#define SONAR_MM_DEN 2000U

/* --- Motor --------------------------------------------------------------- */
#define MOTOR_PWM_FREQ_HZ 20000U
/* Tastverhaeltnis je Stufe in Promille (Ansteuerungsklassen, keine Drehzahl,
 * siehe H5). Am Aufbau anpassen. */
#define MOTOR_DUTY_LOW_PM    400U
#define MOTOR_DUTY_MEDIUM_PM 650U
#define MOTOR_DUTY_HIGH_PM   900U
/* Motorsteuerung: spaetestens nach dieser Zeit wird der Ausgang neu bewertet. */
#define MOTOR_REFRESH_MS 50U
/* Aktualisiert die Safety-Logik ihren Zustand laenger nicht, schaltet die
 * Motorsteuerung selbststaendig ab (Ueberwachung des Safety-Logik-Threads). */
#define SAFETY_ALIVE_TIMEOUT_MS 150U

/* --- Thread-Prioritaeten (SCHED_FIFO) ------------------------------------ */
#define PRIO_SAFETY 80
#define PRIO_MOTOR  70
#define PRIO_SENSOR 60
#define PRIO_COMM   40

#endif /* PI2_CONFIG_H */

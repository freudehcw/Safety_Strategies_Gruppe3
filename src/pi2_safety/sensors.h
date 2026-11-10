/*
 * sensors.h - Sensor-Threads (HC-SR04) mit den Einzelpruefungen P1 und P2.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stdint.h>

/* Zustand der Einzelpruefung eines Sensors (vorherige Messung fuer P2). */
typedef struct {
    bool have_prev;
    uint16_t prev_mm;
    uint64_t prev_ts_us;
} sensor_check_t;

void sensor_check_init(sensor_check_t *ctx);

/*
 * Bewertet eine Messung nach P1 (Messbereich, Echo vorhanden) und P2
 * (Abstandsaenderung gegenueber der vorherigen Messung desselben Sensors).
 *
 * Rueckgabe true = gueltig, *out_mm enthaelt den Abstand; sonst
 * *out_mm = DIST_INVALID_MM.
 *
 * Bezugswert d_(k-1) ist die letzte Messung desselben Sensors, die P1
 * bestanden hat, unabhaengig von ihrem P2-Ergebnis. Gibt es keinen Bezugswert
 * (Programmstart, nach Timeout oder Bereichsfehler), wird P2 nicht geprueft
 * und die Messung gilt als gueltig.
 */
bool sensor_check_sample(sensor_check_t *ctx, bool have_echo, uint32_t distance_mm,
                         uint64_t timestamp_us, uint16_t *out_mm);

/* Thread-Funktion; arg zeigt auf den Sensorindex (const uint32_t *). */
void *sensor_thread(void *arg);

/* Legt den gemeinsamen Startzeitpunkt beider Sensor-Threads fest (monotone
 * Zeit in Mikrosekunden). Vor dem Start der Threads aufrufen. */
void sensors_set_start_time(uint64_t start_us);

#ifdef SSM_TESTMODE
/* --- Testmodus (TC-01a, TC-01b, TC-02, TC-04) ---------------------------- */
/* Ersetzt den Rohmesswert von Sensor idx; mm < 0 hebt die Vorgabe auf. */
void sensors_test_force(uint32_t idx, int32_t mm);
/* Blockiert den Sensor-Thread idx einmalig fuer ms Millisekunden. */
void sensors_test_block(uint32_t idx, uint32_t ms);
#endif

#endif /* SENSORS_H */

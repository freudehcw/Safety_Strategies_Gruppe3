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
 * Konservative Festlegung: Gibt es keine vorherige Messung im Messbereich
 * (Programmstart, nach Timeout oder Bereichsfehler), kann P2 nicht geprueft
 * werden und die Messung gilt als ungueltig. Ein Sensor braucht daher zwei
 * aufeinanderfolgende plausible Messungen, bevor er freigibt.
 */
bool sensor_check_sample(sensor_check_t *ctx, bool have_echo, uint32_t distance_mm,
                         uint64_t timestamp_us, uint16_t *out_mm);

/* Thread-Funktion; arg zeigt auf den Sensorindex (const uint32_t *). */
void *sensor_thread(void *arg);

/* --- Testmodus (TC-01a, TC-01b, TC-02, TC-04) ---------------------------- */
void sensors_set_test_mode(bool enabled);
/* Ersetzt den Rohmesswert von Sensor idx; mm < 0 hebt die Vorgabe auf. */
void sensors_test_force(uint32_t idx, int32_t mm);
/* Blockiert den Sensor-Thread idx einmalig fuer ms Millisekunden. */
void sensors_test_block(uint32_t idx, uint32_t ms);

#endif /* SENSORS_H */

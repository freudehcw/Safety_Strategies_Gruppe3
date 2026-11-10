/*
 * safety_logic.h - Safety-Logik von Pi 2 (SAF-06, SAF-08, SAF-11).
 *
 * safety_evaluate() ist eine reine Funktion ohne Hardware-, Zeit- oder
 * Thread-Abhaengigkeit und wird in tests/ direkt geprueft.
 */
#ifndef SAFETY_LOGIC_H
#define SAFETY_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "shared.h"

/* Zustand, der ueber Zyklen erhalten bleibt. */
typedef struct {
    uint32_t last_seq[SENSOR_COUNT]; /* zuletzt fuer P3 bewerteter Messzyklus */
    uint8_t mismatch_cycles;         /* aufeinanderfolgende P3-Abweichungen */
    bool shutdown;                   /* SHUTDOWN ist Endzustand */
} safety_ctx_t;

typedef struct {
    uint8_t state;       /* STATE_* */
    uint8_t error;       /* ERR_* */
    uint8_t motor_level; /* freigegebene Stufe; LEVEL_OFF = Motor AUS */
} safety_decision_t;

void safety_ctx_init(safety_ctx_t *ctx);

/* Sicherheitsabstand S zur Stufe in mm; 0 bei ungueltiger Stufe. */
uint16_t safety_distance_mm(uint8_t level);

/*
 * Ein Zyklus der Safety-Logik. Pruefreihenfolge wie im Designdokument:
 *   1. Stop/Shutdown                         -> SHUTDOWN
 *   2. Sensoren gueltig und plausibel (P1-P3) -> sonst FEHLER / SENSOR
 *   3. Kommunikation aktuell, Stufe gueltig   -> sonst FEHLER / KOMMUNIKATION
 *   4. Sensordaten aktuell (P4)               -> sonst FEHLER / DATENALTER
 *   5. Sicherheitsabstand S zur Stufe waehlen
 *   6. d_min < S -> OBJEKT_STOPP, sonst BETRIEB mit der Sollstufe
 */
void safety_evaluate(safety_ctx_t *ctx, const sensor_data_t sensors[SENSOR_COUNT],
                     const comm_data_t *comm, bool local_stop, uint64_t now_us,
                     safety_decision_t *decision);

void *safety_thread(void *arg);

#endif /* SAFETY_LOGIC_H */

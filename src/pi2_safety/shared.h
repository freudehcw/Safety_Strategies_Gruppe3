/*
 * shared.h - Gemeinsam verwendete Datensaetze auf Pi 2 (Designdokument,
 * Abschnitt "Gemeinsam verwendete Daten").
 *
 * Regeln:
 *  - Jeder Datensatz hat genau einen schreibenden Thread und einen eigenen
 *    Mutex mit Prioritaetsvererbung (PTHREAD_PRIO_INHERIT).
 *  - Unter einem Mutex wird nur kopiert; Leser arbeiten mit einem Snapshot.
 *  - Ein Thread haelt nie zwei Mutexe gleichzeitig.
 *  - Schlaegt ein Lesezugriff fehl, wird ein genullter Datensatz geliefert
 *    (valid = false, Zustand INIT), also die sichere Seite.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#ifndef SHARED_H
#define SHARED_H

#include <stdbool.h>
#include <stdint.h>

#include "pi2_config.h"

/* Schreibt: Sensor-Thread idx. Liest: Safety-Logik, Kommunikation. */
typedef struct {
    uint16_t distance_mm;  /* DIST_INVALID_MM, wenn kein gueltiger Messwert */
    bool valid;            /* Ergebnis von P1 und P2 */
    uint64_t timestamp_us; /* monotoner Zeitpunkt des Triggers */
    uint32_t seq;          /* Messzaehler; 0 = noch nie gemessen */
} sensor_data_t;

/* Schreibt: Kommunikations-Thread. Liest: Safety-Logik. */
typedef struct {
    uint8_t level;          /* zuletzt gueltig empfangene Sollstufe */
    bool stop;              /* Stop/Shutdown empfangen (bleibt gesetzt) */
    bool have_msg;          /* mindestens eine gueltige Nachricht empfangen */
    uint64_t last_valid_us; /* Empfangszeit der letzten gueltigen Nachricht */
} comm_data_t;

/* Schreibt: Safety-Logik. Liest: Kommunikation, Motorsteuerung. */
typedef struct {
    uint8_t state;           /* STATE_* */
    uint8_t error;           /* ERR_* */
    uint8_t motor_level;     /* freigegebene Stufe; LEVEL_OFF = Motor AUS */
    uint64_t updated_us;     /* Zeitpunkt des letzten Zyklus der Safety-Logik */
    uint64_t state_since_us; /* letzter Wechsel von Zustand, Fehlercode oder Stufe */
    uint32_t generation;     /* wird in jedem Zyklus erhoeht */
    uint32_t transitions;    /* Anzahl dieser Wechsel */
} safety_state_t;

/* Rueckmeldung der Motorsteuerung.
 * Schreibt: Motorsteuerung. Liest: Safety-Logik (Watchdog), Kommunikation (ACK). */
#define MOTOR_LEVEL_UNKNOWN 0xFFU /* Motor AUS konnte nicht gesetzt werden */
typedef struct {
    uint8_t applied_level; /* tatsaechlich umgesetzte Stufe; LEVEL_OFF = Motor AUS */
    uint32_t generation;   /* Generation von safety_state, die umgesetzt wurde */
    uint64_t updated_us;   /* Zeitpunkt der Umsetzung */
} motor_status_t;

int32_t shared_init(void); /* 0 = OK */

void shared_sensor_write(uint32_t idx, const sensor_data_t *data);
void shared_sensor_read(uint32_t idx, sensor_data_t *data);

void shared_comm_write(const comm_data_t *data);
void shared_comm_read(comm_data_t *data);

void shared_state_write(const safety_state_t *state);
void shared_state_read(safety_state_t *state);
/*
 * Wartet, bis ein neuerer Zustand als last_generation vorliegt oder
 * deadline_us erreicht ist, und liefert den aktuellen Zustand.
 * Rueckgabe: true, wenn ein neuer Zustand vorliegt.
 */
bool shared_state_wait(uint32_t last_generation, uint64_t deadline_us, safety_state_t *state);

void shared_motor_write(const motor_status_t *status);
void shared_motor_read(motor_status_t *status);

/* Lokale Stop-Anforderung (SIGINT/SIGTERM, Testkonsole). */
void shared_request_stop(void);
bool shared_stop_requested(void);

/* Laufsteuerung aller Threads. */
void shared_set_running(bool running);
bool shared_running(void);

#endif /* SHARED_H */

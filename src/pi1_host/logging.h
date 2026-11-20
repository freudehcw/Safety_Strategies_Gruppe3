/*
 * logging.h - Logging von Pi 1 als CSV-Datei (Designdokument Abschnitt 2.10,
 * Festlegung 11). Logging ist KEINE Safety-Funktion.
 *
 * Ablauf: Die erzeugenden Threads (main, input, comm) legen Eintraege in je
 * einen eigenen Ringpuffer (eigener Mutex, nur kopieren). Der Logging-Thread
 * leert die Puffer alle 100 ms und schreibt die Zeilen in die Datei. Auf die
 * SD-Karte (fflush + fsync) wird einmal pro Sekunde und sofort nach einem
 * Fehlerereignis geschrieben. So verzoegert Logging weder Bedienung noch
 * Kommunikation.
 *
 * Volle Puffer: Messwerte werden verworfen, sobald nur noch die fuer
 * Ereignisse reservierten Plaetze frei sind (Ereignisse haben Vorrang). Die
 * Anzahl verworfener Eintraege wird als eigenes Ereignis protokolliert.
 *
 * CSV-Spalten: wall_time, mono_ms, type, event, state, error,
 * level_requested, level_active, d1_mm, d2_mm. wall_time ist nur informativ
 * (CLOCK_REALTIME, der Pi hat keine Echtzeituhr). Ungueltige Abstaende
 * (0xFFFF) bleiben leer.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_LOGGING_H
#define PI1_LOGGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "pi1_config.h"
#include "protocol.h"
#include "types.h"

/* Erzeugende Threads; jeder hat seinen eigenen Ringpuffer. */
#define LOG_SRC_MAIN   0U
#define LOG_SRC_INPUT  1U
#define LOG_SRC_COMM    2U
#define LOG_SRC_DISPLAY 3U
#define LOG_SRC_COUNT   4U

#define LOG_TYPE_EVENT 1U
#define LOG_TYPE_MEAS  2U

#define LOG_LEVEL_NONE 0xFFU /* noch keine Sollstufe gewaehlt */

typedef struct {
    uint8_t type;                         /* LOG_TYPE_EVENT oder LOG_TYPE_MEAS */
    bool urgent;                          /* Fehlerereignis: sofort auf die Karte */
    struct timespec wall;                 /* CLOCK_REALTIME, nur informativ */
    uint64_t mono_ms;                     /* CLOCK_MONOTONIC in ms */
    char_t event[PI1_LOG_EVENT_TEXT_LEN]; /* nur bei EVENT */
    uint8_t level_req;                    /* LOG_LEVEL_NONE oder LEVEL_* */
    bool has_status;                      /* status ist gueltig */
    proto_status_t status;
} log_record_t;

/* Ringpuffer eines erzeugenden Threads. Zugriff nur unter dessen Mutex. */
typedef struct {
    log_record_t items[PI1_LOG_RING_LEN];
    uint32_t head;           /* Index des aeltesten Eintrags */
    uint32_t count;          /* belegte Eintraege */
    uint32_t dropped_meas;   /* verworfene Messwerte (Summe) */
    uint32_t dropped_events; /* verworfene Ereignisse (Summe, nur wenn alles voll) */
} log_ring_t;

/* --- Reine Hilfsfunktionen (ohne Mutex, fuer Unit-Tests) ------------------- */

void log_ring_reset(log_ring_t *ring);
/* true = eingetragen, false = verworfen (Zaehler erhoeht). */
bool log_ring_push(log_ring_t *ring, const log_record_t *rec);
/* true = aeltesten Eintrag nach rec kopiert und entfernt. */
bool log_ring_pop(log_ring_t *ring, log_record_t *rec);

/* Schreibt den CSV-Kopf bzw. eine CSV-Zeile (mit '\n'). Laenge oder -1. */
int32_t log_format_header(char_t *buf, size_t cap);
int32_t log_format_row(const log_record_t *rec, char_t *buf, size_t cap);

/* --- Betrieb ----------------------------------------------------------------- */

/*
 * Legt die Puffer an und oeffnet die Datei <dir>/pi1_<Startzeit>.csv. Liefert
 * -1, wenn die Datei nicht angelegt werden kann; das Programm laeuft dann
 * ohne Datei weiter (Logging ist keine Safety-Funktion), die Eintraege werden
 * verworfen. -2 bei Fehler der Mutexe (dann kein Logging-Thread starten).
 */
int32_t log_init(const char_t *dir);

/* Pfad der Log-Datei ("" wenn keine Datei offen ist). */
const char_t *log_file_path(void);

/* Ereignis protokollieren. status darf NULL sein. */
void log_event(uint8_t src, const char_t *text, bool urgent,
               uint8_t level_req, const proto_status_t *status);

/* Messwert (jede gueltige STATUS-Nachricht, 10 Hz). */
void log_measurement(uint8_t src, uint8_t level_req, const proto_status_t *status);

/* Logging-Thread; endet, wenn app_ctrl.stop_threads gesetzt ist, nach dem
 * letzten Leeren der Puffer. Danach log_close() aufrufen. */
void *log_thread(void *arg);

/* Schliesst die Datei und gibt die Mutexe frei (nach dem Ende des Threads). */
void log_close(void);

#endif /* PI1_LOGGING_H */

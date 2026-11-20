/*
 * shared.h - Gemeinsame Datensaetze der Threads von Pi 1
 * (Designdokument Abschnitt 2.7, sinngemaess fuer Pi 1).
 *
 * Regeln:
 *   - Jeder Datensatz hat genau EINEN schreibenden Thread und einen eigenen Mutex.
 *   - Unter dem Mutex wird nur kopiert (kein I/O, kein Logging, kein I2C).
 *   - Ein Thread haelt immer nur einen Mutex gleichzeitig (kein Deadlock).
 *   - Leser arbeiten mit einer lokalen Kopie (Snapshot).
 *
 *   Datensatz      schreibt        liest
 *   operator_cmd   input           main, comm, display
 *   app_ctrl       main            comm, input, display, logging
 *   link_status    comm            main, display
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_SHARED_H
#define PI1_SHARED_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/* Bedienereingaben (Festlegung 1 und 2). */
typedef struct {
    bool level_chosen; /* false bis zur ersten Wahl: dann KEIN Heartbeat */
    uint8_t level;     /* LEVEL_LOW, LEVEL_MEDIUM oder LEVEL_HIGH */
    bool quit;         /* Taste q */
} operator_cmd_t;

/* Steuerung des Programmablaufs durch main. */
typedef struct {
    bool shutdown;     /* Programmende angefordert: comm sendet STOP */
    bool stop_threads; /* input, display und logging sollen enden */
} app_ctrl_t;

/* Ergebnis des kontrollierten Beendens (Abschnitt 2.11). */
#define STOP_RESULT_NONE         0U /* noch kein Programmende */
#define STOP_RESULT_ACKED        1U /* STOP gesendet, ACK empfangen */
#define STOP_RESULT_NO_ACK       2U /* STOP gesendet, kein ACK innerhalb 2 s */
#define STOP_RESULT_NOT_POSSIBLE 3U /* keine Verbindung/Session: STOP nicht moeglich */

/* Zustand der Verbindung und letzter gueltiger STATUS. */
typedef struct {
    bool online;             /* Session aufgebaut (erste STATUS-Nachricht empfangen) */
    bool have_status;        /* mindestens ein gueltiger STATUS in dieser Verbindung */
    proto_status_t status;   /* letzter gueltiger STATUS */
    uint64_t last_status_us; /* CLOCK_MONOTONIC beim Empfang des letzten gueltigen STATUS */
    uint32_t session_id;
    uint8_t stop_result;     /* STOP_RESULT_* */
    bool comm_finished;      /* Kommunikations-Thread hat seine Arbeit beendet */
} link_status_t;

/* Legt die Mutexe an. 0 = OK, -1 = Fehler. */
int32_t shared_init(void);
void shared_destroy(void);

void shared_operator_write(const operator_cmd_t *cmd);
void shared_operator_read(operator_cmd_t *cmd);

void shared_ctrl_write(const app_ctrl_t *ctrl);
void shared_ctrl_read(app_ctrl_t *ctrl);

void shared_link_write(const link_status_t *link);
void shared_link_read(link_status_t *link);

#endif /* PI1_SHARED_H */

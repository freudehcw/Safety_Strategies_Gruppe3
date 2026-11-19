/*
 * session.h - Session-ID und Sequenznummern einer Verbindung
 * (Designdokument Abschnitt 5.2 T2, SEC-03).
 *
 * Rollenneutral: Die Server-Seite (Pi 2) erzeugt mit session_start_server()
 * eine neue Session-ID, die Client-Seite (Pi 1) uebernimmt sie mit
 * session_adopt() aus der ersten STATUS-Nachricht. Danach pruefen beide
 * jede empfangene Nachricht mit session_check_rx() und holen fuer jede
 * gesendete Nachricht die naechste Nummer mit session_next_tx_seq().
 *
 * Die Funktionen werten nur Zahlen aus. Das GCM-Tag muss VORHER gueltig
 * sein (Pruefreihenfolge Tag, Session-ID, Sequenznummer, Wertebereich).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stdint.h>

/* Rueckgabewerte von session_check_rx() und session_next_tx_seq() */
#define SESSION_OK            0
#define SESSION_ERR_INACTIVE  (-1) /* keine Session vorhanden */
#define SESSION_ERR_ID        (-2) /* fremde Session-ID -> verwerfen */
#define SESSION_ERR_OLD_SEQ   (-3) /* Sequenznummer <= zuletzt akzeptierte -> verwerfen */
#define SESSION_ERR_EXHAUSTED (-4) /* eigene Sequenznummer hat UINT32_MAX erreicht */
#define SESSION_ERR_RANDOM    (-5) /* getrandom() fehlgeschlagen */
#define SESSION_ERR_ARG       (-6) /* ungueltiges Argument */

typedef struct {
    bool active;          /* true, sobald die Session-ID bekannt ist */
    uint32_t id;          /* Session-ID dieser Verbindung */
    uint32_t tx_seq;      /* zuletzt vergebene eigene Sequenznummer (0 = noch keine) */
    uint32_t rx_last_seq; /* zuletzt akzeptierte Sequenznummer der Gegenseite */
} session_t;

/* Setzt die Session zurueck (inaktiv, alle Zaehler 0), z. B. nach Verbindungsabbau. */
void session_reset(session_t *s);

/*
 * Server-Seite: neue zufaellige Session-ID (getrandom), Zaehler auf 0,
 * Session aktiv. SESSION_OK oder SESSION_ERR_RANDOM / SESSION_ERR_ARG.
 */
int32_t session_start_server(session_t *s);

/*
 * Client-Seite: uebernimmt Session-ID und Sequenznummer aus der ersten
 * gueltigen Nachricht der Gegenseite (Tag bereits geprueft). Die eigene
 * erste Nachricht traegt danach die Sequenznummer 1.
 */
void session_adopt(session_t *s, uint32_t id, uint32_t seq);

/*
 * Prueft Session-ID und Sequenznummer einer empfangenen Nachricht. Nur bei
 * SESSION_OK wird die Sequenznummer als zuletzt akzeptierte gespeichert.
 */
int32_t session_check_rx(session_t *s, uint32_t id, uint32_t seq);

/*
 * Liefert die Sequenznummer fuer die naechste eigene Nachricht (1, 2, ...).
 * Ist UINT32_MAX bereits vergeben, wird SESSION_ERR_EXHAUSTED geliefert: Der
 * Aufrufer muss die Verbindung abbauen; nach dem Neuaufbau gibt es eine neue
 * Session und die Zaehlung beginnt wieder bei 1 (kein Ueberlauf, Abschnitt 5.2).
 */
int32_t session_next_tx_seq(session_t *s, uint32_t *seq);

#endif /* SESSION_H */

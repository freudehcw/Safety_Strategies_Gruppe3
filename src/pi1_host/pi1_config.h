/*
 * pi1_config.h - Konstanten, die nur Raspberry Pi 1 betreffen. Gemeinsame
 * Werte (Takte, Timeouts, Typcodes) stehen in src/common/config.h und
 * src/common/protocol.h.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_CONFIG_H
#define PI1_CONFIG_H

#include "config.h"

/* --- Kommunikation (Abschnitt 2.8, Ergaenzungen E und F im Plan) ----------- */
#define PI1_RECONNECT_PERIOD_MS      1000U /* Abschnitt 2.8: jede Sekunde neu verbinden */
#define PI1_CONNECT_TIMEOUT_MS       5000U /* laufenden Verbindungsaufbau abbrechen */
#define PI1_FIRST_STATUS_TIMEOUT_MS  1000U /* Plan F: erste STATUS-Nachricht bleibt aus */
#define PI1_LINK_TIMEOUT_MS          CFG_COMM_TIMEOUT_MS /* 300 ms -> "Verbindung getrennt" */
#define PI1_ACK_TIMEOUT_MS           2000U /* Festlegung 3: hoechstens 2 s auf ACK warten */
#define PI1_SEND_TIMEOUT_MS          20    /* ein Frame passt immer in den Sendepuffer */
#define PI1_POLL_TIMEOUT_MS          CFG_COMM_POLL_TIMEOUT_MS /* 50 ms */

/* --- Anzeige (Abschnitt 2.10, Festlegung 9) ---------------------------------- */
#define PI1_DISPLAY_PERIOD_MS        100U  /* ein Anzeigezyklus */
#define PI1_LCD_COLS                 20U
#define PI1_LCD_ROWS                 4U

/* --- Logging (Abschnitt 2.10, Festlegung 11) ---------------------------------- */
#define PI1_LOG_PERIOD_MS            100U  /* Puffer leeren */
#define PI1_LOG_FLUSH_PERIOD_MS      1000U /* einmal pro Sekunde auf die Karte */
#define PI1_LOG_RING_LEN             128U  /* Eintraege je erzeugendem Thread */
#define PI1_LOG_EVENT_RESERVE        32U   /* Plaetze nur fuer Ereignisse (Vorrang) */
#define PI1_LOG_EVENT_TEXT_LEN       48U
#define PI1_LOG_PATH_LEN             256U

/* --- Bedienung und Hauptprogramm -------------------------------------------- */
#define PI1_INPUT_POLL_MS            50
#define PI1_MAIN_PERIOD_MS           20U

/* --- Standardwerte der Kommandozeile (Festlegung 7) ------------------------- */
#define PI1_DEFAULT_TCP_HOST         "127.0.0.1"
#define PI1_DEFAULT_TCP_PORT         5555U
#define PI1_DEFAULT_I2C_BUS          "/dev/i2c-1"
#define PI1_DEFAULT_LCD_ADDR         0x27U
#define PI1_DEFAULT_LOG_DIR          "./logs"

#endif /* PI1_CONFIG_H */

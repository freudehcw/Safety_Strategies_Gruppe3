/*
 * config.h - Gemeinsame Konstanten und Grenzwerte (Pi 1 und Pi 2).
 *
 * Alle Werte sind Design-Startwerte aus dem Designdokument (Abschnitt
 * "Zentrale Designparameter") und muessen am realen Aufbau verifiziert werden.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#ifndef CONFIG_H
#define CONFIG_H

/* --- Kommunikation (H2, SAF-05) --------------------------------------- */
#define CFG_HEARTBEAT_PERIOD_MS   100U  /* Pi 1 -> Pi 2 */
#define CFG_STATUS_PERIOD_MS      100U  /* Pi 2 -> Pi 1 */
#define CFG_COMM_TIMEOUT_MS       300U  /* keine gueltige Nachricht -> Fehler */
#define CFG_COMM_POLL_TIMEOUT_MS  50    /* poll()-Timeout Kommunikations-Thread */
#define CFG_RFCOMM_CHANNEL        1U    /* fester RFCOMM-Kanal */

/* --- Safety-Logik (SAF-06) -------------------------------------------- */
#define CFG_SAFETY_PERIOD_MS      50U

/* --- Sensorplausibilitaet (H1, H3, SAF-03, SAF-04) --------------------- */
#define CFG_DIST_MIN_MM           20U   /* P1: 2 cm */
#define CFG_DIST_MAX_MM           4000U /* P1: 400 cm */
#define CFG_MAX_RATE_MM_PER_MS    2U    /* P2: 0,2 cm/ms = 2 m/s */
#define CFG_SENSOR_DEV_MAX_MM     50U   /* P3: 5 cm */
#define CFG_SENSOR_DEV_CYCLES     2U    /* P3: aufeinanderfolgende Messzyklen */
#define CFG_SENSOR_MAX_AGE_MS     150U  /* P4 */

#endif /* CONFIG_H */

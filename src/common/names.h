/*
 * names.h - Klartextnamen fuer Zustand, Fehlercode und Stufe
 * (Kodierung laut Designdokument Tabelle 11). Verwendet von Pi 1 (LCD, Log)
 * und Pi 2 (Protokoll der Zustandswechsel).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef NAMES_H
#define NAMES_H

#include <stdint.h>

#include "types.h"

/* Liefert immer einen gueltigen Text; unbekannte Werte ergeben "?". */
const char_t *name_state(uint8_t state);
const char_t *name_error(uint8_t error);
const char_t *name_level(uint8_t level); /* 0x00 = "AUS" */

#endif /* NAMES_H */

/*
 * input.h - Bedienung von Pi 1 ueber die Tastatur im Terminal
 * (Designdokument Abschnitt 2.1, Festlegung 1):
 *   1 = LOW, 2 = MEDIUM, 3 = HIGH, q = Programmende.
 *
 * Das Terminal wird in den nicht-kanonischen Modus geschaltet (Taste wirkt
 * sofort, ohne Enter). input_terminal_restore() stellt den alten Zustand
 * wieder her und muss beim Beenden in JEDEM Fall aufgerufen werden.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_INPUT_H
#define PI1_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

/*
 * Schaltet stdin in den nicht-kanonischen Modus, falls es ein Terminal ist.
 * Ist stdin kein Terminal (z. B. Tasten per Pipe im Test), wird nichts
 * umgeschaltet. 0 = OK, -1 = Fehler.
 */
int32_t input_terminal_setup(void);

/* Stellt den gesicherten Terminalzustand wieder her (mehrfach aufrufbar). */
void input_terminal_restore(void);

/*
 * Wie input_terminal_restore(), aber nur mit async-signal-sicheren Aufrufen
 * (tcsetattr). Fuer Handler von Absturzsignalen.
 */
void input_terminal_restore_from_signal(void);

/* Wertet eine Taste aus. Liefert die Stufe (LEVEL_*), 'q' fuer Programmende
 * oder 0 fuer eine unbekannte Taste. Reine Funktion (Unit-Test). */
uint8_t input_decode_key(char_t key);
#define INPUT_KEY_QUIT ((uint8_t)'q')

/* Gibt die Tastenhilfe aus. */
void input_print_help(void);

/* Eingabe-Thread; einziger Schreiber von operator_cmd. */
void *input_thread(void *arg);

#endif /* PI1_INPUT_H */

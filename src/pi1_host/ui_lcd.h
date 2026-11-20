/*
 * ui_lcd.h - Anzeige-Thread von Pi 1 (Designdokument Abschnitt 2.6 und 2.10,
 * Festlegungen 2, 9, 10). Die Anzeige ist KEINE Safety-Funktion.
 *
 * Der Thread liest alle 100 ms eine Kopie der gemeinsamen Daten, erzeugt
 * daraus vier Zeilen zu je 20 Zeichen und gibt sie ueber ein Backend aus:
 * LCD 20x4 ueber I2C (Betrieb) oder Konsole (Test am PC). Eigener Thread,
 * damit langsame I2C-Zugriffe den Heartbeat nicht verzoegern.
 *
 * Zeilen:
 *   1  Status: <Zustand>            ("--" wenn Verbindung getrennt)
 *   2  Soll:<Stufe> Ist:<Stufe>     ("Stufe waehlen 1/2/3" ohne Wahl)
 *   3  d_min: <x.y> cm              ("--" wenn kein gueltiger Wert)
 *   4  Fehlercode, "Verbindung getrennt" oder Stand des Programmendes
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_UI_LCD_H
#define PI1_UI_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "pi1_config.h"
#include "shared.h"
#include "types.h"

#define DISPLAY_LINE_LEN (PI1_LCD_COLS + 1U) /* 20 Zeichen + '\0' */

typedef struct {
    bool use_console;      /* true: Konsole statt I2C-LCD */
    const char_t *i2c_bus; /* z. B. "/dev/i2c-1" */
    uint8_t lcd_addr;      /* z. B. 0x27 */
} display_cfg_t;

/* Alles, was die Anzeige braucht (Kopien der gemeinsamen Daten). */
typedef struct {
    operator_cmd_t op;
    link_status_t link;
    app_ctrl_t ctrl;
    uint64_t now_us;
} display_input_t;

/* --- Reine Funktionen (Unit-Tests) ------------------------------------------- */

/* true, wenn innerhalb der letzten 300 ms ein gueltiger STATUS kam (Abschnitt 2.6). */
bool display_link_ok(const link_status_t *link, uint64_t now_us);

/* Festlegung 10: kleinerer der gueltigen Werte; DIST_INVALID_MM, wenn keiner gilt. */
uint16_t display_dmin(uint16_t d1_mm, uint16_t d2_mm);

/* Erzeugt die vier Zeilen, jede genau 20 Zeichen lang (mit Leerzeichen aufgefuellt). */
void display_format(const display_input_t *in, char_t lines[PI1_LCD_ROWS][DISPLAY_LINE_LEN]);

/* --- Thread -------------------------------------------------------------------- */

/* arg zeigt auf ein display_cfg_t, das bis zum Thread-Ende gueltig bleibt. */
void *display_thread(void *arg);

#endif /* PI1_UI_LCD_H */

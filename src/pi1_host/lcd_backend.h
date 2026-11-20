/*
 * lcd_backend.h - Ausgabe der vier Anzeigezeilen: Konsole (lcd_console.c)
 * oder LCD 20x4 HD44780 ueber I2C-Backpack PCF8574 (lcd_i2c.c, Festlegung 9).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_LCD_BACKEND_H
#define PI1_LCD_BACKEND_H

#include <stdint.h>

#include "types.h"

/* --- Konsole ------------------------------------------------------------------ */

/* Merkt sich, ob stdout ein Terminal ist. Immer erfolgreich. */
void lcd_console_init(void);

/* Gibt die Zeile row (0-3) aus; text hat 20 Zeichen. 0 = OK. */
int32_t lcd_console_write_line(uint8_t row, const char_t *text);

/* Am Ende jedes Anzeigezyklus: Ausgabe abschliessen. */
void lcd_console_refresh(void);

void lcd_console_close(void);

/* --- I2C-LCD -------------------------------------------------------------------- */

/* Oeffnet den I2C-Bus und initialisiert das LCD. 0 = OK, -1 = Fehler. */
int32_t lcd_i2c_init(const char_t *bus, uint8_t addr);

/* Schreibt Zeile row (0-3), 20 Zeichen. 0 = OK, -1 = Fehler (LCD ausgefallen). */
int32_t lcd_i2c_write_line(uint8_t row, const char_t *text);

void lcd_i2c_close(void);

#endif /* PI1_LCD_BACKEND_H */

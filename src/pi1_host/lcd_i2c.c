/*
 * lcd_i2c.c - LCD 20x4 (HD44780) ueber I2C-Backpack PCF8574 (Modul HW-061),
 * Linux-Schnittstelle i2c-dev, 4-Bit-Modus (Festlegung 9).
 *
 * STAND: Platzhalter. Die Umsetzung folgt, sobald der Raspberry Pi verbunden
 * ist (Meilenstein 6, zweiter Teil). Bis dahin melden die Funktionen einen
 * Fehler; das Programm laeuft dann ohne LCD weiter (Ausfall des LCD darf das
 * Programm nicht anhalten). Am PC: Option --lcd-console verwenden.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "lcd_backend.h"

int32_t lcd_i2c_init(const char_t *bus, uint8_t addr)
{
    (void)bus;
    (void)addr;
    return -1;
}

int32_t lcd_i2c_write_line(uint8_t row, const char_t *text)
{
    (void)row;
    (void)text;
    return -1;
}

void lcd_i2c_close(void)
{
}

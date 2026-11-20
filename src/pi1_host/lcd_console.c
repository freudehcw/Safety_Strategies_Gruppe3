/*
 * lcd_console.c - Konsolen-Backend der Anzeige fuer Tests am PC (Festlegung 9).
 *
 * Im Terminal wird der LCD-Inhalt als eine Zeile "| Zeile1 | Zeile2 | ..."
 * an Ort und Stelle ueberschrieben (\r und "Zeile loeschen"). Ist stdout
 * kein Terminal (z. B. Umleitung in eine Datei), wird nur bei einer Aenderung
 * eine neue Zeile ausgegeben.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "lcd_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pi1_config.h"

static bool g_is_tty = false;
static bool g_changed = false;
static char_t g_lines[PI1_LCD_ROWS][PI1_LCD_COLS + 1U];

void lcd_console_init(void)
{
    g_is_tty = (isatty(STDOUT_FILENO) == 1);
    (void)memset(g_lines, 0, sizeof(g_lines));
    g_changed = true;
}

int32_t lcd_console_write_line(uint8_t row, const char_t *text)
{
    int32_t result = -1;

    if ((row < PI1_LCD_ROWS) && (text != NULL)) {
        if (strncmp(g_lines[row], text, PI1_LCD_COLS) != 0) {
            (void)snprintf(g_lines[row], sizeof(g_lines[row]), "%s", text);
            g_changed = true;
        }
        result = 0;
    }
    return result;
}

/* Kopiert eine Zeile ohne Leerzeichen am Ende (kuerzere Konsolenzeile). */
static void trimmed(const char_t *in, char_t *out, size_t cap)
{
    size_t len;

    (void)snprintf(out, cap, "%s", in);
    len = strlen(out);
    while ((len > 0U) && (out[len - 1U] == ' ')) {
        len--;
        out[len] = '\0';
    }
}

void lcd_console_refresh(void)
{
    /* Im Terminal immer neu zeichnen (eine Ereignismeldung kann die Zeile
     * ueberschrieben haben), sonst nur bei Aenderung. */
    if (g_is_tty || g_changed) {
        char_t t[PI1_LCD_ROWS][PI1_LCD_COLS + 1U];
        uint8_t row;

        for (row = 0U; row < PI1_LCD_ROWS; row++) {
            trimmed(g_lines[row], t[row], sizeof(t[row]));
        }
        (void)printf("%sLCD| %s | %s | %s | %s%s", g_is_tty ? "\r\033[K" : "",
                     t[0], t[1], t[2], t[3], g_is_tty ? "" : "\n");
        (void)fflush(stdout);
        g_changed = false;
    }
}

void lcd_console_close(void)
{
    if (g_is_tty) {
        (void)printf("\n");
        (void)fflush(stdout);
    }
}

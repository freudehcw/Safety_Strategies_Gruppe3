/*
 * ui_lcd.c - Anzeige-Thread von Pi 1. Beschreibung siehe ui_lcd.h.
 *
 * Abschnitt 2.6: Kam laenger als 300 ms kein gueltiger STATUS, wird
 * "Verbindung getrennt" angezeigt und der zuletzt empfangene Zustand NICHT
 * mehr als aktuell dargestellt (Zustand, Ist-Stufe und d_min zeigen "--").
 *
 * Fehler des LCD werden einmal protokolliert; danach laeuft das Programm ohne
 * LCD weiter (Abschnitt 2.10: Ausfall des LCD darf das Programm nicht anhalten).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "ui_lcd.h"

#include <stdio.h>
#include <string.h>

#include "lcd_backend.h"
#include "logging.h"
#include "pi1_names.h"
#include "protocol.h"
#include "time_utils.h"

/* ===== Reine Funktionen ======================================================= */

bool display_link_ok(const link_status_t *link, uint64_t now_us)
{
    bool ok = false;

    if ((link != NULL) && link->online && link->have_status) {
        ok = (time_age_us(now_us, link->last_status_us) <=
              ((uint64_t)PI1_LINK_TIMEOUT_MS * TIME_US_PER_MS));
    }
    return ok;
}

uint16_t display_dmin(uint16_t d1_mm, uint16_t d2_mm)
{
    uint16_t dmin = (uint16_t)DIST_INVALID_MM;

    if (d1_mm != (uint16_t)DIST_INVALID_MM) {
        dmin = d1_mm;
    }
    if ((d2_mm != (uint16_t)DIST_INVALID_MM) && (d2_mm < dmin)) {
        dmin = d2_mm;
    }
    return dmin;
}

/* Schreibt text in line, kuerzt auf 20 Zeichen und fuellt mit Leerzeichen auf. */
static void set_line(char_t line[DISPLAY_LINE_LEN], const char_t *text)
{
    size_t len = strlen(text);
    size_t i;

    if (len > (size_t)PI1_LCD_COLS) {
        len = (size_t)PI1_LCD_COLS;
    }
    for (i = 0U; i < (size_t)PI1_LCD_COLS; i++) {
        line[i] = (i < len) ? text[i] : ' ';
    }
    line[PI1_LCD_COLS] = '\0';
}

/* Zeile 4 waehrend des Programmendes (Abschnitt 2.11). */
static const char_t *shutdown_text(uint8_t stop_result)
{
    const char_t *text;

    switch (stop_result) {
    case STOP_RESULT_ACKED:
        text = "Beendet (ACK)";
        break;
    case STOP_RESULT_NO_ACK:
        text = "Beendet ohne ACK";
        break;
    case STOP_RESULT_NOT_POSSIBLE:
        text = "Beendet ohne STOP";
        break;
    default:
        text = "STOP, warte auf ACK";
        break;
    }
    return text;
}

void display_format(const display_input_t *in, char_t lines[PI1_LCD_ROWS][DISPLAY_LINE_LEN])
{
    char_t buf[48];
    bool link_ok;
    const proto_status_t *st;

    if ((in == NULL) || (lines == NULL)) {
        return;
    }
    link_ok = display_link_ok(&in->link, in->now_us);
    st = &in->link.status;

    /* Zeile 1: Zustand von Pi 2 */
    (void)snprintf(buf, sizeof(buf), "Status: %s", link_ok ? name_state(st->state) : "--");
    set_line(lines[0], buf);

    /* Zeile 2: Soll- und Ist-Stufe; ohne Wahl Aufforderung (Festlegung 2) */
    if (!in->op.level_chosen) {
        (void)snprintf(buf, sizeof(buf), "Stufe waehlen 1/2/3");
    } else {
        (void)snprintf(buf, sizeof(buf), "Soll:%s Ist:%s", name_level(in->op.level),
                       link_ok ? name_level(st->level) : "--");
    }
    set_line(lines[1], buf);

    /* Zeile 3: d_min in cm mit einer Nachkommastelle (Festlegung 10) */
    {
        uint16_t dmin = display_dmin(st->d1_mm, st->d2_mm);

        if (link_ok && (dmin != (uint16_t)DIST_INVALID_MM)) {
            (void)snprintf(buf, sizeof(buf), "d_min: %u.%u cm",
                           (unsigned)(dmin / 10U), (unsigned)(dmin % 10U));
        } else {
            (void)snprintf(buf, sizeof(buf), "d_min: --");
        }
    }
    set_line(lines[2], buf);

    /* Zeile 4: Programmende, Verbindung oder Fehlercode */
    if (in->ctrl.shutdown) {
        (void)snprintf(buf, sizeof(buf), "%s", shutdown_text(in->link.stop_result));
    } else if (!link_ok) {
        (void)snprintf(buf, sizeof(buf), "Verbindung getrennt");
    } else if (st->error != ERR_NONE) {
        (void)snprintf(buf, sizeof(buf), "Fehler:%s", name_error(st->error));
    } else {
        (void)snprintf(buf, sizeof(buf), "kein Fehler");
    }
    set_line(lines[3], buf);
}

/* ===== Thread ======================================================================= */

/* Gibt alle Zeilen aus. 0 = OK, -1 = Ausgabe fehlgeschlagen. */
static int32_t output_lines(bool console, char_t lines[PI1_LCD_ROWS][DISPLAY_LINE_LEN],
                            char_t shown[PI1_LCD_ROWS][DISPLAY_LINE_LEN])
{
    int32_t result = 0;
    uint8_t row;

    for (row = 0U; (row < PI1_LCD_ROWS) && (result == 0); row++) {
        if (console) {
            result = lcd_console_write_line(row, lines[row]);
        } else if (strcmp(lines[row], shown[row]) != 0) {
            /* I2C ist langsam (rund 12 ms je Zeile): nur geaenderte Zeilen schreiben. */
            result = lcd_i2c_write_line(row, lines[row]);
            if (result == 0) {
                (void)memcpy(shown[row], lines[row], DISPLAY_LINE_LEN);
            }
        } else {
            /* Zeile unveraendert */
        }
    }
    if (console) {
        lcd_console_refresh();
    }
    return result;
}

void *display_thread(void *arg)
{
    const display_cfg_t *cfg = (const display_cfg_t *)arg;
    const uint64_t period_us = (uint64_t)PI1_DISPLAY_PERIOD_MS * TIME_US_PER_MS;
    char_t lines[PI1_LCD_ROWS][DISPLAY_LINE_LEN];
    char_t shown[PI1_LCD_ROWS][DISPLAY_LINE_LEN];
    bool console = true;
    bool lcd_ok = true;
    bool running = true;
    uint64_t next_us = time_now_us();

    (void)memset(shown, 0, sizeof(shown));
    if (cfg != NULL) {
        console = cfg->use_console;
    }
    if (console) {
        lcd_console_init();
    } else if ((cfg == NULL) || (lcd_i2c_init(cfg->i2c_bus, cfg->lcd_addr) != 0)) {
        log_event(LOG_SRC_DISPLAY, "LCD_AUSFALL Initialisierung", true, LOG_LEVEL_NONE, NULL);
        lcd_ok = false;
    } else {
        log_event(LOG_SRC_DISPLAY, "LCD bereit", false, LOG_LEVEL_NONE, NULL);
    }

    while (running) {
        display_input_t in;

        (void)memset(&in, 0, sizeof(in));
        shared_operator_read(&in.op);
        shared_link_read(&in.link);
        shared_ctrl_read(&in.ctrl);
        in.now_us = time_now_us();
        running = !in.ctrl.stop_threads;

        if (lcd_ok) {
            display_format(&in, lines);
            if (output_lines(console, lines, shown) != 0) {
                log_event(LOG_SRC_DISPLAY, "LCD_AUSFALL Schreiben", true, LOG_LEVEL_NONE, NULL);
                lcd_ok = false;
                lcd_i2c_close();
            }
        }

        if (running) {
            next_us += period_us;
            if (next_us <= time_now_us()) {
                next_us = time_now_us() + period_us;
            }
            time_sleep_until_us(next_us);
        }
    }

    if (console) {
        lcd_console_close();
    } else if (lcd_ok) {
        lcd_i2c_close();
    } else {
        /* LCD bereits geschlossen */
    }
    return NULL;
}

/*
 * test_pi1.c - Unit-Tests der hardwareunabhaengigen Teile von Pi 1:
 * Anzeige (d_min, "Verbindung getrennt", Zeilen), Tastenauswertung,
 * Log-Puffer (Vorrang der Ereignisse), CSV-Format und Kommandozeile.
 * Ohne externe Testbibliothek.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include <stdio.h>
#include <string.h>

#include "input.h"
#include "logging.h"
#include "options.h"
#include "pi1_config.h"
#include "protocol.h"
#include "time_utils.h"
#include "types.h"
#include "ui_lcd.h"

static int32_t g_checks = 0;
static int32_t g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            (void)printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                        \
    } while (false)

#define MS(x) ((uint64_t)(x) * TIME_US_PER_MS)

/* --- d_min und Verbindung ------------------------------------------------------ */

static void test_dmin(void)
{
    CHECK(display_dmin(500U, 300U) == 300U);
    CHECK(display_dmin(300U, 500U) == 300U);
    CHECK(display_dmin(DIST_INVALID_MM, 420U) == 420U);   /* nur gueltige Werte */
    CHECK(display_dmin(420U, DIST_INVALID_MM) == 420U);
    CHECK(display_dmin(DIST_INVALID_MM, DIST_INVALID_MM) == DIST_INVALID_MM);
    CHECK(display_dmin(0U, 10U) == 0U);
}

static void test_link_ok(void)
{
    link_status_t link;
    uint64_t t0 = MS(10000);

    (void)memset(&link, 0, sizeof(link));
    CHECK(!display_link_ok(&link, t0));
    link.online = true;
    link.have_status = true;
    link.last_status_us = t0;
    CHECK(display_link_ok(&link, t0));
    CHECK(display_link_ok(&link, t0 + MS(300)));          /* genau 300 ms: noch aktuell */
    CHECK(!display_link_ok(&link, t0 + MS(300) + 1U));    /* laenger als 300 ms: getrennt */
    link.online = false;
    CHECK(!display_link_ok(&link, t0));
}

/* --- Anzeigezeilen ----------------------------------------------------------------- */

static void test_display_lines(void)
{
    display_input_t in;
    char_t l[PI1_LCD_ROWS][DISPLAY_LINE_LEN];
    uint8_t row;

    (void)memset(&in, 0, sizeof(in));
    in.now_us = MS(5000);

    /* Start: keine Stufe, keine Verbindung */
    display_format(&in, l);
    for (row = 0U; row < PI1_LCD_ROWS; row++) {
        CHECK(strlen(l[row]) == (size_t)PI1_LCD_COLS); /* immer genau 20 Zeichen */
    }
    CHECK(strncmp(l[0], "Status: --", 10U) == 0);
    CHECK(strncmp(l[1], "Stufe waehlen 1/2/3", 19U) == 0);
    CHECK(strncmp(l[2], "d_min: --", 9U) == 0);
    CHECK(strncmp(l[3], "Verbindung getrennt", 19U) == 0);

    /* Betrieb */
    in.op.level_chosen = true;
    in.op.level = LEVEL_MEDIUM;
    in.link.online = true;
    in.link.have_status = true;
    in.link.last_status_us = in.now_us - MS(50);
    in.link.status.state = STATE_BETRIEB;
    in.link.status.error = ERR_NONE;
    in.link.status.level = LEVEL_MEDIUM;
    in.link.status.d1_mm = 1234U;
    in.link.status.d2_mm = 987U;
    display_format(&in, l);
    CHECK(strncmp(l[0], "Status: BETRIEB     ", 20U) == 0);
    CHECK(strncmp(l[1], "Soll:MEDIUM Ist:MEDIUM", 20U) == 0); /* auf 20 gekuerzt */
    CHECK(strncmp(l[2], "d_min: 98.7 cm", 14U) == 0);
    CHECK(strncmp(l[3], "kein Fehler", 11U) == 0);

    /* Laengster Zustand passt genau in 20 Zeichen */
    in.link.status.state = STATE_OBJEKT_STOPP;
    display_format(&in, l);
    CHECK(strcmp(l[0], "Status: OBJEKT_STOPP") == 0);

    /* Fehler mit Code */
    in.link.status.state = STATE_FEHLER;
    in.link.status.error = ERR_KOMMUNIKATION;
    in.link.status.level = LEVEL_OFF;
    display_format(&in, l);
    CHECK(strcmp(l[3], "Fehler:KOMMUNIKATION") == 0);
    CHECK(strncmp(l[1], "Soll:MEDIUM Ist:AUS", 19U) == 0);

    /* Kein gueltiger Abstand */
    in.link.status.d1_mm = DIST_INVALID_MM;
    in.link.status.d2_mm = DIST_INVALID_MM;
    display_format(&in, l);
    CHECK(strncmp(l[2], "d_min: --", 9U) == 0);

    /* 301 ms ohne STATUS: alter Zustand wird nicht mehr als aktuell gezeigt */
    in.link.status.d1_mm = 500U;
    in.link.last_status_us = in.now_us - MS(301);
    display_format(&in, l);
    CHECK(strncmp(l[0], "Status: --", 10U) == 0);
    CHECK(strncmp(l[1], "Soll:MEDIUM Ist:--", 18U) == 0);
    CHECK(strncmp(l[2], "d_min: --", 9U) == 0);
    CHECK(strncmp(l[3], "Verbindung getrennt", 19U) == 0);

    /* Programmende */
    in.ctrl.shutdown = true;
    in.link.stop_result = STOP_RESULT_NONE;
    display_format(&in, l);
    CHECK(strncmp(l[3], "STOP, warte auf ACK", 19U) == 0);
    in.link.stop_result = STOP_RESULT_NO_ACK;
    display_format(&in, l);
    CHECK(strncmp(l[3], "Beendet ohne ACK", 16U) == 0);
}

/* --- Tasten ------------------------------------------------------------------------ */

static void test_keys(void)
{
    CHECK(input_decode_key('1') == LEVEL_LOW);
    CHECK(input_decode_key('2') == LEVEL_MEDIUM);
    CHECK(input_decode_key('3') == LEVEL_HIGH);
    CHECK(input_decode_key('q') == INPUT_KEY_QUIT);
    CHECK(input_decode_key('Q') == INPUT_KEY_QUIT);
    CHECK(input_decode_key('0') == 0U);
    CHECK(input_decode_key('4') == 0U);
    CHECK(input_decode_key(' ') == 0U);
}

/* --- Log-Puffer ------------------------------------------------------------------ */

static void test_log_ring(void)
{
    static log_ring_t ring;
    log_record_t meas;
    log_record_t ev;
    log_record_t out;
    uint32_t i;
    uint32_t stored = 0U;

    (void)memset(&meas, 0, sizeof(meas));
    (void)memset(&ev, 0, sizeof(ev));
    meas.type = LOG_TYPE_MEAS;
    ev.type = LOG_TYPE_EVENT;
    log_ring_reset(&ring);

    /* Messwerte fuellen nur bis zur Reserve */
    for (i = 0U; i < PI1_LOG_RING_LEN; i++) {
        meas.mono_ms = i;
        if (log_ring_push(&ring, &meas)) {
            stored++;
        }
    }
    CHECK(stored == (PI1_LOG_RING_LEN - PI1_LOG_EVENT_RESERVE));
    CHECK(ring.dropped_meas == PI1_LOG_EVENT_RESERVE);

    /* Ereignisse passen noch in die Reserve (Vorrang) */
    for (i = 0U; i < PI1_LOG_EVENT_RESERVE; i++) {
        CHECK(log_ring_push(&ring, &ev));
    }
    CHECK(ring.count == PI1_LOG_RING_LEN);
    CHECK(!log_ring_push(&ring, &ev)); /* jetzt wirklich voll */
    CHECK(ring.dropped_events == 1U);

    /* Reihenfolge bleibt erhalten (aeltester zuerst) */
    CHECK(log_ring_pop(&ring, &out));
    CHECK((out.type == LOG_TYPE_MEAS) && (out.mono_ms == 0U));
    CHECK(log_ring_pop(&ring, &out));
    CHECK(out.mono_ms == 1U);
    while (log_ring_pop(&ring, &out)) {
        /* leeren */
    }
    CHECK(ring.count == 0U);
    CHECK(!log_ring_pop(&ring, &out));
}

static void test_csv(void)
{
    log_record_t rec;
    char_t line[256];

    CHECK(log_format_header(line, sizeof(line)) > 0);
    CHECK(strcmp(line, "wall_time,mono_ms,type,event,state,error,"
                       "level_requested,level_active,d1_mm,d2_mm\n") == 0);

    (void)memset(&rec, 0, sizeof(rec));
    rec.type = LOG_TYPE_MEAS;
    rec.mono_ms = 123456U;
    rec.level_req = LEVEL_HIGH;
    rec.has_status = true;
    rec.status.state = STATE_OBJEKT_STOPP;
    rec.status.error = ERR_NONE;
    rec.status.level = LEVEL_OFF;
    rec.status.d1_mm = 250U;
    rec.status.d2_mm = DIST_INVALID_MM;
    rec.wall.tv_sec = 0;
    CHECK(log_format_row(&rec, line, sizeof(line)) > 0);
    /* wall_time haengt von der Zeitzone ab; Rest ab dem ersten Komma pruefen */
    CHECK(strstr(line, ",123456,MEAS,,OBJEKT_STOPP,-,HIGH,AUS,250,\n") != NULL);

    rec.type = LOG_TYPE_EVENT;
    rec.has_status = false;
    rec.level_req = LOG_LEVEL_NONE;
    (void)snprintf(rec.event, sizeof(rec.event), "TEST, mit Komma");
    CHECK(log_format_row(&rec, line, sizeof(line)) > 0);
    CHECK(strstr(line, ",EVENT,TEST; mit Komma,,,-,,,\n") != NULL);

    CHECK(log_format_row(&rec, line, 10U) < 0); /* zu kleiner Puffer */
}

/* --- Kommandozeile ---------------------------------------------------------------- */

static void test_options(void)
{
    options_t opt;
    char_t err[128];
    char_t *a1[] = { "pi1_app", NULL };
    char_t *a2[] = { "pi1_app", "--tcp", "--tcp-port", "6000", "--lcd-console", "--key", "/tmp/k.bin", NULL };
    char_t *a3[] = { "pi1_app", "--bt-addr", "DC:A6:32:01:02:03", "--lcd-addr", "0x3f", NULL };
    char_t *a4[] = { "pi1_app", "--bt-addr", "DC:A6:32:01:02", NULL };
    char_t *a5[] = { "pi1_app", "--tcp", "--tcp-port", "70000", NULL };
    char_t *a6[] = { "pi1_app", "--unbekannt", NULL };
    char_t *a7[] = { "pi1_app", "--tcp", "--channel", NULL };

    CHECK(options_parse(1, a1, &opt, err, sizeof(err)) != 0); /* RFCOMM ohne Adresse */

    CHECK(options_parse(7, a2, &opt, err, sizeof(err)) == 0);
    CHECK(opt.transport.kind == TRANSPORT_KIND_TCP);
    CHECK(opt.transport.tcp_port == 6000U);
    CHECK(strcmp(opt.transport.host, "127.0.0.1") == 0);
    CHECK(opt.lcd_console);
    CHECK(strcmp(opt.key_path, "/tmp/k.bin") == 0);
    CHECK(strcmp(opt.log_dir, "./logs") == 0);

    CHECK(options_parse(5, a3, &opt, err, sizeof(err)) == 0);
    CHECK(opt.transport.kind == TRANSPORT_KIND_RFCOMM);
    CHECK(strcmp(opt.transport.bt_addr, "DC:A6:32:01:02:03") == 0);
    CHECK(opt.transport.rfcomm_channel == 1U);
    CHECK(opt.lcd_addr == 0x3FU);
    CHECK(strcmp(opt.key_path, "/etc/ssm/psk.bin") == 0);
    CHECK(strcmp(opt.i2c_bus, "/dev/i2c-1") == 0);

    CHECK(options_parse(3, a4, &opt, err, sizeof(err)) != 0); /* Adresse zu kurz */
    CHECK(options_parse(4, a5, &opt, err, sizeof(err)) != 0); /* Port zu gross */
    CHECK(options_parse(2, a6, &opt, err, sizeof(err)) != 0);
    CHECK(options_parse(3, a7, &opt, err, sizeof(err)) != 0); /* Wert fehlt */
}

int main(void)
{
    test_dmin();
    test_link_ok();
    test_display_lines();
    test_keys();
    test_log_ring();
    test_csv();
    test_options();

    (void)printf("%d Pruefungen, %d Fehler\n", (int)g_checks, (int)g_failures);
    return (g_failures == 0) ? 0 : 1;
}

/*
 * test_pi2.c - Unit-Tests der hardwareunabhaengigen Pi-2-Logik:
 * Einzelpruefungen P1/P2, Safety-Logik (P3, P4, Zustaende, Wiederanlauf),
 * Motorstufen und Nachrichtenformat.
 *
 * Die Testnamen verweisen auf die Testfaelle TC-xx des Designdokuments.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "motor.h"
#include "protocol.h"
#include "safety_logic.h"
#include "sensors.h"
#include "shared.h"

static int32_t g_checks = 0;
static int32_t g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            (void)printf("FEHLGESCHLAGEN %s:%" PRId32 ": %s\n", __FILE__, (int32_t)__LINE__, #cond); \
        }                                                                        \
    } while (0)

#define MS(x) ((uint64_t)(x) * 1000ULL)
#define T0 MS(10000) /* beliebiger Startzeitpunkt */

/* --- Hilfsfunktionen ------------------------------------------------------ */

static sensor_data_t sensor(uint16_t mm, bool valid, uint64_t ts_us, uint32_t seq)
{
    sensor_data_t s;
    s.distance_mm = mm;
    s.valid = valid;
    s.timestamp_us = ts_us;
    s.seq = seq;
    return s;
}

static comm_data_t comm(uint8_t level, uint64_t last_valid_us)
{
    comm_data_t c;
    c.level = level;
    c.stop = false;
    c.have_msg = true;
    c.last_valid_us = last_valid_us;
    return c;
}

/* Ein Zyklus mit frischen Daten zum Zeitpunkt now. */
static safety_decision_t cycle(safety_ctx_t *ctx, uint16_t d1, uint16_t d2, uint8_t level,
                               uint64_t now_us, uint32_t seq)
{
    sensor_data_t s[SENSOR_COUNT];
    comm_data_t c = comm(level, now_us - MS(20));
    safety_decision_t d;

    s[0] = sensor(d1, true, now_us - MS(40), seq);
    s[1] = sensor(d2, true, now_us - MS(10), seq);
    safety_evaluate(ctx, s, &c, false, now_us, &d);
    return d;
}

/* --- P1 / P2 --------------------------------------------------------------- */

static void test_p1_range(void) /* TC-01, TC-01a */
{
    sensor_check_t ctx;
    uint16_t mm = 0U;

    sensor_check_init(&ctx);
    CHECK(!sensor_check_sample(&ctx, true, 1000U, MS(0) + 1U, &mm)); /* erste Messung: P2 nicht pruefbar */
    CHECK(mm == DIST_INVALID_MM);
    CHECK(sensor_check_sample(&ctx, true, 1000U, MS(60), &mm));
    CHECK(mm == 1000U);

    CHECK(!sensor_check_sample(&ctx, true, 4001U, MS(120), &mm)); /* > 400 cm */
    CHECK(!sensor_check_sample(&ctx, true, 19U, MS(180), &mm));   /* < 2 cm */
    CHECK(!sensor_check_sample(&ctx, false, 0U, MS(240), &mm));   /* kein Echo */
    CHECK(mm == DIST_INVALID_MM);

    /* Grenzen des Messbereichs sind gueltig */
    sensor_check_init(&ctx);
    (void)sensor_check_sample(&ctx, true, 20U, MS(300), &mm);
    CHECK(sensor_check_sample(&ctx, true, 20U, MS(360), &mm));
    sensor_check_init(&ctx);
    (void)sensor_check_sample(&ctx, true, 4000U, MS(420), &mm);
    CHECK(sensor_check_sample(&ctx, true, 4000U, MS(480), &mm));
}

static void test_p2_rate(void) /* TC-01b */
{
    sensor_check_t ctx;
    uint16_t mm = 0U;

    sensor_check_init(&ctx);
    (void)sensor_check_sample(&ctx, true, 1000U, MS(1000), &mm);
    /* 60 ms * 2 mm/ms = 120 mm erlaubt */
    CHECK(sensor_check_sample(&ctx, true, 1120U, MS(1060), &mm));
    CHECK(sensor_check_sample(&ctx, true, 1000U, MS(1120), &mm));
    /* Sprung um 121 mm in 60 ms: unplausibel */
    CHECK(!sensor_check_sample(&ctx, true, 879U, MS(1180), &mm));
    CHECK(mm == DIST_INVALID_MM);
    /* Bleibt der neue Wert stabil, ist die naechste Messung wieder gueltig */
    CHECK(sensor_check_sample(&ctx, true, 879U, MS(1240), &mm));
    CHECK(mm == 879U);
    /* Nach einem ungueltigen Wert (P1) ist die erste Messung nicht pruefbar */
    CHECK(!sensor_check_sample(&ctx, false, 0U, MS(1300), &mm));
    CHECK(!sensor_check_sample(&ctx, true, 879U, MS(1360), &mm));
    CHECK(sensor_check_sample(&ctx, true, 879U, MS(1420), &mm));
    /* Gleicher Zeitstempel: nicht pruefbar */
    CHECK(!sensor_check_sample(&ctx, true, 879U, MS(1420), &mm));
}

/* --- Safety-Logik ----------------------------------------------------------- */

static void test_normal_operation(void)
{
    safety_ctx_t ctx;
    safety_decision_t d;

    safety_ctx_init(&ctx);
    d = cycle(&ctx, 1000U, 1010U, LEVEL_LOW, T0, 1U);
    CHECK(d.state == STATE_BETRIEB);
    CHECK(d.error == ERR_NONE);
    CHECK(d.motor_level == LEVEL_LOW);

    d = cycle(&ctx, 1000U, 1010U, LEVEL_HIGH, T0 + MS(50), 2U);
    CHECK(d.state == STATE_BETRIEB);
    CHECK(d.motor_level == LEVEL_HIGH);
}

static void test_startup_stays_safe(void) /* SAF-02 */
{
    safety_ctx_t ctx;
    sensor_data_t s[SENSOR_COUNT];
    comm_data_t c;
    safety_decision_t d;

    safety_ctx_init(&ctx);
    (void)memset(s, 0, sizeof(s));
    (void)memset(&c, 0, sizeof(c));

    /* Noch keine Messung, noch keine Nachricht */
    safety_evaluate(&ctx, s, &c, false, T0, &d);
    CHECK(d.state == STATE_FEHLER);
    CHECK(d.error == ERR_SENSOR);
    CHECK(d.motor_level == LEVEL_OFF);

    /* Sensoren gueltig, aber noch keine Nachricht von Pi 1 */
    s[0] = sensor(1000U, true, T0, 1U);
    s[1] = sensor(1000U, true, T0, 1U);
    safety_evaluate(&ctx, s, &c, false, T0 + MS(10), &d);
    CHECK(d.state == STATE_FEHLER);
    CHECK(d.error == ERR_KOMMUNIKATION);
    CHECK(d.motor_level == LEVEL_OFF);

    /* Ungueltige Argumente fuehren nie zu einer Freigabe */
    safety_evaluate(NULL, s, &c, false, T0, &d);
    CHECK(d.motor_level == LEVEL_OFF);
    CHECK(d.state == STATE_FEHLER);
}

static void test_invalid_sensor(void) /* TC-01 */
{
    safety_ctx_t ctx;
    sensor_data_t s[SENSOR_COUNT];
    comm_data_t c = comm(LEVEL_LOW, T0);
    safety_decision_t d;

    safety_ctx_init(&ctx);
    s[0] = sensor(1000U, true, T0, 1U);
    s[1] = sensor(DIST_INVALID_MM, false, T0, 1U);
    safety_evaluate(&ctx, s, &c, false, T0 + MS(10), &d);
    CHECK(d.state == STATE_FEHLER);
    CHECK(d.error == ERR_SENSOR);
    CHECK(d.motor_level == LEVEL_OFF);

    s[0] = sensor(DIST_INVALID_MM, false, T0, 1U);
    s[1] = sensor(1000U, true, T0, 1U);
    safety_evaluate(&ctx, s, &c, false, T0 + MS(10), &d);
    CHECK(d.error == ERR_SENSOR);
    CHECK(d.motor_level == LEVEL_OFF);
}

static void test_cross_check(void) /* TC-02 */
{
    safety_ctx_t ctx;
    safety_decision_t d;

    safety_ctx_init(&ctx);
    /* Abweichung genau 50 mm ist noch zulaessig */
    d = cycle(&ctx, 1000U, 1050U, LEVEL_LOW, T0, 1U);
    CHECK(d.state == STATE_BETRIEB);

    /* Erster Messzyklus mit Abweichung > 50 mm: noch kein Fehler */
    d = cycle(&ctx, 1000U, 1051U, LEVEL_LOW, T0 + MS(60), 2U);
    CHECK(d.state == STATE_BETRIEB);
    /* Derselbe Messzyklus wird nicht doppelt gezaehlt (Safety-Zyklus < Messzyklus) */
    d = cycle(&ctx, 1000U, 1051U, LEVEL_LOW, T0 + MS(110), 2U);
    CHECK(d.state == STATE_BETRIEB);
    /* Zweiter aufeinanderfolgender Messzyklus: Fehler, Motor AUS */
    d = cycle(&ctx, 1000U, 1051U, LEVEL_LOW, T0 + MS(120), 3U);
    CHECK(d.state == STATE_FEHLER);
    CHECK(d.error == ERR_SENSOR);
    CHECK(d.motor_level == LEVEL_OFF);
    /* Fehler bleibt, solange die Abweichung besteht */
    d = cycle(&ctx, 1000U, 1200U, LEVEL_LOW, T0 + MS(180), 4U);
    CHECK(d.state == STATE_FEHLER);
    /* Nicht selbsthaltend: plausibler Messzyklus hebt den Fehler auf (SAF-11) */
    d = cycle(&ctx, 1000U, 1010U, LEVEL_LOW, T0 + MS(240), 5U);
    CHECK(d.state == STATE_BETRIEB);

    /* Einzelne Abweichung, danach wieder plausibel: Zaehler wird zurueckgesetzt */
    d = cycle(&ctx, 1000U, 1100U, LEVEL_LOW, T0 + MS(300), 6U);
    CHECK(d.state == STATE_BETRIEB);
    d = cycle(&ctx, 1000U, 1000U, LEVEL_LOW, T0 + MS(360), 7U);
    CHECK(d.state == STATE_BETRIEB);
    d = cycle(&ctx, 1000U, 1100U, LEVEL_LOW, T0 + MS(420), 8U);
    CHECK(d.state == STATE_BETRIEB);
}

static void test_comm_timeout(void) /* TC-03 */
{
    safety_ctx_t ctx;
    sensor_data_t s[SENSOR_COUNT];
    comm_data_t c = comm(LEVEL_MEDIUM, T0);
    safety_decision_t d;
    uint64_t now;

    safety_ctx_init(&ctx);

    /* Genau 300 ms nach der letzten gueltigen Nachricht: noch gueltig */
    now = T0 + MS(300);
    s[0] = sensor(1000U, true, now - MS(20), 1U);
    s[1] = sensor(1000U, true, now - MS(20), 1U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.state == STATE_BETRIEB);
    CHECK(d.motor_level == LEVEL_MEDIUM);

    /* Mehr als 300 ms: Kommunikationsfehler, Motor AUS */
    now = T0 + MS(300) + 1U;
    s[0] = sensor(1000U, true, now - MS(20), 2U);
    s[1] = sensor(1000U, true, now - MS(20), 2U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.state == STATE_FEHLER);
    CHECK(d.error == ERR_KOMMUNIKATION);
    CHECK(d.motor_level == LEVEL_OFF);

    /* Wiederanlauf nach neuer gueltiger Nachricht, ohne Quittierung (TC-07) */
    c = comm(LEVEL_MEDIUM, now);
    now += MS(50);
    s[0] = sensor(1000U, true, now - MS(20), 3U);
    s[1] = sensor(1000U, true, now - MS(20), 3U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.state == STATE_BETRIEB);
    CHECK(d.motor_level == LEVEL_MEDIUM);

    /* Ungueltige Stufe in comm_data: kein Default-Betrieb */
    c = comm(0x07U, now);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.state == STATE_FEHLER);
    CHECK(d.error == ERR_KOMMUNIKATION);
    c = comm(LEVEL_OFF, now);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.motor_level == LEVEL_OFF);

    /* Zeitstempel in der Zukunft oder 0 gilt als veraltet */
    c = comm(LEVEL_LOW, now + MS(1));
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.error == ERR_KOMMUNIKATION);
}

static void test_stale_data(void) /* TC-04 */
{
    safety_ctx_t ctx;
    sensor_data_t s[SENSOR_COUNT];
    comm_data_t c;
    safety_decision_t d;
    uint64_t now = T0 + MS(1000);

    safety_ctx_init(&ctx);
    c = comm(LEVEL_LOW, now - MS(10));

    /* Genau 150 ms alt: noch gueltig */
    s[0] = sensor(1000U, true, now - MS(150), 1U);
    s[1] = sensor(1000U, true, now - MS(30), 1U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.state == STATE_BETRIEB);

    /* Aelter als 150 ms: DATENALTER, Motor AUS (ein Sensor genuegt) */
    s[0] = sensor(1000U, true, now - MS(150) - 1U, 1U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.state == STATE_FEHLER);
    CHECK(d.error == ERR_DATENALTER);
    CHECK(d.motor_level == LEVEL_OFF);

    s[0] = sensor(1000U, true, now - MS(30), 2U);
    s[1] = sensor(1000U, true, now - MS(151), 1U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.error == ERR_DATENALTER);
}

static void test_object_stop(void) /* TC-05, TC-07 */
{
    static const uint8_t levels[] = { LEVEL_LOW, LEVEL_MEDIUM, LEVEL_HIGH };
    safety_ctx_t ctx;
    safety_decision_t d;
    uint32_t seq = 1U;
    uint64_t now = T0;
    size_t i;

    CHECK(safety_distance_mm(LEVEL_LOW) < safety_distance_mm(LEVEL_MEDIUM));
    CHECK(safety_distance_mm(LEVEL_MEDIUM) < safety_distance_mm(LEVEL_HIGH));
    CHECK(safety_distance_mm(LEVEL_OFF) == 0U);
    CHECK(safety_distance_mm(0x09U) == 0U);

    safety_ctx_init(&ctx);
    for (i = 0U; i < (sizeof(levels) / sizeof(levels[0])); i++) {
        uint16_t s_mm = safety_distance_mm(levels[i]);

        /* d_min == S: Betrieb erlaubt */
        d = cycle(&ctx, s_mm, (uint16_t)(s_mm + 10U), levels[i], now, seq);
        CHECK(d.state == STATE_BETRIEB);
        CHECK(d.motor_level == levels[i]);
        now += MS(60);
        seq++;

        /* d_min < S: Objekt-Stopp; der kleinere Wert entscheidet */
        d = cycle(&ctx, (uint16_t)(s_mm + 10U), (uint16_t)(s_mm - 1U), levels[i], now, seq);
        CHECK(d.state == STATE_OBJEKT_STOPP);
        CHECK(d.error == ERR_NONE);
        CHECK(d.motor_level == LEVEL_OFF);
        now += MS(60);
        seq++;

        /* Objekt entfernt: Wiederanlauf mit der angeforderten Stufe */
        d = cycle(&ctx, (uint16_t)(s_mm + 20U), (uint16_t)(s_mm + 20U), levels[i], now, seq);
        CHECK(d.state == STATE_BETRIEB);
        CHECK(d.motor_level == levels[i]);
        now += MS(60);
        seq++;
    }

    /* Abstand reicht fuer LOW, aber nicht fuer HIGH */
    d = cycle(&ctx, (uint16_t)(CFG_S_LOW_MM + 10U), (uint16_t)(CFG_S_LOW_MM + 10U), LEVEL_LOW, now, seq);
    CHECK(d.state == STATE_BETRIEB);
    seq++;
    d = cycle(&ctx, (uint16_t)(CFG_S_LOW_MM + 10U), (uint16_t)(CFG_S_LOW_MM + 10U), LEVEL_HIGH,
              now + MS(60), seq);
    CHECK(d.state == STATE_OBJEKT_STOPP);
}

static void test_shutdown(void) /* TC-06, SAF-09 */
{
    safety_ctx_t ctx;
    sensor_data_t s[SENSOR_COUNT];
    comm_data_t c = comm(LEVEL_LOW, T0);
    safety_decision_t d;

    safety_ctx_init(&ctx);
    s[0] = sensor(1000U, true, T0, 1U);
    s[1] = sensor(1000U, true, T0, 1U);

    c.stop = true;
    safety_evaluate(&ctx, s, &c, false, T0 + MS(10), &d);
    CHECK(d.state == STATE_SHUTDOWN);
    CHECK(d.motor_level == LEVEL_OFF);

    /* SHUTDOWN ist Endzustand, auch wenn danach alles in Ordnung waere */
    c.stop = false;
    safety_evaluate(&ctx, s, &c, false, T0 + MS(20), &d);
    CHECK(d.state == STATE_SHUTDOWN);
    CHECK(d.motor_level == LEVEL_OFF);

    /* Lokaler Stop (Signal) hat Vorrang vor allen anderen Pruefungen */
    safety_ctx_init(&ctx);
    s[1] = sensor(DIST_INVALID_MM, false, T0, 1U);
    safety_evaluate(&ctx, s, &c, true, T0 + MS(10), &d);
    CHECK(d.state == STATE_SHUTDOWN);
    CHECK(d.motor_level == LEVEL_OFF);
}

static void test_check_priority(void)
{
    safety_ctx_t ctx;
    sensor_data_t s[SENSOR_COUNT];
    comm_data_t c;
    safety_decision_t d;
    uint64_t now = T0 + MS(2000);

    /* Sensorfehler + Kommunikationsfehler + Datenalter: Reihenfolge 2, 3, 4 */
    safety_ctx_init(&ctx);
    c = comm(LEVEL_LOW, now - MS(500));
    s[0] = sensor(DIST_INVALID_MM, false, now - MS(500), 1U);
    s[1] = sensor(1000U, true, now - MS(500), 1U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.error == ERR_SENSOR);

    s[0] = sensor(1000U, true, now - MS(500), 2U);
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.error == ERR_KOMMUNIKATION);

    c = comm(LEVEL_LOW, now - MS(10));
    safety_evaluate(&ctx, s, &c, false, now, &d);
    CHECK(d.error == ERR_DATENALTER);
    CHECK(d.motor_level == LEVEL_OFF);
}

static void test_motor_levels(void)
{
    CHECK(motor_duty_permille(LEVEL_OFF) == 0U);
    CHECK(motor_duty_permille(0x04U) == 0U);
    CHECK(motor_duty_permille(0xFFU) == 0U);
    CHECK(motor_duty_permille(LEVEL_LOW) > 0U);
    CHECK(motor_duty_permille(LEVEL_LOW) < motor_duty_permille(LEVEL_MEDIUM));
    CHECK(motor_duty_permille(LEVEL_MEDIUM) < motor_duty_permille(LEVEL_HIGH));
    CHECK(motor_duty_permille(LEVEL_HIGH) <= 1000U);
}

/* --- Nachrichtenformat ------------------------------------------------------ */

static void test_protocol(void)
{
    proto_frame_t tx;
    proto_frame_t rx;
    proto_status_t st = { STATE_BETRIEB, ERR_NONE, LEVEL_HIGH, 1234U, DIST_INVALID_MM };
    proto_status_t st2;
    uint8_t buf[PROTO_MAX_FRAME];
    uint8_t small[PROTO_MAX_FRAME - 1U];
    size_t len = 0U;
    size_t total = 0U;
    size_t i;

    /* STATUS: Roundtrip und Byte-Reihenfolge */
    (void)memset(&tx, 0, sizeof(tx));
    tx.type = MSG_STATUS;
    tx.len = PROTO_STATUS_LEN;
    tx.session_id = 0xA1B2C3D4UL;
    tx.seq = 0x01020304UL;
    proto_status_pack(&st, tx.payload);
    CHECK(proto_encode(&tx, buf, sizeof(buf), &len) == 0);
    CHECK(len == 46U);
    CHECK(buf[0] == MSG_STATUS);
    CHECK((buf[1] == 0x00U) && (buf[2] == 0x07U));
    CHECK((buf[3] == 0xA1U) && (buf[6] == 0xD4U));
    CHECK((buf[7] == 0x01U) && (buf[10] == 0x04U));
    CHECK(proto_peek(buf, &total) == 0);
    CHECK(total == len);
    CHECK(proto_decode(buf, len, &rx) == 0);
    CHECK(rx.type == MSG_STATUS);
    CHECK(rx.session_id == tx.session_id);
    CHECK(rx.seq == tx.seq);
    CHECK(proto_status_unpack(rx.payload, &st2) == 0);
    CHECK((st2.state == st.state) && (st2.level == st.level));
    CHECK((st2.d1_mm == 1234U) && (st2.d2_mm == DIST_INVALID_MM));

    /* Jedes verfaelschte Byte (ausser Typ/Laenge, s. u.) wird erkannt */
    for (i = PROTO_TYPE_LEN_BYTES; i < len; i++) {
        buf[i] ^= 0x01U;
        CHECK(proto_decode(buf, len, &rx) != 0);
        buf[i] ^= 0x01U;
    }
    CHECK(proto_decode(buf, len, &rx) == 0);
    CHECK(proto_decode(buf, len - 1U, &rx) != 0);
    CHECK(proto_encode(&tx, small, sizeof(small), &len) != 0);

    /* Unbekannter Typ und falsche Laenge */
    buf[0] = 0x05U;
    CHECK(proto_peek(buf, &total) != 0);
    buf[0] = MSG_HEARTBEAT; /* erwartet n = 1, im Frame steht 7 */
    CHECK(proto_peek(buf, &total) != 0);
    CHECK(proto_payload_len(0x00U) < 0);
    tx.type = MSG_HEARTBEAT;
    tx.len = 2U;
    CHECK(proto_encode(&tx, buf, sizeof(buf), &len) != 0);

    /* HEARTBEAT und STOP */
    tx.len = PROTO_HEARTBEAT_LEN;
    tx.payload[0] = LEVEL_MEDIUM;
    CHECK(proto_encode(&tx, buf, sizeof(buf), &len) == 0);
    CHECK(len == 40U);
    CHECK(proto_decode(buf, len, &rx) == 0);
    CHECK(rx.payload[0] == LEVEL_MEDIUM);
    tx.type = MSG_STOP;
    tx.len = 0U;
    CHECK(proto_encode(&tx, buf, sizeof(buf), &len) == 0);
    CHECK(len == 39U);
    CHECK(proto_decode(buf, len, &rx) == 0);
    CHECK(rx.type == MSG_STOP);

    /* Wertebereiche */
    CHECK(proto_level_valid(LEVEL_LOW) && proto_level_valid(LEVEL_HIGH));
    CHECK(!proto_level_valid(LEVEL_OFF));
    CHECK(!proto_level_valid(0x04U));
    tx.payload[0] = 0x05U; /* unbekannter Zustand */
    CHECK(proto_status_unpack(tx.payload, &st2) != 0);
}

int main(void)
{
    test_p1_range();
    test_p2_rate();
    test_normal_operation();
    test_startup_stays_safe();
    test_invalid_sensor();
    test_cross_check();
    test_comm_timeout();
    test_stale_data();
    test_object_stop();
    test_shutdown();
    test_check_priority();
    test_motor_levels();
    test_protocol();

    (void)printf("%" PRId32 " Pruefungen, %" PRId32 " fehlgeschlagen\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

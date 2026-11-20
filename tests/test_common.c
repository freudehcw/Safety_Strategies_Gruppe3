/*
 * test_common.c - Unit-Tests fuer src/common/ (Nachrichtenformat, AES-256-GCM,
 * Session, Datenstrom, Schluesseldatei). Ohne externe Testbibliothek.
 *
 * Bezug: Designdokument 2.8 (Frame, Kodierung, Pruefreihenfolge), 5.2 T1/T2,
 * SEC-01 bis SEC-03, TC-08 (verfaelschtes Tag/Payload), TC-09 (Replay,
 * fremde Session).
 *
 * Quellen der Testvektoren: McGrew/Viega, "The Galois/Counter Mode of
 * Operation (GCM)", 2005, Test Cases 13 und 14 (AES-256, 96-Bit-IV); die
 * Werte sind in NIST SP 800-38D referenziert. Kryptografie: OpenSSL EVP
 * (https://www.openssl.org) ueber crypto_utils.c.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "crypto_utils.h"
#include "proto_stream.h"
#include "protocol.h"
#include "session.h"
#include "types.h"

/* --- Kleine Assert-Makros ---------------------------------------------------- */

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

/* Fester Testschluessel, nur fuer die Tests. */
static const uint8_t TEST_KEY[CRYPTO_KEY_LEN] = {
    0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
    0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU,
    0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
    0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU
};

/* Erzeugt einen Frame mit den angegebenen Kopfwerten und kodiert ihn. */
static size_t make_frame(uint8_t type, uint32_t sid, uint32_t seq,
                         const uint8_t *payload, uint16_t len, uint8_t *buf)
{
    proto_frame_t f;
    size_t out = 0U;

    (void)memset(&f, 0, sizeof(f));
    f.type = type;
    f.len = len;
    f.session_id = sid;
    f.seq = seq;
    if ((payload != NULL) && (len > 0U)) {
        (void)memcpy(f.payload, payload, len);
    }
    if (proto_encode(&f, buf, PROTO_MAX_FRAME, &out) != 0) {
        out = 0U;
    }
    return out;
}

/* --- AES-256-GCM gegen bekannte Testvektoren --------------------------------- */

static void test_gcm_vectors(void)
{
    static const uint8_t zero_key[CRYPTO_KEY_LEN] = { 0U };
    static const uint8_t zero_nonce[CRYPTO_NONCE_LEN] = { 0U };
    static const uint8_t zero_pt[16] = { 0U };
    /* Test Case 13: leerer Klartext, keine AAD */
    static const uint8_t tag13[CRYPTO_TAG_LEN] = {
        0x53U, 0x0fU, 0x8aU, 0xfbU, 0xc7U, 0x45U, 0x36U, 0xb9U,
        0xa9U, 0x63U, 0xb4U, 0xf1U, 0xc4U, 0xcbU, 0x73U, 0x8bU
    };
    /* Test Case 14: 16 Null-Bytes Klartext, keine AAD */
    static const uint8_t ct14[16] = {
        0xceU, 0xa7U, 0x40U, 0x3dU, 0x4dU, 0x60U, 0x6bU, 0x6eU,
        0x07U, 0x4eU, 0xc5U, 0xd3U, 0xbaU, 0xf3U, 0x9dU, 0x18U
    };
    static const uint8_t tag14[CRYPTO_TAG_LEN] = {
        0xd0U, 0xd1U, 0xc8U, 0xa7U, 0x99U, 0x99U, 0x6bU, 0xf0U,
        0x26U, 0x5bU, 0x98U, 0xb5U, 0xd4U, 0x8aU, 0xb9U, 0x19U
    };
    uint8_t ct[16];
    uint8_t pt[16];
    uint8_t tag[CRYPTO_TAG_LEN];

    CHECK(crypto_init(zero_key) == 0);

    CHECK(crypto_seal(NULL, 0U, zero_nonce, NULL, 0U, NULL, tag) == 0);
    CHECK(memcmp(tag, tag13, sizeof(tag)) == 0);

    CHECK(crypto_seal(NULL, 0U, zero_nonce, zero_pt, sizeof(zero_pt), ct, tag) == 0);
    CHECK(memcmp(ct, ct14, sizeof(ct)) == 0);
    CHECK(memcmp(tag, tag14, sizeof(tag)) == 0);

    CHECK(crypto_open(NULL, 0U, zero_nonce, ct14, sizeof(ct14), tag14, pt) == 0);
    CHECK(memcmp(pt, zero_pt, sizeof(pt)) == 0);

    /* Verfaelschtes Tag: pt bleibt unveraendert (Inhalt erst nach Tag-Pruefung). */
    (void)memset(pt, 0xAA, sizeof(pt));
    (void)memcpy(tag, tag14, sizeof(tag));
    tag[0] ^= 0x01U;
    CHECK(crypto_open(NULL, 0U, zero_nonce, ct14, sizeof(ct14), tag, pt) != 0);
    CHECK(pt[0] == 0xAAU);
    CHECK(pt[15] == 0xAAU);

    crypto_cleanup();
    CHECK(crypto_seal(NULL, 0U, zero_nonce, NULL, 0U, NULL, tag) != 0); /* nicht initialisiert */
}

/* --- Roundtrip aller Nachrichtentypen ----------------------------------------- */

static void test_roundtrip_all_types(void)
{
    static const uint8_t types[4] = { MSG_HEARTBEAT, MSG_STOP, MSG_STATUS, MSG_ACK };
    /* Erstes Byte 0x02 ist fuer HEARTBEAT die Stufe MEDIUM, fuer STATUS der
     * Zustand OBJEKT_STOPP; STOP und ACK verwenden keine Nutzdaten. */
    uint8_t payload[PROTO_MAX_PAYLOAD] = { 0U };
    proto_status_t st = { STATE_OBJEKT_STOPP, ERR_NONE, LEVEL_LOW, 250U, DIST_INVALID_MM };
    size_t i;

    proto_status_pack(&st, payload);

    for (i = 0U; i < 4U; i++) {
        uint8_t buf[PROTO_MAX_FRAME];
        proto_frame_t rx;
        int32_t expected = proto_payload_len(types[i]);
        uint16_t n = (uint16_t)expected;
        size_t len = make_frame(types[i], 0xCAFEBABEUL, 7U, payload, n, buf);

        CHECK(expected >= 0);
        CHECK(len == ((size_t)PROTO_OVERHEAD + (size_t)n));
        CHECK(proto_decode(buf, len, &rx) == 0);
        CHECK(rx.type == types[i]);
        CHECK(rx.len == n);
        CHECK(rx.session_id == 0xCAFEBABEUL);
        CHECK(rx.seq == 7U);
        CHECK(memcmp(rx.payload, payload, n) == 0);
    }
}

static void test_status_roundtrip(void)
{
    proto_status_t st = { STATE_FEHLER, ERR_DATENALTER, LEVEL_OFF, 1234U, DIST_INVALID_MM };
    proto_status_t out;
    uint8_t payload[PROTO_STATUS_LEN];

    proto_status_pack(&st, payload);
    /* Reihenfolge laut Tabelle 11: Zustand, Fehler, Stufe, d1, d2 (Big Endian) */
    CHECK(payload[0] == STATE_FEHLER);
    CHECK(payload[1] == ERR_DATENALTER);
    CHECK(payload[2] == LEVEL_OFF);
    CHECK((payload[3] == 0x04U) && (payload[4] == 0xD2U)); /* 1234 = 0x04D2 */
    CHECK((payload[5] == 0xFFU) && (payload[6] == 0xFFU));
    CHECK(proto_status_unpack(payload, &out) == 0);
    CHECK((out.state == st.state) && (out.error == st.error) && (out.level == st.level));
    CHECK((out.d1_mm == 1234U) && (out.d2_mm == DIST_INVALID_MM));

    /* Wertebereich: unbekannte Werte werden verworfen */
    payload[0] = 0x05U;
    CHECK(proto_status_unpack(payload, &out) != 0);
    payload[0] = STATE_BETRIEB;
    payload[1] = 0x04U;
    CHECK(proto_status_unpack(payload, &out) != 0);
    payload[1] = ERR_NONE;
    payload[2] = 0x04U;
    CHECK(proto_status_unpack(payload, &out) != 0);
    payload[2] = LEVEL_HIGH;
    CHECK(proto_status_unpack(payload, &out) == 0);
}

/* --- Big-Endian-Kopf und Verschluesselung ------------------------------------ */

static void test_header_layout(void)
{
    uint8_t buf[PROTO_MAX_FRAME];
    uint8_t buf2[PROTO_MAX_FRAME];
    uint8_t payload[PROTO_STATUS_LEN] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U };
    size_t len = make_frame(MSG_STATUS, 0x11223344UL, 0xA1B2C3D4UL, payload, 7U, buf);
    size_t len2 = make_frame(MSG_STATUS, 0x11223344UL, 0xA1B2C3D4UL, payload, 7U, buf2);

    CHECK(len == 46U);
    CHECK(buf[0] == MSG_STATUS);
    CHECK((buf[1] == 0x00U) && (buf[2] == 0x07U));
    CHECK((buf[3] == 0x11U) && (buf[4] == 0x22U) && (buf[5] == 0x33U) && (buf[6] == 0x44U));
    CHECK((buf[7] == 0xA1U) && (buf[8] == 0xB2U) && (buf[9] == 0xC3U) && (buf[10] == 0xD4U));
    /* Nutzdaten sind verschluesselt (nicht im Klartext im Frame) */
    CHECK(memcmp(&buf[23], payload, sizeof(payload)) != 0);
    /* Jede Nachricht hat eine neue Nonce, daher anderer Chiffretext */
    CHECK(len2 == len);
    CHECK(memcmp(&buf[11], &buf2[11], CRYPTO_NONCE_LEN) != 0);
    CHECK(memcmp(&buf[23], &buf2[23], 7U) != 0);
}

/* --- Verfaelschungen (TC-08) ----------------------------------------------------- */

static void test_tamper(void)
{
    uint8_t buf[PROTO_MAX_FRAME];
    uint8_t payload[PROTO_STATUS_LEN] = { STATE_BETRIEB, ERR_NONE, LEVEL_HIGH, 0U, 100U, 0U, 120U };
    proto_frame_t rx;
    size_t len = make_frame(MSG_STATUS, 1U, 1U, payload, 7U, buf);
    size_t i;
    int32_t accepted = 0;

    CHECK(proto_decode(buf, len, &rx) == 0);
    /* Jedes einzelne Byte verfaelschen: AAD (0-10, ausser Typ/Laenge, die
     * proto_peek bereits abweist), Nonce, Nutzdaten und Tag. */
    for (i = 0U; i < len; i++) {
        buf[i] ^= 0x01U;
        if (proto_decode(buf, len, &rx) == 0) {
            accepted++;
        }
        buf[i] ^= 0x01U;
    }
    CHECK(accepted == 0);

    /* Falscher Schluessel */
    {
        uint8_t other[CRYPTO_KEY_LEN];
        (void)memcpy(other, TEST_KEY, sizeof(other));
        other[31] ^= 0x80U;
        CHECK(crypto_init(other) == 0);
        CHECK(proto_decode(buf, len, &rx) != 0);
        CHECK(crypto_init(TEST_KEY) == 0);
        CHECK(proto_decode(buf, len, &rx) == 0);
    }
}

/* --- Typ und Laenge ------------------------------------------------------------ */

static void test_type_length(void)
{
    uint8_t buf[PROTO_MAX_FRAME];
    uint8_t lvl = LEVEL_LOW;
    size_t total = 0U;
    proto_frame_t rx;
    size_t len = make_frame(MSG_HEARTBEAT, 5U, 9U, &lvl, 1U, buf);

    CHECK(len == 40U);
    CHECK(proto_peek(buf, &total) == 0);
    CHECK(total == 40U);

    buf[0] = 0x05U; /* unbekannter Typ */
    CHECK(proto_peek(buf, &total) != 0);
    buf[0] = 0x00U;
    CHECK(proto_peek(buf, &total) != 0);
    buf[0] = MSG_HEARTBEAT;
    buf[2] = 0x02U; /* falsche Laenge fuer HEARTBEAT */
    CHECK(proto_peek(buf, &total) != 0);
    buf[1] = 0x01U; /* Laenge 0x0102 */
    CHECK(proto_peek(buf, &total) != 0);
    buf[1] = 0x00U;
    buf[2] = 0x01U;
    CHECK(proto_decode(buf, len, &rx) == 0);
    CHECK(proto_decode(buf, len - 1U, &rx) != 0); /* abgeschnitten */

    /* Kodieren mit falscher Laenge oder unbekanntem Typ wird abgelehnt */
    CHECK(make_frame(MSG_STOP, 1U, 1U, &lvl, 1U, buf) == 0U);
    CHECK(make_frame(0x09U, 1U, 1U, NULL, 0U, buf) == 0U);
}

/* --- Session und Sequenznummern (TC-09) ------------------------------------------ */

static void test_session(void)
{
    session_t srv;
    session_t cli;
    uint32_t seq = 0U;

    session_reset(&cli);
    CHECK(session_check_rx(&cli, 1U, 1U) == SESSION_ERR_INACTIVE);
    CHECK(session_next_tx_seq(&cli, &seq) == SESSION_ERR_INACTIVE);

    CHECK(session_start_server(&srv) == SESSION_OK);
    CHECK(srv.active && (srv.tx_seq == 0U) && (srv.rx_last_seq == 0U));
    CHECK(session_next_tx_seq(&srv, &seq) == SESSION_OK);
    CHECK(seq == 1U); /* erste Nachricht des Servers: Sequenznummer 1 */

    /* Client uebernimmt die Session aus dem ersten STATUS */
    session_adopt(&cli, srv.id, seq);
    CHECK(cli.active && (cli.id == srv.id) && (cli.rx_last_seq == 1U));
    CHECK(session_next_tx_seq(&cli, &seq) == SESSION_OK);
    CHECK(seq == 1U); /* erster Heartbeat traegt Sequenznummer 1 */

    /* Server prueft Heartbeats */
    CHECK(session_check_rx(&srv, srv.id, 1U) == SESSION_OK);
    CHECK(session_check_rx(&srv, srv.id, 1U) == SESSION_ERR_OLD_SEQ); /* doppelt */
    CHECK(session_check_rx(&srv, srv.id, 3U) == SESSION_OK);          /* Luecke erlaubt */
    CHECK(session_check_rx(&srv, srv.id, 2U) == SESSION_ERR_OLD_SEQ); /* alt */
    CHECK(session_check_rx(&srv, srv.id ^ 1U, 4U) == SESSION_ERR_ID); /* fremde Session */
    CHECK(srv.rx_last_seq == 3U); /* abgelehnte Nachrichten aendern nichts */

    /* Obergrenze empfangen: UINT32_MAX wird angenommen, danach nichts mehr */
    CHECK(session_check_rx(&srv, srv.id, UINT32_MAX) == SESSION_OK);
    CHECK(session_check_rx(&srv, srv.id, UINT32_MAX) == SESSION_ERR_OLD_SEQ);

    /* Obergrenze senden: UINT32_MAX ist die letzte Nummer, kein Ueberlauf */
    cli.tx_seq = UINT32_MAX - 1U;
    CHECK(session_next_tx_seq(&cli, &seq) == SESSION_OK);
    CHECK(seq == UINT32_MAX);
    CHECK(session_next_tx_seq(&cli, &seq) == SESSION_ERR_EXHAUSTED);
    CHECK(cli.tx_seq == UINT32_MAX);

    /* Neue Verbindung: neue Session, Zaehlung beginnt wieder bei 1 */
    {
        uint32_t old_id = srv.id;
        CHECK(session_start_server(&srv) == SESSION_OK);
        CHECK(session_next_tx_seq(&srv, &seq) == SESSION_OK);
        CHECK(seq == 1U);
        /* Nachricht der alten Session wird verworfen (mit 2^-32 Restrisiko gleicher ID) */
        if (srv.id != old_id) {
            CHECK(session_check_rx(&srv, old_id, 5U) == SESSION_ERR_ID);
        }
    }
}

/* --- Datenstrom ----------------------------------------------------------------- */

static void test_stream(void)
{
    proto_stream_t st;
    uint8_t a[PROTO_MAX_FRAME];
    uint8_t b[PROTO_MAX_FRAME];
    uint8_t out[PROTO_MAX_FRAME];
    uint8_t lvl = LEVEL_HIGH;
    size_t la = make_frame(MSG_HEARTBEAT, 3U, 1U, &lvl, 1U, a);
    size_t lb = make_frame(MSG_ACK, 3U, 2U, NULL, 0U, b);
    size_t out_len = 0U;
    size_t i;
    int32_t frames = 0;

    proto_stream_reset(&st);
    CHECK(proto_stream_free(&st) == PROTO_STREAM_BUF_LEN);
    CHECK(proto_stream_next(&st, out, sizeof(out), &out_len) == PROTO_STREAM_NEED_MORE);

    /* Zwei Frames byteweise einspeisen */
    for (i = 0U; i < (la + lb); i++) {
        uint8_t byte = (i < la) ? a[i] : b[i - la];
        CHECK(proto_stream_push(&st, &byte, 1U) == 0);
        if (proto_stream_next(&st, out, sizeof(out), &out_len) == PROTO_STREAM_FRAME) {
            frames++;
            CHECK(out_len == ((frames == 1) ? la : lb));
        }
    }
    CHECK(frames == 2);
    CHECK(st.len == 0U);

    /* Zwei Frames in einem Stueck */
    CHECK(proto_stream_push(&st, a, la) == 0);
    CHECK(proto_stream_push(&st, b, lb) == 0);
    CHECK(proto_stream_next(&st, out, sizeof(out), &out_len) == PROTO_STREAM_FRAME);
    CHECK((out_len == la) && (memcmp(out, a, la) == 0));
    CHECK(proto_stream_next(&st, out, sizeof(out), &out_len) == PROTO_STREAM_FRAME);
    CHECK((out_len == lb) && (memcmp(out, b, lb) == 0));

    /* Unbekannter Typ -> nicht mehr synchron */
    {
        uint8_t junk[3] = { 0x07U, 0x00U, 0x00U };
        CHECK(proto_stream_push(&st, junk, 3U) == 0);
        CHECK(proto_stream_next(&st, out, sizeof(out), &out_len) == PROTO_STREAM_DESYNC);
    }
    /* Falsche Laenge fuer bekannten Typ -> nicht mehr synchron */
    proto_stream_reset(&st);
    {
        uint8_t bad[3] = { MSG_STATUS, 0x00U, 0x06U };
        CHECK(proto_stream_push(&st, bad, 3U) == 0);
        CHECK(proto_stream_next(&st, out, sizeof(out), &out_len) == PROTO_STREAM_DESYNC);
    }
    /* Ueberlauf des Puffers wird abgelehnt */
    proto_stream_reset(&st);
    {
        uint8_t big[PROTO_STREAM_BUF_LEN + 1U];
        (void)memset(big, 0, sizeof(big));
        CHECK(proto_stream_push(&st, big, sizeof(big)) != 0);
        CHECK(st.len == 0U);
    }
}

/* --- Schluesseldatei ------------------------------------------------------------ */

/* Schreibt n Bytes in eine neue Datei mit den Rechten mode. */
static int32_t write_key_file(const char_t *path, size_t n, mode_t mode)
{
    int32_t result = -1;
    FILE *f = fopen(path, "wb");

    if (f != NULL) {
        size_t written = fwrite(TEST_KEY, 1U, n, f);
        if ((fclose(f) == 0) && (written == n) && (chmod(path, mode) == 0)) {
            result = 0;
        }
    }
    return result;
}

static void test_key_file(void)
{
    char_t dir[] = "/tmp/ssm_test_XXXXXX";
    char_t path[64];
    uint8_t key[CRYPTO_KEY_LEN];

    CHECK(mkdtemp(dir) != NULL);
    (void)snprintf(path, sizeof(path), "%s/psk.bin", dir);

    CHECK(crypto_load_key_file(path, key) == CRYPTO_KEY_ERR_OPEN);

    CHECK(write_key_file(path, CRYPTO_KEY_LEN, (mode_t)0600) == 0);
    CHECK(crypto_load_key_file(path, key) == CRYPTO_KEY_OK);
    CHECK(memcmp(key, TEST_KEY, sizeof(key)) == 0);

    CHECK(chmod(path, (mode_t)0400) == 0); /* enger als 0600 ist erlaubt */
    CHECK(crypto_load_key_file(path, key) == CRYPTO_KEY_OK);
    CHECK(chmod(path, (mode_t)0640) == 0);
    CHECK(crypto_load_key_file(path, key) == CRYPTO_KEY_ERR_PERM);
    CHECK(chmod(path, (mode_t)0604) == 0);
    CHECK(crypto_load_key_file(path, key) == CRYPTO_KEY_ERR_PERM);

    CHECK(write_key_file(path, CRYPTO_KEY_LEN - 1U, (mode_t)0600) == 0);
    CHECK(crypto_load_key_file(path, key) == CRYPTO_KEY_ERR_SIZE);

    CHECK(crypto_load_key_file(dir, key) == CRYPTO_KEY_ERR_TYPE);
    CHECK(crypto_load_key_file(NULL, key) == CRYPTO_KEY_ERR_ARG);
    CHECK(crypto_key_error_text(CRYPTO_KEY_ERR_PERM) != NULL);

    CHECK(unlink(path) == 0);
    CHECK(rmdir(dir) == 0);
}

int main(void)
{
    test_gcm_vectors();

    if (crypto_init(TEST_KEY) != 0) {
        (void)printf("crypto_init fehlgeschlagen\n");
        return 1;
    }
    test_roundtrip_all_types();
    test_status_roundtrip();
    test_header_layout();
    test_tamper();
    test_type_length();
    test_session();
    test_stream();
    test_key_file();
    crypto_cleanup();

    (void)printf("%d Pruefungen, %d Fehler\n", (int)g_checks, (int)g_failures);
    return (g_failures == 0) ? 0 : 1;
}

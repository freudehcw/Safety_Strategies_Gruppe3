/*
 * protocol.h - Nachrichtenformat zwischen Pi 1 und Pi 2 (Designdokument,
 * Abschnitt "Bluetooth-Kommunikation", Tabellen Frame-Aufbau und Kodierung).
 *
 * Frame (Big Endian, Feld fuer Feld serialisiert):
 *   0      Typ            1 B  \
 *   1      Laenge n       2 B   | AAD (11 B, unverschluesselt,
 *   3      Session-ID     4 B   |      authentifiziert)
 *   7      Sequenznummer  4 B  /
 *   11     Nonce         12 B
 *   23     Nutzdaten      n B
 *   23+n   Tag           16 B
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Nachrichtentypen */
#define MSG_HEARTBEAT 0x01U /* Pi 1 -> Pi 2, n = 1: Sollstufe */
#define MSG_STOP      0x02U /* Pi 1 -> Pi 2, n = 0 */
#define MSG_STATUS    0x03U /* Pi 2 -> Pi 1, n = 7 */
#define MSG_ACK       0x04U /* Pi 2 -> Pi 1, n = 0 */

/* Geschwindigkeitsstufen */
#define LEVEL_OFF    0x00U /* nur in STATUS: Motor AUS */
#define LEVEL_LOW    0x01U
#define LEVEL_MEDIUM 0x02U
#define LEVEL_HIGH   0x03U

/* Systemzustaende von Pi 2 */
#define STATE_INIT         0x00U
#define STATE_BETRIEB      0x01U
#define STATE_OBJEKT_STOPP 0x02U
#define STATE_FEHLER       0x03U
#define STATE_SHUTDOWN     0x04U

/* Fehlercodes */
#define ERR_NONE          0x00U
#define ERR_SENSOR        0x01U /* P1-P3 */
#define ERR_KOMMUNIKATION 0x02U /* Timeout oder ungueltiger Befehl */
#define ERR_DATENALTER    0x03U /* P4 */

#define DIST_INVALID_MM 0xFFFFU /* kein gueltiger Messwert */

#define PROTO_TYPE_LEN_BYTES 3U  /* Typ + Laenge */
#define PROTO_AAD_LEN        11U
#define PROTO_NONCE_LEN      12U
#define PROTO_TAG_LEN        16U
#define PROTO_OVERHEAD       (PROTO_AAD_LEN + PROTO_NONCE_LEN + PROTO_TAG_LEN)
#define PROTO_MAX_PAYLOAD    7U
#define PROTO_MAX_FRAME      (PROTO_OVERHEAD + PROTO_MAX_PAYLOAD)

#define PROTO_HEARTBEAT_LEN 1U
#define PROTO_STATUS_LEN    7U

typedef struct {
    uint8_t type;
    uint16_t len;
    uint32_t session_id;
    uint32_t seq;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} proto_frame_t;

typedef struct {
    uint8_t state;
    uint8_t error;
    uint8_t level;
    uint16_t d1_mm;
    uint16_t d2_mm;
} proto_status_t;

/* Nutzdatenlaenge fuer einen Typ; -1 bei unbekanntem Typ. */
int32_t proto_payload_len(uint8_t type);

/*
 * Prueft Typ und Laenge aus den ersten 3 Bytes eines Frames. Der Aufrufer
 * muss sicherstellen, dass buf mindestens PROTO_TYPE_LEN_BYTES Bytes enthaelt.
 * 0 = OK (total_len = Gesamtlaenge des Frames), -1 = unbekannter Typ oder
 * falsche Laenge (Datenstrom nicht mehr synchron -> Verbindung abbauen).
 */
int32_t proto_peek(const uint8_t *buf, size_t *total_len);

/* Serialisiert und schuetzt einen Frame. 0 = OK, -1 = Fehler. */
int32_t proto_encode(const proto_frame_t *frame, uint8_t *buf, size_t cap, size_t *out_len);

/* Prueft Format und Tag eines vollstaendigen Frames. 0 = OK, -1 = verwerfen. */
int32_t proto_decode(const uint8_t *buf, size_t len, proto_frame_t *frame);

bool proto_level_valid(uint8_t level); /* LOW, MEDIUM oder HIGH */

void proto_status_pack(const proto_status_t *status, uint8_t payload[PROTO_STATUS_LEN]);
/* 0 = OK, -1 = Wert ausserhalb des definierten Bereichs. */
int32_t proto_status_unpack(const uint8_t payload[PROTO_STATUS_LEN], proto_status_t *status);

#endif /* PROTOCOL_H */

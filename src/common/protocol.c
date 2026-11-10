/*
 * protocol.c - Serialisierung und Pruefung der Frames zwischen Pi 1 und Pi 2
 * (Format siehe protocol.h).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#include "protocol.h"

#include <string.h>

#include "crypto_utils.h"

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int32_t proto_payload_len(uint8_t type)
{
    int32_t len;

    switch (type) {
    case MSG_HEARTBEAT:
        len = (int32_t)PROTO_HEARTBEAT_LEN;
        break;
    case MSG_STATUS:
        len = (int32_t)PROTO_STATUS_LEN;
        break;
    case MSG_STOP:
    case MSG_ACK:
        len = 0;
        break;
    default:
        len = -1;
        break;
    }
    return len;
}

int32_t proto_peek(const uint8_t *buf, size_t *total_len)
{
    int32_t result = -1;

    if ((buf != NULL) && (total_len != NULL)) {
        int32_t expected = proto_payload_len(buf[0]);
        uint16_t len = get_u16(&buf[1]);

        if ((expected >= 0) && ((int32_t)len == expected)) {
            *total_len = (size_t)PROTO_OVERHEAD + (size_t)len;
            result = 0;
        }
    }
    return result;
}

int32_t proto_encode(const proto_frame_t *frame, uint8_t *buf, size_t cap, size_t *out_len)
{
    int32_t result = -1;

    if ((frame != NULL) && (buf != NULL) && (out_len != NULL)) {
        int32_t expected = proto_payload_len(frame->type);
        size_t total = (size_t)PROTO_OVERHEAD + (size_t)frame->len;

        if ((expected >= 0) && ((int32_t)frame->len == expected) && (cap >= total)) {
            uint8_t *nonce = &buf[PROTO_AAD_LEN];
            uint8_t *data = &buf[PROTO_AAD_LEN + PROTO_NONCE_LEN];

            buf[0] = frame->type;
            put_u16(&buf[1], frame->len);
            put_u32(&buf[3], frame->session_id);
            put_u32(&buf[7], frame->seq);

            if ((crypto_random(nonce, PROTO_NONCE_LEN) == 0) &&
                (crypto_seal(buf, PROTO_AAD_LEN, nonce, frame->payload, frame->len,
                             data, &data[frame->len]) == 0)) {
                *out_len = total;
                result = 0;
            }
        }
    }
    return result;
}

int32_t proto_decode(const uint8_t *buf, size_t len, proto_frame_t *frame)
{
    int32_t result = -1;
    size_t total = 0U;

    if ((buf != NULL) && (frame != NULL) && (len >= (size_t)PROTO_OVERHEAD) &&
        (proto_peek(buf, &total) == 0) && (total == len)) {
        const uint8_t *nonce = &buf[PROTO_AAD_LEN];
        const uint8_t *data = &buf[PROTO_AAD_LEN + PROTO_NONCE_LEN];
        uint16_t n = get_u16(&buf[1]);

        (void)memset(frame, 0, sizeof(*frame));
        /* Nutzdaten werden erst nach erfolgreicher Tag-Pruefung uebernommen (SEC-02). */
        if (crypto_open(buf, PROTO_AAD_LEN, nonce, data, n, &data[n], frame->payload) == 0) {
            frame->type = buf[0];
            frame->len = n;
            frame->session_id = get_u32(&buf[3]);
            frame->seq = get_u32(&buf[7]);
            result = 0;
        }
    }
    return result;
}

bool proto_level_valid(uint8_t level)
{
    return (level == LEVEL_LOW) || (level == LEVEL_MEDIUM) || (level == LEVEL_HIGH);
}

void proto_status_pack(const proto_status_t *status, uint8_t payload[PROTO_STATUS_LEN])
{
    if ((status != NULL) && (payload != NULL)) {
        payload[0] = status->state;
        payload[1] = status->error;
        payload[2] = status->level;
        put_u16(&payload[3], status->d1_mm);
        put_u16(&payload[5], status->d2_mm);
    }
}

int32_t proto_status_unpack(const uint8_t payload[PROTO_STATUS_LEN], proto_status_t *status)
{
    int32_t result = -1;

    if ((payload != NULL) && (status != NULL) &&
        (payload[0] <= STATE_SHUTDOWN) && (payload[1] <= ERR_DATENALTER) &&
        (payload[2] <= LEVEL_HIGH)) {
        status->state = payload[0];
        status->error = payload[1];
        status->level = payload[2];
        status->d1_mm = get_u16(&payload[3]);
        status->d2_mm = get_u16(&payload[5]);
        result = 0;
    }
    return result;
}

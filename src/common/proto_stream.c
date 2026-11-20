/*
 * proto_stream.c - Zerlegt den Byte-Datenstrom in Frames
 * (Designdokument Abschnitt 2.8). Beschreibung siehe proto_stream.h.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "proto_stream.h"

#include <string.h>

void proto_stream_reset(proto_stream_t *st)
{
    if (st != NULL) {
        st->len = 0U;
    }
}

size_t proto_stream_free(const proto_stream_t *st)
{
    size_t free_len = 0U;

    if ((st != NULL) && (st->len <= sizeof(st->buf))) {
        free_len = sizeof(st->buf) - st->len;
    }
    return free_len;
}

int32_t proto_stream_push(proto_stream_t *st, const uint8_t *data, size_t len)
{
    int32_t result = -1;

    if ((st != NULL) && ((data != NULL) || (len == 0U)) && (len <= proto_stream_free(st))) {
        if (len > 0U) {
            (void)memcpy(&st->buf[st->len], data, len);
            st->len += len;
        }
        result = 0;
    }
    return result;
}

int32_t proto_stream_next(proto_stream_t *st, uint8_t *frame, size_t cap, size_t *frame_len)
{
    int32_t result = PROTO_STREAM_NEED_MORE;

    if ((st == NULL) || (frame == NULL) || (frame_len == NULL) ||
        (cap < (size_t)PROTO_MAX_FRAME)) {
        result = PROTO_STREAM_DESYNC;
    } else if (st->len >= (size_t)PROTO_TYPE_LEN_BYTES) {
        size_t total = 0U;

        /* Abschnitt 2.8: zuerst Typ und Laenge pruefen. Passen sie nicht
         * zusammen, ist der Datenstrom nicht mehr synchron. */
        if (proto_peek(st->buf, &total) != 0) {
            result = PROTO_STREAM_DESYNC;
        } else if ((total > cap) || (total > sizeof(st->buf))) {
            result = PROTO_STREAM_DESYNC; /* kann bei gueltigem Typ nicht auftreten */
        } else if (st->len < total) {
            result = PROTO_STREAM_NEED_MORE; /* Frame noch unvollstaendig */
        } else {
            /* Genau die angegebene Anzahl Bytes entnehmen, Rest nach vorne. */
            (void)memcpy(frame, st->buf, total);
            *frame_len = total;
            st->len -= total;
            if (st->len > 0U) {
                (void)memmove(st->buf, &st->buf[total], st->len);
            }
            result = PROTO_STREAM_FRAME;
        }
    } else {
        /* Typ und Laenge noch nicht vollstaendig */
    }
    return result;
}

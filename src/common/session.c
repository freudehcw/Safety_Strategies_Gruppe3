/*
 * session.c - Session-ID und Sequenznummern einer Verbindung
 * (Designdokument Abschnitt 5.2 T2, SEC-03). Beschreibung siehe session.h.
 *
 * Quelle fuer den Zufall der Session-ID: getrandom(2), Linux man-pages
 * (ueber crypto_random() in crypto_utils.c).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "session.h"

#include <stddef.h>

#include "crypto_utils.h"

void session_reset(session_t *s)
{
    if (s != NULL) {
        s->active = false;
        s->id = 0U;
        s->tx_seq = 0U;
        s->rx_last_seq = 0U;
    }
}

int32_t session_start_server(session_t *s)
{
    int32_t result = SESSION_ERR_ARG;

    if (s != NULL) {
        uint8_t rnd[4];

        session_reset(s);
        /* Abschnitt 5.2 T2: nach jedem Verbindungsaufbau neue, zufaellige
         * Session-ID, damit Nachrichten aus frueheren Verbindungen auch dann
         * verworfen werden, wenn die Sequenznummer wieder bei 1 beginnt. */
        if (crypto_random(rnd, sizeof(rnd)) == 0) {
            s->id = ((uint32_t)rnd[0] << 24) | ((uint32_t)rnd[1] << 16) |
                    ((uint32_t)rnd[2] << 8) | (uint32_t)rnd[3];
            s->active = true;
            result = SESSION_OK;
        } else {
            result = SESSION_ERR_RANDOM;
        }
    }
    return result;
}

void session_adopt(session_t *s, uint32_t id, uint32_t seq)
{
    if (s != NULL) {
        /* Abschnitt 2.8 / 5.2: Pi 1 uebernimmt die Session-ID aus der ersten
         * STATUS-Nachricht. Diese Nachricht gilt als akzeptiert, deshalb wird
         * ihre Sequenznummer gespeichert. Die eigene Zaehlung beginnt bei 0,
         * die erste eigene Nachricht traegt also die Nummer 1. */
        s->id = id;
        s->rx_last_seq = seq;
        s->tx_seq = 0U;
        s->active = true;
    }
}

int32_t session_check_rx(session_t *s, uint32_t id, uint32_t seq)
{
    int32_t result;

    if (s == NULL) {
        result = SESSION_ERR_ARG;
    } else if (!s->active) {
        result = SESSION_ERR_INACTIVE;
    } else if (id != s->id) {
        result = SESSION_ERR_ID;        /* Nachricht aus anderer Verbindung */
    } else if (seq <= s->rx_last_seq) {
        result = SESSION_ERR_OLD_SEQ;   /* Replay oder doppelte Nachricht */
    } else {
        s->rx_last_seq = seq;
        result = SESSION_OK;
    }
    return result;
}

int32_t session_next_tx_seq(session_t *s, uint32_t *seq)
{
    int32_t result;

    if ((s == NULL) || (seq == NULL)) {
        result = SESSION_ERR_ARG;
    } else if (!s->active) {
        result = SESSION_ERR_INACTIVE;
    } else if (s->tx_seq == UINT32_MAX) {
        /* Abschnitt 5.2: kein Ueberlauf. Die Verbindung muss abgebaut werden. */
        result = SESSION_ERR_EXHAUSTED;
    } else {
        s->tx_seq++;
        *seq = s->tx_seq;
        result = SESSION_OK;
    }
    return result;
}

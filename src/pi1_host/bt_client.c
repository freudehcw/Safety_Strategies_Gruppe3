/*
 * bt_client.c - Kommunikations- und Heartbeat-Thread von Pi 1.
 * Ablauf siehe bt_client.h.
 *
 * Zeitverhalten:
 *   - Alle Zeiten mit CLOCK_MONOTONIC (Abschnitt 2.7, T3).
 *   - Empfang mit poll() und hoechstens 50 ms Timeout; der Timeout wird
 *     verkuerzt, damit der naechste Heartbeat nicht verpasst wird.
 *   - Der letzte Rest (< 1 ms) bis zum Heartbeat-Zeitpunkt wird mit
 *     clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) gewartet. Die
 *     Zeitpunkte sind absolut (+100 ms), Verzoegerungen summieren sich nicht
 *     auf (Festlegung 5).
 *
 * Pruefreihenfolge einer empfangenen Nachricht (Abschnitt 2.8, SEC-02):
 *   Typ und Laenge (proto_stream, sonst Verbindungsabbau) -> GCM-Tag
 *   (proto_decode) -> Session-ID und Sequenznummer (session_check_rx) ->
 *   Wertebereich (proto_status_unpack). Jede fehlgeschlagene Pruefung fuehrt
 *   zum Verwerfen und wird protokolliert (TC-08, TC-09).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "bt_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "logging.h"
#include "pi1_config.h"
#include "names.h"
#include "proto_stream.h"
#include "protocol.h"
#include "session.h"
#include "shared.h"
#include "time_utils.h"

/* Phasen des Kommunikations-Threads */
#define PHASE_DISCONNECTED 0U
#define PHASE_CONNECTING   1U
#define PHASE_WAIT_STATUS  2U
#define PHASE_ONLINE       3U
#define PHASE_STOPPING     4U
#define PHASE_DONE         5U

#define US_PER_MS TIME_US_PER_MS

typedef struct {
    const comm_cfg_t *cfg;
    uint8_t phase;
    transport_t tp;
    proto_stream_t stream;
    session_t session;
    link_status_t link;        /* eigene Kopie, wird nach shared geschrieben */
    operator_cmd_t op;         /* Snapshot der Bedienereingaben */
    uint64_t next_attempt_us;  /* naechster Verbindungsversuch */
    uint64_t phase_since_us;   /* Beginn von VERBINDEN, WARTEN oder STOPPEN */
    uint64_t next_hb_us;       /* naechster Heartbeat-Zeitpunkt (absolut) */
    bool hb_running;           /* Heartbeat-Takt laeuft */
    bool link_lost_logged;     /* Ereignis "STATUS_TIMEOUT" bereits protokolliert */
    bool connect_fail_logged;  /* Fehlschlag des Verbindungsaufbaus bereits protokolliert */
} comm_t;

/* ===== Hilfsfunktionen ========================================================== */

static uint8_t level_for_log(const comm_t *c)
{
    return c->op.level_chosen ? c->op.level : LOG_LEVEL_NONE;
}

static void comm_log(const comm_t *c, const char_t *text, bool urgent)
{
    log_event(LOG_SRC_COMM, text, urgent, level_for_log(c),
              c->link.have_status ? &c->link.status : NULL);
}

/* Baut die Verbindung ab. Beim Stoppen ist der Thread danach fertig. */
static void disconnect(comm_t *c, const char_t *reason)
{
    char_t text[PI1_LOG_EVENT_TEXT_LEN];
    bool was_stopping = (c->phase == PHASE_STOPPING);

    if (transport_is_open(&c->tp)) {
        (void)snprintf(text, sizeof(text), "VERBINDUNG_VERLOREN %s", reason);
        comm_log(c, text, true);
    }
    transport_close(&c->tp);
    session_reset(&c->session);
    proto_stream_reset(&c->stream);
    c->link.online = false;
    c->link.have_status = false;
    c->link.session_id = 0U;
    c->hb_running = false;
    c->link_lost_logged = false;

    if (was_stopping) {
        /* Verbindung waehrend des Wartens auf ACK verloren: kein ACK. */
        c->link.stop_result = STOP_RESULT_NO_ACK;
        comm_log(c, "KEIN_ACK Verbindung beendet", true);
        c->phase = PHASE_DONE;
    } else {
        c->phase = PHASE_DISCONNECTED; /* naechster Versuch zu next_attempt_us */
    }
}

/* Verschluesselt und sendet einen Frame. false = Verbindung abgebaut. */
static bool send_frame(comm_t *c, uint8_t type, const uint8_t *payload, uint16_t len)
{
    proto_frame_t frame;
    uint8_t buf[PROTO_MAX_FRAME];
    size_t total = 0U;
    uint32_t seq = 0U;
    int32_t seq_rc = session_next_tx_seq(&c->session, &seq);
    bool ok = false;

    if (seq_rc == SESSION_ERR_EXHAUSTED) {
        /* Abschnitt 5.2: kein Ueberlauf; neue Verbindung = neue Session. */
        disconnect(c, "Sequenznummer ausgeschoepft");
    } else if (seq_rc != SESSION_OK) {
        disconnect(c, "keine Session");
    } else {
        (void)memset(&frame, 0, sizeof(frame));
        frame.type = type;
        frame.len = len;
        frame.session_id = c->session.id;
        frame.seq = seq;
        if ((payload != NULL) && (len > 0U) && (len <= PROTO_MAX_PAYLOAD)) {
            (void)memcpy(frame.payload, payload, len);
        }
        if (proto_encode(&frame, buf, sizeof(buf), &total) != 0) {
            disconnect(c, "Verschluesselung fehlgeschlagen");
        } else if (transport_send(&c->tp, buf, total, PI1_SEND_TIMEOUT_MS) != TRANSPORT_OK) {
            /* Teilweise gesendeter Frame waere nicht mehr synchron. */
            disconnect(c, "Sendefehler");
        } else {
            ok = true;
        }
    }
    return ok;
}

/* Verwirft eine Nachricht und protokolliert den Grund (TC-08, TC-09). */
static void discard(comm_t *c, const char_t *reason)
{
    char_t text[PI1_LOG_EVENT_TEXT_LEN];

    (void)snprintf(text, sizeof(text), "VERWORFEN %s", reason);
    comm_log(c, text, true);
}

/* Uebernimmt einen gueltigen STATUS und protokolliert Aenderungen. */
static void accept_status(comm_t *c, const proto_status_t *st)
{
    char_t text[PI1_LOG_EVENT_TEXT_LEN];
    bool first = !c->link.have_status;
    proto_status_t old = c->link.status;

    c->link.status = *st;
    c->link.have_status = true;
    c->link.last_status_us = time_now_us();

    /* Abschnitt 2.10: Zustandswechsel und neuer Fehlercode sind Ereignisse. */
    if (first || (old.state != st->state)) {
        (void)snprintf(text, sizeof(text), "ZUSTAND %s", name_state(st->state));
        comm_log(c, text, st->state == STATE_FEHLER);
    }
    if ((!first && (old.error != st->error)) || (first && (st->error != ERR_NONE))) {
        (void)snprintf(text, sizeof(text), "FEHLERCODE %s", name_error(st->error));
        comm_log(c, text, st->error != ERR_NONE);
    }
    if (c->link_lost_logged) {
        comm_log(c, "STATUS_WIEDER Verbindung aktuell", false);
        c->link_lost_logged = false;
    }
    /* Messwerte bei jeder STATUS-Nachricht (10 Hz) */
    log_measurement(LOG_SRC_COMM, level_for_log(c), st);
}

/* ===== Empfang ======================================================================= */

/* Erste STATUS-Nachricht: Session-ID uebernehmen (Abschnitt 2.8, 5.2 T2). */
static void handle_first_frame(comm_t *c, const proto_frame_t *f)
{
    proto_status_t st;

    if (f->type != MSG_STATUS) {
        discard(c, "vor erster STATUS-Nachricht");
    } else if (proto_status_unpack(f->payload, &st) != 0) {
        discard(c, "Wertebereich (erste STATUS)");
    } else {
        char_t text[PI1_LOG_EVENT_TEXT_LEN];

        /* Hinweis (Plan G): Die Aktualitaet dieser ersten Nachricht kann Pi 1
         * nicht pruefen. Eine eingespielte alte Nachricht fuehrt nur dazu, dass
         * Pi 2 die Heartbeats verwirft und in FEHLER bleibt (sicherer Zustand). */
        session_adopt(&c->session, f->session_id, f->seq);
        c->phase = PHASE_ONLINE;
        c->link.online = true;
        c->link.session_id = f->session_id;
        c->hb_running = false;
        (void)snprintf(text, sizeof(text), "SESSION %08" PRIx32 " aufgebaut", f->session_id);
        comm_log(c, text, false);
        accept_status(c, &st);
    }
}

static void handle_frame(comm_t *c, const uint8_t *buf, size_t len)
{
    proto_frame_t f;
    int32_t rc;

    /* 1. GCM-Tag (und Format). Inhalt erst danach verwenden (SEC-02). */
    if (proto_decode(buf, len, &f) != 0) {
        discard(c, "Tag ungueltig");
    } else if (c->phase == PHASE_WAIT_STATUS) {
        handle_first_frame(c, &f);
    } else {
        /* 2./3. Session-ID und Sequenznummer (SEC-03) */
        rc = session_check_rx(&c->session, f.session_id, f.seq);
        if (rc == SESSION_ERR_ID) {
            discard(c, "fremde Session-ID");
        } else if (rc == SESSION_ERR_OLD_SEQ) {
            discard(c, "alte Sequenznummer");
        } else if (rc != SESSION_OK) {
            discard(c, "keine Session");
        } else if (f.type == MSG_STATUS) {
            proto_status_t st;
            /* 4. Wertebereich */
            if (proto_status_unpack(f.payload, &st) == 0) {
                accept_status(c, &st);
            } else {
                discard(c, "Wertebereich");
            }
        } else if (f.type == MSG_ACK) {
            if (c->phase == PHASE_STOPPING) {
                c->link.stop_result = STOP_RESULT_ACKED;
                comm_log(c, "ACK empfangen", false);
                transport_close(&c->tp);
                c->link.online = false;
                c->phase = PHASE_DONE;
            } else {
                discard(c, "ACK ohne STOP");
            }
        } else {
            /* Plan H: HEARTBEAT/STOP sind in dieser Richtung nicht vorgesehen. */
            discard(c, "Typ falsche Richtung");
        }
    }
}

/* Empfaengt hoechstens timeout_ms lang und verarbeitet vollstaendige Frames. */
static void receive(comm_t *c, int32_t timeout_ms)
{
    uint8_t rx[PROTO_STREAM_BUF_LEN];
    size_t got = 0U;
    size_t space = proto_stream_free(&c->stream);
    int32_t rc = transport_recv(&c->tp, rx, (space < sizeof(rx)) ? space : sizeof(rx),
                                &got, timeout_ms);

    if (rc == TRANSPORT_CLOSED) {
        disconnect(c, "Gegenseite hat geschlossen");
    } else if (rc != TRANSPORT_OK) {
        disconnect(c, "Empfangsfehler");
    } else if ((got > 0U) && (proto_stream_push(&c->stream, rx, got) != 0)) {
        disconnect(c, "Empfangspuffer voll");
    } else {
        uint8_t frame[PROTO_MAX_FRAME];
        size_t frame_len = 0U;
        bool more = true;

        while (more && (c->phase >= PHASE_WAIT_STATUS) && (c->phase <= PHASE_STOPPING)) {
            int32_t next = proto_stream_next(&c->stream, frame, sizeof(frame), &frame_len);

            if (next == PROTO_STREAM_FRAME) {
                handle_frame(c, frame, frame_len);
            } else if (next == PROTO_STREAM_DESYNC) {
                /* Abschnitt 2.8: unbekannter Typ/falsche Laenge -> Verbindung abbauen */
                disconnect(c, "Typ oder Laenge ungueltig");
                more = false;
            } else {
                more = false;
            }
        }
    }
}

/* ===== Heartbeat ============================================================== */

/* Liefert den poll-Timeout bis zum naechsten Heartbeat (hoechstens 50 ms). */
static int32_t heartbeat_timeout_ms(comm_t *c, uint64_t now_us)
{
    int32_t timeout_ms = PI1_POLL_TIMEOUT_MS;

    /* Festlegung 2: ohne gewaehlte Stufe kein Heartbeat. */
    if ((c->phase == PHASE_ONLINE) && c->op.level_chosen && proto_level_valid(c->op.level)) {
        if (!c->hb_running) {
            c->hb_running = true;
            c->next_hb_us = now_us; /* erster Heartbeat sofort, Sequenznummer 1 */
        }
        if (c->next_hb_us <= now_us) {
            timeout_ms = 0;
        } else if ((c->next_hb_us - now_us) < ((uint64_t)PI1_POLL_TIMEOUT_MS * US_PER_MS)) {
            timeout_ms = (int32_t)((c->next_hb_us - now_us) / US_PER_MS);
        } else {
            /* Standard-Timeout */
        }
    } else {
        c->hb_running = false;
    }
    return timeout_ms;
}

static void heartbeat_if_due(comm_t *c)
{
    const uint64_t period_us = (uint64_t)CFG_HEARTBEAT_PERIOD_MS * US_PER_MS;
    uint64_t now_us = time_now_us();

    if ((c->phase == PHASE_ONLINE) && c->hb_running && ((now_us + US_PER_MS) > c->next_hb_us)) {
        uint8_t level = c->op.level;

        /* Rest < 1 ms: genau bis zum absoluten Zeitpunkt schlafen. */
        if (now_us < c->next_hb_us) {
            time_sleep_until_us(c->next_hb_us);
        }
        if (send_frame(c, MSG_HEARTBEAT, &level, (uint16_t)PROTO_HEARTBEAT_LEN)) {
            c->next_hb_us += period_us;
            now_us = time_now_us();
            if (c->next_hb_us <= now_us) {
                /* Mehr als eine Periode verspaetet: nicht nachholen. */
                c->next_hb_us = now_us + period_us;
            }
        }
    }
}

/* ===== Phasen ================================================================== */

static void on_connected(comm_t *c)
{
    c->phase = PHASE_WAIT_STATUS;
    c->phase_since_us = time_now_us();
    c->connect_fail_logged = false;
    session_reset(&c->session);
    proto_stream_reset(&c->stream);
    comm_log(c, "VERBUNDEN warte auf STATUS", false);
}

static void connect_failed(comm_t *c, const char_t *reason)
{
    transport_close(&c->tp);
    c->phase = PHASE_DISCONNECTED;
    if (!c->connect_fail_logged) {
        char_t text[PI1_LOG_EVENT_TEXT_LEN];
        /* Nur der erste Fehlschlag einer Serie wird protokolliert. */
        (void)snprintf(text, sizeof(text), "VERBINDUNGSAUFBAU fehlgeschlagen (%s)", reason);
        comm_log(c, text, false);
        c->connect_fail_logged = true;
    }
}

static void step_disconnected(comm_t *c)
{
    uint64_t now_us = time_now_us();

    if (now_us >= c->next_attempt_us) {
        /* Abschnitt 2.8: jede Sekunde ein neuer Versuch */
        int32_t rc;

        c->next_attempt_us = now_us + ((uint64_t)PI1_RECONNECT_PERIOD_MS * US_PER_MS);
        rc = transport_connect_start(&c->tp, &c->cfg->transport);
        if (rc == TRANSPORT_OK) {
            on_connected(c);
        } else if (rc == TRANSPORT_PENDING) {
            c->phase = PHASE_CONNECTING;
            c->phase_since_us = now_us;
        } else {
            connect_failed(c, "Start");
        }
    } else {
        uint64_t wake_us = now_us + ((uint64_t)PI1_POLL_TIMEOUT_MS * US_PER_MS);
        time_sleep_until_us((c->next_attempt_us < wake_us) ? c->next_attempt_us : wake_us);
    }
}

static void step_connecting(comm_t *c)
{
    int32_t rc = transport_connect_poll(&c->tp, PI1_POLL_TIMEOUT_MS);

    if (rc == TRANSPORT_OK) {
        on_connected(c);
    } else if (rc == TRANSPORT_PENDING) {
        if (time_age_us(time_now_us(), c->phase_since_us) >
            ((uint64_t)PI1_CONNECT_TIMEOUT_MS * US_PER_MS)) {
            connect_failed(c, "Zeitueberschreitung");
        }
    } else {
        connect_failed(c, "abgelehnt");
    }
}

static void step_connected(comm_t *c)
{
    uint64_t now_us = time_now_us();
    int32_t timeout_ms = heartbeat_timeout_ms(c, now_us);

    receive(c, timeout_ms);
    now_us = time_now_us();

    if (c->phase == PHASE_WAIT_STATUS) {
        /* Plan F: erste STATUS-Nachricht bleibt aus -> neu verbinden */
        if (time_age_us(now_us, c->phase_since_us) >
            ((uint64_t)PI1_FIRST_STATUS_TIMEOUT_MS * US_PER_MS)) {
            disconnect(c, "keine erste STATUS-Nachricht");
        }
    } else if (c->phase == PHASE_ONLINE) {
        /* Abschnitt 2.6: 300 ms ohne gueltigen STATUS -> "Verbindung getrennt".
         * Die Verbindung selbst bleibt bestehen (Plan H). */
        if (!c->link_lost_logged &&
            (time_age_us(now_us, c->link.last_status_us) >
             ((uint64_t)PI1_LINK_TIMEOUT_MS * US_PER_MS))) {
            comm_log(c, "STATUS_TIMEOUT Verbindung getrennt", true);
            c->link_lost_logged = true;
        }
        heartbeat_if_due(c);
    } else if (c->phase == PHASE_STOPPING) {
        /* Festlegung 3: hoechstens 2 s auf ACK warten */
        if (time_age_us(now_us, c->phase_since_us) > ((uint64_t)PI1_ACK_TIMEOUT_MS * US_PER_MS)) {
            c->link.stop_result = STOP_RESULT_NO_ACK;
            comm_log(c, "KEIN_ACK innerhalb 2 s", true);
            transport_close(&c->tp);
            c->link.online = false;
            c->phase = PHASE_DONE;
        }
    } else {
        /* Verbindung wurde in receive() abgebaut */
    }
}

/* Programmende angefordert (Abschnitt 2.11, Plan E). */
static void begin_stop(comm_t *c)
{
    if (c->phase == PHASE_ONLINE) {
        if (send_frame(c, MSG_STOP, NULL, 0U)) {
            c->phase = PHASE_STOPPING;
            c->phase_since_us = time_now_us();
            c->hb_running = false; /* nach STOP keine Heartbeats mehr */
            comm_log(c, "STOP gesendet", false);
        } else {
            /* send_frame hat die Verbindung bereits abgebaut */
            c->link.stop_result = STOP_RESULT_NOT_POSSIBLE;
            comm_log(c, "STOP nicht moeglich (Sendefehler)", true);
            c->phase = PHASE_DONE;
        }
    } else {
        transport_close(&c->tp);
        c->link.online = false;
        c->link.stop_result = STOP_RESULT_NOT_POSSIBLE;
        comm_log(c, "STOP nicht moeglich (keine Verbindung)", true);
        c->phase = PHASE_DONE;
    }
}

/* ===== Thread ===================================================================== */

void *comm_thread(void *arg)
{
    static comm_t c; /* statisch: Puffer nicht auf dem Thread-Stack */

    (void)memset(&c, 0, sizeof(c));
    c.cfg = (const comm_cfg_t *)arg;
    transport_init(&c.tp);
    session_reset(&c.session);
    proto_stream_reset(&c.stream);
    c.phase = (c.cfg != NULL) ? PHASE_DISCONNECTED : PHASE_DONE;
    c.next_attempt_us = time_now_us();
    shared_link_write(&c.link);

    while (c.phase != PHASE_DONE) {
        app_ctrl_t ctrl;

        (void)memset(&ctrl, 0, sizeof(ctrl));
        shared_operator_read(&c.op);
        shared_ctrl_read(&ctrl);

        if (ctrl.shutdown && (c.phase != PHASE_STOPPING)) {
            begin_stop(&c);
        }

        switch (c.phase) {
        case PHASE_DISCONNECTED:
            step_disconnected(&c);
            break;
        case PHASE_CONNECTING:
            step_connecting(&c);
            break;
        case PHASE_WAIT_STATUS:
        case PHASE_ONLINE:
        case PHASE_STOPPING:
            step_connected(&c);
            break;
        default:
            /* PHASE_DONE: Schleife endet */
            break;
        }
        shared_link_write(&c.link);
    }

    transport_close(&c.tp);
    c.link.online = false;
    c.link.comm_finished = true;
    shared_link_write(&c.link);
    return NULL;
}

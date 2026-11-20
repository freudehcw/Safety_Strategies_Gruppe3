/*
 * bt_server.c - Kommunikations-Thread von Pi 2: Verbindung annehmen,
 * Nachrichten pruefen, comm_data schreiben, STATUS und ACK senden.
 *
 * Uebertragung, Session-Verwaltung und Zerlegung des Datenstroms kommen aus
 * src/common/ (transport, session, proto_stream), damit Pi 1 und Pi 2 die
 * Protokollregeln mit demselben Code pruefen. Hier steht nur, was Pi 2
 * daraus macht.
 *
 * Kryptografie: Ver- und Entschluesselung mit AES-256-GCM ueber OpenSSL EVP
 * (https://www.openssl.org) in src/common/crypto_utils.c, aufgerufen ueber
 * proto_encode/proto_decode. Nur dieser Thread verwendet die Kryptografie.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2
 * und Claude Opus 5.5), geprueft von: <Name>
 */
#include "bt_server.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "proto_stream.h"
#include "protocol.h"
#include "session.h"
#include "shared.h"
#include "time_utils.h"
#include "transport.h"
#include "types.h"

#define TCP_BIND_HOST  "127.0.0.1"
#define SEND_TIMEOUT_MS 20 /* ein Frame ist klein; laenger darf Senden nicht dauern */

typedef struct {
    transport_t link;       /* Verbindung zu Pi 1 */
    session_t session;      /* Session-ID und Sequenznummern */
    proto_stream_t stream;  /* noch nicht verarbeitete empfangene Bytes */
    uint32_t discarded;     /* verworfene Nachrichten dieser Verbindung */
    uint64_t next_status_us;
} connection_t;

typedef struct {
    bool seen;              /* Zustand SHUTDOWN wurde bemerkt */
    bool done;              /* ACK gesendet oder aufgegeben */
    uint32_t generation;    /* erste Generation von safety_state im Zustand SHUTDOWN */
    uint64_t since_us;      /* Zeitpunkt, an dem SHUTDOWN bemerkt wurde */
} shutdown_t;

static transport_t g_listener;
static atomic_bool g_shutdown_acked;

/* --- Verbindungsaufbau -------------------------------------------------- */

int32_t bt_server_init(const bt_server_cfg_t *cfg)
{
    int32_t result = -1;

    atomic_store(&g_shutdown_acked, false);
    transport_init(&g_listener);
    if (cfg != NULL) {
        transport_cfg_t tcfg;

        (void)memset(&tcfg, 0, sizeof(tcfg));
        tcfg.rfcomm_channel = (uint8_t)CFG_RFCOMM_CHANNEL;
        if (cfg->use_tcp) {
            /* Entwicklung: nur lokal erreichbar */
            tcfg.kind = TRANSPORT_KIND_TCP;
            tcfg.tcp_port = cfg->tcp_port;
            (void)strncpy(tcfg.host, TCP_BIND_HOST, sizeof(tcfg.host) - 1U);
        } else {
            tcfg.kind = TRANSPORT_KIND_RFCOMM;
        }
        if (transport_listen(&g_listener, &tcfg) == TRANSPORT_OK) {
            result = 0;
        }
    }
    return result;
}

void bt_server_close(void)
{
    transport_close(&g_listener);
}

bool bt_server_shutdown_acked(void)
{
    return atomic_load(&g_shutdown_acked);
}

static void conn_close(connection_t *conn, const char_t *reason)
{
    if (transport_is_open(&conn->link)) {
        transport_close(&conn->link);
        session_reset(&conn->session);
        (void)fprintf(stderr, "[comm] Verbindung getrennt (%s), %" PRIu32 " Nachricht(en) verworfen\n",
                      reason, conn->discarded);
    }
}

/* --- Senden -------------------------------------------------------------- */

static bool send_frame(connection_t *conn, uint8_t type, const uint8_t *payload, uint16_t len)
{
    proto_frame_t frame;
    uint8_t buf[PROTO_MAX_FRAME];
    size_t total = 0U;
    int32_t seq_rc;
    bool ok = false;

    (void)memset(&frame, 0, sizeof(frame));
    frame.type = type;
    frame.len = len;
    frame.session_id = conn->session.id;
    if ((payload != NULL) && (len > 0U) && (len <= PROTO_MAX_PAYLOAD)) {
        (void)memcpy(frame.payload, payload, len);
    }

    /* Designdokument 5.2: Die Sequenznummer laeuft nie ueber. Ist UINT32_MAX
     * vergeben, liefert session_next_tx_seq einen Fehler und die Verbindung
     * wird abgebaut; nach dem Neuaufbau beginnt sie mit neuer Session-ID bei 1. */
    seq_rc = session_next_tx_seq(&conn->session, &frame.seq);
    if ((seq_rc == SESSION_OK) && ((payload != NULL) || (len == 0U)) &&
        (proto_encode(&frame, buf, sizeof(buf), &total) == 0) &&
        (transport_send(&conn->link, buf, total, SEND_TIMEOUT_MS) == TRANSPORT_OK)) {
        ok = true;
    }
    if (!ok) {
        /* Ein teilweise gesendeter Frame waere nicht mehr synchron: abbauen. */
        conn_close(conn, (seq_rc == SESSION_ERR_EXHAUSTED) ? "Sequenznummer ausgeschoepft"
                                                           : "Sendefehler");
    }
    return ok;
}

static bool send_status(connection_t *conn)
{
    safety_state_t state;
    sensor_data_t s0;
    sensor_data_t s1;
    proto_status_t status;
    uint8_t payload[PROTO_STATUS_LEN];

    shared_state_read(&state);
    shared_sensor_read(0U, &s0);
    shared_sensor_read(1U, &s1);

    status.state = state.state;
    status.error = state.error;
    status.level = state.motor_level;
    status.d1_mm = s0.valid ? s0.distance_mm : (uint16_t)DIST_INVALID_MM;
    status.d2_mm = s1.valid ? s1.distance_mm : (uint16_t)DIST_INVALID_MM;
    proto_status_pack(&status, payload);

    return send_frame(conn, MSG_STATUS, payload, (uint16_t)PROTO_STATUS_LEN);
}

/* --- Empfangen ----------------------------------------------------------- */

/* Prueft einen vollstaendigen Frame und uebernimmt gueltige Befehle. */
static void handle_frame(connection_t *conn, comm_data_t *comm, const uint8_t *buf, size_t len)
{
    proto_frame_t frame;
    bool accepted = false;
    bool invalid_command = false;

    /* Reihenfolge (SEC-02): zuerst das GCM-Tag (proto_decode), dann Session-ID
     * und Sequenznummer (session_check_rx), zuletzt der Wertebereich. */
    if ((proto_decode(buf, len, &frame) == 0) &&
        (session_check_rx(&conn->session, frame.session_id, frame.seq) == SESSION_OK)) {
        switch (frame.type) {
        case MSG_HEARTBEAT:
            if (proto_level_valid(frame.payload[0])) {
                comm->level = frame.payload[0];
                accepted = true;
            } else {
                invalid_command = true; /* unbekannte Stufe */
            }
            break;
        case MSG_STOP:
            comm->stop = true;
            accepted = true;
            break;
        default:
            /* STATUS/ACK sind in dieser Richtung nicht vorgesehen */
            invalid_command = true;
            break;
        }
    }

    if (accepted) {
        comm->have_msg = true;
        comm->last_valid_us = time_now_us();
        shared_comm_write(comm);
    } else {
        conn->discarded++;
        if (invalid_command) {
            /* Abschnitt 2.6 "ungueltiger Befehl": Die Nachricht ist echt (Tag,
             * Session-ID und Sequenznummer stimmen), ihr Inhalt aber ungueltig.
             * Die letzte gueltige Nachricht wird sofort entwertet, damit die
             * Safety-Logik im naechsten Zyklus FEHLER / KOMMUNIKATION setzt.
             * Ein gesetztes Stop-Flag bleibt erhalten. */
            comm->have_msg = false;
            comm->last_valid_us = 0U;
            shared_comm_write(comm);
        }
    }
}

/* Wartet hoechstens timeout_ms auf Daten und verarbeitet alle vollstaendigen Frames. */
static void receive_data(connection_t *conn, comm_data_t *comm, int32_t timeout_ms)
{
    uint8_t chunk[PROTO_STREAM_BUF_LEN];
    size_t got = 0U;
    size_t room = proto_stream_free(&conn->stream);
    int32_t rc = TRANSPORT_ERR;

    if (room > 0U) {
        rc = transport_recv(&conn->link, chunk, (room < sizeof(chunk)) ? room : sizeof(chunk),
                            &got, timeout_ms);
    }

    if (room == 0U) {
        /* Puffer voll ohne vollstaendigen Frame: kann nur bei kaputtem Datenstrom passieren */
        conn_close(conn, "Datenstrom nicht synchron");
    } else if (rc == TRANSPORT_CLOSED) {
        conn_close(conn, "Gegenstelle hat geschlossen");
    } else if ((rc != TRANSPORT_OK) || (proto_stream_push(&conn->stream, chunk, got) != 0)) {
        conn_close(conn, "Empfangsfehler");
    } else {
        uint8_t frame[PROTO_MAX_FRAME];
        size_t frame_len = 0U;
        int32_t next = proto_stream_next(&conn->stream, frame, sizeof(frame), &frame_len);

        while (next == PROTO_STREAM_FRAME) {
            handle_frame(conn, comm, frame, frame_len);
            next = proto_stream_next(&conn->stream, frame, sizeof(frame), &frame_len);
        }
        if (next == PROTO_STREAM_DESYNC) {
            /* Abschnitt 2.8: unbekannter Typ oder falsche Laenge */
            conn_close(conn, "Datenstrom nicht synchron");
        }
    }
}

static void accept_connection(connection_t *conn, int32_t timeout_ms)
{
    if (transport_accept(&g_listener, &conn->link, timeout_ms) == TRANSPORT_OK) {
        proto_stream_reset(&conn->stream);
        conn->discarded = 0U;
        if (session_start_server(&conn->session) != SESSION_OK) {
            conn_close(conn, "keine Session-ID");
        } else {
            (void)fprintf(stderr, "[comm] Verbindung aufgebaut, Session-ID %08" PRIx32 "\n",
                          conn->session.id);
            /* Pi 2 sendet zuerst einen STATUS mit der neuen Session-ID (T2). */
            if (send_status(conn)) {
                conn->next_status_us = time_now_us() +
                                       ((uint64_t)CFG_STATUS_PERIOD_MS * TIME_US_PER_MS);
            }
        }
    }
}

/* --- Ablauf -------------------------------------------------------------- */

/* Wartezeit fuer den naechsten Empfang: hoechstens 50 ms, aber nicht ueber
 * den naechsten STATUS-Zeitpunkt hinaus. */
static int32_t receive_timeout_ms(const connection_t *conn)
{
    const uint64_t max_us = (uint64_t)CFG_COMM_POLL_TIMEOUT_MS * TIME_US_PER_MS;
    uint64_t now_us = time_now_us();
    int32_t timeout_ms = CFG_COMM_POLL_TIMEOUT_MS;

    if (conn->next_status_us <= now_us) {
        timeout_ms = 0;
    } else if ((conn->next_status_us - now_us) < max_us) {
        timeout_ms = (int32_t)((conn->next_status_us - now_us) / TIME_US_PER_MS) + 1;
    } else {
        /* Standard-Timeout */
    }
    return timeout_ms;
}

/* SAF-09: Im Zustand SHUTDOWN wird das ACK erst gesendet, wenn die
 * Motorsteuerung fuer eine Generation ab SHUTDOWN "Motor AUS" gemeldet hat.
 * Bleibt das laenger als SHUTDOWN_MOTOR_WAIT_MS aus, wird kein ACK gesendet.
 * Das ACK beantwortet nur ein STOP von Pi 1 (stop_received). Endet Pi 2 aus
 * eigenem Grund (Signal), erhaelt Pi 1 nur den letzten STATUS mit SHUTDOWN. */
static void handle_shutdown(connection_t *conn, shutdown_t *sd, bool stop_received)
{
    safety_state_t state;

    shared_state_read(&state);
    if ((state.state == STATE_SHUTDOWN) && !sd->done) {
        motor_status_t motor;
        bool motor_is_off;

        if (!sd->seen) {
            sd->seen = true;
            sd->generation = state.generation;
            sd->since_us = time_now_us();
        }
        shared_motor_read(&motor);
        /* Differenz ueberlaufsicher: Generation liegt bei oder nach SHUTDOWN */
        motor_is_off = (motor.applied_level == LEVEL_OFF) &&
                       ((uint32_t)(motor.generation - sd->generation) < 0x80000000UL);

        if (motor_is_off) {
            /* Letzter STATUS (SHUTDOWN), danach ACK. Scheitert das Senden, ist
             * die Verbindung bereits abgebaut und es gibt nichts zu bestaetigen. */
            if (transport_is_open(&conn->link) && send_status(conn) && stop_received &&
                !send_frame(conn, MSG_ACK, NULL, 0U)) {
                (void)fprintf(stderr, "[comm] ACK konnte nicht gesendet werden\n");
            }
            sd->done = true;
        } else if (time_age_us(time_now_us(), sd->since_us) >
                   ((uint64_t)SHUTDOWN_MOTOR_WAIT_MS * TIME_US_PER_MS)) {
            (void)fprintf(stderr, "[comm] Motor AUS nicht bestaetigt: kein ACK gesendet\n");
            sd->done = true;
        } else {
            /* weiter auf die Rueckmeldung der Motorsteuerung warten */
        }
        if (sd->done) {
            atomic_store(&g_shutdown_acked, true);
        }
    }
}

/* STATUS alle 100 ms (SAF-05); bei Verspaetung wird neu aufgesetzt. */
static void send_periodic_status(connection_t *conn)
{
    const uint64_t period_us = (uint64_t)CFG_STATUS_PERIOD_MS * TIME_US_PER_MS;
    uint64_t now_us = time_now_us();

    if (transport_is_open(&conn->link) && (now_us >= conn->next_status_us) && send_status(conn)) {
        conn->next_status_us += period_us;
        if (conn->next_status_us <= now_us) {
            conn->next_status_us = now_us + period_us;
        }
    }
}

void *bt_server_thread(void *arg)
{
    connection_t conn;
    comm_data_t comm; /* einziger Schreiber von comm_data */
    shutdown_t sd = { false, false, 0U, 0U };

    (void)arg;
    (void)memset(&conn, 0, sizeof(conn));
    (void)memset(&comm, 0, sizeof(comm));
    transport_init(&conn.link);
    session_reset(&conn.session);
    proto_stream_reset(&conn.stream);

    /* Kein Aufruf blockiert laenger als 50 ms; das Ausbleiben von Nachrichten
     * bewertet die Safety-Logik ueber den Zeitstempel in comm_data. */
    while (shared_running()) {
        if (transport_is_open(&conn.link)) {
            receive_data(&conn, &comm, receive_timeout_ms(&conn));
        } else {
            accept_connection(&conn, CFG_COMM_POLL_TIMEOUT_MS);
        }
        handle_shutdown(&conn, &sd, comm.stop);
        send_periodic_status(&conn);
    }

    conn_close(&conn, "Programmende");
    return NULL;
}

/*
 * mock_pi2.c - Simulierter Raspberry Pi 2 als TESTWERKZEUG fuer Pi 1.
 * Enthaelt KEINE Safety-Logik. Nutzt den gemeinsamen Code aus src/common/
 * (Protokoll, AES-256-GCM, Session, Transport) ueber TCP.
 *
 * Verhalten:
 *   - Server auf TCP (Standard 127.0.0.1:5555), genau eine Verbindung.
 *   - Pro Verbindung neue Session-ID; zuerst STATUS, dann STATUS mit 10 Hz.
 *   - Inhalt des STATUS: BETRIEB mit der zuletzt empfangenen Stufe, wenn in
 *     den letzten 300 ms ein gueltiger Heartbeat kam, sonst FEHLER /
 *     KOMMUNIKATION (nur zur Anzeige, keine Safety-Entscheidung).
 *     Mit "set" lassen sich Zustand, Fehler und Abstaende fest vorgeben.
 *   - STOP wird mit STATUS (SHUTDOWN) und ACK beantwortet, danach wird die
 *     Verbindung geschlossen und auf die naechste gewartet.
 *   - Heartbeat-Abstaende werden gemessen: jede Sekunde eine Zusammenfassung,
 *     mit --verbose jeder Heartbeat, am Ende eine Gesamtstatistik.
 *
 * Aufruf:
 *   mock_pi2 [--port 5555] [--key PFAD] [--verbose] [--no-ack]
 *
 * Befehle auf der Standardeingabe (eine Zeile je Befehl):
 *   pause | resume        STATUS aussetzen / wieder senden ("Verbindung getrennt")
 *   drop                  Verbindung trennen und neu annehmen (Wiederaufbau, TC-07)
 *   badtag                naechster STATUS mit verfaelschtem Tag (TC-08)
 *   badpayload            naechster STATUS mit verfaelschten Nutzdaten (TC-08)
 *   replay                letzten STATUS unveraendert erneut senden (TC-09)
 *   oldsession            STATUS mit der Session-ID der vorherigen Verbindung (TC-09)
 *   badvalue              STATUS mit ungueltigem Zustand 0x07 (Wertebereich)
 *   badtype               Frame mit unbekanntem Typ 0x09 (Pi 1 muss trennen)
 *   badlen                STATUS-Kopf mit falscher Laenge 6 (Pi 1 muss trennen)
 *   noack | ack           STOP nicht / wieder mit ACK beantworten
 *   set S E D1 D2         Zustand S, Fehler E, Abstaende in mm fest vorgeben
 *   auto                  wieder automatischer Inhalt
 *   stats                 Heartbeat-Statistik ausgeben
 *   quit                  beenden
 *
 * Kryptografie: OpenSSL EVP (https://www.openssl.org), AES-256-GCM nach
 * NIST SP 800-38D, Nonce aus getrandom(2); siehe src/common/crypto_utils.c.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "crypto_utils.h"
#include "proto_stream.h"
#include "protocol.h"
#include "session.h"
#include "time_utils.h"
#include "transport.h"
#include "types.h"

#define MOCK_DEFAULT_PORT  5555U
#define MOCK_LINE_LEN      128U
#define MOCK_HB_TIMEOUT_MS 300U
#define MOCK_POLL_MS       5

/* Statistik der Heartbeat-Abstaende */
typedef struct {
    uint64_t last_us;
    uint32_t count;      /* Anzahl gemessener Abstaende */
    uint64_t sum_us;
    uint64_t min_us;
    uint64_t max_us;
    uint32_t outside;    /* ausserhalb 100 ms +- 10 ms */
} hb_stats_t;

typedef struct {
    transport_t listener;
    transport_t conn;
    proto_stream_t stream;
    session_t session;
    uint32_t prev_session_id;
    bool have_prev_session;
    uint8_t last_frame[PROTO_MAX_FRAME];
    size_t last_frame_len;
    uint64_t next_status_us;
    uint64_t last_hb_us;
    uint8_t last_level;
    bool paused;
    bool no_ack;
    bool forced;
    proto_status_t forced_status;
    bool verbose;
    hb_stats_t total;   /* gesamter Lauf */
    hb_stats_t window;  /* aktuelle Sekunde */
    uint64_t next_report_us;
    bool quit;
} mock_t;

static void stats_reset(hb_stats_t *s)
{
    (void)memset(s, 0, sizeof(*s));
    s->min_us = UINT64_MAX;
}

static void stats_add(hb_stats_t *s, uint64_t dt_us)
{
    s->count++;
    s->sum_us += dt_us;
    if (dt_us < s->min_us) {
        s->min_us = dt_us;
    }
    if (dt_us > s->max_us) {
        s->max_us = dt_us;
    }
    if ((dt_us < 90000U) || (dt_us > 110000U)) {
        s->outside++;
    }
}

static void stats_print(const char_t *title, const hb_stats_t *s)
{
    if (s->count == 0U) {
        (void)printf("%s: keine Heartbeat-Abstaende\n", title);
    } else {
        (void)printf("%s: n=%" PRIu32 " mittel=%.2f ms min=%.2f ms max=%.2f ms ausserhalb 100+-10 ms: %" PRIu32 "\n",
                     title, s->count, (double)s->sum_us / (double)s->count / 1000.0,
                     (double)s->min_us / 1000.0, (double)s->max_us / 1000.0, s->outside);
    }
    (void)fflush(stdout);
}

/* --- Senden ---------------------------------------------------------------------- */

static void close_conn(mock_t *m, const char_t *reason)
{
    if (transport_is_open(&m->conn)) {
        (void)printf("[mock] Verbindung geschlossen: %s\n", reason);
        m->prev_session_id = m->session.id;
        m->have_prev_session = true;
    }
    transport_close(&m->conn);
    session_reset(&m->session);
    proto_stream_reset(&m->stream);
    m->window.last_us = 0U;
    m->total.last_us = 0U;
}

static void send_raw(mock_t *m, const uint8_t *buf, size_t len)
{
    if (transport_send(&m->conn, buf, len, 50) != TRANSPORT_OK) {
        close_conn(m, "Sendefehler");
    }
}

/* Kodiert einen Frame mit der angegebenen Session-ID und naechster Sequenznummer. */
static size_t build_frame(mock_t *m, uint8_t type, uint32_t sid, const uint8_t *payload,
                          uint16_t len, uint8_t *buf)
{
    proto_frame_t f;
    uint32_t seq = 0U;
    size_t total = 0U;

    (void)memset(&f, 0, sizeof(f));
    if (session_next_tx_seq(&m->session, &seq) == SESSION_OK) {
        f.type = type;
        f.len = len;
        f.session_id = sid;
        f.seq = seq;
        if ((payload != NULL) && (len > 0U)) {
            (void)memcpy(f.payload, payload, len);
        }
        if (proto_encode(&f, buf, PROTO_MAX_FRAME, &total) != 0) {
            total = 0U;
        }
    }
    return total;
}

static void current_status(const mock_t *m, proto_status_t *st)
{
    if (m->forced) {
        *st = m->forced_status;
    } else if ((m->last_hb_us != 0U) &&
               (time_age_us(time_now_us(), m->last_hb_us) <= ((uint64_t)MOCK_HB_TIMEOUT_MS * TIME_US_PER_MS))) {
        st->state = STATE_BETRIEB;
        st->error = ERR_NONE;
        st->level = m->last_level;
        st->d1_mm = 812U;
        st->d2_mm = 805U;
    } else {
        st->state = STATE_FEHLER;
        st->error = ERR_KOMMUNIKATION;
        st->level = LEVEL_OFF;
        st->d1_mm = 812U;
        st->d2_mm = 805U;
    }
}

static void send_status_with(mock_t *m, const proto_status_t *st, uint32_t sid, bool keep)
{
    uint8_t payload[PROTO_STATUS_LEN];
    uint8_t buf[PROTO_MAX_FRAME];
    size_t len;

    proto_status_pack(st, payload);
    len = build_frame(m, MSG_STATUS, sid, payload, (uint16_t)PROTO_STATUS_LEN, buf);
    if (len > 0U) {
        if (keep) {
            (void)memcpy(m->last_frame, buf, len);
            m->last_frame_len = len;
        }
        send_raw(m, buf, len);
    }
}

static void send_status(mock_t *m)
{
    proto_status_t st;

    current_status(m, &st);
    send_status_with(m, &st, m->session.id, true);
}

/* --- Fehlereinspeisung ------------------------------------------------------------- */

static void inject_tampered(mock_t *m, size_t offset_from_end, const char_t *what)
{
    proto_status_t st;
    uint8_t payload[PROTO_STATUS_LEN];
    uint8_t buf[PROTO_MAX_FRAME];
    size_t len;

    current_status(m, &st);
    proto_status_pack(&st, payload);
    len = build_frame(m, MSG_STATUS, m->session.id, payload, (uint16_t)PROTO_STATUS_LEN, buf);
    if (len > offset_from_end) {
        buf[len - 1U - offset_from_end] ^= 0x01U;
        (void)printf("[mock] sende STATUS mit verfaelschtem %s\n", what);
        send_raw(m, buf, len);
    }
}

static void inject(mock_t *m, const char_t *cmd)
{
    if (strcmp(cmd, "badtag") == 0) {
        inject_tampered(m, 0U, "Tag");                       /* letztes Byte = Tag */
    } else if (strcmp(cmd, "badpayload") == 0) {
        inject_tampered(m, (size_t)PROTO_TAG_LEN, "Nutzdaten"); /* letztes Nutzdatenbyte */
    } else if (strcmp(cmd, "replay") == 0) {
        if (m->last_frame_len > 0U) {
            (void)printf("[mock] sende letzten STATUS erneut (Replay)\n");
            send_raw(m, m->last_frame, m->last_frame_len);
        }
    } else if (strcmp(cmd, "oldsession") == 0) {
        if (m->have_prev_session) {
            proto_status_t st;
            current_status(m, &st);
            (void)printf("[mock] sende STATUS mit alter Session-ID %08" PRIx32 "\n", m->prev_session_id);
            send_status_with(m, &st, m->prev_session_id, false);
        } else {
            (void)printf("[mock] noch keine vorherige Session (erst 'drop')\n");
        }
    } else if (strcmp(cmd, "badvalue") == 0) {
        proto_status_t st;
        current_status(m, &st);
        st.state = 0x07U;
        (void)printf("[mock] sende STATUS mit ungueltigem Zustand 0x07\n");
        send_status_with(m, &st, m->session.id, false);
    } else if (strcmp(cmd, "badtype") == 0) {
        uint8_t raw[3] = { 0x09U, 0x00U, 0x00U };
        (void)printf("[mock] sende unbekannten Typ 0x09\n");
        send_raw(m, raw, sizeof(raw));
    } else if (strcmp(cmd, "badlen") == 0) {
        uint8_t raw[3] = { MSG_STATUS, 0x00U, 0x06U };
        (void)printf("[mock] sende STATUS-Kopf mit Laenge 6\n");
        send_raw(m, raw, sizeof(raw));
    } else {
        /* nicht erreichbar */
    }
}

/* --- Befehle -------------------------------------------------------------------------- */

static void handle_command(mock_t *m, char_t *line)
{
    char_t *nl = strchr(line, '\n');
    unsigned s = 0U;
    unsigned e = 0U;
    unsigned d1 = 0U;
    unsigned d2 = 0U;

    if (nl != NULL) {
        *nl = '\0';
    }
    if (strcmp(line, "pause") == 0) {
        m->paused = true;
        (void)printf("[mock] STATUS ausgesetzt\n");
    } else if (strcmp(line, "resume") == 0) {
        m->paused = false;
        m->next_status_us = time_now_us();
        (void)printf("[mock] STATUS wieder aktiv\n");
    } else if (strcmp(line, "drop") == 0) {
        close_conn(m, "Befehl drop");
    } else if ((strcmp(line, "badtag") == 0) || (strcmp(line, "badpayload") == 0) ||
               (strcmp(line, "replay") == 0) || (strcmp(line, "oldsession") == 0) ||
               (strcmp(line, "badvalue") == 0) || (strcmp(line, "badtype") == 0) ||
               (strcmp(line, "badlen") == 0)) {
        if (transport_is_open(&m->conn) && m->session.active) {
            inject(m, line);
        } else {
            (void)printf("[mock] keine Verbindung\n");
        }
    } else if (strcmp(line, "noack") == 0) {
        m->no_ack = true;
        (void)printf("[mock] STOP wird nicht bestaetigt\n");
    } else if (strcmp(line, "ack") == 0) {
        m->no_ack = false;
        (void)printf("[mock] STOP wird mit ACK bestaetigt\n");
    } else if (sscanf(line, "set %u %u %u %u", &s, &e, &d1, &d2) == 4) {
        m->forced = true;
        m->forced_status.state = (uint8_t)s;
        m->forced_status.error = (uint8_t)e;
        m->forced_status.level = m->last_level;
        m->forced_status.d1_mm = (uint16_t)d1;
        m->forced_status.d2_mm = (uint16_t)d2;
        (void)printf("[mock] STATUS fest: Zustand %u Fehler %u d1 %u d2 %u\n", s, e, d1, d2);
    } else if (strcmp(line, "auto") == 0) {
        m->forced = false;
        (void)printf("[mock] STATUS automatisch\n");
    } else if (strcmp(line, "stats") == 0) {
        stats_print("[mock] gesamt", &m->total);
    } else if (strcmp(line, "quit") == 0) {
        m->quit = true;
    } else if (line[0] != '\0') {
        (void)printf("[mock] unbekannter Befehl: %s\n", line);
    } else {
        /* leere Zeile */
    }
    (void)fflush(stdout);
}

static void poll_stdin(mock_t *m)
{
    struct pollfd pfd;

    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if ((poll(&pfd, 1U, 0) > 0) && ((pfd.revents & POLLIN) != 0)) {
        char_t line[MOCK_LINE_LEN];
        if (fgets(line, (int)sizeof(line), stdin) != NULL) {
            handle_command(m, line);
        }
    }
}

/* --- Empfang ------------------------------------------------------------------------- */

static void handle_frame(mock_t *m, const uint8_t *buf, size_t len)
{
    proto_frame_t f;

    if (proto_decode(buf, len, &f) != 0) {
        (void)printf("[mock] verworfen: Tag ungueltig\n");
    } else if (session_check_rx(&m->session, f.session_id, f.seq) != SESSION_OK) {
        (void)printf("[mock] verworfen: Session/Sequenz (sid %08" PRIx32 " seq %" PRIu32 ")\n",
                     f.session_id, f.seq);
    } else if ((f.type == MSG_HEARTBEAT) && proto_level_valid(f.payload[0])) {
        uint64_t now_us = time_now_us();

        if (m->total.last_us != 0U) {
            uint64_t dt = now_us - m->total.last_us;
            stats_add(&m->total, dt);
            stats_add(&m->window, dt);
            if (m->verbose) {
                (void)printf("[mock] HB seq=%" PRIu32 " Stufe=%u dt=%.2f ms\n", f.seq,
                             (unsigned)f.payload[0], (double)dt / 1000.0);
            }
        } else {
            (void)printf("[mock] erster Heartbeat seq=%" PRIu32 " Stufe=%u\n", f.seq, (unsigned)f.payload[0]);
        }
        if (f.payload[0] != m->last_level) {
            (void)printf("[mock] Sollstufe jetzt %u\n", (unsigned)f.payload[0]);
        }
        m->total.last_us = now_us;
        m->last_hb_us = now_us;
        m->last_level = f.payload[0];
    } else if (f.type == MSG_STOP) {
        proto_status_t st;
        (void)printf("[mock] STOP empfangen\n");
        current_status(m, &st);
        st.state = STATE_SHUTDOWN;
        st.error = ERR_NONE;
        st.level = LEVEL_OFF;
        send_status_with(m, &st, m->session.id, true);
        if (!m->no_ack && transport_is_open(&m->conn)) {
            uint8_t out[PROTO_MAX_FRAME];
            size_t n = build_frame(m, MSG_ACK, m->session.id, NULL, 0U, out);
            if (n > 0U) {
                send_raw(m, out, n);
                (void)printf("[mock] ACK gesendet\n");
            }
            close_conn(m, "nach ACK");
        } else {
            (void)printf("[mock] kein ACK (noack)\n");
        }
    } else {
        (void)printf("[mock] verworfen: Typ %u / ungueltiger Inhalt\n", (unsigned)f.type);
    }
    (void)fflush(stdout);
}

static void receive(mock_t *m, int32_t timeout_ms)
{
    uint8_t rx[PROTO_STREAM_BUF_LEN];
    size_t got = 0U;
    size_t space = proto_stream_free(&m->stream);
    int32_t rc = transport_recv(&m->conn, rx, (space < sizeof(rx)) ? space : sizeof(rx), &got, timeout_ms);

    if (rc == TRANSPORT_CLOSED) {
        close_conn(m, "Gegenseite hat geschlossen");
    } else if (rc != TRANSPORT_OK) {
        close_conn(m, "Empfangsfehler");
    } else if ((got > 0U) && (proto_stream_push(&m->stream, rx, got) == 0)) {
        uint8_t frame[PROTO_MAX_FRAME];
        size_t len = 0U;
        int32_t next = proto_stream_next(&m->stream, frame, sizeof(frame), &len);

        while ((next == PROTO_STREAM_FRAME) && transport_is_open(&m->conn)) {
            handle_frame(m, frame, len);
            next = proto_stream_next(&m->stream, frame, sizeof(frame), &len);
        }
        if (next == PROTO_STREAM_DESYNC) {
            close_conn(m, "Typ oder Laenge ungueltig");
        }
    } else {
        /* keine Daten */
    }
}

/* --- Hauptschleife ----------------------------------------------------------------------- */

static void accept_if_waiting(mock_t *m)
{
    if (transport_accept(&m->listener, &m->conn, MOCK_POLL_MS) == TRANSPORT_OK) {
        if (session_start_server(&m->session) != SESSION_OK) {
            close_conn(m, "keine Session-ID");
        } else {
            (void)printf("[mock] Verbindung angenommen, Session-ID %08" PRIx32 "\n", m->session.id);
            proto_stream_reset(&m->stream);
            m->last_hb_us = 0U;
            m->last_level = LEVEL_OFF;
            m->next_status_us = time_now_us();
            send_status(m); /* zuerst STATUS mit neuer Session-ID */
            m->next_status_us += (uint64_t)CFG_STATUS_PERIOD_MS * TIME_US_PER_MS;
        }
        (void)fflush(stdout);
    }
}

static void run(mock_t *m)
{
    const uint64_t period_us = (uint64_t)CFG_STATUS_PERIOD_MS * TIME_US_PER_MS;

    m->next_report_us = time_now_us() + 1000000U;
    while (!m->quit) {
        uint64_t now_us;

        poll_stdin(m);
        if (!transport_is_open(&m->conn)) {
            accept_if_waiting(m);
        } else {
            receive(m, MOCK_POLL_MS);
            now_us = time_now_us();
            if (transport_is_open(&m->conn) && !m->paused && (now_us >= m->next_status_us)) {
                send_status(m);
                m->next_status_us += period_us;
                if (m->next_status_us <= now_us) {
                    m->next_status_us = now_us + period_us;
                }
            }
        }
        now_us = time_now_us();
        if (now_us >= m->next_report_us) {
            if (m->window.count > 0U) {
                stats_print("[mock] letzte Sekunde", &m->window);
            }
            stats_reset(&m->window);
            m->next_report_us += 1000000U;
        }
    }
}

int main(int argc, char **argv)
{
    static mock_t m;
    transport_cfg_t cfg;
    char_t key_path[256];
    uint8_t key[CRYPTO_KEY_LEN];
    int32_t a;
    int32_t rc;

    (void)memset(&m, 0, sizeof(m));
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.kind = TRANSPORT_KIND_TCP;
    (void)snprintf(cfg.host, sizeof(cfg.host), "127.0.0.1");
    cfg.tcp_port = (uint16_t)MOCK_DEFAULT_PORT;
    (void)snprintf(key_path, sizeof(key_path), "%s", CFG_PSK_DEFAULT_PATH);

    for (a = 1; a < argc; a++) {
        if ((strcmp(argv[a], "--port") == 0) && ((a + 1) < argc)) {
            long p = strtol(argv[a + 1], NULL, 10);
            cfg.tcp_port = ((p > 0) && (p <= 65535)) ? (uint16_t)p : 0U;
            a++;
        } else if ((strcmp(argv[a], "--key") == 0) && ((a + 1) < argc)) {
            (void)snprintf(key_path, sizeof(key_path), "%s", argv[a + 1]);
            a++;
        } else if (strcmp(argv[a], "--verbose") == 0) {
            m.verbose = true;
        } else if (strcmp(argv[a], "--no-ack") == 0) {
            m.no_ack = true;
        } else {
            (void)fprintf(stderr, "Aufruf: %s [--port 5555] [--key PFAD] [--verbose] [--no-ack]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    rc = crypto_load_key_file(key_path, key);
    if ((rc != CRYPTO_KEY_OK) || (crypto_init(key) != 0)) {
        (void)fprintf(stderr, "Fehler: %s: %s\n", key_path,
                      (rc != CRYPTO_KEY_OK) ? crypto_key_error_text(rc) : "OpenSSL");
        crypto_wipe(key, sizeof(key));
        return EXIT_FAILURE;
    }
    crypto_wipe(key, sizeof(key));

    transport_init(&m.listener);
    transport_init(&m.conn);
    session_reset(&m.session);
    stats_reset(&m.total);
    stats_reset(&m.window);
    if (transport_listen(&m.listener, &cfg) != TRANSPORT_OK) {
        (void)fprintf(stderr, "Fehler: TCP-Port %u nicht verfuegbar\n", (unsigned)cfg.tcp_port);
        crypto_cleanup();
        return EXIT_FAILURE;
    }
    (void)printf("[mock] simulierter Pi 2 wartet auf 127.0.0.1:%u (Befehle: pause, resume, drop, "
                 "badtag, badpayload, replay, oldsession, badvalue, badtype, badlen, noack, ack, "
                 "set S E D1 D2, auto, stats, quit)\n", (unsigned)cfg.tcp_port);
    (void)fflush(stdout);

    run(&m);

    stats_print("[mock] gesamt", &m.total);
    transport_close(&m.conn);
    transport_close(&m.listener);
    crypto_cleanup();
    return EXIT_SUCCESS;
}

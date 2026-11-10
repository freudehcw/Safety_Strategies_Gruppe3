#include "bt_server.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifdef HAVE_BLUEZ
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#endif

#include "config.h"
#include "crypto_utils.h"
#include "protocol.h"
#include "shared.h"
#include "time_utils.h"
#include "types.h"

#define RX_BUF_LEN 256U

typedef struct {
    int32_t fd;                 /* -1 = keine Verbindung */
    uint32_t session_id;    /* Zufallswert je Verbindung */
    uint32_t tx_seq;        /* zuletzt gesendete Sequenznummer */
    uint32_t rx_last_seq;   /* zuletzt akzeptierte Sequenznummer */
    uint8_t rx[RX_BUF_LEN];
    size_t rx_len;
    uint32_t discarded;     /* verworfene Nachrichten dieser Verbindung */
    uint64_t next_status_us;
} connection_t;

static int32_t g_listen_fd = -1;
static atomic_bool g_shutdown_acked;

/* --- Verbindungsaufbau -------------------------------------------------- */

static int32_t listen_tcp(uint16_t port)
{
    int32_t fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd >= 0) {
        struct sockaddr_in addr;
        int32_t one = 1;

        (void)memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one)) != 0) ||
            (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) ||
            (listen(fd, 1) != 0)) {
            (void)close(fd);
            fd = -1;
        }
    }
    return fd;
}

static int32_t listen_rfcomm(void)
{
    int32_t fd = -1;

#ifdef HAVE_BLUEZ
    fd = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if (fd >= 0) {
        struct sockaddr_rc addr;

        (void)memset(&addr, 0, sizeof(addr)); /* rc_bdaddr = BDADDR_ANY */
        addr.rc_family = AF_BLUETOOTH;
        addr.rc_channel = (uint8_t)CFG_RFCOMM_CHANNEL;

        if ((bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) ||
            (listen(fd, 1) != 0)) {
            (void)close(fd);
            fd = -1;
        }
    }
#endif
    return fd;
}

int32_t bt_server_init(const bt_server_cfg_t *cfg)
{
    int32_t result = -1;

    atomic_store(&g_shutdown_acked, false);
    if (cfg != NULL) {
        g_listen_fd = cfg->use_tcp ? listen_tcp(cfg->tcp_port) : listen_rfcomm();
        if (g_listen_fd >= 0) {
            result = 0;
        }
    }
    return result;
}

void bt_server_close(void)
{
    if (g_listen_fd >= 0) {
        (void)close(g_listen_fd);
        g_listen_fd = -1;
    }
}

bool bt_server_shutdown_acked(void)
{
    return atomic_load(&g_shutdown_acked);
}

static void conn_close(connection_t *conn, const char_t *reason)
{
    if (conn->fd >= 0) {
        (void)close(conn->fd);
        conn->fd = -1;
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
    bool ok = false;

    (void)memset(&frame, 0, sizeof(frame));
    frame.type = type;
    frame.len = len;
    frame.session_id = conn->session_id;
    frame.seq = conn->tx_seq + 1U;
    if ((payload != NULL) && (len > 0U) && (len <= PROTO_MAX_PAYLOAD)) {
        (void)memcpy(frame.payload, payload, len);
    }

    if (((payload != NULL) || (len == 0U)) &&
        (proto_encode(&frame, buf, sizeof(buf), &total) == 0)) {
        ssize_t n = send(conn->fd, buf, total, MSG_NOSIGNAL | MSG_DONTWAIT);
        if ((n >= 0) && ((size_t)n == total)) {
            conn->tx_seq = frame.seq;
            ok = true;
        }
    }
    if (!ok) {
        /* Teilweise gesendeter Frame waere nicht mehr synchron: Verbindung abbauen. */
        conn_close(conn, "Sendefehler");
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

    /* Reihenfolge: Tag, Session-ID, Sequenznummer, Wertebereich (SEC-02). */
    if ((proto_decode(buf, len, &frame) == 0) &&
        (frame.session_id == conn->session_id) &&
        (frame.seq > conn->rx_last_seq)) {
        conn->rx_last_seq = frame.seq;

        switch (frame.type) {
        case MSG_HEARTBEAT:
            if (proto_level_valid(frame.payload[0])) {
                comm->level = frame.payload[0];
                accepted = true;
            }
            break;
        case MSG_STOP:
            comm->stop = true;
            accepted = true;
            break;
        default:
            /* STATUS/ACK sind in dieser Richtung nicht vorgesehen */
            break;
        }
    }

    if (accepted) {
        comm->have_msg = true;
        comm->last_valid_us = time_now_us();
        shared_comm_write(comm);
    } else {
        conn->discarded++;
    }
}

static void receive_data(connection_t *conn, comm_data_t *comm)
{
    ssize_t n = recv(conn->fd, &conn->rx[conn->rx_len], sizeof(conn->rx) - conn->rx_len,
                     MSG_DONTWAIT);

    if (n > 0) {
        size_t pos = 0U;
        bool in_sync = true;

        conn->rx_len += (size_t)n;
        while (in_sync && ((conn->rx_len - pos) >= (size_t)PROTO_TYPE_LEN_BYTES)) {
            size_t total = 0U;

            if (proto_peek(&conn->rx[pos], &total) != 0) {
                in_sync = false; /* unbekannter Typ oder falsche Laenge */
            } else if ((conn->rx_len - pos) < total) {
                break; /* Frame noch unvollstaendig */
            } else {
                handle_frame(conn, comm, &conn->rx[pos], total);
                pos += total;
            }
        }

        if (!in_sync) {
            conn_close(conn, "Datenstrom nicht synchron");
        } else if (pos > 0U) {
            conn->rx_len -= pos;
            (void)memmove(conn->rx, &conn->rx[pos], conn->rx_len);
        } else {
            /* nichts verbraucht */
        }
    } else if (n == 0) {
        conn_close(conn, "Gegenstelle hat geschlossen");
    } else if ((errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) {
        conn_close(conn, "Empfangsfehler");
    } else {
        /* keine Daten vorhanden */
    }
}

static void accept_connection(connection_t *conn)
{
    int32_t fd = accept4(g_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (fd >= 0) {
        uint8_t rnd[4];

        if (crypto_random(rnd, sizeof(rnd)) == 0) {
            conn->fd = fd;
            conn->session_id = ((uint32_t)rnd[0] << 24) | ((uint32_t)rnd[1] << 16) |
                               ((uint32_t)rnd[2] << 8) | (uint32_t)rnd[3];
            conn->tx_seq = 0U;
            conn->rx_last_seq = 0U;
            conn->rx_len = 0U;
            conn->discarded = 0U;
            (void)fprintf(stderr, "[comm] Verbindung aufgebaut, Session-ID %08" PRIx32 "\n",
                          conn->session_id);
            /* Pi 2 sendet zuerst einen STATUS mit der neuen Session-ID. */
            if (send_status(conn)) {
                conn->next_status_us = time_now_us() +
                                       ((uint64_t)CFG_STATUS_PERIOD_MS * TIME_US_PER_MS);
            }
        } else {
            (void)close(fd);
        }
    }
}

void *bt_server_thread(void *arg)
{
    const uint64_t status_period_us = (uint64_t)CFG_STATUS_PERIOD_MS * TIME_US_PER_MS;
    connection_t conn;
    comm_data_t comm; /* einziger Schreiber von comm_data */
    bool ack_sent = false;

    (void)arg;
    (void)memset(&conn, 0, sizeof(conn));
    (void)memset(&comm, 0, sizeof(comm));
    conn.fd = -1;

    while (shared_running()) {
        struct pollfd pfd;
        safety_state_t state;
        int32_t timeout_ms = CFG_COMM_POLL_TIMEOUT_MS;
        int32_t ready;

        pfd.revents = 0;
        pfd.events = POLLIN;
        if (conn.fd >= 0) {
            uint64_t now_us = time_now_us();

            pfd.fd = conn.fd;
            if (conn.next_status_us <= now_us) {
                timeout_ms = 0;
            } else if ((conn.next_status_us - now_us) <
                       ((uint64_t)CFG_COMM_POLL_TIMEOUT_MS * TIME_US_PER_MS)) {
                timeout_ms = (int32_t)((conn.next_status_us - now_us) / TIME_US_PER_MS) + 1;
            } else {
                /* Standard-Timeout */
            }
        } else {
            pfd.fd = g_listen_fd;
        }

        /* Blockiert nie laenger als 50 ms; das Ausbleiben von Nachrichten
         * bewertet die Safety-Logik ueber den Zeitstempel in comm_data. */
        ready = poll(&pfd, 1U, timeout_ms);

        if (conn.fd < 0) {
            if ((ready > 0) && ((pfd.revents & POLLIN) != 0)) {
                accept_connection(&conn);
            }
        } else if (ready > 0) {
            if ((pfd.revents & POLLIN) != 0) {
                receive_data(&conn, &comm);
            } else if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                conn_close(&conn, "Verbindungsfehler");
            } else {
                /* kein relevantes Ereignis */
            }
        } else {
            /* Timeout oder EINTR */
        }

        shared_state_read(&state);
        if ((state.state == STATE_SHUTDOWN) && !ack_sent) {
            /* Letzter STATUS (SHUTDOWN), danach ACK. Scheitert das Senden, ist
             * die Verbindung bereits abgebaut und es gibt nichts zu bestaetigen. */
            if ((conn.fd >= 0) && send_status(&conn) &&
                !send_frame(&conn, MSG_ACK, NULL, 0U)) {
                (void)fprintf(stderr, "[comm] ACK konnte nicht gesendet werden\n");
            }
            ack_sent = true;
            atomic_store(&g_shutdown_acked, true);
        }

        if (conn.fd >= 0) {
            uint64_t now_us = time_now_us();

            if (now_us >= conn.next_status_us) {
                if (send_status(&conn)) {
                    conn.next_status_us += status_period_us;
                    if (conn.next_status_us <= now_us) {
                        conn.next_status_us = now_us + status_period_us;
                    }
                }
            }
        }
    }

    conn_close(&conn, "Programmende");
    return NULL;
}

/*
 * transport.c - Uebertragung ueber TCP (Test auf Linux-PC) und RFCOMM
 * (Betrieb, BlueZ-Socket-API). Beschreibung siehe transport.h.
 *
 * Alle Sockets sind nicht blockierend (SOCK_NONBLOCK); gewartet wird nur mit
 * poll() und begrenztem Timeout (Designdokument 2.8: Kommunikations-Thread
 * blockiert nie dauerhaft). send() mit MSG_NOSIGNAL, damit eine getrennte
 * Verbindung kein SIGPIPE ausloest.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "transport.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef HAVE_BLUEZ
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#endif

#include "time_utils.h"

/* --- Allgemein ------------------------------------------------------------- */

void transport_init(transport_t *t)
{
    if (t != NULL) {
        t->fd = -1;
        t->kind = 0U;
    }
}

bool transport_is_open(const transport_t *t)
{
    return (t != NULL) && (t->fd >= 0);
}

void transport_close(transport_t *t)
{
    if ((t != NULL) && (t->fd >= 0)) {
        (void)close(t->fd); /* Aufraeumpfad: keine sinnvolle Reaktion auf Fehler */
        t->fd = -1;
    }
}

/* Wartet hoechstens timeout_ms auf events. 1 = bereit, 0 = Timeout, -1 = Fehler. */
static int32_t wait_fd(int32_t fd, int16_t events, int32_t timeout_ms, int16_t *revents)
{
    struct pollfd pfd;
    int32_t rc;
    int32_t result;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    rc = poll(&pfd, 1U, (timeout_ms < 0) ? 0 : timeout_ms);
    if (rc > 0) {
        *revents = pfd.revents;
        result = 1;
    } else if ((rc == 0) || (errno == EINTR)) {
        *revents = 0;
        result = 0; /* Timeout oder durch Signal unterbrochen: wie Timeout behandeln */
    } else {
        result = -1;
    }
    return result;
}

/* --- Adressen --------------------------------------------------------------- */

/* Fuellt eine IPv4-Adresse. 0 = OK, -1 = ungueltige Adresse. */
static int32_t make_tcp_addr(const transport_cfg_t *cfg, struct sockaddr_in *addr)
{
    int32_t result = -1;

    (void)memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(cfg->tcp_port); /* Network Byte Order */
    if ((cfg->tcp_port != 0U) && (inet_pton(AF_INET, cfg->host, &addr->sin_addr) == 1)) {
        result = 0;
    }
    return result;
}

/* --- Client ----------------------------------------------------------------- */

static int32_t connect_tcp(transport_t *t, const transport_cfg_t *cfg)
{
    int32_t result = TRANSPORT_ERR;
    struct sockaddr_in addr;

    if (make_tcp_addr(cfg, &addr) == 0) {
        t->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (t->fd >= 0) {
            if (connect(t->fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) == 0) {
                result = TRANSPORT_OK;
            } else if (errno == EINPROGRESS) {
                result = TRANSPORT_PENDING; /* Abschluss mit transport_connect_poll() */
            } else {
                transport_close(t);
            }
        }
    }
    return result;
}

static int32_t connect_rfcomm(transport_t *t, const transport_cfg_t *cfg)
{
    int32_t result = TRANSPORT_ERR;

    /* Meilenstein 8: wird umgesetzt, sobald der Raspberry Pi verbunden ist. */
    (void)t;
    (void)cfg;
    return result;
}

int32_t transport_connect_start(transport_t *t, const transport_cfg_t *cfg)
{
    int32_t result = TRANSPORT_ERR;

    if ((t != NULL) && (cfg != NULL)) {
        transport_close(t);
        t->kind = cfg->kind;
        if (cfg->kind == TRANSPORT_KIND_TCP) {
            result = connect_tcp(t, cfg);
        } else if (cfg->kind == TRANSPORT_KIND_RFCOMM) {
            result = connect_rfcomm(t, cfg);
        } else {
            result = TRANSPORT_ERR; /* unbekannte Uebertragungsart */
        }
    }
    return result;
}

int32_t transport_connect_poll(transport_t *t, int32_t timeout_ms)
{
    int32_t result = TRANSPORT_ERR;

    if (transport_is_open(t)) {
        int16_t revents = 0;
        int32_t ready = wait_fd(t->fd, (int16_t)POLLOUT, timeout_ms, &revents);

        if (ready == 0) {
            result = TRANSPORT_PENDING;
        } else if (ready > 0) {
            /* Ergebnis des nicht blockierenden connect() steht in SO_ERROR. */
            int so_error = 0;
            socklen_t so_len = (socklen_t)sizeof(so_error);

            if ((getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0) &&
                (so_error == 0)) {
                result = TRANSPORT_OK;
            }
        } else {
            /* poll-Fehler */
        }
        if (result == TRANSPORT_ERR) {
            transport_close(t);
        }
    }
    return result;
}

/* --- Server ----------------------------------------------------------------- */

static int32_t listen_tcp(transport_t *l, const transport_cfg_t *cfg)
{
    int32_t result = TRANSPORT_ERR;
    struct sockaddr_in addr;

    if (make_tcp_addr(cfg, &addr) == 0) {
        l->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (l->fd >= 0) {
            int one = 1;

            if ((setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one)) == 0) &&
                (bind(l->fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) == 0) &&
                (listen(l->fd, 1) == 0)) {
                result = TRANSPORT_OK;
            } else {
                transport_close(l);
            }
        }
    }
    return result;
}

static int32_t listen_rfcomm(transport_t *l, const transport_cfg_t *cfg)
{
    /* Meilenstein 8: wird umgesetzt, sobald der Raspberry Pi verbunden ist. */
    (void)l;
    (void)cfg;
    return TRANSPORT_ERR;
}

int32_t transport_listen(transport_t *listener, const transport_cfg_t *cfg)
{
    int32_t result = TRANSPORT_ERR;

    if ((listener != NULL) && (cfg != NULL)) {
        transport_close(listener);
        listener->kind = cfg->kind;
        if (cfg->kind == TRANSPORT_KIND_TCP) {
            result = listen_tcp(listener, cfg);
        } else if (cfg->kind == TRANSPORT_KIND_RFCOMM) {
            result = listen_rfcomm(listener, cfg);
        } else {
            result = TRANSPORT_ERR;
        }
    }
    return result;
}

int32_t transport_accept(transport_t *listener, transport_t *conn, int32_t timeout_ms)
{
    int32_t result = TRANSPORT_ERR;

    if (transport_is_open(listener) && (conn != NULL)) {
        int16_t revents = 0;
        int32_t ready = wait_fd(listener->fd, (int16_t)POLLIN, timeout_ms, &revents);

        if (ready == 0) {
            result = TRANSPORT_PENDING;
        } else if (ready > 0) {
            int32_t fd = accept4(listener->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (fd >= 0) {
                transport_close(conn);
                conn->fd = fd;
                conn->kind = listener->kind;
                result = TRANSPORT_OK;
            } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
                result = TRANSPORT_PENDING;
            } else {
                /* Fehler beim Annehmen */
            }
        } else {
            /* poll-Fehler */
        }
    }
    return result;
}

/* --- Senden und Empfangen ----------------------------------------------------- */

int32_t transport_send(transport_t *t, const uint8_t *buf, size_t len, int32_t timeout_ms)
{
    int32_t result = TRANSPORT_OK;
    size_t done = 0U;
    uint64_t deadline_us = time_now_us() + ((uint64_t)((timeout_ms < 0) ? 0 : timeout_ms) * TIME_US_PER_MS);

    if (!transport_is_open(t) || ((buf == NULL) && (len > 0U))) {
        result = TRANSPORT_ERR;
    }
    while ((result == TRANSPORT_OK) && (done < len)) {
        ssize_t n = send(t->fd, &buf[done], len - done, MSG_NOSIGNAL | MSG_DONTWAIT);

        if (n > 0) {
            done += (size_t)n;
        } else if ((n < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))) {
            /* Sendepuffer voll: bis zur Frist auf Platz warten */
            uint64_t now_us = time_now_us();
            int16_t revents = 0;

            if (now_us >= deadline_us) {
                result = TRANSPORT_ERR;
            } else if (wait_fd(t->fd, (int16_t)POLLOUT,
                               (int32_t)((deadline_us - now_us) / TIME_US_PER_MS) + 1,
                               &revents) < 0) {
                result = TRANSPORT_ERR;
            } else {
                /* erneut versuchen */
            }
        } else {
            result = TRANSPORT_ERR;
        }
    }
    return result;
}

int32_t transport_recv(transport_t *t, uint8_t *buf, size_t cap, size_t *got, int32_t timeout_ms)
{
    int32_t result = TRANSPORT_ERR;

    if (transport_is_open(t) && (buf != NULL) && (got != NULL)) {
        int16_t revents = 0;
        int32_t ready = wait_fd(t->fd, (int16_t)POLLIN, timeout_ms, &revents);

        *got = 0U;
        if (ready == 0) {
            result = TRANSPORT_OK; /* Timeout, keine Daten */
        } else if (ready > 0) {
            ssize_t n = (cap > 0U) ? recv(t->fd, buf, cap, MSG_DONTWAIT) : 0;

            if (cap == 0U) {
                result = TRANSPORT_OK; /* kein Platz: nichts lesen */
            } else if (n > 0) {
                *got = (size_t)n;
                result = TRANSPORT_OK;
            } else if (n == 0) {
                result = TRANSPORT_CLOSED; /* Gegenseite hat geschlossen */
            } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
                result = TRANSPORT_OK;
            } else {
                result = TRANSPORT_ERR;
            }
        } else {
            result = TRANSPORT_ERR;
        }
    }
    return result;
}

/*
 * transport.h - Austauschbare Uebertragung: RFCOMM (Betrieb) oder TCP (Test
 * auf einem Linux-PC). Designdokument Abschnitt 2.8.
 *
 * Rollenneutral: Client-Funktionen (Pi 1) und Server-Funktionen (Pi 2,
 * simulierter Pi 2, genau eine Verbindung). Protokoll und Kryptografie sind
 * fuer beide Uebertragungsarten identisch und liegen nicht hier.
 *
 * Kein Aufruf blockiert laenger als der uebergebene Timeout. Sockets sind
 * nicht blockierend; gewartet wird nur mit poll(). Deshalb ist auch der
 * Verbindungsaufbau zweigeteilt (transport_connect_start/_poll): Ein
 * RFCOMM-Verbindungsaufbau kann mehrere Sekunden dauern und darf den
 * Kommunikations-Thread nicht aufhalten.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* Uebertragungsart */
#define TRANSPORT_KIND_TCP    1U
#define TRANSPORT_KIND_RFCOMM 2U

#define TRANSPORT_HOST_LEN    64U /* IPv4-Adresse als Text, inkl. '\0' */
#define TRANSPORT_BTADDR_LEN  18U /* "AA:BB:CC:DD:EE:FF" inkl. '\0' */

/* Rueckgabewerte */
#define TRANSPORT_OK          0
#define TRANSPORT_PENDING     1  /* Verbindungsaufbau laeuft noch / nichts angenommen */
#define TRANSPORT_ERR        (-1) /* Fehler; Verbindung ist geschlossen */
#define TRANSPORT_CLOSED     (-2) /* Gegenseite hat die Verbindung geschlossen */

typedef struct {
    uint8_t kind;                        /* TRANSPORT_KIND_TCP oder _RFCOMM */
    char_t host[TRANSPORT_HOST_LEN];     /* TCP: Adresse (Client: Ziel, Server: Bind-Adresse) */
    uint16_t tcp_port;                   /* TCP: Port */
    char_t bt_addr[TRANSPORT_BTADDR_LEN]; /* RFCOMM-Client: Adresse des Servers */
    uint8_t rfcomm_channel;              /* RFCOMM: Kanal (Standard CFG_RFCOMM_CHANNEL) */
} transport_cfg_t;

typedef struct {
    int32_t fd;   /* -1 = nicht offen */
    uint8_t kind;
} transport_t;

/* Markiert t als geschlossen (fd = -1). Vor der ersten Verwendung aufrufen. */
void transport_init(transport_t *t);

/* true, wenn t einen offenen Socket hat. */
bool transport_is_open(const transport_t *t);

/* Schliesst den Socket (falls offen). */
void transport_close(transport_t *t);

/* --- Client ---------------------------------------------------------------- */

/*
 * Startet den Verbindungsaufbau (nicht blockierend). TRANSPORT_OK = sofort
 * verbunden, TRANSPORT_PENDING = laeuft, weiter mit transport_connect_poll(),
 * TRANSPORT_ERR = fehlgeschlagen.
 */
int32_t transport_connect_start(transport_t *t, const transport_cfg_t *cfg);

/*
 * Wartet hoechstens timeout_ms auf den Abschluss des Verbindungsaufbaus.
 * TRANSPORT_OK = verbunden, TRANSPORT_PENDING = laeuft noch,
 * TRANSPORT_ERR = fehlgeschlagen (Socket geschlossen).
 */
int32_t transport_connect_poll(transport_t *t, int32_t timeout_ms);

/* --- Server ---------------------------------------------------------------- */

/* Oeffnet einen lauschenden Socket. TRANSPORT_OK oder TRANSPORT_ERR. */
int32_t transport_listen(transport_t *listener, const transport_cfg_t *cfg);

/*
 * Wartet hoechstens timeout_ms auf eine Verbindung. TRANSPORT_OK = conn ist
 * verbunden, TRANSPORT_PENDING = keine Verbindung, TRANSPORT_ERR = Fehler.
 */
int32_t transport_accept(transport_t *listener, transport_t *conn, int32_t timeout_ms);

/* --- Senden und Empfangen -------------------------------------------------- */

/*
 * Sendet genau len Bytes; wartet dafuer insgesamt hoechstens timeout_ms.
 * TRANSPORT_OK oder TRANSPORT_ERR. Bei TRANSPORT_ERR ist der Frame
 * moeglicherweise nur teilweise gesendet: Der Aufrufer muss die Verbindung
 * abbauen, weil der Datenstrom nicht mehr synchron ist.
 */
int32_t transport_send(transport_t *t, const uint8_t *buf, size_t len, int32_t timeout_ms);

/*
 * Wartet hoechstens timeout_ms auf Daten und liest hoechstens cap Bytes.
 * TRANSPORT_OK (got = Anzahl, 0 bei Timeout), TRANSPORT_CLOSED oder TRANSPORT_ERR.
 */
int32_t transport_recv(transport_t *t, uint8_t *buf, size_t cap, size_t *got, int32_t timeout_ms);

#endif /* TRANSPORT_H */

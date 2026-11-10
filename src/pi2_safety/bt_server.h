/*
 * bt_server.h - Kommunikations-Thread von Pi 2 (Server).
 *
 * Transport: RFCOMM ueber BlueZ (Normalbetrieb) oder TCP auf localhost
 * (Entwicklung ohne Bluetooth, z. B. mit tools/pi1_sim.py).
 */
#ifndef BT_SERVER_H
#define BT_SERVER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool use_tcp;      /* true: TCP auf 127.0.0.1 statt RFCOMM */
    uint16_t tcp_port;
} bt_server_cfg_t;

/* Oeffnet den lauschenden Socket. 0 = OK, -1 = Fehler. */
int32_t bt_server_init(const bt_server_cfg_t *cfg);
void bt_server_close(void);

void *bt_server_thread(void *arg);

/* true, sobald im Zustand SHUTDOWN das ACK gesendet wurde (oder keine
 * Verbindung besteht). */
bool bt_server_shutdown_acked(void);

#endif /* BT_SERVER_H */

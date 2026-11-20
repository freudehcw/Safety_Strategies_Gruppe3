/*
 * bt_client.h - Kommunikations- und Heartbeat-Thread von Pi 1 (Client).
 * Designdokument Abschnitte 2.1, 2.6, 2.8, 2.11 und 5.2 (T1-T3).
 *
 * Ablauf (Phasen):
 *   GETRENNT   -> jede Sekunde ein Verbindungsversuch (RFCOMM oder TCP)
 *   VERBINDEN  -> nicht blockierender Verbindungsaufbau (hoechstens 5 s)
 *   WARTEN     -> auf die erste STATUS-Nachricht warten und daraus die
 *                 Session-ID uebernehmen (hoechstens 1 s, sonst neu verbinden)
 *   ONLINE     -> alle 100 ms HEARTBEAT mit der Sollstufe (nur wenn eine Stufe
 *                 gewaehlt ist), STATUS empfangen und pruefen
 *   STOPPEN    -> STOP gesendet, hoechstens 2 s auf ACK warten
 *   FERTIG     -> Thread endet
 *
 * Der Thread ist der einzige Schreiber von link_status und der einzige
 * Nutzer von crypto_seal/crypto_open (siehe crypto_utils.h).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_BT_CLIENT_H
#define PI1_BT_CLIENT_H

#include "transport.h"

typedef struct {
    transport_cfg_t transport; /* Ziel: RFCOMM-Adresse oder TCP-Host/Port */
} comm_cfg_t;

/* arg zeigt auf ein comm_cfg_t, das bis zum Thread-Ende gueltig bleibt. */
void *comm_thread(void *arg);

#endif /* PI1_BT_CLIENT_H */

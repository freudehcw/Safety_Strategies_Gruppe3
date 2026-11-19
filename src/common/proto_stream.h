/*
 * proto_stream.h - Zerlegt den Byte-Datenstrom (RFCOMM/TCP) in Frames
 * (Designdokument Abschnitt 2.8).
 *
 * RFCOMM liefert einen Datenstrom, keine Pakete. Empfangene Bytes werden mit
 * proto_stream_push() angehaengt; proto_stream_next() liefert einen Frame,
 * sobald er vollstaendig ist. Vorher werden Typ und Laenge geprueft
 * (proto_peek). Ein unbekannter Typ oder eine falsche Laenge liefert
 * PROTO_STREAM_DESYNC: Der Datenstrom ist dann nicht mehr synchron, und der
 * Aufrufer muss die Verbindung abbauen.
 *
 * Der Puffer ist statisch im Struct, es gibt keine dynamische Speicherverwaltung.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PROTO_STREAM_H
#define PROTO_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#define PROTO_STREAM_BUF_LEN 256U /* mehr als 5 Frames maximaler Groesse */

/* Rueckgabewerte von proto_stream_next() */
#define PROTO_STREAM_FRAME     1  /* ein vollstaendiger Frame wurde kopiert */
#define PROTO_STREAM_NEED_MORE 0  /* noch kein vollstaendiger Frame */
#define PROTO_STREAM_DESYNC   (-1) /* unbekannter Typ oder falsche Laenge -> Verbindung abbauen */

typedef struct {
    uint8_t buf[PROTO_STREAM_BUF_LEN];
    size_t len; /* belegte Bytes am Anfang von buf */
} proto_stream_t;

/* Leert den Puffer (z. B. nach einem Verbindungsaufbau). */
void proto_stream_reset(proto_stream_t *st);

/* Freier Platz im Puffer; so viele Bytes darf der Aufrufer hoechstens empfangen. */
size_t proto_stream_free(const proto_stream_t *st);

/* Haengt len Bytes an. 0 = OK, -1 = kein Platz oder ungueltiges Argument. */
int32_t proto_stream_push(proto_stream_t *st, const uint8_t *data, size_t len);

/*
 * Entnimmt den naechsten vollstaendigen Frame nach frame (Kapazitaet cap,
 * mindestens PROTO_MAX_FRAME). Liefert PROTO_STREAM_FRAME, PROTO_STREAM_NEED_MORE
 * oder PROTO_STREAM_DESYNC.
 */
int32_t proto_stream_next(proto_stream_t *st, uint8_t *frame, size_t cap, size_t *frame_len);

#endif /* PROTO_STREAM_H */

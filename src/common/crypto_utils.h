/*
 * crypto_utils.h - Absicherung der Nutzdaten eines Frames.
 *
 * ACHTUNG - PLATZHALTER, KEINE SECURITY:
 * Die Schnittstelle entspricht bereits AES-256-GCM (AAD, 96-Bit-Nonce,
 * 128-Bit-Tag, siehe SEC-01/SEC-02), die aktuelle Implementierung
 * verschluesselt aber NICHT und schreibt nur eine CRC-32 in das Tag-Feld.
 * Damit werden zufaellige Uebertragungsfehler erkannt (Safety), aber kein
 * Angreifer abgewehrt. Fuer die Security-Umsetzung wird nur crypto_utils.c
 * durch eine OpenSSL-EVP-Implementierung ersetzt; Frame-Format und Aufrufer
 * bleiben unveraendert.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <stddef.h>
#include <stdint.h>

#define CRYPTO_NONCE_LEN 12U
#define CRYPTO_TAG_LEN   16U

/* Fuellt buf mit Zufallsbytes (getrandom). 0 = OK, -1 = Fehler. */
int32_t crypto_random(uint8_t *buf, size_t len);

/* Schuetzt pt (len Bytes) -> ct, schreibt das Tag. 0 = OK, -1 = Fehler. */
int32_t crypto_seal(const uint8_t *aad, size_t aad_len,
                const uint8_t nonce[CRYPTO_NONCE_LEN],
                const uint8_t *pt, size_t len,
                uint8_t *ct, uint8_t tag[CRYPTO_TAG_LEN]);

/* Prueft das Tag und liefert pt. 0 = OK, -1 = Tag ungueltig/Fehler. */
int32_t crypto_open(const uint8_t *aad, size_t aad_len,
                const uint8_t nonce[CRYPTO_NONCE_LEN],
                const uint8_t *ct, size_t len,
                const uint8_t tag[CRYPTO_TAG_LEN],
                uint8_t *pt);

#endif /* CRYPTO_UTILS_H */

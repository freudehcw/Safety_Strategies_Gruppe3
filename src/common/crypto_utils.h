/*
 * crypto_utils.h - Absicherung der Nutzdaten eines Frames mit AES-256-GCM
 * (Designdokument Abschnitt 5.2 T1/T2, SEC-01 bis SEC-03).
 *
 * Quellen (keine eigene Implementierung kryptografischer Verfahren):
 *   - OpenSSL 3.x, https://www.openssl.org, EVP-Schnittstelle:
 *     EVP_CIPHER_CTX_new, EVP_CIPHER_CTX_free, EVP_aes_256_gcm,
 *     EVP_EncryptInit_ex, EVP_EncryptUpdate, EVP_EncryptFinal_ex,
 *     EVP_DecryptInit_ex, EVP_DecryptUpdate, EVP_DecryptFinal_ex,
 *     EVP_CIPHER_CTX_ctrl mit EVP_CTRL_GCM_SET_IVLEN, EVP_CTRL_GCM_GET_TAG,
 *     EVP_CTRL_GCM_SET_TAG, OPENSSL_cleanse.
 *   - Verfahren: NIST SP 800-38D, "Recommendation for Block Cipher Modes of
 *     Operation: Galois/Counter Mode (GCM) and GMAC", 2007.
 *   - Nonce und Zufallswerte: getrandom(2), Linux man-pages.
 *
 * Nutzung: Einmal crypto_init() mit dem Schluessel aufrufen, danach
 * crypto_seal()/crypto_open() verwenden, am Ende crypto_cleanup().
 *
 * NICHT THREADSICHER: Die beiden OpenSSL-Kontexte werden in crypto_init()
 * einmal angelegt und danach wiederverwendet, damit im laufenden Betrieb kein
 * Speicher reserviert wird (keine dynamische Speicherverwaltung im Betrieb).
 * Deshalb darf pro Prozess nur EIN Thread crypto_seal()/crypto_open()
 * aufrufen; bei Pi 1 und Pi 2 ist das der Kommunikations-Thread.
 * crypto_random() ist davon nicht betroffen und darf ueberall verwendet werden.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5;
 * Erstfassung Claude Fable 5.2), geprueft von: <Name>
 */
#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"

#define CRYPTO_KEY_LEN   32U /* AES-256: 256-Bit-Schluessel */
#define CRYPTO_NONCE_LEN 12U /* 96-Bit-Nonce (SEC-01) */
#define CRYPTO_TAG_LEN   16U /* 128-Bit-Tag (SEC-02) */
#define CRYPTO_MAX_DATA_LEN 64U /* Obergrenze fuer Nutzdaten je Aufruf */

/* Rueckgabewerte von crypto_load_key_file() */
#define CRYPTO_KEY_OK          0
#define CRYPTO_KEY_ERR_OPEN   (-1) /* Datei fehlt oder ist nicht lesbar */
#define CRYPTO_KEY_ERR_TYPE   (-2) /* keine regulaere Datei */
#define CRYPTO_KEY_ERR_PERM   (-3) /* Rechte weiter als 0600 */
#define CRYPTO_KEY_ERR_SIZE   (-4) /* nicht genau CRYPTO_KEY_LEN Byte */
#define CRYPTO_KEY_ERR_READ   (-5) /* Lesefehler */
#define CRYPTO_KEY_ERR_ARG    (-6) /* ungueltiges Argument */

/* Fuellt buf mit Zufallsbytes (getrandom). 0 = OK, -1 = Fehler. */
int32_t crypto_random(uint8_t *buf, size_t len);

/*
 * Liest den Schluessel aus einer Datei: genau CRYPTO_KEY_LEN Byte Rohdaten,
 * regulaere Datei, Rechte nicht weiter als 0600. Liefert CRYPTO_KEY_OK oder
 * einen der Fehlercodes oben. Der Schluessel wird nie ausgegeben.
 */
int32_t crypto_load_key_file(const char_t *path, uint8_t key[CRYPTO_KEY_LEN]);

/* Kurzer Text zu einem Fehlercode von crypto_load_key_file() (fuer Meldungen). */
const char_t *crypto_key_error_text(int32_t code);

/*
 * Legt die OpenSSL-Kontexte an und setzt den Schluessel. Der Aufrufer sollte
 * seine Kopie des Schluessels danach mit crypto_wipe() loeschen.
 * 0 = OK, -1 = Fehler.
 */
int32_t crypto_init(const uint8_t key[CRYPTO_KEY_LEN]);

/* Gibt die Kontexte frei. Danach schlagen crypto_seal()/crypto_open() fehl. */
void crypto_cleanup(void);

/* Ueberschreibt Speicher mit Nullen (OPENSSL_cleanse), z. B. Schluesselkopien. */
void crypto_wipe(uint8_t *buf, size_t len);

/*
 * Verschluesselt pt (len Bytes) nach ct und schreibt das Tag; aad wird nur
 * authentifiziert. len darf 0 sein (STOP, ACK). 0 = OK, -1 = Fehler.
 */
int32_t crypto_seal(const uint8_t *aad, size_t aad_len,
                const uint8_t nonce[CRYPTO_NONCE_LEN],
                const uint8_t *pt, size_t len,
                uint8_t *ct, uint8_t tag[CRYPTO_TAG_LEN]);

/*
 * Prueft das Tag und liefert erst danach pt (SEC-02: Inhalt erst nach
 * erfolgreicher Tag-Pruefung verwenden). Bei ungueltigem Tag bleibt pt
 * unveraendert. 0 = OK, -1 = Tag ungueltig oder Fehler.
 */
int32_t crypto_open(const uint8_t *aad, size_t aad_len,
                const uint8_t nonce[CRYPTO_NONCE_LEN],
                const uint8_t *ct, size_t len,
                const uint8_t tag[CRYPTO_TAG_LEN],
                uint8_t *pt);

#endif /* CRYPTO_UTILS_H */

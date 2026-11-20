/*
 * crypto_utils.c - AES-256-GCM fuer die Frames zwischen Pi 1 und Pi 2,
 * Laden des Pre-Shared-Keys und Zufallszahlen (Designdokument 5.2 T1/T2,
 * SEC-01 bis SEC-03).
 *
 * Quellen (keine eigene Implementierung kryptografischer Verfahren):
 *   - OpenSSL 3.x, https://www.openssl.org, EVP-Schnittstelle:
 *     EVP_CIPHER_CTX_new, EVP_CIPHER_CTX_free, EVP_aes_256_gcm,
 *     EVP_EncryptInit_ex, EVP_EncryptUpdate, EVP_EncryptFinal_ex,
 *     EVP_DecryptInit_ex, EVP_DecryptUpdate, EVP_DecryptFinal_ex,
 *     EVP_CIPHER_CTX_ctrl (EVP_CTRL_GCM_SET_IVLEN, EVP_CTRL_GCM_GET_TAG,
 *     EVP_CTRL_GCM_SET_TAG), OPENSSL_cleanse.
 *     Vorgehen nach der OpenSSL-Dokumentation "EVP_EncryptInit(3)",
 *     Abschnitt "GCM and OCB Modes".
 *   - Verfahren: NIST SP 800-38D (Galois/Counter Mode), 2007.
 *   - Zufall: getrandom(2), Linux man-pages.
 *
 * Der Tag-Vergleich erfolgt in EVP_DecryptFinal_ex (OpenSSL), nicht hier.
 *
 * Abweichung "keine dynamische Speicherverwaltung im Betrieb": Die beiden
 * Kontexte werden nur in crypto_init() angelegt und pro Nachricht lediglich
 * mit einer neuen Nonce neu initialisiert. Ob OpenSSL intern dennoch Speicher
 * reserviert, kann das Programm nicht beeinflussen (siehe README, MISRA-
 * Abweichungen). Weil die Kontexte wiederverwendet werden, ist dieses Modul
 * nicht threadsicher; nur der Kommunikations-Thread verwendet seal/open.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5;
 * Erstfassung Claude Fable 5.2), geprueft von: <Name>
 */
#include "crypto_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

/* Kontexte fuer Ver- und Entschluesselung; NULL = nicht initialisiert. */
static EVP_CIPHER_CTX *g_enc_ctx = NULL;
static EVP_CIPHER_CTX *g_dec_ctx = NULL;

/* --- Zufall ---------------------------------------------------------------- */

int32_t crypto_random(uint8_t *buf, size_t len)
{
    size_t done = 0U;
    int32_t result = (buf != NULL) ? 0 : -1;

    while ((done < len) && (result == 0)) {
        ssize_t n = getrandom(&buf[done], len - done, 0U);
        if (n > 0) {
            done += (size_t)n;
        } else if ((n < 0) && (errno == EINTR)) {
            /* durch Signal unterbrochen: erneut versuchen */
        } else {
            result = -1;
        }
    }
    return result;
}

/* --- Schluessel ------------------------------------------------------------ */

/* Liest genau len Bytes aus fd. 0 = OK, -1 = Fehler oder Dateiende. */
static int32_t read_exact(int32_t fd, uint8_t *buf, size_t len)
{
    size_t done = 0U;
    int32_t result = 0;

    while ((done < len) && (result == 0)) {
        ssize_t n = read(fd, &buf[done], len - done);
        if (n > 0) {
            done += (size_t)n;
        } else if ((n < 0) && (errno == EINTR)) {
            /* erneut versuchen */
        } else {
            result = -1;
        }
    }
    return result;
}

int32_t crypto_load_key_file(const char_t *path, uint8_t key[CRYPTO_KEY_LEN])
{
    int32_t result = CRYPTO_KEY_ERR_ARG;

    if ((path != NULL) && (key != NULL)) {
        int32_t fd = open(path, O_RDONLY | O_CLOEXEC);

        if (fd < 0) {
            result = CRYPTO_KEY_ERR_OPEN;
        } else {
            struct stat st;

            /* Rechte und Groesse am geoeffneten Deskriptor pruefen, damit
             * zwischen Pruefen und Lesen keine andere Datei untergeschoben
             * werden kann. */
            if (fstat(fd, &st) != 0) {
                result = CRYPTO_KEY_ERR_READ;
            } else if (!S_ISREG(st.st_mode)) {
                result = CRYPTO_KEY_ERR_TYPE;
            } else if ((st.st_mode & (mode_t)0077) != 0U) {
                /* Gruppe oder andere haben Rechte: weiter als 0600 */
                result = CRYPTO_KEY_ERR_PERM;
            } else if (st.st_size != (off_t)CRYPTO_KEY_LEN) {
                result = CRYPTO_KEY_ERR_SIZE;
            } else if (read_exact(fd, key, CRYPTO_KEY_LEN) != 0) {
                crypto_wipe(key, CRYPTO_KEY_LEN);
                result = CRYPTO_KEY_ERR_READ;
            } else {
                result = CRYPTO_KEY_OK;
            }
            (void)close(fd);
        }
    }
    return result;
}

const char_t *crypto_key_error_text(int32_t code)
{
    const char_t *text;

    switch (code) {
    case CRYPTO_KEY_OK:
        text = "OK";
        break;
    case CRYPTO_KEY_ERR_OPEN:
        text = "Schluesseldatei fehlt oder ist nicht lesbar";
        break;
    case CRYPTO_KEY_ERR_TYPE:
        text = "Schluesseldatei ist keine regulaere Datei";
        break;
    case CRYPTO_KEY_ERR_PERM:
        text = "Rechte der Schluesseldatei weiter als 0600 (chmod 600)";
        break;
    case CRYPTO_KEY_ERR_SIZE:
        text = "Schluesseldatei hat nicht genau 32 Byte";
        break;
    case CRYPTO_KEY_ERR_READ:
        text = "Schluesseldatei konnte nicht gelesen werden";
        break;
    default:
        text = "ungueltiges Argument";
        break;
    }
    return text;
}

void crypto_wipe(uint8_t *buf, size_t len)
{
    if ((buf != NULL) && (len > 0U)) {
        OPENSSL_cleanse(buf, len);
    }
}

/* --- Initialisierung -------------------------------------------------------- */

int32_t crypto_init(const uint8_t key[CRYPTO_KEY_LEN])
{
    int32_t result = -1;

    crypto_cleanup(); /* erneuter Aufruf: alte Kontexte freigeben */

    if (key != NULL) {
        g_enc_ctx = EVP_CIPHER_CTX_new();
        g_dec_ctx = EVP_CIPHER_CTX_new();

        /* Ablauf nach EVP_EncryptInit(3): zuerst Verfahren, dann Nonce-Laenge
         * (96 Bit sind fuer GCM Standard, wird trotzdem explizit gesetzt),
         * dann Schluessel. Die Nonce folgt je Nachricht in seal/open. */
        if ((g_enc_ctx != NULL) && (g_dec_ctx != NULL) &&
            (EVP_EncryptInit_ex(g_enc_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1) &&
            (EVP_CIPHER_CTX_ctrl(g_enc_ctx, EVP_CTRL_GCM_SET_IVLEN, (int)CRYPTO_NONCE_LEN, NULL) == 1) &&
            (EVP_EncryptInit_ex(g_enc_ctx, NULL, NULL, key, NULL) == 1) &&
            (EVP_DecryptInit_ex(g_dec_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1) &&
            (EVP_CIPHER_CTX_ctrl(g_dec_ctx, EVP_CTRL_GCM_SET_IVLEN, (int)CRYPTO_NONCE_LEN, NULL) == 1) &&
            (EVP_DecryptInit_ex(g_dec_ctx, NULL, NULL, key, NULL) == 1)) {
            result = 0;
        } else {
            crypto_cleanup();
        }
    }
    return result;
}

void crypto_cleanup(void)
{
    /* EVP_CIPHER_CTX_free loescht auch das Schluesselmaterial im Kontext. */
    if (g_enc_ctx != NULL) {
        EVP_CIPHER_CTX_free(g_enc_ctx);
        g_enc_ctx = NULL;
    }
    if (g_dec_ctx != NULL) {
        EVP_CIPHER_CTX_free(g_dec_ctx);
        g_dec_ctx = NULL;
    }
}

/* --- Verschluesseln und Pruefen ---------------------------------------------- */

/* Gemeinsame Pruefung der Argumente von seal und open. */
static bool args_valid(const uint8_t *aad, size_t aad_len, const uint8_t *nonce,
                       const uint8_t *in, size_t len, const uint8_t *out,
                       const uint8_t *tag)
{
    bool ok = (nonce != NULL) && (tag != NULL) &&
              (aad_len <= (size_t)CRYPTO_MAX_DATA_LEN) &&
              (len <= (size_t)CRYPTO_MAX_DATA_LEN);

    if ((aad_len > 0U) && (aad == NULL)) {
        ok = false;
    }
    if ((len > 0U) && ((in == NULL) || (out == NULL))) {
        ok = false;
    }
    return ok;
}

int32_t crypto_seal(const uint8_t *aad, size_t aad_len,
                const uint8_t nonce[CRYPTO_NONCE_LEN],
                const uint8_t *pt, size_t len,
                uint8_t *ct, uint8_t tag[CRYPTO_TAG_LEN])
{
    int32_t result = -1;
    int outl = 0;
    bool ok = (g_enc_ctx != NULL) && args_valid(aad, aad_len, nonce, pt, len, ct, tag);

    /* Neue Nonce setzen; Verfahren und Schluessel bleiben aus crypto_init. */
    if (ok) {
        ok = (EVP_EncryptInit_ex(g_enc_ctx, NULL, NULL, NULL, nonce) == 1);
    }
    /* AAD (Bytes 0-10 des Frames) nur authentifizieren: Ausgabepuffer NULL. */
    if (ok && (aad_len > 0U)) {
        ok = (EVP_EncryptUpdate(g_enc_ctx, NULL, &outl, aad, (int)aad_len) == 1);
    }
    if (ok && (len > 0U)) {
        ok = (EVP_EncryptUpdate(g_enc_ctx, ct, &outl, pt, (int)len) == 1) &&
             (outl == (int)len);
    }
    if (ok) {
        uint8_t final_block[16];
        /* Bei GCM liefert Final keine weiteren Daten, schliesst aber das Tag ab. */
        ok = (EVP_EncryptFinal_ex(g_enc_ctx, final_block, &outl) == 1) && (outl == 0);
    }
    if (ok) {
        ok = (EVP_CIPHER_CTX_ctrl(g_enc_ctx, EVP_CTRL_GCM_GET_TAG, (int)CRYPTO_TAG_LEN, tag) == 1);
    }
    if (ok) {
        result = 0;
    }
    return result;
}

int32_t crypto_open(const uint8_t *aad, size_t aad_len,
                const uint8_t nonce[CRYPTO_NONCE_LEN],
                const uint8_t *ct, size_t len,
                const uint8_t tag[CRYPTO_TAG_LEN],
                uint8_t *pt)
{
    int32_t result = -1;
    int outl = 0;
    /* EVP_DecryptUpdate liefert den Klartext, BEVOR das Tag geprueft ist.
     * Deshalb zuerst in einen lokalen Puffer; nach pt wird erst kopiert, wenn
     * EVP_DecryptFinal_ex das Tag bestaetigt hat (SEC-02). */
    uint8_t tmp[CRYPTO_MAX_DATA_LEN];
    uint8_t tag_copy[CRYPTO_TAG_LEN];
    bool ok = (g_dec_ctx != NULL) && args_valid(aad, aad_len, nonce, ct, len, pt, tag);

    if (ok) {
        ok = (EVP_DecryptInit_ex(g_dec_ctx, NULL, NULL, NULL, nonce) == 1);
    }
    if (ok && (aad_len > 0U)) {
        ok = (EVP_DecryptUpdate(g_dec_ctx, NULL, &outl, aad, (int)aad_len) == 1);
    }
    if (ok && (len > 0U)) {
        ok = (EVP_DecryptUpdate(g_dec_ctx, tmp, &outl, ct, (int)len) == 1) &&
             (outl == (int)len);
    }
    if (ok) {
        /* EVP_CTRL_GCM_SET_TAG erwartet einen nicht konstanten Zeiger. */
        (void)memcpy(tag_copy, tag, CRYPTO_TAG_LEN);
        ok = (EVP_CIPHER_CTX_ctrl(g_dec_ctx, EVP_CTRL_GCM_SET_TAG, (int)CRYPTO_TAG_LEN, tag_copy) == 1);
    }
    if (ok) {
        uint8_t final_block[16];
        /* Hier vergleicht OpenSSL das Tag; != 1 heisst: Nachricht verfaelscht. */
        ok = (EVP_DecryptFinal_ex(g_dec_ctx, final_block, &outl) == 1);
    }
    if (ok) {
        if (len > 0U) {
            (void)memcpy(pt, tmp, len);
        }
        result = 0;
    }
    crypto_wipe(tmp, sizeof(tmp));
    return result;
}

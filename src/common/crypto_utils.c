/*
 * crypto_utils.c - PLATZHALTER ohne Security (siehe crypto_utils.h).
 * Tag = CRC-32 (IEEE 802.3) ueber AAD || Nonce || Nutzdaten, Rest 0.
 */
#include "crypto_utils.h"

#include <errno.h>
#include <string.h>
#include <sys/random.h>
#include <sys/types.h>

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    uint32_t c = crc;
    size_t i;
    uint32_t bit;

    for (i = 0U; i < len; i++) {
        c ^= (uint32_t)data[i];
        for (bit = 0U; bit < 8U; bit++) {
            if ((c & 1U) != 0U) {
                c = (c >> 1) ^ 0xEDB88320UL;
            } else {
                c >>= 1;
            }
        }
    }
    return c;
}

static void compute_tag(const uint8_t *aad, size_t aad_len,
                        const uint8_t nonce[CRYPTO_NONCE_LEN],
                        const uint8_t *data, size_t len,
                        uint8_t tag[CRYPTO_TAG_LEN])
{
    uint32_t crc = 0xFFFFFFFFUL;

    crc = crc32_update(crc, aad, aad_len);
    crc = crc32_update(crc, nonce, CRYPTO_NONCE_LEN);
    crc = crc32_update(crc, data, len);
    crc ^= 0xFFFFFFFFUL;

    (void)memset(tag, 0, CRYPTO_TAG_LEN);
    tag[0] = (uint8_t)(crc >> 24);
    tag[1] = (uint8_t)(crc >> 16);
    tag[2] = (uint8_t)(crc >> 8);
    tag[3] = (uint8_t)crc;
}

int32_t crypto_random(uint8_t *buf, size_t len)
{
    size_t done = 0U;
    int32_t result = (buf != NULL) ? 0 : -1;

    while ((done < len) && (result == 0)) {
        ssize_t n = getrandom(&buf[done], len - done, 0U);
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

int32_t crypto_seal(const uint8_t *aad, size_t aad_len,
                const uint8_t nonce[CRYPTO_NONCE_LEN],
                const uint8_t *pt, size_t len,
                uint8_t *ct, uint8_t tag[CRYPTO_TAG_LEN])
{
    int32_t result = -1;

    if ((aad != NULL) && (nonce != NULL) && (ct != NULL) && (tag != NULL) &&
        ((pt != NULL) || (len == 0U))) {
        if (len > 0U) {
            (void)memcpy(ct, pt, len);
        }
        compute_tag(aad, aad_len, nonce, ct, len, tag);
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
    uint8_t expected[CRYPTO_TAG_LEN];
    int32_t result = -1;

    if ((aad != NULL) && (nonce != NULL) && (tag != NULL) &&
        (((ct != NULL) && (pt != NULL)) || (len == 0U))) {
        compute_tag(aad, aad_len, nonce, ct, len, expected);
        if (memcmp(expected, tag, CRYPTO_TAG_LEN) == 0) {
            if (len > 0U) {
                (void)memcpy(pt, ct, len);
            }
            result = 0;
        }
    }
    return result;
}

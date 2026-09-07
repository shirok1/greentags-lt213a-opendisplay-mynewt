#include "security.h"
#include "mbedtls/aes.h"
#include "mbedtls/ccm.h"
#include "mbedtls/cmac.h"
#include "mbedtls/platform_util.h"
#include "platform.h"
#include "storage.h"
#include <string.h>
static struct {
    uint8_t key[16], id[8], challenge[16];
    uint64_t rx, tx;
    uint32_t challenge_at, last_activity;
    uint8_t authenticated, pending, have_rx;
} auth;
static uint32_t attempts_at;
static unsigned attempts;
static int equal(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned diff = 0;
    while (n--)
        diff |= *a++ ^ *b++;
    return diff == 0;
}
static int cmac(const uint8_t *key, const uint8_t *p, size_t n, uint8_t out[16]) {
    return mbedtls_cipher_cmac(mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB), key, 128,
                               p, n, out);
}
void od_security_reset(void) { mbedtls_platform_zeroize(&auth, sizeof auth); }
int od_security_enabled(void) {
    const uint8_t *p = od_store_record(0x27);
    unsigned any = 0;
    if (!p || !p[0])
        return 0;
    for (unsigned i = 1; i <= 16; i++)
        any |= p[i];
    return !!any;
}
/* Keep the AES authentication scratch frame off the nested encrypted-send path. */
static __attribute__((noinline)) void authenticate(const uint8_t *b, size_t n, unsigned mtu,
                                                   od_send_fn send, void *arg) {
    uint8_t response[23] = {0, 0x50, 0xff};
    size_t length = 3;
    const uint8_t *config = od_store_record(0x27);
    if (!od_security_enabled()) {
        response[2] = 3;
        goto done;
    }
    uint32_t now = od_millis();
    if (auth.pending && (uint32_t)(now - auth.challenge_at) > 30000) {
        auth.pending = 0;
        mbedtls_platform_zeroize(auth.challenge, sizeof auth.challenge);
    }
    if ((uint32_t)(now - attempts_at) >= 60000) {
        attempts = 0;
        attempts_at = now;
    }
    if (attempts >= 10) {
        response[2] = 4;
        goto done;
    }
    if (n == 3 && b[2] == 0) {
        ++attempts;
        od_security_reset();
        if (mtu < 37 || od_random(auth.challenge, 16))
            goto done;
        auth.pending = 1;
        auth.challenge_at = now;
        response[2] = 0;
        memcpy(response + 3, auth.challenge, 16);
        od_device_id(response + 19);
        length = 23;
    } else if (n == 34 && auth.pending && (uint32_t)(now - auth.challenge_at) <= 30000) {
        uint8_t input[64], tag[16], id[4];
        od_device_id(id);
        auth.pending = 0;
        memcpy(input, auth.challenge, 16);
        memcpy(input + 16, b + 2, 16);
        memcpy(input + 32, id, 4);
        if (cmac(config + 1, input, 36, tag) || !equal(tag, b + 18, 16)) {
            od_security_reset();
            response[2] = 1;
            goto done;
        }
        memcpy(input, "OpenDisplay session", 19);
        input[19] = 0;
        memcpy(input + 20, id, 4);
        memcpy(input + 24, b + 2, 16);
        memcpy(input + 40, auth.challenge, 16);
        input[56] = 0;
        input[57] = 128;
        if (cmac(config + 1, input, 58, tag))
            goto bad;
        memset(input, 0, 8);
        input[7] = 1;
        memcpy(input + 8, tag, 8);
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        int rc = mbedtls_aes_setkey_enc(&aes, config + 1, 128);
        if (!rc)
            rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, auth.key);
        mbedtls_aes_free(&aes);
        if (rc)
            goto bad;
        memcpy(input, b + 2, 16);
        memcpy(input + 16, auth.challenge, 16);
        if (cmac(auth.key, input, 32, tag))
            goto bad;
        memcpy(auth.id, tag, 8);
        memcpy(input, auth.challenge, 16);
        memcpy(input + 16, b + 2, 16);
        memcpy(input + 32, id, 4);
        if (cmac(auth.key, input, 36, tag))
            goto bad;
        response[2] = 0;
        memcpy(response + 3, tag, 16);
        length = 19;
        /* Separate device/host nonce spaces; official Python decrypt accepts
         * this counter value. Never reuse a request nonce in a response. */
        auth.tx = UINT64_C(1) << 63;
        auth.authenticated = 1;
        auth.last_activity = now;
        mbedtls_platform_zeroize(input, sizeof input);
        mbedtls_platform_zeroize(tag, sizeof tag);
        goto done;
    bad:
        od_security_reset();
    }
done:
    if (send(response, length, arg))
        od_security_reset();
}
struct secure_sender {
    od_send_fn send;
    void *arg;
};
static int encrypted_send(const uint8_t *b, size_t n, void *arg) {
    struct secure_sender *sender = arg;
    uint8_t packet[OD_MAX_RESPONSE + 29], plain[OD_MAX_RESPONSE - 1];
    if (n < 2 || n > OD_MAX_RESPONSE || !auth.authenticated || auth.tx == UINT64_MAX)
        return -1;
    packet[0] = b[0];
    packet[1] = b[1];
    memcpy(packet + 2, auth.id, 8);
    for (unsigned i = 0; i < 8; i++)
        packet[10 + i] = auth.tx >> (56 - 8 * i);
    ++auth.tx;
    plain[0] = n - 2;
    memcpy(plain + 1, b + 2, n - 2);
    mbedtls_ccm_context c;
    mbedtls_ccm_init(&c);
    int rc = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, auth.key, 128);
    if (!rc)
        rc = mbedtls_ccm_encrypt_and_tag(&c, n - 1, packet + 5, 13, packet, 2, plain, packet + 18,
                                         packet + 17 + n, 12);
    mbedtls_ccm_free(&c);
    mbedtls_platform_zeroize(plain, sizeof plain);
    if (rc)
        return -1;
    return sender->send(packet, n + 29, sender->arg);
}
void od_secure_command(struct od_session *s, uint8_t *b, size_t n, const uint8_t msd[16],
                       unsigned mtu, od_send_fn send, void *arg) {
    if (n < 2 || b[0]) {
        const uint8_t r[] = {0xff, n > 1 ? b[1] : 0};
        send(r, 2, arg);
        return;
    }
    if (b[1] == 0x50) {
        authenticate(b, n, mtu, send, arg);
        return;
    }
    const uint8_t *cfg = od_store_record(0x27);
    unsigned timeout = cfg ? (cfg[17] | cfg[18] << 8) : 0;
    if (auth.authenticated && timeout &&
        (uint32_t)(od_millis() - auth.last_activity) / 1000 >= timeout)
        od_security_reset();
    int protected = od_security_enabled() && b[1] != 0x43 && b[1] != 0x44;
    if (protected && !auth.authenticated && !((b[1] == 0x41 || b[1] == 0x42) && (cfg[19] & 1))) {
        const uint8_t r[] = {0xfe, b[1]};
        send(r, 2, arg);
        return;
    }
    struct secure_sender sender = {send, arg};
    size_t ignored;
    const uint8_t *old = od_store_data(&ignored);
    if (protected && auth.authenticated) {
        uint64_t counter = 0;
        if (n < 31 || n > OD_MAX_COMMAND || mtu < 34 || !equal(b + 2, auth.id, 8))
            goto reject;
        for (unsigned i = 0; i < 8; i++)
            counter = (counter << 8) | b[10 + i];
        if (counter >> 63 || (auth.have_rx && counter <= auth.rx))
            goto reject;
        mbedtls_ccm_context c;
        mbedtls_ccm_init(&c);
        int rc = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, auth.key, 128);
        if (!rc)
            rc = mbedtls_ccm_auth_decrypt(&c, n - 30, b + 5, 13, b, 2, b + 18, b + 18, b + n - 12,
                                          12);
        mbedtls_ccm_free(&c);
        if (rc || b[18] != n - 31)
            goto reject;
        auth.rx = counter;
        auth.have_rx = 1;
        auth.last_activity = od_millis();
        n = b[18] + 2;
        memmove(b + 2, b + 19, n - 2);
        od_command(s, b, n, msd, mtu - 29, encrypted_send, &sender);
    } else
        od_command(s, b, n, msd, mtu, send, arg);
    if (old != od_store_data(&ignored))
        od_security_reset();
    return;
reject:
    {
        const uint8_t r[] = {0xff, b[1]};
        send(r, 2, arg);
        od_security_reset();
        od_abort(s);
    }
}

#include "platform.h"
#include "protocol.h"
#include "security.h"
#include "storage.h"
#include <stdlib.h>
#include <string.h>
static uint8_t flash[0x20000], replies[300][129], image[6000];
static unsigned sizes[300], count, image_size, refresh_count;
static int fail_flash = -1, refresh_error;
static struct od_session session;
static uint32_t now;
const uint8_t *od_flash_ptr(uint32_t a) {
    if (a >= sizeof flash)
        abort();
    return flash + a;
}
int od_flash_erase(uint32_t a) {
    if (fail_flash == 0)
        return -1;
    if (fail_flash > 0)
        --fail_flash;
    if (a < OD_SLOT0 || a + 1024 > sizeof flash)
        abort();
    memset(flash + a, 255, 1024);
    return 0;
}
int od_flash_write(uint32_t a, const void *buf, size_t n) {
    if (fail_flash == 0)
        return -1;
    if (fail_flash > 0)
        --fail_flash;
    if (a < OD_SLOT0 || a + n > sizeof flash)
        abort();
    const uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        if ((flash[a + i] & p[i]) != p[i])
            return -1;
        flash[a + i] = p[i];
    }
    return 0;
}
uint32_t od_millis(void) { return now; }
int od_random(void *p, size_t n) {
    static unsigned seed;
    uint8_t *b = p;
    while (n--)
        *b++ = ++seed;
    return 0;
}
void od_device_id(uint8_t p[4]) { memcpy(p, "test", 4); }
void od_reboot(void) {
    od_store_init();
    od_security_reset();
}
int od_read_msd(uint8_t p[16]) {
    memset(p, 0, 16);
    return 0;
}
int epd_begin(void) {
    image_size = 0;
    return 0;
}
int epd_begin_partial(unsigned x, unsigned y, unsigned w, unsigned h) {
    image_size = 0;
    return 0;
}
int epd_write(const uint8_t *b, size_t n) {
    if (n > sizeof image - image_size)
        abort();
    memcpy(image + image_size, b, n);
    image_size += n;
    return 0;
}
int epd_refresh(void) {
    ++refresh_count;
    return refresh_error;
}
int epd_off(void) { return 0; }
static int send(const uint8_t *b, size_t n, void *arg) {
    if (count >= 300 || n > 129)
        abort();
    memcpy(replies[count], b, n);
    sizes[count++] = n;
    return 0;
}
void test_reset(void) {
    memset(flash, 255, sizeof flash);
    memset(&session, 0, sizeof session);
    od_store_init();
    od_security_reset();
    now += 61000;
    fail_flash = -1;
    refresh_error = 0;
    refresh_count = 0;
}
void test_reconnect(void) {
    od_abort(&session);
    od_store_abort();
    od_security_reset();
}
void test_time(unsigned ms) { now += ms; }
void test_set_time(unsigned ms) { now = ms; }
unsigned test_transfer_remaining(void) { return od_transfer_remaining_ms(&session); }
unsigned test_expire_transfer(void) {
    if (!od_expire_transfer(&session))
        return 0;
    od_store_abort();
    od_security_reset();
    return 1;
}
void test_fail_flash(int n) { fail_flash = n; }
void test_fail_refresh(int n) { refresh_error = n; }
unsigned test_refreshes(void) { return refresh_count; }
unsigned test_command(const uint8_t *b, unsigned n, unsigned mtu) {
    uint8_t input[244], msd[16] = {0};
    count = 0;
    if (test_expire_transfer())
        return 0; /* Worker drops the connection without executing this command. */
    if (n > sizeof input)
        abort();
    memcpy(input, b, n);
    od_secure_command(&session, input, n, msd, mtu, send, NULL);
    return count;
}
unsigned test_reply(unsigned i, uint8_t *out) {
    if (i >= count)
        abort();
    memcpy(out, replies[i], sizes[i]);
    return sizes[i];
}
unsigned test_image(uint8_t *out) {
    memcpy(out, image, image_size);
    return image_size;
}
unsigned test_config(uint8_t *out) {
    size_t n;
    const uint8_t *p = od_store_data(&n);
    memcpy(out, p, n);
    return n;
}
void test_boot(void) { od_reboot(); }

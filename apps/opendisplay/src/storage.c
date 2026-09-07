#include "storage.h"
#include "config_data.h"
#include <string.h>
#define MAGIC 0x4f444346u
static uint32_t active, sequence, staging;
static uint16_t expected, received;
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t crc(const uint8_t *p, size_t n) {
    uint16_t c = 0xffff;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(i < 2 ? 0 : p[i]) << 8;
        for (unsigned j = 0; j < 8; j++)
            c = (c << 1) ^ ((c & 0x8000) ? 0x1021 : 0);
    }
    return c;
}
static unsigned record_size(uint8_t kind) {
    switch (kind) {
    case 1:
    case 2:
        return 22;
    case 4:
        return 30;
    case 0x20:
        return 46;
    case 0x27:
        return 64;
    default:
        return 0; /* No external peripherals exist on this board. */
    }
}
int od_config_validate(const uint8_t *p, size_t n) {
    if (n < 5 || n > OD_CONFIG_MAX || p[2] != 1 || crc(p, n - 2) != (p[n - 2] | p[n - 1] << 8))
        return -1;
    unsigned seen = 0;
    for (size_t i = 3; i < n - 2;) {
        if (i + 2 > n - 2 || p[i] != 0)
            return -1;
        unsigned kind = p[i + 1], size = record_size(kind), bit;
        if (!size || i + 2 + size > n - 2)
            return -1;
        bit = kind == 1 ? 1 : kind == 2 ? 2 : kind == 4 ? 4 : kind == 0x20 ? 8 : 16;
        if (seen & bit)
            return -1;
        seen |= bit;
        const uint8_t *r = p + i + 2;
        if (kind == 0x20 && (r[0] != 0 || r[1] != 1 || r[4] != 104 || r[5] || r[6] != 212 || r[7] ||
                             r[21] != 0 || memcmp(r + 15, od_config + 100, 5) ||
                             r[23] != od_config[108] || r[20] != 1 || r[22] != 25))
            return -1;
        if (kind == 0x27 && (r[0] > 1 || (r[19] & ~1u)))
            return -1; /* No reset GPIO or key-display feature. */
        i += 2 + size;
    }
    return (seen & 15) == 15 ? 0 : -1;
}
static int valid_slot(uint32_t address) {
    const uint8_t *p = od_flash_ptr(address);
    size_t n = u32(p + 8);
    return u32(p) == MAGIC && n <= OD_CONFIG_MAX && !od_config_validate(p + 16, n);
}
void od_store_abort(void) {
    expected = received = 0;
    staging = 0;
}
void od_store_init(void) {
    active = sequence = 0;
    od_store_abort();
    if (valid_slot(OD_SLOT0))
        active = OD_SLOT0;
    if (valid_slot(OD_SLOT1) &&
        (!active || (int32_t)(u32(od_flash_ptr(OD_SLOT1) + 4) - u32(od_flash_ptr(active) + 4)) > 0))
        active = OD_SLOT1;
    if (active)
        sequence = u32(od_flash_ptr(active) + 4);
}
const uint8_t *od_store_data(size_t *n) {
    if (!active) {
        *n = sizeof od_config;
        return od_config;
    }
    *n = u32(od_flash_ptr(active) + 8);
    return od_flash_ptr(active) + 16;
}
const uint8_t *od_store_record(uint8_t kind) {
    size_t n;
    const uint8_t *p = od_store_data(&n);
    for (size_t i = 3; i < n - 2; i += 2 + record_size(p[i + 1]))
        if (p[i + 1] == kind)
            return p + i + 2;
    return NULL;
}
int od_store_begin(size_t n) {
    od_store_abort();
    if (n < 5 || n > OD_CONFIG_MAX)
        return -1;
    uint32_t next = active == OD_SLOT0 ? OD_SLOT1 : OD_SLOT0;
    for (unsigned i = 0; i < OD_SLOT_BYTES; i += 1024)
        if (od_flash_erase(next + i))
            return -1;
    staging = next;
    expected = n;
    return 0;
}
int od_store_append(const uint8_t *p, size_t n) {
    if (!staging || !n || received > expected || n > (size_t)(expected - received)) {
        od_store_abort();
        return -1;
    }
    if (od_flash_write(staging + 16 + received, p, n)) {
        od_store_abort();
        return -1;
    }
    received += n;
    if (received != expected)
        return 0;
    if (od_config_validate(od_flash_ptr(staging) + 16, expected)) {
        od_store_abort();
        return -1;
    }
    uint32_t header[4] = {MAGIC, sequence + 1, expected, 0};
    if (od_flash_write(staging + 4, header + 1, 12) || od_flash_write(staging, header, 4)) {
        od_store_abort();
        return -1;
    }
    active = staging;
    ++sequence;
    od_store_abort();
    return 1;
}
int od_store_clear(void) {
    return od_store_begin(sizeof od_config) || od_store_append(od_config, sizeof od_config) != 1
               ? -1
               : 0;
}

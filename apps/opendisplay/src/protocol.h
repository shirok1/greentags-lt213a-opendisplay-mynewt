#ifndef OD_PROTOCOL_H
#define OD_PROTOCOL_H
#include "epd.h"
#include "inflate.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define OD_MAX_COMMAND 244u
#define OD_MAX_RESPONSE 100u
#define OD_BLE_IDLE_MS 120000u
#define OD_TRANSFER_TIMEOUT_MS 900000u
struct od_session {
    uint16_t written, total;
    bool active, compressed, partial, etag_valid, new_etag_valid, pipe, failed, seen;
    uint32_t etag, new_etag, mask, started_at;
    uint8_t next, highest, window, ack_every, since_ack;
    uint16_t frame;
    struct {
        uint8_t data[241];
        uint8_t len, seq;
        bool used;
    } pending;
    struct od_inflate inflate;
};
typedef int (*od_send_fn)(const uint8_t *, size_t, void *);
void od_abort(struct od_session *s);
/* UINT32_MAX when inactive, zero when the absolute transfer deadline expires. */
uint32_t od_transfer_remaining_ms(const struct od_session *s);
bool od_expire_transfer(struct od_session *s);
void od_command(struct od_session *s, const uint8_t *buf, size_t len, const uint8_t msd[16],
                unsigned mtu, od_send_fn send, void *arg);
#endif

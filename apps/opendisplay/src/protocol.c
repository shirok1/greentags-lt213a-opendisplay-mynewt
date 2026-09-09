#include "protocol.h"
#include "platform.h"
#include "storage.h"
#include <string.h>
static uint16_t le16(const uint8_t *b) { return b[0] | b[1] << 8; }
static uint16_t be16(const uint8_t *b) { return b[1] | b[0] << 8; }
static uint32_t le32(const uint8_t *b) { return (uint32_t)le16(b) | (uint32_t)le16(b + 2) << 16; }
static uint32_t be32(const uint8_t *b) { return (uint32_t)be16(b) << 16 | be16(b + 2); }
static void put32(uint8_t *b, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        b[i] = n >> (8 * i);
}
void od_abort(struct od_session *s) {
    if (s->active) {
        epd_off();
        s->etag_valid = false;
    }
    s->active = false;
    s->written = 0;
    memset(s->reorder, 0, sizeof s->reorder);
}
static int emitted(uint8_t value, void *arg) {
    struct od_session *s = arg;
    if (s->written >= s->total || epd_write(&value, 1))
        return -1;
    ++s->written;
    return 0;
}
static int stream(struct od_session *s, const uint8_t *b, size_t n) {
    if (s->compressed)
        return od_inflate_feed(&s->inflate, b, n, emitted, s);
    if (s->written > s->total || n > (size_t)(s->total - s->written) || epd_write(b, n))
        return -1;
    s->written += n;
    return 0;
}
static int begin(struct od_session *s, bool compressed, bool partial, unsigned x, unsigned y,
                 unsigned w, unsigned h) {
    od_abort(s);
    s->compressed = compressed;
    s->partial = partial;
    s->failed = false;
    s->total = partial ? w / 8 * h * 2 : EPD_FRAME_BYTES;
    s->new_etag_valid = false;
    if (compressed)
        od_inflate_init(&s->inflate, s->total);
    if (partial ? epd_begin_partial(x, y, w, h) : epd_begin()) {
        epd_off();
        return -1;
    }
    s->active = true;
    return 0;
}
static unsigned geometry(struct od_session *s, uint32_t old, unsigned x, unsigned y, unsigned w,
                         unsigned h) {
    if (!s->etag_valid || old != s->etag) {
        s->etag_valid = false;
        return 1;
    }
    if (!w || !h || x >= 104 || y >= 212 || w > 104 - x || h > 212 - y) {
        s->etag_valid = false;
        return 3;
    }
    if ((x & 7) || (w & 7)) {
        s->etag_valid = false;
        return 4;
    }
    return 0;
}
static int sack(struct od_session *s, od_send_fn send, void *arg, unsigned error) {
    uint8_t r[8] = {error ? 0xff : 0, 0x81};
    unsigned off = 2;
    if (error)
        r[off++] = error;
    r[off++] = s->highest;
    put32(r + off, s->mask);
    s->since_ack = 0;
    return send(r, off + 4, arg);
}
static void pipe_data(struct od_session *s, const uint8_t *b, size_t n, od_send_fn send,
                      void *arg) {
    if (s->failed)
        return;
    unsigned error = 4;
    if (!s->active || !s->pipe || n < 4 || n > s->frame)
        goto fail;
    uint8_t seq = b[2];
    int delta = (int8_t)(seq - s->next);
    if (delta < 0) {
        /* Only accept retransmission of a recently consumed chunk. */
        if (delta < -32 || !s->seen)
            goto fail;
        if (sack(s, send, arg, 0))
            od_abort(s);
        return;
    }
    if (delta >= s->window)
        goto fail;
    /* W=2 needs only one future-packet slot; consume the expected packet
     * directly from the command buffer, then drain the pending successor. */
    if (delta == 0) {
        if (stream(s, b + 3, n - 3)) {
            error = s->compressed ? 2 : 3;
            goto fail;
        }
        ++s->next;
    } else if (s->reorder[0].used) {
        if (s->reorder[0].seq != seq || s->reorder[0].len != n - 3 ||
            memcmp(s->reorder[0].data, b + 3, n - 3))
            goto fail;
    } else {
        memcpy(s->reorder[0].data, b + 3, n - 3);
        s->reorder[0].len = n - 3;
        s->reorder[0].seq = seq;
        s->reorder[0].used = true;
    }
    if (!s->seen) {
        s->highest = seq;
        s->mask = 0;
        s->seen = true;
    } else {
        int d = (int8_t)(seq - s->highest);
        if (d > 0) {
            s->mask = d >= 32 ? 0 : s->mask << d;
            if (d <= 32)
                s->mask |= 1u << (d - 1);
            s->highest = seq;
        } else if (d < 0 && d >= -32)
            s->mask |= 1u << (-d - 1);
    }
    while (s->reorder[0].used && s->reorder[0].seq == s->next) {
        unsigned i = 0;
        if (stream(s, s->reorder[i].data, s->reorder[i].len)) {
            error = s->compressed ? 2 : 3;
            goto fail;
        }
        s->reorder[i].used = false;
        ++s->next;
    }
    if (++s->since_ack >= s->ack_every || s->written == s->total)
        if (sack(s, send, arg, 0))
            od_abort(s);
    return;
fail:
    sack(s, send, arg, error);
    od_abort(s);
    s->failed = true;
}
void od_command(struct od_session *s, const uint8_t *b, size_t len, const uint8_t msd[16],
                unsigned mtu, od_send_fn send, void *arg) {
    uint8_t r[OD_MAX_RESPONSE] = {0xff, len > 1 ? b[1] : 0};
    size_t n = 2;
    if (len < 2 || len > OD_MAX_COMMAND || b[0])
        goto reply;
    switch (b[1]) {
    case 0x0f:
        if (len != 2)
            break;
        od_abort(s);
        od_store_abort();
        od_reboot();
        return;
    case 0x40: {
        if (len != 2 || mtu < 10)
            break;
        size_t total, offset = 0;
        const uint8_t *data = od_store_data(&total);
        unsigned capacity = mtu - 3 < sizeof r ? mtu - 3 : sizeof r;
        uint16_t chunk = 0;
        do {
            r[0] = 0;
            r[1] = 0x40;
            r[2] = chunk;
            r[3] = chunk >> 8;
            n = 4;
            if (!chunk) {
                r[n++] = total;
                r[n++] = total >> 8;
            }
            size_t count = total - offset;
            if (count > capacity - n)
                count = capacity - n;
            memcpy(r + n, data + offset, count);
            if (send(r, n + count, arg))
                return;
            offset += count;
            ++chunk;
        } while (offset < total);
        return;
    }
    case 0x41: {
        od_abort(s);
        od_store_abort();
        size_t total = len - 2, offset = 2;
        if (len == 204) {
            total = le16(b + 2);
            offset = 4;
            if (total <= 200)
                break;
        } else if (total > 200)
            break;
        if (!od_store_begin(total) && od_store_append(b + offset, len - offset) >= 0)
            r[0] = 0;
        break;
    }
    case 0x42:
        if (len > 2 && len <= 202 && od_store_append(b + 2, len - 2) >= 0)
            r[0] = 0;
        else
            od_store_abort();
        break;
    case 0x45:
        if (len == 2) {
            od_abort(s);
            if (!od_store_clear())
                r[0] = 0;
        }
        break;
    case 0x43:
        if (len != 2)
            break;
        r[0] = 0;
        r[2] = 0;
        r[3] = 2;
        r[4] = 0;
        r[5] = 0;
        n = 6;
        break;
    case 0x44:
        if (len != 2)
            break;
        r[0] = 0;
        memcpy(r + 2, msd, 16);
        n = 18;
        break;
    case 0x51: /* No Nordic/MCUboot bootloader or OTA slot exists. */
        break;
    case 0x52:
    case 0x53:
        r[2] = 0;
        r[3] = 0;
        n = 4;
        break; /* Canonical hardware-unsupported codes. */
    case 0x73:
    case 0x75:
    case 0x77:
        break; /* No LED/buzzer hardware. */
    case 0x83:
        r[2] = 0xff;
        r[3] = 4;
        n = 4;
        if (len < 3)
            r[3] = 1;
        else if (b[2] == 0)
            r[3] = len == 3 ? 2 : 1;
        else if (b[2] == 1 || b[2] == 0x10) {
            if (len < 6 || (b[2] == 0x10 && len != 6))
                r[3] = 1;
            else if (b[3] > 4)
                r[3] = 5;
            else if (!be16(b + 4) || be16(b + 4) > 512)
                r[3] = 6;
            else if (b[2] == 1 && be16(b + 4) != len - 6)
                r[3] = 1;
            else
                r[3] = 3; /* Hardware absent: START is never accepted. */
        } else if (b[2] == 0x11 || b[2] == 0x12) {
            r[3] = ((b[2] == 0x11 && len == 3) || (b[2] == 0x12 && len != 3)) ? 1 : 7;
        }
        break;
    case 0x70:
        od_abort(s);
        s->pipe = false;
        s->failed = false;
        if (len != 2 && (len < 6 || len > 202 || le32(b + 2) != EPD_FRAME_BYTES))
            break;
        if (begin(s, len != 2, false, 0, 0, 104, 212))
            break;
        if (len > 6 && stream(s, b + 6, len - 6)) {
            od_abort(s);
            break;
        }
        r[0] = 0;
        break;
    case 0x76: {
        s->pipe = false;
        od_abort(s);
        unsigned err = 6;
        if (len < 19 || len > 202)
            goto partial_error;
        if (b[2] & ~1u) {
            err = 5;
            goto partial_error;
        }
        unsigned x = be16(b + 11), y = be16(b + 13), w = be16(b + 15), h = be16(b + 17);
        err = geometry(s, be32(b + 3), x, y, w, h);
        if (err)
            goto partial_error;
        if (begin(s, b[2] & 1, true, x, y, w, h)) {
            err = 6;
            goto partial_error;
        }
        s->new_etag = be32(b + 7);
        s->new_etag_valid = true;
        if (stream(s, b + 19, len - 19)) {
            od_abort(s);
            err = 6;
            goto partial_error;
        }
        r[0] = 0;
        break;
    partial_error:
        r[2] = err;
        r[3] = 0;
        n = 4;
        break;
    }
    case 0x80: {
        od_abort(s);
        s->pipe = true;
        s->failed = false;
        s->seen = false;
        s->next = s->highest = s->since_ack = 0;
        s->mask = 0;
        unsigned err = 1, x = 0, y = 0, w = 104, h = 212;
        if ((len != 12 && len != 24) || b[2] != 1 || !b[4] || b[4] > 32 || !b[5] || b[5] > 32 ||
            le16(b + 6) < 4)
            goto pipe_error;
        if (b[3] & ~3u) {
            err = 2;
            goto pipe_error;
        }
        bool partial = b[3] & 2;
        if (len != (partial ? 24u : 12u))
            goto pipe_error;
        if (partial) {
            x = le16(b + 16);
            y = le16(b + 18);
            w = le16(b + 20);
            h = le16(b + 22);
            unsigned g = geometry(s, le32(b + 12), x, y, w, h);
            if (g) {
                err = g == 1 ? 5 : 7;
                goto pipe_error;
            }
        }
        if (le32(b + 8) != (partial ? w / 8 * h * 2 : EPD_FRAME_BYTES)) {
            err = 3;
            goto pipe_error;
        }
        if (mtu < 10)
            goto pipe_error;
        s->frame = le16(b + 6);
        if (s->frame > 244)
            s->frame = 244;
        if (s->frame > mtu - 3)
            s->frame = mtu - 3;
        s->window = b[4] > 2 ? 2 : b[4];
        s->ack_every = b[5] > s->window ? s->window : b[5];
        if (begin(s, b[3] & 1, partial, x, y, w, h)) {
            err = 3;
            goto pipe_error;
        }
        r[0] = 0;
        r[2] = 1;
        r[3] = s->window;
        r[4] = s->ack_every;
        r[5] = s->frame;
        r[6] = s->frame >> 8;
        r[7] = partial ? 3 : 1;
        n = 8;
        break;
    pipe_error:
        r[2] = err;
        r[3] = 0;
        n = 4;
        s->failed = true;
        break;
    }
    case 0x81:
        pipe_data(s, b, len, send, arg);
        return;
    case 0x71:
        if (!s->active || s->pipe)
            break;
        if (len > 232 || stream(s, b + 2, len - 2)) {
            if (s->partial) {
                r[2] = 6;
                r[3] = 0;
                n = 4;
            }
            od_abort(s);
            break;
        }
        r[0] = 0;
        break;
    case 0x72:
    case 0x82: {
        bool end_pipe = b[1] == 0x82;
        if (end_pipe && s->active && s->pipe && s->seen)
            if (sack(s, send, arg, 0)) {
                od_abort(s);
                return;
            }
        bool valid_length = len == 3 || len == 7 || (end_pipe && s->partial && len == 2);
        if (!s->active || end_pipe != s->pipe || !valid_length ||
            (len > 2 && b[2] > (s->partial ? 2 : 1)) || s->written != s->total ||
            (s->compressed && !od_inflate_complete(&s->inflate)) || s->reorder[0].used) {
            od_abort(s);
            break;
        }
        if (len == 7) {
            s->new_etag = be32(b + 3);
            s->new_etag_valid = true;
        }
        r[0] = 0;
        if (send(r, 2, arg)) {
            od_abort(s);
            return;
        }
        r[1] = epd_refresh() ? 0x74 : 0x73;
        epd_off();
        s->active = false;
        s->written = 0;
        s->etag_valid = r[1] == 0x73 && s->new_etag_valid;
        if (s->etag_valid)
            s->etag = s->new_etag;
        send(r, 2, arg);
        return;
    }
    default:
        break;
    }
reply:
    if (send(r, n, arg))
        od_abort(s);
}

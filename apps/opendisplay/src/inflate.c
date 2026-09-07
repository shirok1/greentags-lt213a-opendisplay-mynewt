#include "inflate.h"
#include "uzlib.h"

void od_inflate_init(struct od_inflate *z, uint32_t limit) {
    z->done = z->failed = 0;
    od_zlib_stream_reset(limit);
}

int od_inflate_feed(struct od_inflate *z, const uint8_t *p, size_t n,
                    int (*emit)(uint8_t, void *), void *arg) {
    if (z->failed)
        return -1;
    if (od_zlib_stream_push(p, n, false) == OD_ZLIB_STATUS_ERROR)
        goto fail;
    /* Drain before returning: the vendor retains the caller's input pointer.
     * A small stack buffer avoids both heap allocation and a full framebuffer. */
    for (;;) {
        uint8_t output[16];
        size_t produced;
        od_zlib_status_t rc = od_zlib_stream_poll(output, sizeof output, &produced);
        if (rc == OD_ZLIB_STATUS_ERROR)
            goto fail;
        for (size_t i = 0; i < produced; ++i)
            if (emit(output[i], arg))
                goto fail;
        if (rc == OD_ZLIB_STATUS_DONE) {
            z->done = 1;
            return 0;
        }
        if (rc == OD_ZLIB_STATUS_NEEDS_INPUT)
            return 0;
    }
fail:
    z->failed = 1;
    z->done = 0;
    return -1;
}

int od_inflate_complete(const struct od_inflate *z) {
    return z->done && !z->failed;
}

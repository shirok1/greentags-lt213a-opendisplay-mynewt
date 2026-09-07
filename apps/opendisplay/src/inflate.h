#ifndef OD_INFLATE_H
#define OD_INFLATE_H
#include <stddef.h>
#include <stdint.h>
/* Streaming zlib decoder, RFC 1950/1951; the advertised window is 512 bytes. */
/* The vendor decoder is a singleton, owned exclusively by the display worker. */
struct od_inflate {
    uint8_t done, failed;
};
void od_inflate_init(struct od_inflate *z, uint32_t limit);
int od_inflate_feed(struct od_inflate *z, const uint8_t *p, size_t n, int (*emit)(uint8_t, void *),
                    void *arg);
int od_inflate_complete(const struct od_inflate *z);
#endif

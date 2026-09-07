#include "inflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct od_inflate z;
static unsigned char expected[6000];
static size_t offset, length;
static int emit(uint8_t b, void *arg) { return offset >= length || expected[offset++] != b; }
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    FILE *f = fopen(argv[1], "rb"), *r = fopen(argv[2], "rb");
    if (!f || !r)
        return 2;
    length = fread(expected, 1, sizeof expected, r);
    fclose(r);
    od_inflate_init(&z, length);
    unsigned chunk = atoi(argv[3]);
    unsigned char input[244];
    size_t n;
    if (!chunk || chunk > 244)
        return 2;
    while ((n = fread(input, 1, chunk, f)))
        if (od_inflate_feed(&z, input, n, emit, NULL)) {
            fclose(f);
            return 1;
        }
    fclose(f);
    return !(od_inflate_complete(&z) && offset == length);
}

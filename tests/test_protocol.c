/* Run the real protocol/storage/security handlers under ASan and UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
extern void test_reset(void);
extern unsigned test_command(const uint8_t *, unsigned, unsigned);
int main(void) {
    uint32_t rng = 0x83f910ad;
    uint8_t packet[244];
    test_reset();
    for (unsigned op = 0; op < 256; op++) {
        for (unsigned n = 0; n <= 244; n++) {
            for (unsigned j = 0; j < n; j++) {
                rng = rng * 1664525 + 1013904223;
                packet[j] = rng >> 24;
            }
            packet[0] = 0;
            packet[1] = op;
            assert(test_command(packet, n, 23 + (rng & 1) * 224) < 300);
            if ((n & 31) == 0)
                test_reset();
        }
    }
    puts("sanitizers: 62,720 opcode/length mutations passed");
}

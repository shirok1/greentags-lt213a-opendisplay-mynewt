/* GDEW0213T5 sequence based on the supplied Good Display Arduino example.
 * 4-wire, MSB-first SPI; BS=0. No framebuffer or bidirectional reads needed.
 * These functions run only in the display task (never the BLE event task).
 */
#include "epd.h"
#include "hal/hal_gpio.h"
#include "os/os.h"
#include "syscfg/syscfg.h"
#define MOSI MYNEWT_VAL(EPD_MOSI)
#define SCK MYNEWT_VAL(EPD_SCK)
#define CS MYNEWT_VAL(EPD_CS)
#define DC MYNEWT_VAL(EPD_DC)
#define RESET MYNEWT_VAL(EPD_RESET)
#define BUSY MYNEWT_VAL(EPD_BUSY)
#define BS MYNEWT_VAL(EPD_BS)

static unsigned partial_size, partial_offset;
/* Rail remains connected on LT213A; only a successful sleep command is cached. */
static int asleep;
static void delay_ms(unsigned ms) { os_time_delay(os_time_ms_to_ticks32(ms) + 1); }
static void byte(int data, uint8_t value) {
    hal_gpio_write(DC, data);
    hal_gpio_write(CS, 0);
    for (unsigned bit = 0; bit < 8; ++bit) {
        hal_gpio_write(SCK, 0);
        hal_gpio_write(MOSI, !!(value & 0x80));
        value <<= 1;
        hal_gpio_write(SCK, 1);
    }
    hal_gpio_write(CS, 1);
    hal_gpio_write(SCK, 0);
}
static void cmd(uint8_t value) { byte(0, value); }
static void data(uint8_t value) { byte(1, value); }
static int ready(void) {
    os_time_t start = os_time_get();
    do {
        cmd(0x71);
        if (hal_gpio_read(BUSY)) {
            delay_ms(200);
            return 0;
        }
        delay_ms(10);
    } while ((os_time_t)(os_time_get() - start) < 15 * OS_TICKS_PER_SEC);
    return -1;
}
void epd_gpio_init(void) {
    asleep = 0;
    hal_gpio_init_out(CS, 1);
    hal_gpio_init_out(SCK, 0);
    hal_gpio_init_out(MOSI, 0);
    hal_gpio_init_out(DC, 0);
    hal_gpio_init_out(RESET, 1);
    hal_gpio_init_out(BS, 0);
    hal_gpio_init_in(BUSY, HAL_GPIO_PULL_NONE);
}
int epd_begin(void) {
    partial_size = partial_offset = 0;
    asleep = 0;
    hal_gpio_write(RESET, 0);
    delay_ms(10);
    hal_gpio_write(RESET, 1);
    delay_ms(10);
    cmd(0x06);
    data(0x17);
    data(0x17);
    data(0x17);
    cmd(0x04);
    if (ready())
        return -1;
    cmd(0x00);
    data(0x1f);
    data(0x0d);
    cmd(0x61);
    data(104);
    data(0);
    data(212);
    cmd(0x50);
    data(0x97);
    cmd(0x10);
    for (unsigned i = 0; i < EPD_FRAME_BYTES; ++i) {
        data(0xff);
        /* Let host events run during the initial old-image fill. */
        if ((i & 127) == 127)
            os_time_delay(1);
    }
    cmd(0x13);
    return 0;
}
int epd_write(const uint8_t *buf, size_t len) {
    while (len--) {
        if (partial_size && partial_offset == partial_size)
            cmd(0x13);
        data(partial_size ? (uint8_t)~*buf++ : *buf++);
        ++partial_offset;
    }
    return 0;
}
int epd_refresh(void) {
    cmd(0x12);
    /* Datasheet requires 200 us before polling; the supplied Good Display
     * examples wait 100 ms here before checking BUSY. Match the examples. */
    delay_ms(100);
    return ready();
}
int epd_off(void) {
    if (asleep)
        return 0;
    cmd(0x50);
    data(0xf7);
    cmd(0x02);
    int rc = ready();
    if (!rc) {
        cmd(0x07);
        data(0xa5);
        asleep = 1;
        hal_gpio_write(MOSI, 0);
        hal_gpio_write(DC, 0);
    }
    return rc;
}

/* Sequence and RAM polarity from the supplied GDEW0213T5_Arduino_P20201021
 * partial example. Correct its square-area assumption and y-end arithmetic. */
int epd_begin_partial(unsigned x, unsigned y, unsigned width, unsigned height) {
    if (!width || !height || (x & 7) || (width & 7) || x + width > 104 || y + height > 212)
        return -1;
    asleep = 0;
    hal_gpio_write(RESET, 0);
    delay_ms(10);
    hal_gpio_write(RESET, 1);
    delay_ms(10);
    cmd(0x01);
    data(3);
    data(2);
    data(0x21);
    data(0x21);
    cmd(0x06);
    data(0x17);
    data(0x17);
    data(0x17);
    cmd(0x04);
    if (ready())
        return -1;
    cmd(0x00);
    data(0xbf);
    data(0x0d);
    cmd(0x30);
    data(0x3c);
    cmd(0x61);
    data(104);
    data(0);
    data(212);
    cmd(0x82);
    data(8);
    cmd(0x50);
    data(0x47);
    for (unsigned table = 0; table < 5; table++) {
        cmd(0x20 + table);
        for (unsigned i = 0; i < (table ? 42u : 44u); i++)
            data(i == 0             ? (table == 2   ? 0x20
                                       : table == 3 ? 0x10
                                                    : 0)
                 : i == 1 || i == 5 ? 1
                 : i == 2           ? 14
                                    : 0);
    }
    cmd(0x91);
    cmd(0x90);
    data(x);
    data(x + width - 1);
    data(y >> 8);
    data(y);
    data((y + height - 1) >> 8);
    data(y + height - 1);
    data(0x28);
    cmd(0x10);
    partial_size = width / 8 * height;
    partial_offset = 0;
    return 0;
}

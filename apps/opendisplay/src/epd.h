#ifndef OD_EPD_H
#define OD_EPD_H
#include <stddef.h>
#include <stdint.h>
#define EPD_WIDTH 104u
#define EPD_HEIGHT 212u
#define EPD_FRAME_BYTES (EPD_WIDTH / 8u * EPD_HEIGHT)
void epd_gpio_init(void);
int epd_begin(void);
int epd_begin_partial(unsigned x, unsigned y, unsigned width, unsigned height);
int epd_write(const uint8_t *data, size_t len);
int epd_refresh(void);
int epd_off(void);
#endif

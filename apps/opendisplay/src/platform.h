#ifndef OD_PLATFORM_H
#define OD_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
uint32_t od_millis(void);
int od_random(void *data, size_t len);
void od_device_id(uint8_t id[4]);
void od_reboot(void);
int od_read_msd(uint8_t data[16]);
#endif

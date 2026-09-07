#ifndef OD_STORAGE_H
#define OD_STORAGE_H
#include <stddef.h>
#include <stdint.h>
#define OD_CONFIG_MAX 4096u
#define OD_SLOT_BYTES 5120u
#define OD_SLOT0 0x1d800u
#define OD_SLOT1 (OD_SLOT0 + OD_SLOT_BYTES)
const uint8_t *od_flash_ptr(uint32_t address);
int od_flash_erase(uint32_t address);
int od_flash_write(uint32_t address, const void *data, size_t len);
void od_store_init(void);
const uint8_t *od_store_data(size_t *len);
const uint8_t *od_store_record(uint8_t type);
int od_store_begin(size_t total);
int od_store_append(const uint8_t *data, size_t len);
int od_store_clear(void);
void od_store_abort(void);
int od_config_validate(const uint8_t *data, size_t len);
#endif

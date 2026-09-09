#include "syscfg/syscfg.h"
#include "platform.h"
#include "hal/hal_flash.h"
#include "hal/hal_system.h"
#include "host/ble_hs_hci.h"
#include "nrfx.h"
#include "os/os.h"
#include "storage.h"
const uint8_t *od_flash_ptr(uint32_t address) { return (const uint8_t *)address; }
int od_flash_erase(uint32_t a) {
    if (a < OD_SLOT0 || a >= 0x20000 || (a & 1023))
        return -1;
    int rc = hal_flash_erase(0, a, 1024);
    os_time_delay(1);
    return rc;
}
int od_flash_write(uint32_t a, const void *p, size_t n) {
    if (a < OD_SLOT0 || a > 0x20000 || n > 0x20000 - a)
        return -1;
    return hal_flash_write(0, a, p, n);
}
uint32_t od_millis(void) { return (uint64_t)os_time_get() * 1000 / OS_TICKS_PER_SEC; }
int od_random(void *p, size_t n) { return ble_hs_hci_rand(p, n); }
void od_device_id(uint8_t id[4]) {
    uint32_t v = NRF_FICR->DEVICEID[0];
    for (unsigned i = 0; i < 4; i++)
        id[i] = v >> (24 - 8 * i);
}
void od_reboot(void) { hal_system_reset(); }

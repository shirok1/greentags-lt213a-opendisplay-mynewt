#include "syscfg/syscfg.h"
#include "platform.h"
#include "hal/hal_flash.h"
#include "hal/hal_system.h"
#include "host/ble_hs_hci.h"
#include "nrfx.h"
#include "os/os.h"
#include "storage.h"
#include <string.h>
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
int od_read_msd(uint8_t p[16]) {
    int rc = 0;
    memset(p, 0, 16);
    p[0] = 0x46;
    p[1] = 0x24;
    /* Internal temperature and supply ADC: bounded peripheral polling. */
    NRF_TEMP->EVENTS_DATARDY = 0;
    NRF_TEMP->TASKS_START = 1;
    uint32_t start = od_millis();
    while (!NRF_TEMP->EVENTS_DATARDY && (uint32_t)(od_millis() - start) < 50)
        os_time_delay(1);
    if (NRF_TEMP->EVENTS_DATARDY) {
        int t = (int32_t)NRF_TEMP->TEMP / 2 + 80;
        p[13] = t < 0 ? 0 : t > 255 ? 255 : t;
    }
    if (!NRF_TEMP->EVENTS_DATARDY)
        rc = -1;
    NRF_TEMP->TASKS_STOP = 1;
    NRF_ADC->ENABLE = 1;
    /* 10-bit, VDD/3 input, 1.2V bandgap reference: full scale 3.6V. */
    NRF_ADC->CONFIG = (ADC_CONFIG_RES_10bit << ADC_CONFIG_RES_Pos) |
                      (ADC_CONFIG_INPSEL_SupplyOneThirdPrescaling << ADC_CONFIG_INPSEL_Pos);
    NRF_ADC->EVENTS_END = 0;
    NRF_ADC->TASKS_START = 1;
    start = od_millis();
    while (!NRF_ADC->EVENTS_END && (uint32_t)(od_millis() - start) < 50)
        os_time_delay(1);
    if (NRF_ADC->EVENTS_END) {
        unsigned v = (NRF_ADC->RESULT * 360u + 511) / 1023;
        p[14] = v;
        p[15] = (v >> 8) & 1;
    }
    if (!NRF_ADC->EVENTS_END)
        rc = -1;
    NRF_ADC->TASKS_STOP = 1;
    NRF_ADC->ENABLE = 0;
    return rc;
}

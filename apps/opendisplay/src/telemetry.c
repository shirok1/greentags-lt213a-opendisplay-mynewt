#include "platform.h"
#include "nrfx.h"
#include "os/os.h"
#include <string.h>

/* Worker-owned peripherals; VDD is the battery proxy on this CR2450 board. */
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

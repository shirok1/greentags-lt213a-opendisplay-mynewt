#include "platform.h"
#include "nrfx.h"
#include "os/os.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct test_temp test_temp;
struct test_adc test_adc;
static uint32_t now, adc_value;
static int temp_timeout, adc_timeout;
uint32_t od_millis(void) { return now; }
void os_time_delay(os_time_t ticks) {
    now += (ticks * 1000 + 127) / 128;
    if (test_temp.TASKS_START && !temp_timeout) {
        test_temp.EVENTS_DATARDY = 1;
        test_temp.TEMP = 100; /* 25 degrees C in quarter degrees. */
    }
    if (test_adc.TASKS_START && !adc_timeout) {
        assert(test_adc.ENABLE == 1);
        assert(test_adc.CONFIG ==
               ((ADC_CONFIG_RES_10bit << ADC_CONFIG_RES_Pos) |
                (ADC_CONFIG_INPSEL_SupplyOneThirdPrescaling << ADC_CONFIG_INPSEL_Pos)));
        test_adc.EVENTS_END = 1;
        test_adc.RESULT = adc_value;
    }
}
static int sample(uint8_t msd[16]) {
    memset(&test_temp, 0, sizeof test_temp);
    memset(&test_adc, 0, sizeof test_adc);
    memset(msd, 0xa5, 16);
    int rc = od_read_msd(msd);
    assert(test_adc.ENABLE == 0 && test_adc.TASKS_STOP == 1);
    assert(test_temp.TASKS_STOP == 1);
    return rc;
}
int main(void) {
    uint8_t msd[16];
    unsigned previous = 0;
    for (adc_value = 0; adc_value <= 1023; ++adc_value) {
        assert(sample(msd) == 0);
        unsigned voltage = msd[14] | ((msd[15] & 1) << 8);
        assert(voltage >= previous && voltage <= 360);
        assert(msd[0] == 0x46 && msd[1] == 0x24 && msd[13] == 130);
        for (unsigned i = 2; i < 13; ++i)
            assert(msd[i] == 0);
        assert((msd[15] & ~1u) == 0);
        previous = voltage;
    }
    assert(previous == 360);
    adc_value = 853; /* 3.00 V: 300 units, requires status bit 0. */
    assert(sample(msd) == 0 && msd[14] == 0x2c && msd[15] == 1);
    adc_value = 568; /* 2.00 V: low byte only. */
    assert(sample(msd) == 0 && msd[14] == 200 && msd[15] == 0);
    for (unsigned i = 1; i <= 3; ++i) {
        temp_timeout = i & 1;
        adc_timeout = i & 2;
        now = UINT32_MAX - 20;
        uint32_t start = now;
        assert(sample(msd) == -1);
        assert((uint32_t)(now - start) >= 50 && (uint32_t)(now - start) < 130);
    }
    temp_timeout = adc_timeout = 0;
    assert(sample(msd) == 0);
    puts("telemetry: ADC range, battery encoding, timeouts, wrap and peripheral shutdown passed");
}

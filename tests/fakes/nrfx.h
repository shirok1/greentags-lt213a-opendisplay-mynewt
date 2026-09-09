#ifndef TEST_NRFX_H
#define TEST_NRFX_H
#include <stdint.h>
#include "nrf51_bitfields.h"
/* Only the registers used by telemetry; field values use Nordic's definitions. */
struct test_temp {
    volatile uint32_t EVENTS_DATARDY, TASKS_START, TASKS_STOP;
    volatile int32_t TEMP;
};
struct test_adc {
    volatile uint32_t ENABLE, CONFIG, EVENTS_END, TASKS_START, TASKS_STOP, RESULT;
};
extern struct test_temp test_temp;
extern struct test_adc test_adc;
#define NRF_TEMP (&test_temp)
#define NRF_ADC (&test_adc)
#endif

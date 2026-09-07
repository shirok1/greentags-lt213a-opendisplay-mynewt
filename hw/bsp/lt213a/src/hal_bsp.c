#include <assert.h>
#include "os/mynewt.h"
#include "mynewt_cm.h"
#include "mcu/nrf51_hal.h"
#include "mcu/nrf51_clock.h"
#include "nrfx.h"
#include "hal/hal_bsp.h"
#include "hal/hal_system.h"
#include "hal/hal_flash.h"
#include "bsp/bsp.h"
/* Shared HFXO reference counting prevents calibration from stopping the
 * crystal while NimBLE is using the radio. CTIV=8 calibrates every 2 seconds.
 * CAL and CTSTART are separated by calibration completion (> one LFCLK tick). */
static volatile uint8_t calibration_waiting;
static void calibration_start(void) {
    NRF_CLOCK->EVENTS_DONE = 0;
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    calibration_waiting = 1;
    nrf51_clock_hfxo_request();
    NRF_CLOCK->INTENSET = CLOCK_INTENSET_HFCLKSTARTED_Msk;
    if ((NRF_CLOCK->HFCLKSTAT & (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk)) ==
        (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Xtal)) {
        calibration_waiting = 0;
        NRF_CLOCK->INTENCLR = CLOCK_INTENCLR_HFCLKSTARTED_Msk;
        NRF_CLOCK->TASKS_CAL = 1;
    }
}
static void calibration_irq(void) {
    if (calibration_waiting && NRF_CLOCK->EVENTS_HFCLKSTARTED) {
        NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
        NRF_CLOCK->INTENCLR = CLOCK_INTENCLR_HFCLKSTARTED_Msk;
        calibration_waiting = 0;
        NRF_CLOCK->TASKS_CAL = 1;
    }
    if (NRF_CLOCK->EVENTS_DONE) {
        NRF_CLOCK->EVENTS_DONE = 0;
        nrf51_clock_hfxo_release();
        NRF_CLOCK->TASKS_CTSTART = 1;
    }
    if (NRF_CLOCK->EVENTS_CTTO) {
        NRF_CLOCK->EVENTS_CTTO = 0;
        calibration_start();
    }
}
static void calibration_init(void) {
    /* Finish first calibration before the scheduler / BLE starts. */
    nrf51_clock_hfxo_request();
    while ((NRF_CLOCK->HFCLKSTAT & (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk)) !=
           (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Xtal)) {}
    NRF_CLOCK->EVENTS_DONE = 0;
    NRF_CLOCK->TASKS_CAL = 1;
    while (!NRF_CLOCK->EVENTS_DONE) {}
    NRF_CLOCK->EVENTS_DONE = 0;
    nrf51_clock_hfxo_release();
    NRF_CLOCK->CTIV = 8;
    NRF_CLOCK->EVENTS_CTTO = 0;
    NVIC_SetVector(POWER_CLOCK_IRQn, (uint32_t)calibration_irq);
    NVIC_SetPriority(POWER_CLOCK_IRQn, (1 << __NVIC_PRIO_BITS) - 1);
    NRF_CLOCK->INTENSET = CLOCK_INTENSET_DONE_Msk | CLOCK_INTENSET_CTTO_Msk;
    NVIC_ClearPendingIRQ(POWER_CLOCK_IRQn);
    NVIC_EnableIRQ(POWER_CLOCK_IRQn);
    NRF_CLOCK->TASKS_CTSTART = 1;
}
static const struct hal_bsp_mem_dump dump = { &_ram_start, RAM_SIZE };
const struct hal_flash *hal_bsp_flash_dev(uint8_t id) { return id == 0 ? &nrf_flash_dev : NULL; }
const struct hal_bsp_mem_dump *hal_bsp_core_dump(int *n) { *n = 1; return &dump; }
int hal_bsp_power_state(int state) { return 0; }
uint32_t hal_bsp_get_nvic_priority(int irq, uint32_t pri) { return irq == RADIO_IRQn ? 0 : pri; }
void hal_bsp_init(void) {
    int rc;
    hal_system_clock_start();
    calibration_init();
    rc = hal_timer_init(3, NULL); assert(rc == 0);
    rc = os_cputime_init(MYNEWT_VAL(OS_CPUTIME_FREQ)); assert(rc == 0);
}
void hal_bsp_deinit(void) { Cortex_DisableAll(); }

"""Board-specific patches applied to downloaded Mynewt dependencies."""
from pathlib import Path

# Allow calibrated LFRC only on the BSP that provides its calibration driver.
p = Path('repos/apache-mynewt-core/hw/mcu/nordic/nrf51xxx/syscfg.yml')
s = p.read_text()
old = "'!BLE_CONTROLLER || (MCU_LFCLK_SOURCE != \"LFRC\")'"
new = "'!BLE_CONTROLLER || (MCU_LFCLK_SOURCE != \"LFRC\") || LT213A_LFRC_CALIBRATED'"
if new not in s:
    if s.count(old) != 1:
        raise SystemExit('Unexpected Mynewt LFRC restriction; review patch before building')
    p.write_text(s.replace(old, new))

# nRF51 hal_os_tick: os_tick_idle() invokes rtc1_timer_handler() after any WFI
# wake, not only on a compare interrupt. Woken by another pending IRQ (e.g. the
# BLE LL RTC0 compare) before the RTC1 compare fires, sub24() yields a negative
# delta; dividing int by uint32_t wraps ticks to ~2^24, so os_time jumps forward
# by hours, every callout fires at once, and the link layer scheduling collapses
# (advertising goes silent for minutes). Skip processing until the compare that
# is still armed actually fires.
p = Path('repos/apache-mynewt-core/hw/mcu/nordic/nrf51xxx/src/hal_os_tick.c')
s = p.read_text()
old = """    counter = nrf51_os_tick_counter();
    delta = sub24(counter, lastocmp);
    ticks = delta / timer_ticks_per_ostick;
    os_time_advance(ticks);"""
new = """    counter = nrf51_os_tick_counter();
    delta = sub24(counter, lastocmp);
    if (delta < 0) {
        /* Woken before the compare fired; it is still armed. */
        OS_EXIT_CRITICAL(sr);
        return;
    }
    ticks = delta / timer_ticks_per_ostick;
    os_time_advance(ticks);"""
if new not in s:
    if s.count(old) != 1:
        raise SystemExit('Unexpected Mynewt hal_os_tick.c; review patch before building')
    p.write_text(s.replace(old, new))

"""Allow calibrated LFRC only on the BSP that provides its calibration driver."""
from pathlib import Path
p = Path('repos/apache-mynewt-core/hw/mcu/nordic/nrf51xxx/syscfg.yml')
s = p.read_text()
old = "'!BLE_CONTROLLER || (MCU_LFCLK_SOURCE != \"LFRC\")'"
new = "'!BLE_CONTROLLER || (MCU_LFCLK_SOURCE != \"LFRC\") || LT213A_LFRC_CALIBRATED'"
if new not in s:
    if s.count(old) != 1:
        raise SystemExit('Unexpected Mynewt LFRC restriction; review patch before building')
    p.write_text(s.replace(old, new))

#pragma once

// Powers the peripheral rails through the AXP2101 PMIC. Must run after
// board_i2c_init(); the display, touch panel, IMU, RTC and codecs are all dark
// until this has enabled their rails.
void board_power_init(void);

// Logs the AXP2101 rail, charger and ADC state: register 0x90, registers 0x00
// and 0x01, battery and VSYS voltages in mV and the die temperature. Intended
// for the periodic heartbeat, so it never aborts on a failed I2C read.
void board_power_log_status(void);

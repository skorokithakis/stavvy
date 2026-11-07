#pragma once

// Powers the peripheral rails through the AXP2101 PMIC. Must run after
// board_i2c_init(); the display, touch panel, IMU, RTC and codecs are all dark
// until this has enabled their rails.
void board_power_init(void);

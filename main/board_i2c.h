#pragma once

#include "driver/i2c_master.h"

// Every I2C peripheral on the board (power management, touch, IMU, RTC and both
// audio codecs) is wired to the same pair of pins, so one bus is created at
// boot and shared by all board modules.
void board_i2c_init(void);

i2c_master_bus_handle_t board_i2c_bus(void);

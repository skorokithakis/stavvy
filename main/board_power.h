#pragma once

#include <stdint.h>

#include "esp_err.h"

// Powers the peripheral rails through the AXP2101 PMIC. Must run after
// board_i2c_init(); the display, touch panel, IMU, RTC and codecs are all dark
// until this has enabled their rails.
void board_power_init(void);

// One AXP2101 reading, shared by the heartbeat's log line and the metrics push.
typedef struct {
    uint8_t rail_enable;
    uint8_t status1;
    uint8_t status2;
    // 1 mV per count.
    uint16_t battery_millivolts;
    uint16_t system_millivolts;
    // Raw ADC counts and the value in tenths of a degree, following the
    // AXP2101 convention of 22 C at a raw value of 7274.
    uint16_t temperature_raw;
    int temperature_tenths;
} board_power_status;

// Reads every value the heartbeat reports. Never aborts on a bus error: it
// returns the failing I2C error, so a transient fault only costs one log line
// or one metrics snapshot.
esp_err_t board_power_read_status(board_power_status *status);

// Logs a reading taken by board_power_read_status(). Pass NULL when that read
// failed; it then logs the warning and nothing else. Intended for the periodic
// heartbeat, which takes one reading and passes it here and to the metrics
// push.
void board_power_log_status(const board_power_status *status);

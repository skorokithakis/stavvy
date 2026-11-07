#pragma once

// Brings up the CST9220 touch controller's reset and interrupt lines, checks
// that it answers over I2C, and starts the task that turns a touch into a
// display recovery request. Call after board_power_init() so the touch rail is
// powered, and after display_init() so a recovery has a panel to reset.
void touch_init(void);

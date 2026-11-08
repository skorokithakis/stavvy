#pragma once

#include <stdbool.h>

// Reports whether the board is moving, so the display can react to it being
// picked up and put down. Call after board_i2c_init() and board_power_init();
// the IMU runs from the 3.3 V rail the PMIC setup leaves on.
//
// The callback runs in the motion task once per transition and must be short:
// it must not block, and a slow callback delays the next sample.
typedef void (*motion_callback_t)(bool moving);

// Brings up the QMI8658, checks that it answers, and starts the task that polls
// it. If the IMU does not answer or reports an unexpected identity, the fault
// is logged and no task is started, so the callback never fires; the rest of
// the firmware keeps running.
void motion_init(motion_callback_t callback);

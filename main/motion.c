#include "motion.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_i2c.h"

static const char *TAG = "motion";

// The schematic ties SDO/SA0 to ground, which selects the QMI8658's alternate
// address 0x6B; 0x6A is the address used when the pin floats on its internal
// pull-up. Waveshare calls 0x6B QMI8658_ADDRESS_HIGH, and the schematic labels
// the part with 0x6B. The IMU is on the 3.3 V rail from the AXP2101's DC1,
// which board_power_init() keeps on, so it needs no further rail setup.
#define QMI8658_I2C_ADDRESS 0x6B
#define QMI8658_TRANSFER_TIMEOUT_MS 100

// Register addresses from the QMI8658C datasheet, revision 0.9.
#define QMI8658_REGISTER_WHO_AM_I 0x00
#define QMI8658_REGISTER_CONTROL_1 0x02
#define QMI8658_REGISTER_CONTROL_2 0x03
#define QMI8658_REGISTER_CONTROL_3 0x04
#define QMI8658_REGISTER_CONTROL_5 0x06
#define QMI8658_REGISTER_CONTROL_7 0x08
#define QMI8658_REGISTER_RESET 0x60
#define QMI8658_REGISTER_ACCELERATION_X_LOW 0x35

// The acceleration registers are followed directly by the rotation rate
// registers, so one read starting at 0x35 returns all six axes.
#define QMI8658_DATA_BYTES 12
#define QMI8658_WHO_AM_I_VALUE 0x05
#define QMI8658_RESET_COMMAND 0xB0
#define QMI8658_RESET_SETTLE_MS 15

// CONTROL_1 bit 6 enables register address auto-increment, which the twelve
// byte read relies on. Bit 5 stays clear: setting it makes the part return
// big-endian data. Waveshare's driver writes 0x60 here but then decodes
// little-endian anyway; ESPHome writes 0x40 and decodes little-endian, which is
// what the datasheet describes.
#define QMI8658_CONTROL_1_ADDRESS_AUTO_INCREMENT 0x40

// CONTROL_2 and CONTROL_3 hold the full scale in bits 6 to 4 and the output
// data rate in bits 3 to 0. The ranges are picked to keep normal handling
// inside them: ±4 g and ±512 degrees per second. A harder knock or flick
// saturates, which only makes the sample look more like movement.
#define QMI8658_ACCELEROMETER_RANGE_4G 0x10
#define QMI8658_GYROSCOPE_RANGE_512_DEGREES_PER_SECOND 0x50
// The accelerometer output data rate is ignored while the gyroscope is enabled:
// in that mode both sensors run from the gyroscope's rate (datasheet note 13).
// 235 Hz is the setting closest above the 40 Hz poll, so every read returns a
// fresh sample without having to check the data-ready flag.
#define QMI8658_OUTPUT_DATA_RATE_235_HZ 0x05

// A signed 16-bit sample spans the full scale on each side.
#define QMI8658_ACCELEROMETER_COUNTS_PER_G (32768.0f / 4.0f)
#define QMI8658_GYROSCOPE_COUNTS_PER_DEGREE_PER_SECOND (32768.0f / 512.0f)
#define DEGREES_PER_RADIAN 57.29578f

// One task polls the IMU at 40 Hz, inside the 25 to 50 Hz the module is meant
// to use. The holding times are what separate a real pick-up from a screen tap
// or a knock on the desk: those do not last 200 ms, while a put-down is only
// believed after a second of calm so a pause mid-movement does not count.
#define MOTION_POLL_INTERVAL_MS 25
#define MOTION_MOVING_HOLD_MICROSECONDS 200000
#define MOTION_STILL_HOLD_MICROSECONDS 1000000

// The gyroscope takes 60 ms plus three output periods to start, so samples read
// before that would be noise and could show up as a false movement.
#define MOTION_TURN_ON_DELAY_MS 100

#define MOTION_TASK_STACK_BYTES 3072
#define MOTION_TASK_PRIORITY 3

// The thresholds are first guesses; motion_init()'s task logs the peak of every
// movement so they can be tuned on the bench. Rotation rate is in radians per
// second and acceleration deviation is the difference between the measured
// magnitude and the board's resting magnitude (see
// measure_resting_acceleration_magnitude()). The still thresholds are lower
// than the moving ones so a movement that ends slowly does not oscillate
// between the two states. The gyroscope's initial offset may be up to ±10 dps
// (0.17 rad/s), so the still threshold has to sit above that rather than just
// above this board's measured 0.07 rad/s.
#define MOTION_MOVING_ROTATION_RADIANS_PER_SECOND 0.5f
#define MOTION_MOVING_ACCELERATION_DEVIATION_G 0.15f
#define MOTION_STILL_ROTATION_RADIANS_PER_SECOND 0.25f
#define MOTION_STILL_ACCELERATION_DEVIATION_G 0.06f

// The datasheet allows an accelerometer offset of ±100 mg, and this board sits
// at about 1.1 g when resting, so comparing against 1 g would leave the
// resting deviation above the still threshold and the board would never report
// being put down. Averaging while the board is still at boot gives the
// reference the deviation is measured from; picking the board up during boot
// makes the reference wrong until the next reboot.
#define MOTION_CALIBRATION_SAMPLES 20

static i2c_master_dev_handle_t motion_device;
static motion_callback_t motion_callback;
static float resting_acceleration_magnitude;

static esp_err_t read_registers(uint8_t register_address, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(motion_device, &register_address, sizeof(register_address), data, length,
                                       QMI8658_TRANSFER_TIMEOUT_MS);
}

static esp_err_t write_register(uint8_t register_address, uint8_t value)
{
    const uint8_t transfer[2] = {register_address, value};
    return i2c_master_transmit(motion_device, transfer, sizeof(transfer), QMI8658_TRANSFER_TIMEOUT_MS);
}

static void write_register_checked(uint8_t register_address, uint8_t value)
{
    ESP_ERROR_CHECK(write_register(register_address, value));
}

static int16_t read_signed_sample(const uint8_t *data)
{
    return (int16_t)((data[1] << 8) | data[0]);
}

static float measure_resting_acceleration_magnitude(void)
{
    float total = 0;
    for (int sample = 0; sample < MOTION_CALIBRATION_SAMPLES; sample++) {
        vTaskDelay(pdMS_TO_TICKS(MOTION_POLL_INTERVAL_MS));
        uint8_t data[6] = {0};
        ESP_ERROR_CHECK(read_registers(QMI8658_REGISTER_ACCELERATION_X_LOW, data, sizeof(data)));

        const float x = read_signed_sample(&data[0]) / QMI8658_ACCELEROMETER_COUNTS_PER_G;
        const float y = read_signed_sample(&data[2]) / QMI8658_ACCELEROMETER_COUNTS_PER_G;
        const float z = read_signed_sample(&data[4]) / QMI8658_ACCELEROMETER_COUNTS_PER_G;
        total += sqrtf(x * x + y * y + z * z);
    }
    return total / MOTION_CALIBRATION_SAMPLES;
}

static void motion_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    bool moving = false;
    bool movement_candidate = false;
    bool still_candidate = false;
    int64_t movement_candidate_started_microseconds = 0;
    int64_t still_candidate_started_microseconds = 0;
    int64_t movement_started_microseconds = 0;
    float peak_rotation_rate = 0;
    float peak_acceleration_deviation = 0;
    bool last_read_ok = true;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MOTION_POLL_INTERVAL_MS));
        esp_task_wdt_reset();

        uint8_t data[QMI8658_DATA_BYTES] = {0};
        const esp_err_t read_result = read_registers(QMI8658_REGISTER_ACCELERATION_X_LOW, data, sizeof(data));
        // The bus is shared with the PMIC, touch panel, RTC and codecs, so a
        // failed read is a transient error like a failed touch read, not a
        // reason to reboot. Skipping the sample keeps the state machine honest.
        if (read_result != ESP_OK) {
            if (last_read_ok) {
                ESP_LOGW(TAG, "QMI8658 read failed (0x%x)", read_result);
            }
            last_read_ok = false;
            // The hold times are wall-clock, so a candidate that spans a run of
            // failed reads would count time with no samples behind it and could
            // report a movement or a put-down that was never observed.
            movement_candidate = false;
            still_candidate = false;
            continue;
        }
        if (!last_read_ok) {
            ESP_LOGI(TAG, "QMI8658 is answering again");
        }
        last_read_ok = true;

        const float acceleration_x = read_signed_sample(&data[0]) / QMI8658_ACCELEROMETER_COUNTS_PER_G;
        const float acceleration_y = read_signed_sample(&data[2]) / QMI8658_ACCELEROMETER_COUNTS_PER_G;
        const float acceleration_z = read_signed_sample(&data[4]) / QMI8658_ACCELEROMETER_COUNTS_PER_G;
        const float acceleration_magnitude =
            sqrtf(acceleration_x * acceleration_x + acceleration_y * acceleration_y + acceleration_z * acceleration_z);
        const float acceleration_deviation = fabsf(acceleration_magnitude - resting_acceleration_magnitude);

        const float rotation_x = read_signed_sample(&data[6]) / QMI8658_GYROSCOPE_COUNTS_PER_DEGREE_PER_SECOND;
        const float rotation_y = read_signed_sample(&data[8]) / QMI8658_GYROSCOPE_COUNTS_PER_DEGREE_PER_SECOND;
        const float rotation_z = read_signed_sample(&data[10]) / QMI8658_GYROSCOPE_COUNTS_PER_DEGREE_PER_SECOND;
        const float rotation_rate =
            sqrtf(rotation_x * rotation_x + rotation_y * rotation_y + rotation_z * rotation_z) / DEGREES_PER_RADIAN;

        const int64_t now = esp_timer_get_time();
        if (!moving) {
            const bool above_moving_thresholds =
                rotation_rate > MOTION_MOVING_ROTATION_RADIANS_PER_SECOND ||
                acceleration_deviation > MOTION_MOVING_ACCELERATION_DEVIATION_G;
            if (!above_moving_thresholds) {
                movement_candidate = false;
            } else {
                if (!movement_candidate) {
                    movement_candidate = true;
                    movement_candidate_started_microseconds = now;
                    peak_rotation_rate = rotation_rate;
                    peak_acceleration_deviation = acceleration_deviation;
                } else {
                    peak_rotation_rate = fmaxf(peak_rotation_rate, rotation_rate);
                    peak_acceleration_deviation = fmaxf(peak_acceleration_deviation, acceleration_deviation);
                }

                if (now - movement_candidate_started_microseconds >= MOTION_MOVING_HOLD_MICROSECONDS) {
                    moving = true;
                    movement_candidate = false;
                    still_candidate = false;
                    movement_started_microseconds = now;
                    ESP_LOGI(TAG, "Movement started: peak rotation %.2f rad/s, peak acceleration deviation %.2f g",
                             peak_rotation_rate, peak_acceleration_deviation);
                    motion_callback(true);
                }
            }
        } else {
            peak_rotation_rate = fmaxf(peak_rotation_rate, rotation_rate);
            peak_acceleration_deviation = fmaxf(peak_acceleration_deviation, acceleration_deviation);

            const bool below_still_thresholds =
                rotation_rate < MOTION_STILL_ROTATION_RADIANS_PER_SECOND &&
                acceleration_deviation < MOTION_STILL_ACCELERATION_DEVIATION_G;
            if (!below_still_thresholds) {
                still_candidate = false;
            } else {
                if (!still_candidate) {
                    still_candidate = true;
                    still_candidate_started_microseconds = now;
                }
                if (now - still_candidate_started_microseconds >= MOTION_STILL_HOLD_MICROSECONDS) {
                    moving = false;
                    still_candidate = false;
                    ESP_LOGI(TAG,
                             "Movement stopped after %.1f s: peak rotation %.2f rad/s, peak acceleration deviation "
                             "%.2f g",
                             (float)(now - movement_started_microseconds) / 1000000.0f, peak_rotation_rate,
                             peak_acceleration_deviation);
                    motion_callback(false);
                }
            }
        }
    }
}

void motion_init(motion_callback_t callback)
{
    motion_callback = callback;

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_I2C_ADDRESS,
        .scl_speed_hz = 400 * 1000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(board_i2c_bus(), &device_config, &motion_device));

    uint8_t who_am_i = 0;
    const esp_err_t identity_result = read_registers(QMI8658_REGISTER_WHO_AM_I, &who_am_i, sizeof(who_am_i));
    // A dead IMU is a fault worth reporting, but it is not a reason to stop the
    // voice assistant, so the module logs it and stays out of the way instead
    // of aborting the boot. This matches how touch.c treats its controller.
    if (identity_result != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 at 0x%02X did not answer (0x%x); motion detection stays off", QMI8658_I2C_ADDRESS,
                 identity_result);
        return;
    }
    if (who_am_i != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "QMI8658 at 0x%02X answered with WHO_AM_I 0x%02X, expected 0x%02X; motion detection stays off",
                 QMI8658_I2C_ADDRESS, who_am_i, QMI8658_WHO_AM_I_VALUE);
        return;
    }

    // A software reset makes the configuration below deterministic after a
    // firmware restart that did not power-cycle the IMU. The datasheet gives
    // 15 ms for the reset to finish.
    write_register_checked(QMI8658_REGISTER_RESET, QMI8658_RESET_COMMAND);
    vTaskDelay(pdMS_TO_TICKS(QMI8658_RESET_SETTLE_MS));

    write_register_checked(QMI8658_REGISTER_CONTROL_1, QMI8658_CONTROL_1_ADDRESS_AUTO_INCREMENT);
    write_register_checked(QMI8658_REGISTER_CONTROL_2,
                           QMI8658_ACCELEROMETER_RANGE_4G | QMI8658_OUTPUT_DATA_RATE_235_HZ);
    write_register_checked(QMI8658_REGISTER_CONTROL_3,
                           QMI8658_GYROSCOPE_RANGE_512_DEGREES_PER_SECOND | QMI8658_OUTPUT_DATA_RATE_235_HZ);
    // The built-in low-pass filters are left off; the movement detection wants
    // the raw sample, and the holding times already reject short spikes.
    write_register_checked(QMI8658_REGISTER_CONTROL_5, 0x00);
    write_register_checked(QMI8658_REGISTER_CONTROL_7, 0x03);
    vTaskDelay(pdMS_TO_TICKS(MOTION_TURN_ON_DELAY_MS));

    resting_acceleration_magnitude = measure_resting_acceleration_magnitude();
    ESP_LOGI(TAG, "QMI8658 ready at 0x%02X: +/-4 g and +/-512 degrees per second at 235 Hz, resting %.3f g",
             QMI8658_I2C_ADDRESS, resting_acceleration_magnitude);

    const BaseType_t task_result = xTaskCreate(motion_task, "motion", MOTION_TASK_STACK_BYTES, NULL,
                                               MOTION_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_FAIL);
}

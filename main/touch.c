#include "touch.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_i2c.h"
#include "display.h"

static const char *TAG = "touch";

// Verified against the schematic and the Waveshare XiaoZhi board file: the
// touch interrupt is GPIO5 and reset is GPIO11, not the swapped pins in their
// older ESP-IDF example.
#define TOUCH_INTERRUPT_GPIO 5
#define TOUCH_RESET_GPIO 11

#define TOUCH_I2C_ADDRESS 0x5A
#define TOUCH_TRANSFER_TIMEOUT_MS 100

// The CST9217/CST9220 report starts at this register. A read sends the
// two-byte register address and then clocks out the frame. A valid frame has
// the 0xAB marker at byte 6 and the touch count in byte 5; the idle frame has
// neither, so it is treated as no contact. The CST9220 can return thirty bytes
// for five points, but the ten-byte first-point frame is what the Waveshare
// driver uses and what was verified here; reading thirty made the idle frame
// decode as garbage.
#define TOUCH_DATA_REGISTER 0xD000
#define TOUCH_COMMAND_MODE_REGISTER 0xD101
#define TOUCH_PROJECT_ID_REGISTER 0xD204
#define TOUCH_DATA_BYTES (1 * 5 + 5)
#define TOUCH_POINTS_OFFSET 5
#define TOUCH_POINTS_MASK 0x7F
#define TOUCH_ACK_OFFSET 6
#define TOUCH_ACK_VALUE 0xAB
#define TOUCH_CHIP_ID_CST9217 0x9217
#define TOUCH_CHIP_ID_CST9220 0x9220

// A real touch was seen to take GPIO5 low and release it high again, so the
// falling edge wakes the task immediately. The fallback poll keeps detection
// working if an edge is lost while the shared bus is busy; twice a second is
// the vendor driver's "read a few times per second" fallback.
#define TOUCH_FALLBACK_POLL_MS 500
#define TOUCH_TASK_STACK_BYTES 3072
#define TOUCH_TASK_PRIORITY 3

// The reset pulse and the first data read both happen in touch_init(); the
// task only polls afterwards.
#define TOUCH_RESET_PULSE_MS 10
#define TOUCH_BOOT_SETTLE_MS 100

static i2c_master_dev_handle_t touch_device;
static TaskHandle_t touch_task_handle;

static void touch_interrupt_handler(void *argument)
{
    (void)argument;
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(touch_task_handle, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static esp_err_t read_touch_register(uint16_t register_address, uint8_t *data, size_t length)
{
    const uint8_t address[2] = {(uint8_t)(register_address >> 8), (uint8_t)register_address};
    esp_err_t result = i2c_master_transmit(touch_device, address, sizeof(address), TOUCH_TRANSFER_TIMEOUT_MS);
    if (result != ESP_OK) {
        return result;
    }

    // The vendor driver leaves this gap between the register address and the
    // read. Removing it makes the controller return a stale frame.
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_receive(touch_device, data, length, TOUCH_TRANSFER_TIMEOUT_MS);
}

static esp_err_t read_touch_data(uint8_t data[TOUCH_DATA_BYTES])
{
    return read_touch_register(TOUCH_DATA_REGISTER, data, TOUCH_DATA_BYTES);
}

// The CST92xx puts its identity registers behind a command page. Reading them
// proves the reset released and the I2C wiring is right even when nobody has
// touched the screen, which a no-contact report cannot.
static void log_touch_identity(void)
{
    const uint8_t command_mode[2] = {(uint8_t)(TOUCH_COMMAND_MODE_REGISTER >> 8),
                                     (uint8_t)TOUCH_COMMAND_MODE_REGISTER};
    if (i2c_master_transmit(touch_device, command_mode, sizeof(command_mode), TOUCH_TRANSFER_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "Could not enter the CST9220 command page");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t identity[4] = {0};
    if (read_touch_register(TOUCH_PROJECT_ID_REGISTER, identity, sizeof(identity)) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read the CST9220 identity");
        return;
    }

    const uint16_t project_id = (uint16_t)(identity[0] | (identity[1] << 8));
    const uint16_t chip_id = (uint16_t)(identity[2] | (identity[3] << 8));
    if (chip_id != TOUCH_CHIP_ID_CST9217 && chip_id != TOUCH_CHIP_ID_CST9220) {
        ESP_LOGW(TAG, "Unexpected CST92xx chip ID 0x%04X", chip_id);
        return;
    }
    ESP_LOGI(TAG, "%s identity: project 0x%04X, chip 0x%04X",
             chip_id == TOUCH_CHIP_ID_CST9220 ? "CST9220" : "CST9217", project_id, chip_id);
}

// ESPHome's CST9220 driver deliberately does not acknowledge reports: writing
// the marker back makes the controller return invalid frames during a long
// press. It only checks for the marker at offset 6.
static bool touch_report_is_valid(const uint8_t data[TOUCH_DATA_BYTES])
{
    return data[TOUCH_ACK_OFFSET] == TOUCH_ACK_VALUE;
}

static uint8_t touch_report_points(const uint8_t data[TOUCH_DATA_BYTES])
{
    return data[TOUCH_POINTS_OFFSET] & TOUCH_POINTS_MASK;
}

static void touch_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    bool was_touched = false;
    bool last_read_ok = true;
    int last_interrupt_level = gpio_get_level(TOUCH_INTERRUPT_GPIO);

    for (;;) {
        // The GPIO edge wakes this immediately; the timeout is the fallback
        // path when an edge was lost.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TOUCH_FALLBACK_POLL_MS));
        esp_task_wdt_reset();

        const int interrupt_level = gpio_get_level(TOUCH_INTERRUPT_GPIO);
        if (interrupt_level != last_interrupt_level) {
            ESP_LOGI(TAG, "Touch interrupt is now %s", interrupt_level ? "high" : "low");
            last_interrupt_level = interrupt_level;
        }

        uint8_t data[TOUCH_DATA_BYTES] = {0};
        const esp_err_t read_result = read_touch_data(data);

        if (read_result != ESP_OK) {
            if (last_read_ok) {
                ESP_LOGW(TAG, "CST9220 read failed (0x%x)", read_result);
            }
            last_read_ok = false;
            // A failed read leaves the previous touch state unknown, so the
            // next good frame is allowed to request a recovery again.
            was_touched = false;
        } else if (!touch_report_is_valid(data)) {
            // The controller returns a frame without the 0xAB marker while
            // idle, so this is the normal no-contact case rather than an
            // error. It also clears the held state after a release.
            was_touched = false;
        } else {
            if (!last_read_ok) {
                ESP_LOGI(TAG, "CST9220 is answering again");
            }
            last_read_ok = true;

            const uint8_t points = touch_report_points(data);
            if (points > 0 && !was_touched) {
                ESP_LOGI(TAG, "Touch detected (%u point%s), requesting display recovery", points,
                         points == 1 ? "" : "s");
                display_request_recovery();
            }
            was_touched = points > 0;
        }

    }
}

void touch_init(void)
{
    // The interrupt is the fast path; the task's timed read is the fallback.
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << TOUCH_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&reset_config));
    // The CST9220 reset is active low, per the board's XiaoZhi configuration.
    ESP_ERROR_CHECK(gpio_set_level(TOUCH_RESET_GPIO, 0));

    const gpio_config_t interrupt_config = {
        .pin_bit_mask = 1ULL << TOUCH_INTERRUPT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        // The CST9220 interrupt is active low and the observed touch pulses
        // are falling edges.
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&interrupt_config));

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDRESS,
        .scl_speed_hz = 400 * 1000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(board_i2c_bus(), &device_config, &touch_device));

    // Hold reset low for the pulse width, then release it and give the
    // controller the vendor driver's settle time before the first read.
    vTaskDelay(pdMS_TO_TICKS(TOUCH_RESET_PULSE_MS));
    ESP_ERROR_CHECK(gpio_set_level(TOUCH_RESET_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(TOUCH_BOOT_SETTLE_MS));

    log_touch_identity();

    uint8_t data[TOUCH_DATA_BYTES] = {0};
    const esp_err_t read_result = read_touch_data(data);
    if (read_result != ESP_OK) {
        ESP_LOGE(TAG, "CST9220 at 0x%02X did not answer (0x%x)", TOUCH_I2C_ADDRESS, read_result);
    } else {
        ESP_LOGI(TAG, "CST9220 data register ready at 0x%02X, interrupt GPIO%d is %s", TOUCH_I2C_ADDRESS,
                 TOUCH_INTERRUPT_GPIO, gpio_get_level(TOUCH_INTERRUPT_GPIO) ? "high" : "low");
    }

    const BaseType_t task_result = xTaskCreate(touch_task, "touch", TOUCH_TASK_STACK_BYTES, NULL,
                                               TOUCH_TASK_PRIORITY, &touch_task_handle);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_FAIL);
    // The task must exist before the handler can notify it.
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INTERRUPT_GPIO, touch_interrupt_handler, NULL));
}

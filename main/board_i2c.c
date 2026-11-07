#include "board_i2c.h"

#include "esp_log.h"

static const char *TAG = "board_i2c";

#define BOARD_I2C_SDA_PIN 8
#define BOARD_I2C_SCL_PIN 7

static i2c_master_bus_handle_t bus_handle;

void board_i2c_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA_PIN,
        .scl_io_num = BOARD_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        // Waveshare's reference firmware enables the internal pull-ups, and
        // the bus cannot be configured per device, so keeping them on here
        // matches the state their code leaves the bus in.
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));
    ESP_LOGI(TAG, "Bus ready on SDA GPIO%d and SCL GPIO%d", BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return bus_handle;
}

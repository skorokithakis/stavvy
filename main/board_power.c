#include "board_power.h"

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_i2c.h"

static const char *TAG = "board_power";

#define AXP2101_I2C_ADDRESS 0x34
#define AXP2101_TRANSFER_TIMEOUT_MS 100

static void write_register(i2c_master_dev_handle_t device, uint8_t register_address, uint8_t value)
{
    const uint8_t transfer[2] = {register_address, value};
    ESP_ERROR_CHECK(i2c_master_transmit(device, transfer, sizeof(transfer), AXP2101_TRANSFER_TIMEOUT_MS));
}

void board_power_init(void)
{
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDRESS,
        .scl_speed_hz = 400 * 1000,
    };
    i2c_master_dev_handle_t device;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(board_i2c_bus(), &device_config, &device));

    // The register values follow Waveshare's reference firmware for this board.
    // They are plain writes rather than a PMIC driver because this is the only
    // place that needs to talk to the AXP2101 so far.

    // Make the PWR button a power-off source and enable the die
    // over-temperature shutdown.
    write_register(device, 0x22, 0x06);
    // Turning the board off requires holding PWR for 4 s (OFFLEVEL), and 128 ms
    // is enough to turn it on again.
    write_register(device, 0x27, 0x10);

    // Switch off every converter except DC1 and every LDO before programming
    // voltages, so the rails come up cleanly at the values set below.
    write_register(device, 0x80, 0x01);
    write_register(device, 0x90, 0x00);
    write_register(device, 0x91, 0x00);

    // DC1 powers the SoC: 1.5 V base plus 100 mV per step.
    write_register(device, 0x82, (3300 - 1500) / 100);
    // ALDO1 to ALDO4 feed the 3.3 V peripherals: 0.5 V base plus 100 mV per step.
    write_register(device, 0x92, (3300 - 500) / 100);
    write_register(device, 0x93, (3300 - 500) / 100);
    write_register(device, 0x94, (3300 - 500) / 100);
    write_register(device, 0x95, (3300 - 500) / 100);
    // ALDO1 to ALDO4 on.
    write_register(device, 0x90, 0x0F);

    // Battery charging for the single-cell lithium pack: 4.1 V CV, 50 mA
    // precharge, 400 mA fast charge and 25 mA termination.
    write_register(device, 0x64, 0x02);
    write_register(device, 0x61, 0x02);
    write_register(device, 0x62, 0x0A);
    write_register(device, 0x63, 0x01);

    ESP_LOGI(TAG, "AXP2101 configured, peripheral rails at 3.3 V");
}

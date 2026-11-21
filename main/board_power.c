#include "board_power.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_i2c.h"

static const char *TAG = "board_power";

#define AXP2101_I2C_ADDRESS 0x34
#define AXP2101_TRANSFER_TIMEOUT_MS 100

static i2c_master_dev_handle_t power_device;

static void write_register(i2c_master_dev_handle_t device, uint8_t register_address, uint8_t value)
{
    const uint8_t transfer[2] = {register_address, value};
    ESP_ERROR_CHECK(i2c_master_transmit(device, transfer, sizeof(transfer), AXP2101_TRANSFER_TIMEOUT_MS));
}

// The heartbeat uses this form so a transient bus error only costs one status
// line instead of aborting the diagnostics that are meant to report it.
static esp_err_t try_read_register(uint8_t register_address, uint8_t *value)
{
    return i2c_master_transmit_receive(power_device, &register_address, sizeof(register_address), value,
                                       sizeof(*value), AXP2101_TRANSFER_TIMEOUT_MS);
}

static uint8_t read_register(uint8_t register_address)
{
    uint8_t value = 0;
    ESP_ERROR_CHECK(try_read_register(register_address, &value));
    return value;
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
    power_device = device;

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

    // Enable the ADC channels the heartbeat needs: battery (bit 0), the TS pin
    // (bit 1, kept on because the charger uses it), VSYS (bit 3) and the die
    // temperature (bit 4). The reset value only enables battery and TS.
    write_register(device, 0x30, 0x1B);

    // REG 0x21 is the power-off status, which survives until the next power-on.
    // Logging it raw is enough for the fault report: a brownout (bit 3, Vsys
    // under-voltage), a PMIC over-temperature (bit 7) or a software/button
    // power-off (bits 0 and 1) each point somewhere different, and the decoding
    // can follow once a value is known.
    const uint8_t power_off_status = read_register(0x21);
    ESP_LOGI(TAG, "AXP2101 configured, peripheral rails at 3.3 V; power-off status 0x%02X", power_off_status);
}

void board_power_log_status(const board_power_status *status)
{
    if (status == NULL) {
        ESP_LOGW(TAG, "Could not read the AXP2101 status");
        return;
    }

    ESP_LOGI(TAG,
             "AXP2101: rails 0x90=0x%02X, status 0x00=0x%02X 0x01=0x%02X, vbat %u mV (raw %u), vsys %u mV (raw "
             "%u), die %d.%d C (raw %u)",
             status->rail_enable, status->status1, status->status2, status->battery_millivolts,
             (unsigned)status->battery_millivolts, status->system_millivolts, (unsigned)status->system_millivolts,
             status->temperature_tenths / 10, status->temperature_tenths % 10, (unsigned)status->temperature_raw);
}

esp_err_t board_power_read_status(board_power_status *status)
{
    // Register 0x90 is the rail enable mask, 0x00 and 0x01 are the charger
    // status, and the pairs are the battery and VSYS voltages and the die
    // temperature.
    enum { REGISTER_COUNT = 9 };
    static const uint8_t registers[REGISTER_COUNT] = {0x90, 0x00, 0x01, 0x34, 0x35, 0x3A, 0x3B, 0x3C, 0x3D};
    uint8_t values[REGISTER_COUNT];
    for (size_t index = 0; index < REGISTER_COUNT; index++) {
        const esp_err_t result = try_read_register(registers[index], &values[index]);
        if (result != ESP_OK) {
            return result;
        }
    }

    status->rail_enable = values[0];
    status->status1 = values[1];
    status->status2 = values[2];
    // Both voltages are 1 mV per count.
    status->battery_millivolts = (uint16_t)(((values[3] & 0x3F) << 8) | values[4]);
    status->system_millivolts = (uint16_t)(((values[5] & 0x3F) << 8) | values[6]);
    status->temperature_raw = (uint16_t)(((values[7] & 0x3F) << 8) | values[8]);
    status->temperature_tenths = 220 + (7274 - (int)status->temperature_raw) / 2;
    return ESP_OK;
}

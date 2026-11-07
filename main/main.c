#include <stdint.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "board_i2c.h"
#include "board_power.h"
#include "mdns_service.h"
#include "wifi_station.h"

static const char *TAG = "main";

static void log_heap_usage(const char *moment)
{
    ESP_LOGI(TAG, "Free heap %s: %u bytes, minimum free heap: %u bytes", moment,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

static void handle_network_connected(void *handler_argument, esp_event_base_t event_base, int32_t event_id,
                                     void *event_data)
{
    log_heap_usage("after Wi-Fi connect");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting");
    log_heap_usage("at boot");

    // The Wi-Fi driver stores calibration data and the station configuration in
    // NVS, so it has to be initialised before esp_wifi_init().
    ESP_ERROR_CHECK(nvs_flash_init());

    board_i2c_init();
    board_power_init();

    // The default event loop has to exist before a handler can subscribe to it,
    // and the network stack has to be up before Wi-Fi starts.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Subscribe before Wi-Fi starts so the first connection is not missed.
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handle_network_connected, NULL, NULL));

    wifi_station_connect();
    mdns_service_start();
}

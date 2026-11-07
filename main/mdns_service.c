#include "mdns_service.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

static const char *TAG = "mdns_service";

#define MDNS_HOSTNAME_PREFIX "home-assistant-voice"
#define ESPHOME_API_SERVICE "_esphomelib"
#define ESPHOME_API_PROTOCOL "_tcp"
#define ESPHOME_API_PORT 6053

void mdns_service_get_hostname(char *hostname, size_t hostname_size)
{
    uint8_t mac_address[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_address, ESP_MAC_WIFI_STA));
    // The management server finds boards by this prefix, so the MAC suffix is
    // what keeps every board's hostname unique without any provisioning step.
    snprintf(hostname, hostname_size, MDNS_HOSTNAME_PREFIX "-%02x%02x%02x", mac_address[3], mac_address[4],
             mac_address[5]);
}

void mdns_service_start(void)
{
    char hostname[32];
    mdns_service_get_hostname(hostname, sizeof(hostname));

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(hostname));
    // Advertise the ESPHome native API endpoint now so the server can already
    // discover the board; the API server itself is added in a later task.
    ESP_ERROR_CHECK(
        mdns_service_add(NULL, ESPHOME_API_SERVICE, ESPHOME_API_PROTOCOL, ESPHOME_API_PORT, NULL, 0));

    ESP_LOGI(TAG, "Advertising %s.local with %s.%s on port %d", hostname, ESPHOME_API_SERVICE,
             ESPHOME_API_PROTOCOL, ESPHOME_API_PORT);
}

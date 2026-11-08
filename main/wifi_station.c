#include "wifi_station.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_station";

// The DHCP hostname is what routers list the board as. It is separate from the
// mDNS hostname in mdns_service.c, which keeps the home-assistant-voice prefix
// that havpe-server discovers boards by.
#define DHCP_HOSTNAME "stavvy"

static bool credentials_configured(void)
{
    return CONFIG_WIFI_SSID[0] != '\0';
}

static void copy_credential(uint8_t *destination, size_t destination_size, const char *source)
{
    // An SSID can fill all 32 bytes and a password all 64, and in that case
    // neither is NUL terminated, so copy by length instead of using strlcpy().
    const size_t source_length = strlen(source);
    memcpy(destination, source, source_length < destination_size ? source_length : destination_size);
}

static void handle_wifi_event(void *handler_argument, esp_event_base_t event_base, int32_t event_id,
                              void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        if (credentials_configured()) {
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        if (credentials_configured()) {
            ESP_LOGW(TAG, "Disconnected (reason %d), reconnecting", disconnected->reason);
            // esp_wifi_connect() only attempts the access point once, so every
            // disconnect event needs a new attempt. Issuing it from this handler
            // is safe because the call only queues the request.
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
        break;
    }
    default:
        break;
    }
}

void wifi_station_connect(void)
{
    if (!credentials_configured()) {
        ESP_LOGW(TAG, "No Wi-Fi SSID set in menuconfig; the board will not join a network");
    }

    esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
    // Set before the interface starts so the very first DHCP request carries
    // the hostname; lwIP only sends option 12 when the netif has one.
    ESP_ERROR_CHECK(esp_netif_set_hostname(station_netif, DHCP_HOSTNAME));

    const wifi_init_config_t initialization_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initialization_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handle_wifi_event, NULL, NULL));

    wifi_config_t station_config = {0};
    copy_credential(station_config.sta.ssid, sizeof(station_config.sta.ssid), CONFIG_WIFI_SSID);
    copy_credential(station_config.sta.password, sizeof(station_config.sta.password), CONFIG_WIFI_PASSWORD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

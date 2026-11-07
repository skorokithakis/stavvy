idf_component_register(
    SRCS
        "main.c"
        "board_i2c.c"
        "board_power.c"
        "wifi_station.c"
        "mdns_service.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_i2c
        esp_event
        esp_netif
        esp_wifi
        mdns
        nvs_flash
)

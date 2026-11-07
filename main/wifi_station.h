#pragma once

// Joins the network configured in Kconfig and reconnects whenever the link
// drops, for as long as the board is powered. Call after esp_netif_init() and
// esp_event_loop_create_default().
void wifi_station_connect(void);

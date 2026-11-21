#pragma once

#include <stddef.h>

// Copies the last three bytes of the station MAC address as lowercase hex
// (six characters, no separator). This is the suffix that keeps each board's
// mDNS hostname unique without any provisioning step, and the metrics instance
// label carries the same value.
void mdns_service_get_mac_suffix(char *suffix, size_t suffix_size);

// Copies the hostname (without the .local suffix) that mDNS advertises. The
// ESPHome native API reports the same string as its device name, so callers
// that identify the board on the network use one source of truth.
void mdns_service_get_hostname(char *hostname, size_t hostname_size);

// Advertises the board on the local network. The hostname is derived from the
// MAC address so the server that manages boards can find them by prefix without
// any provisioning step.
void mdns_service_start(void);

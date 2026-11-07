#pragma once

#include <stddef.h>

// Copies the hostname (without the .local suffix) that mDNS advertises. The
// ESPHome native API reports the same string as its device name, so callers
// that identify the board on the network use one source of truth.
void mdns_service_get_hostname(char *hostname, size_t hostname_size);

// Advertises the board on the local network. The hostname is derived from the
// MAC address so the server that manages boards can find them by prefix without
// any provisioning step.
void mdns_service_start(void);

#pragma once

// Advertises the board on the local network. The hostname is derived from the
// MAC address so the server that manages boards can find them by prefix without
// any provisioning step.
void mdns_service_start(void);

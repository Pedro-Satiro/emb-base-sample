/* Public API for Wi-Fi connection helpers
 * Provides initialization hook and a connectivity status accessor.
 */
#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include <stdbool.h>

/* Called at system init (SYS_INIT) to bring up Wi-Fi and DHCP.
 * Returns 0 on success, negative errno on failure.
 */
int app_auto_init(void);

/* Returns true when the network interface has an IPv4 address (connected). */
bool wifi_connected(void);

#endif /* WIFI_CONNECT_H */

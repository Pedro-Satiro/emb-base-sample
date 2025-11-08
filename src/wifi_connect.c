#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi_connect.h"

static struct k_sem got_ip_sem;
static struct net_mgmt_event_callback ip_cb;

static bool is_wifi_connected = false;

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

static void ip_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			     struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		char ip[NET_IPV4_ADDR_LEN];
		const struct net_if_config *cfg = net_if_get_config(iface);

		if (cfg && cfg->ip.ipv4) {
			net_addr_ntop(AF_INET, &cfg->ip.ipv4->unicast[0].ipv4.address.in_addr, ip,
				      sizeof(ip));
			LOG_INF("DHCP OK: %s", ip);
			k_sem_give(&got_ip_sem);
		}
	}
}

static int wifi_connect_now(const char *ssid, const char *psk)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params p = {0};

	p.ssid = ssid;
	p.ssid_length = strlen(ssid);
	p.psk = psk;
	p.psk_length = strlen(psk);
	p.security = WIFI_SECURITY_TYPE_PSK;
	p.channel = WIFI_CHANNEL_ANY;

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
	if (ret) {
		LOG_ERR("Failed NET_REQUEST_WIFI_CONNECT (%d)", ret);
		return ret;
	}

	LOG_INF("Connecting on AP \"%s\" ...", ssid);
	return 0;
}

int app_auto_init(void)
{
	LOG_INF("Starting Wi-Fi Connect");

	k_sem_init(&got_ip_sem, 0, 1);
	net_mgmt_init_event_callback(&ip_cb, ip_event_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ip_cb);

	if (wifi_connect_now(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWD) != 0) {
		return -EIO;
	}

	if (k_sem_take(&got_ip_sem, K_SECONDS(30)) != 0) {
		LOG_ERR("DHCP Timeout");
		return -ETIMEDOUT;
	}

	struct zsock_addrinfo hints = {0}, *res = NULL;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	char *hostname_test = "google.com";

	int ret = zsock_getaddrinfo(hostname_test, "80", &hints, &res);
	if (ret) {
		LOG_ERR("DNS failed (%d)", ret);

		return -1;
	} else {
		char ipbuf[NET_IPV4_ADDR_LEN];
		struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;

		net_addr_ntop(AF_INET, &a->sin_addr, ipbuf, sizeof(ipbuf));

		LOG_INF("DNS OK: %s -> %s", ipbuf, hostname_test);
		zsock_freeaddrinfo(res);
	}

	is_wifi_connected = true;

	return 0;
}

bool wifi_connected(void)
{
	return is_wifi_connected;
}
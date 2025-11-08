/* Minimal Wi-Fi connect example for Zephyr
 * Replaces the previous mixed SNTP/ZBus code with a focused Wi-Fi connect demo.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_mgmt.h>

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

static struct k_sem got_ip_sem;
static struct net_mgmt_event_callback ip_cb;
static bool is_wifi_connected = false;

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
            is_wifi_connected = true;
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

    /* Quick DNS test to verify network/DNS */
    struct zsock_addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    const char *hostname_test = "google.com";

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

void main(void)
{
    int ret;

    LOG_INF("=== WiFi connect example ===");
    LOG_INF("SSID: %s", CONFIG_WIFI_SSID);

    ret = app_auto_init();
    if (ret) {
        LOG_ERR("app_auto_init failed: %d", ret);
    } else {
        LOG_INF("WiFi connected and DNS OK");
    }

    while (1) {
        if (is_wifi_connected) {
            LOG_INF("WiFi: connected");
        } else {
            LOG_INF("WiFi: disconnected");
        }
        k_sleep(K_SECONDS(10));
    }
}

static void filter_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Filter: Started");
    sensor_msg_t msg;
    
    while (1) {
        if (k_msgq_get(&input_queue, &msg, K_FOREVER) == 0) {
            if (validate_sensor_data(&msg)) {
                int ret = k_msgq_put(&output_queue, &msg, K_NO_WAIT);
                if (ret != 0) {
                    LOG_WRN("Filter: Output queue full, dropping valid data");
                }
            } else {
                log_invalid_data(&msg);
            }
        }
    }
}

static void consumer_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Consumer: Started");
    sensor_msg_t msg;
    
    while (1) {
        if (k_msgq_get(&output_queue, &msg, K_FOREVER) == 0) {
            if (msg.type == SENSOR_TEMPERATURE) {
                LOG_INF("✓ VALID Temperature: %d°C [ts: %lld]", 
                        msg.value.temperature, msg.timestamp);
            } else {
                LOG_INF("✓ VALID Humidity: %d%% [ts: %lld]", 
                        msg.value.humidity, msg.timestamp);
            }
        }
    }
}

K_THREAD_DEFINE(temp_producer, STACK_SIZE, temperature_producer_thread, 
                NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(humidity_producer, STACK_SIZE, humidity_producer_thread, 
                NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(filter, STACK_SIZE, filter_thread, 
                NULL, NULL, NULL, PRIORITY + 1, 0, 0);

K_THREAD_DEFINE(consumer, STACK_SIZE, consumer_thread, 
                NULL, NULL, NULL, PRIORITY + 2, 0, 0);

int main(void) {
    LOG_INF("=== Sensor Data Processing System ===");
    LOG_INF("Temperature range: %d-%d°C", TEMP_MIN, TEMP_MAX);
    LOG_INF("Humidity range: %d-%d%%", HUMIDITY_MIN, HUMIDITY_MAX);
    LOG_INF("==========================================");
    
    while (1) {
        k_msleep(5000);
        LOG_INF("System running - Queues: Input[%d] Output[%d]", 
                k_msgq_num_used_get(&input_queue),
                k_msgq_num_used_get(&output_queue));
    }
    
    return 0;
}
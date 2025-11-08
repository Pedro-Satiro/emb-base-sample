#include "zephyr/net/net_ip.h"
#include "zephyr/net/socket_service.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/toolchain.h>
#include <zephyr/zbus/zbus.h>

// west packages pip --install
// west blobs fetch hal_espressif


LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#include "wifi_connect.h"
typedef struct {
	struct tm timestamp;
} time_msg_t;

typedef struct {
	struct sockaddr addr;
	socklen_t len;
} sntp_endpoint_t;

ZBUS_CHAN_DEFINE(time_channel, time_msg_t, NULL, NULL, ZBUS_OBSERVERS(log_subs, app_subs),
		 ZBUS_MSG_INIT(.timestamp = 0));

K_THREAD_STACK_DEFINE(thread_sntp_stack, 1024);
K_THREAD_STACK_DEFINE(thread_logger_stack, 1024);
K_THREAD_STACK_DEFINE(thread_app_stack, 1024);

static struct k_thread g_thread_sntp;
static struct k_thread g_thread_logger;
static struct k_thread g_thread_app;

static sntp_endpoint_t g_sntp_endpoint;
static struct sntp_time g_sntp_time;
static K_SEM_DEFINE(g_sntp_async_sem, 0, 1);

static void sntp_async_event_handler(struct net_socket_service_event *pev);
static void logger_task(void *p1, void *p2, void *p3);
static void app_task(void *p1, void *p2, void *p3);
static void sntp_task(void *p1, void *p2, void *p3);

NET_SOCKET_SERVICE_SYNC_DEFINE_STATIC(service_sntp_async, sntp_async_event_handler, 1);

SYS_INIT(app_auto_init, APPLICATION, 50);

static void format_tm(char *buf, size_t buflen, const char *fmt, const struct tm *tm)
{
	if (buf && buflen > 0 && fmt && tm) {
		strftime(buf, buflen, fmt, tm);
	}
}

static void sntp_async_event_handler(struct net_socket_service_event *pev)
{
	int err = sntp_read_async(pev, &g_sntp_time);
	if (err) {
		LOG_ERR("[SNTP] failed to read SNTP response (%d)", err);
		return;
	}

	k_sem_give(&g_sntp_async_sem);
}

void logger_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int err;
	char date_time[32] = {0};
	static struct tm s_logger_clock = {0};
	const struct zbus_channel *ch;
	time_msg_t msg;

	LOG_INF("[LOGGER] Starting service");

	while (1) {
		err = zbus_sub_wait(&log_subs, &ch, K_FOREVER);
		if (err) {
			LOG_WRN("[LOGGER] wait error: %d", err);
			continue;
		}

		err = zbus_chan_read(ch, &msg, K_FOREVER);
		if (err) {
			LOG_WRN("[LOGGER] read error: %d", err);
			continue;
		}

		s_logger_clock = msg.timestamp;

		format_tm(date_time, sizeof(date_time), "%a %Y-%m-%d %H:%M:%S %Z", &s_logger_clock);
		LOG_INF("[LOGGER] Internal clock updated: %s", date_time);
	}
}

void app_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int err;
	bool initial = false;
	char date_time[32] = {0};
	char format[64];
	snprintf(format, sizeof(format), "%%a %%Y-%%m-%%d %%H:%%M:%%S %%Z%+d", CONFIG_LOCAL_TIME);

	const struct zbus_channel *ch;
	static struct tm s_last_ts = {0};
	time_msg_t msg;

	LOG_INF("[APP] Starting service");

	while (1) {
		err = zbus_sub_wait(&app_subs, &ch, K_FOREVER);
		if (err) {
			LOG_WRN("[APP] wait error: %d", err);
			continue;
		}

		err = zbus_chan_read(ch, &msg, K_FOREVER);
		if (err) {
			LOG_WRN("[APP] read error: %d", err);
			continue;
		}

		format_tm(date_time, sizeof(date_time), format, &s_last_ts);
		LOG_DBG("[APP] Last execution time: %s", date_time);

		format_tm(date_time, sizeof(date_time), format, &msg.timestamp);
		LOG_DBG("[APP] Now execution time: %s", date_time);

		if (!initial) {
			s_last_ts = msg.timestamp;
			initial = true;
		}

		int64_t ta = timeutil_timegm64(&s_last_ts);
		int64_t tb = timeutil_timegm64(&msg.timestamp);
		int64_t dt = tb - ta;

		LOG_INF("[APP] Time execution interval: %" PRId64 "s", dt);

		s_last_ts = msg.timestamp;
	}
}

void sntp_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	char format[64];
	snprintf(format, sizeof(format), "%%a %%Y-%%m-%%d %%H:%%M:%%S %%Z%+d", CONFIG_LOCAL_TIME);

	sntp_endpoint_t *endpoint = (sntp_endpoint_t *)arg1;
	struct sntp_ctx ctx;
	int err;

	err = sntp_init_async(&ctx, &endpoint->addr, endpoint->len, &service_sntp_async);
	if (err) {
		LOG_ERR("Failed to init SNTP, ctx: %d", err);
		sntp_close(&ctx);
		return;
	}

	LOG_INF("Starting SNTP Service");

	while (1) {
		struct tm time_utc;

		k_sem_reset(&g_sntp_async_sem);
		err = sntp_send_async(&ctx);
		if (err) {
			LOG_WRN("[SNTP] send error: %d", err);
			continue;
		}

		err = k_sem_take(&g_sntp_async_sem, K_MSEC(1000));
		if (err) {
			LOG_WRN("[SNTP] response timed out (%d)", err);
			continue;
		}

		const struct timespec ts = {
			.tv_sec = g_sntp_time.seconds,
			.tv_nsec = (long)((((uint64_t)g_sntp_time.fraction) * 1000000000ULL) >> 32)};

		sys_clock_settime(CLOCK_REALTIME, &ts);

		uint64_t local_sec = g_sntp_time.seconds + (CONFIG_LOCAL_TIME * 3600);
		gmtime_r(&local_sec, &time_utc);

		char date_time[32];
		format_tm(date_time, sizeof(date_time), format, &time_utc);

		LOG_INF("[SNTP] Localtime updated: %s", date_time);

		time_msg_t msg = {.timestamp = time_utc};
		err = zbus_chan_pub(&time_channel, &msg, K_NO_WAIT);
		if (err) {
			LOG_WRN("[SNTP] failed to publish in channel");
		}

		int sleep_time = (k_cycle_get_32() % 5000) + 500;
		k_sleep(K_MSEC(sleep_time));
	}

	sntp_close_async(&service_sntp_async);
	sntp_close(&ctx);
}

ZBUS_SUBSCRIBER_DEFINE(app_subs, 4);
ZBUS_SUBSCRIBER_DEFINE(log_subs, 4);

int main(void)
{
	int wifi_wait = 0;
	while (!wifi_connected() && wifi_wait < 30) {
		LOG_INF("Waiting for Wi-Fi to become ready... (%ds)", wifi_wait);
		k_sleep(K_SECONDS(1));
		wifi_wait++;
	}

	if (!wifi_connected()) {
		LOG_ERR("Wi-Fi not ready after timeout");
		return -1;
	}

	int err;
	char ipbuf[NET_IPV4_ADDR_LEN];

	struct zsock_addrinfo hints = {0};
	struct zsock_addrinfo *res = NULL;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	LOG_INF("Starting system");

	k_sleep(K_SECONDS(2));

	err = zsock_getaddrinfo(CONFIG_SNTP_HOSTNAME, "123", &hints, &res);
	if (err) {
		LOG_ERR("Failed to get hostname info");
		return -1;
	} else {
		net_addr_ntop(AF_INET, res->ai_addr, ipbuf, sizeof(ipbuf));

		LOG_INF("DNS SNTP OK: %s -> %s", ipbuf, CONFIG_SNTP_HOSTNAME);

		g_sntp_endpoint.len = res->ai_addrlen;
		memcpy(&g_sntp_endpoint.addr, res->ai_addr, res->ai_addrlen);

		zsock_freeaddrinfo(res);
	}

    k_thread_create(&g_thread_sntp, thread_sntp_stack,
	    K_THREAD_STACK_SIZEOF(thread_sntp_stack), sntp_task, &g_sntp_endpoint, NULL,
	    NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);

    k_thread_create(&g_thread_logger, thread_logger_stack,
	    K_THREAD_STACK_SIZEOF(thread_logger_stack), logger_task, NULL, NULL, NULL,
	    K_PRIO_PREEMPT(3), 0, K_NO_WAIT);

    k_thread_create(&g_thread_app, thread_app_stack, K_THREAD_STACK_SIZEOF(thread_app_stack),
	    app_task, NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, K_NO_WAIT);

	LOG_INF("System started successfully");

	return 0;
}
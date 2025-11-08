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

// tentei fazer com  uma rasp zero mas nao tava conseguindo, acabei optando pela esp 32 devkit
// west packages pip --install
// west blobs fetch hal_espressif


LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#include "wifi_connect.h"

struct data {
	struct tm timestamp;
};

struct endpoint {
	struct sockaddr addr;
	socklen_t len;
};

ZBUS_CHAN_DEFINE(time_channel, struct data, NULL, NULL, ZBUS_OBSERVERS(log_subs, app_subs),
		 ZBUS_MSG_INIT(.timestamp = 0));

K_THREAD_STACK_DEFINE(thread_sntp_stack, 1024);
K_THREAD_STACK_DEFINE(thread_logger_stack, 1024);
K_THREAD_STACK_DEFINE(thread_app_stack, 1024);

static struct k_thread thread_sntp_data;
static struct k_thread thread_logger_data;
static struct k_thread thread_app_data;

static struct endpoint sntp_endpoint;
static struct sntp_time s_time;
static K_SEM_DEFINE(sntp_async_received, 0, 1);
static void sntp_service_handler(struct net_socket_service_event *pev);

NET_SOCKET_SERVICE_SYNC_DEFINE_STATIC(service_sntp_async, sntp_service_handler, 1);

SYS_INIT(app_auto_init, APPLICATION, 50);

static void sntp_service_handler(struct net_socket_service_event *pev)
{
	int error;

	error = sntp_read_async(pev, &s_time);
	if (error) {
		LOG_ERR("[SNTP] failed to read SNTP response (%d)", error);
		return;
	}

	k_sem_give(&sntp_async_received);
}

void logger_thread(void *arg1, void *arg2, void *arg3)
{
	int error;
	char date_time[32] = {0};

	static struct tm internal_clock = {0};
	const struct zbus_channel *ch;
	struct data msg;

	LOG_INF("[LOGGER] Starting service");

	while (true) {
		error = zbus_sub_wait(&log_subs, &ch, K_FOREVER);
		if (error) {
			LOG_WRN("[LOGGER] error while waiting channel notification: %d", error);
			continue;
		}

		error = zbus_chan_read(ch, &msg, K_FOREVER);
		if (error) {
			LOG_WRN("[LOGGER] error while reading channel msg: %d", error);
			continue;
		}

		internal_clock = msg.timestamp;

		strftime(date_time, 30, "%a %Y-%m-%d %H:%M:%S %Z", &internal_clock);
		LOG_INF("[LOGGER] Internal clock updated: %s", date_time);
	}
}

void app_thread(void *arg1, void *arg2, void *arg3)
{
	int error;
	bool init_ts = false;
	char date_time[32] = {0};
	char format[64];
	snprintf(format, sizeof(format), "%%a %%Y-%%m-%%d %%H:%%M:%%S %%Z%+d", CONFIG_LOCAL_TIME);

	const struct zbus_channel *ch;
	static struct tm last_timestamp = {0};
	struct data msg;

	LOG_INF("[APP] Starting service");

	while (true) {
		error = zbus_sub_wait(&app_subs, &ch, K_FOREVER);
		if (error) {
			LOG_WRN("[APP] error while waiting channel notification: %d", error);
			continue;
		}

		error = zbus_chan_read(ch, &msg, K_FOREVER);
		if (error) {
			LOG_WRN("[APP] error while reading channel msg: %d", error);
			continue;
		}

		strftime(date_time, 30, format, &last_timestamp);
		LOG_DBG("[APP] Last execution time: %s", date_time);

		strftime(date_time, 30, format, &msg.timestamp);
		LOG_DBG("[APP] Now execution time: %s", date_time);

		if (!init_ts) {
			last_timestamp = msg.timestamp;
			init_ts = true;
		}

		int64_t ta = timeutil_timegm64(&last_timestamp);
		int64_t tb = timeutil_timegm64(&msg.timestamp);
		int64_t dt = tb - ta;

		LOG_INF("[APP] Time execution interval: %" PRId64 "s", dt);

		last_timestamp = msg.timestamp;
	}
}

void sntp_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	char format[64];
	snprintf(format, sizeof(format), "%%a %%Y-%%m-%%d %%H:%%M:%%S %%Z%+d", CONFIG_LOCAL_TIME);

	struct endpoint *sntp_endpoint = (struct endpoint *)arg1;
	struct sntp_ctx ctx;
	int error;

	error = sntp_init_async(&ctx, &sntp_endpoint->addr, sntp_endpoint->len,
				&service_sntp_async);
	if (error) {
		LOG_ERR("Failed to init SNTP, ctx: %d", error);
		sntp_close(&ctx);

		return;
	}

	LOG_INF("Starting SNTP Service");

	while (true) {
		struct tm time_utc;

		k_sem_reset(&sntp_async_received);
		error = sntp_send_async(&ctx);

		if (error) {
			LOG_WRN("[SNTP] Failed to send SNTP query (%d)", error);
			continue;
		}

		error = k_sem_take(&sntp_async_received, K_MSEC(1000));
		if (error) {
			LOG_WRN("[SNTP] response timed out (%d)", error);
			continue;
		}

		const struct timespec ts = {
			.tv_sec = s_time.seconds,
			.tv_nsec = (long)((((uint64_t)s_time.fraction) * 1000000000ULL) >> 32)};

		sys_clock_settime(CLOCK_REALTIME, &ts);

		uint64_t local_sec = s_time.seconds + (CONFIG_LOCAL_TIME * 3600);
		gmtime_r(&local_sec, &time_utc);

		char date_time[32];
		strftime(date_time, 30, format, &time_utc);

		LOG_INF("[SNTP] Localtime updated: %s", date_time);

		struct data msg = {.timestamp = time_utc};
		error = zbus_chan_pub(&time_channel, &msg, K_NO_WAIT);
		if (error) {
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
	if (!wifi_connected()) {
		LOG_INF("Unable to connect to the WIFI.");
		return -1;
	}

	int error;
	char ipbuf[NET_IPV4_ADDR_LEN];

	struct zsock_addrinfo hints;
	struct zsock_addrinfo *res;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	LOG_INF("Starting system");

	k_sleep(K_SECONDS(2));

	error = zsock_getaddrinfo(CONFIG_SNTP_HOSTNAME, "123", &hints, &res);
	if (error) {
		LOG_ERR("Failed to get hostname info");
		return -1;
	} else {
		net_addr_ntop(AF_INET, res->ai_addr, ipbuf, sizeof(ipbuf));

		LOG_INF("DNS SNTP OK: %s -> %s", ipbuf, CONFIG_SNTP_HOSTNAME);

		sntp_endpoint.len = res->ai_addrlen;
		memcpy(&sntp_endpoint.addr, res->ai_addr, res->ai_addrlen);

		zsock_freeaddrinfo(res);
	}

	k_thread_create(&thread_sntp_data, thread_sntp_stack,
			K_THREAD_STACK_SIZEOF(thread_sntp_stack), sntp_thread, &sntp_endpoint, NULL,
			NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);

	k_thread_create(&thread_logger_data, thread_logger_stack,
			K_THREAD_STACK_SIZEOF(thread_logger_stack), logger_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(3), 0, K_NO_WAIT);

	k_thread_create(&thread_app_data, thread_app_stack, K_THREAD_STACK_SIZEOF(thread_app_stack),
			app_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(3), 0, K_NO_WAIT);

	LOG_INF("System started successfully");

	return 0;
}
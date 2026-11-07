/*
 * WiFi, NTP clock, tariff download and the trip/service upload queue.
 * One thread (`net`) does all blocking network work. The UI never waits for it.
 */
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/http/client.h>
#include <zephyr/logging/log.h>
#include "hw.h"
#include "meter.h"
#include "persist.h"
#include "tariff.h"
#include "postq.h"
#include "wdog.h"

LOG_MODULE_REGISTER(net, LOG_LEVEL_INF);

#define SERVER_HOST "node.raxitaxi.ir"
#define SERVER_PORT "443"
#define TARIFF_PATH "/api/taxi/TaxiFare?driver_type=1&serial="
#define NTP_SERVER "ntp.day.ir"
#define TZ_OFFSET_S 12600 /* Iran, +03:30, no DST */

#define IDLE_BEFORE_SEND_MS 3000 /* send only when no key was used for 3 s */
#define RESYNC_MS (10 * 60 * 1000)
#define CONNECT_RETRY_MS 30000
#define RESERVE_SSID "Usefi"

/* ---------------- state ---------------- */
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ip_cb;
static volatile bool connected;
static volatile bool connect_pending;
static volatile bool tariff_requested = true;
static volatile int64_t tariff_try_after; /* ms uptime, no tariff request before this */
static char serial[13];

/* ---------------- post queue: see postq.c ---------------- */
static K_SEM_DEFINE(q_wake, 0, 1);

/* ---------------- reserve scan ---------------- */
static volatile bool reserve_scan_req;
static volatile bool reserve_found;

const char *net_serial(void)
{
	return serial;
}

bool net_wifi_connected(void)
{
	return connected;
}

int cfg_service_count(void)
{
	return persist.service_count;
}

int cfg_service(int idx)
{
	return (idx >= 0 && idx < SERVICE_MAX) ? persist.service[idx] : 0;
}

void net_reserve_scan_request(void)
{
	reserve_scan_req = true;
}

bool net_reserve_found(void)
{
	return reserve_found;
}

void net_reserve_reset(void)
{
	reserve_found = false;
	reserve_scan_req = false;
}

/* ---------------- public helpers used by the UI ---------------- */
bool net_post(const char *path_query)
{
	bool ok = postq_push(path_query);

	k_sem_give(&q_wake);
	return ok;
}

void net_service_send(int service_number, int start)
{
	char url[URL_MAX];

	snprintf(url, sizeof(url), "/api/taxi/StartService?&serial=%s&serviceNum=%d&start=%d",
		 serial, service_number, start);
	net_post(url);
}

void net_trip_start(int bt)
{
	char url[URL_MAX];

	snprintf(url, sizeof(url), "/api/taxi/SetTaxiStart?serial=%s&bt_number=%d", serial, bt);
	net_post(url);
}

void trip_submit(int bt, const meter_snap_t *s, const char *start_hhmm)
{
	char url[URL_MAX];
	int h, m;
	/* keep every number short so the record always fits in the queue slot */
	const int64_t cap = 999999999;
	int64_t trip = MIN(s->trip_price, cap), traffic = MIN(s->traffic_price, cap);
	int64_t night = MIN(s->night_price, cap), total = MIN(s->total_rial, cap);

	clock_get_hm(&h, &m);
	/* trip_price = route price, night_price = night part of it, traffic_price = waiting.
	 * traffic_distance is not measured yet (0). */
	int n = snprintf(url, sizeof(url),
		 "/api/taxi/SetTaxiEnd?driver_id=%s&serial=%s&bt_number=%d&start_time=%s"
		 "&stop_time=%02d:%02d&trip_distance=%d&trip_duration=%d&traffic_distance=0"
		 "&traffic_duration=%d&avg_speed=%d&max_speed=%d&trip_price=%lld"
		 "&traffic_price=%lld&night_price=%lld&total_price=%lld",
		 serial, serial, bt, start_hhmm, h, m, s->dist_m, s->duration_s, s->wait_s,
		 s->avg_speed_kmh, s->max_speed_kmh, (long long)trip, (long long)traffic,
		 (long long)night, (long long)total);
	if (n < 0 || n >= (int)sizeof(url)) {
		LOG_ERR("trip record too long, NOT queued");
		return;
	}
	net_post(url);
}

void net_reload_tariff(void)
{
	tariff_try_after = 0;
	tariff_requested = true;
}

void net_reset_wifi(void)
{
	persist_reset_wifi();
	LOG_WRN("WiFi reset to default, rebooting");
	k_sleep(K_MSEC(200));
	sys_reboot(SYS_REBOOT_COLD);
}

/* ---------------- WiFi ---------------- */
static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t ev, struct net_if *iface)
{
	switch (ev) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *st = cb->info;

		if (st && st->status) {
			LOG_WRN("WiFi connect failed: %d", st->status);
			connect_pending = false;
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("WiFi disconnected");
		connected = false;
		connect_pending = false;
		display_set_wifi(false);
		break;
	case NET_EVENT_WIFI_SCAN_RESULT: {
		const struct wifi_scan_result *r = cb->info;

		if (r && r->ssid_length == strlen(RESERVE_SSID) &&
		    memcmp(r->ssid, RESERVE_SSID, r->ssid_length) == 0) {
			reserve_found = true;
		}
		break;
	}
	default:
		break;
	}
}

static void ip_event(struct net_mgmt_event_callback *cb, uint64_t ev, struct net_if *iface)
{
	if (ev == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("WiFi connected, got IPv4");
		connected = true;
		connect_pending = false;
		tariff_requested = true;
		display_set_wifi(true);
	}
}

static void wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		LOG_ERR("no wifi interface");
		return;
	}

	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)persist.ssid,
		.ssid_length = strlen(persist.ssid),
		.psk = (const uint8_t *)persist.pass,
		.psk_length = strlen(persist.pass),
		.security = persist.pass[0] ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
		.band = WIFI_FREQ_BAND_UNKNOWN,
		.channel = WIFI_CHANNEL_ANY,
		.mfp = WIFI_MFP_OPTIONAL,
	};

	LOG_INF("WiFi connecting to %s", persist.ssid);
	connect_pending = true;
	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p))) {
		LOG_ERR("WiFi connect request failed");
		connect_pending = false;
	}
}

static void wifi_scan(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (iface) {
		struct wifi_scan_params sp = {0};

		net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &sp, sizeof(sp));
	}
}

static void read_serial(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		return;
	}
	struct net_linkaddr *la = net_if_get_link_addr(iface);

	if (la && la->len >= 6) {
		snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", la->addr[0],
			 la->addr[1], la->addr[2], la->addr[3], la->addr[4], la->addr[5]);
	}
}

/* ---------------- NTP ---------------- */
static bool ntp_sync(void)
{
	struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
	struct zsock_addrinfo *res = NULL;

	if (zsock_getaddrinfo(NTP_SERVER, "123", &hints, &res) != 0 || !res) {
		LOG_WRN("NTP DNS failed");
		return false;
	}

	struct sntp_ctx ctx;
	struct sntp_time ts;
	int rc = sntp_init(&ctx, res->ai_addr, res->ai_addrlen);

	zsock_freeaddrinfo(res);
	if (rc < 0) {
		LOG_WRN("sntp_init failed: %d", rc);
		return false;
	}
	rc = sntp_query(&ctx, 4000, &ts);
	sntp_close(&ctx);
	if (rc < 0) {
		LOG_WRN("sntp_query failed: %d", rc);
		return false;
	}
	clock_set_epoch((int64_t)ts.seconds, TZ_OFFSET_S);
	LOG_INF("time synced: %llu", (unsigned long long)ts.seconds);
	return true;
}

/* ---------------- HTTPS ---------------- */
struct resp_ctx {
	char *buf;
	size_t size;
	size_t len;
	int status;
};

static int resp_handler(struct http_response *rsp, enum http_final_call final_data, void *user)
{
	struct resp_ctx *c = user;

	c->status = rsp->http_status_code;
	if (rsp->body_found && rsp->body_frag_start && c->buf) {
		size_t n = MIN(rsp->body_frag_len, c->size - 1 - c->len);

		memcpy(c->buf + c->len, rsp->body_frag_start, n);
		c->len += n;
		c->buf[c->len] = '\0';
	}
	return 0;
}

/* Returns the HTTP status code, or a negative error. */
static int https_request(enum http_method method, const char *path, char *body, size_t body_size)
{
	static uint8_t rx_buf[1024];
	struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *res = NULL;
	int ret;

	if (zsock_getaddrinfo(SERVER_HOST, SERVER_PORT, &hints, &res) != 0 || !res) {
		LOG_WRN("server DNS failed");
		return -EHOSTUNREACH;
	}

	int sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);

	if (sock < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}

	/* NOTE: no CA certificate is installed, so the server is NOT verified.
	 * This is the same as the old Arduino firmware. Add a CA with tls_credential_add()
	 * and use TLS_PEER_VERIFY_REQUIRED + TLS_SEC_TAG_LIST for real security. */
	int verify = TLS_PEER_VERIFY_NONE;

	struct zsock_timeval tmo = {.tv_sec = 10, .tv_usec = 0};

	zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
	zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
	zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, SERVER_HOST, sizeof(SERVER_HOST));

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		ret = -errno;
		zsock_close(sock);
		LOG_WRN("connect failed: %d", ret);
		return ret;
	}

	static const char *headers[] = {"Content-Length: 0\r\n", NULL};
	struct resp_ctx ctx = {.buf = body, .size = body_size, .len = 0, .status = 0};
	struct http_request req = {
		.method = method,
		.url = path,
		.host = SERVER_HOST,
		.protocol = "HTTP/1.1",
		.response = resp_handler,
		.recv_buf = rx_buf,
		.recv_buf_len = sizeof(rx_buf),
	};

	if (method == HTTP_POST) {
		req.optional_headers = headers;
	}
	if (body && body_size) {
		body[0] = '\0';
	}

	ret = http_client_req(sock, &req, 10 * MSEC_PER_SEC, &ctx);
	zsock_close(sock);
	if (ret < 0) {
		LOG_WRN("http request failed: %d", ret);
		return ret;
	}
	return ctx.status;
}

static void tariff_fetch(void)
{
	static char body[2048];
	char path[96];

	snprintf(path, sizeof(path), TARIFF_PATH "%s", serial);
	int st = https_request(HTTP_GET, path, body, sizeof(body));

	tariff_try_after = k_uptime_get() + 15000; /* back off if it fails */
	if (st == 200) {
		if (tariff_apply_json(body, strlen(body))) {
			tariff_requested = false;
			tariff_try_after = k_uptime_get() + RESYNC_MS;
		}
	} else if (st == 403) {
		LOG_WRN("tariff: 403, subscription expired");
		meter_cfg.authorized = false;
		tariff_requested = false;
		tariff_try_after = k_uptime_get() + RESYNC_MS;
	} else {
		LOG_WRN("tariff request failed: %d", st);
	}
}

/* Send the OLDEST record only (strict order). Returns true if the queue is empty or the
 * record was accepted by the server. */
static bool queue_send_head(void)
{
	char url[URL_MAX];

	if (!postq_peek(url, sizeof(url))) {
		return true;
	}

	int st = https_request(HTTP_POST, url, NULL, 0);

	if (st == 200 || st == 301) {
		postq_pop();
		postq_flush(); /* save at once: a power loss now must not send it twice */
		LOG_INF("sent (HTTP %d), %d left", st, postq_count());
		return true;
	}
	LOG_WRN("send failed: %d (record stays in the queue)", st);
	return false;
}

/* ---------------- thread ---------------- */
#define SEND_BACKOFF_MIN_MS 5000
#define SEND_BACKOFF_MAX_MS 120000
#define FLASH_WRITE_IDLE_MS 500 /* write flash only when no key was used for a moment */

static void net_thread(void *a, void *b, void *c)
{
	int64_t last_connect = -CONNECT_RETRY_MS;
	int64_t last_sync = 0;
	int64_t last_scan = 0;
	int64_t last_ntp_try = -5000;
	int64_t send_after = 0;
	int send_fails = 0;
	bool synced = false;
	int wd = wdog_add(150000); /* blocking TLS/DNS calls can take a while */

	while (1) {
		wdog_feed(wd);
		/* wake up for a new record, or every 300 ms */
		k_sem_take(&q_wake, K_MSEC(300));
		int64_t now = k_uptime_get();

		/* 1. save queue changes (flash write can stall other threads for a few ms,
		 *    so do it while the driver is not pressing keys) */
		if (postq_dirty() && key_rest_ms() >= FLASH_WRITE_IDLE_MS) {
			postq_flush();
		}

		if (!connected) {
			synced = false;
			if (!connect_pending && now - last_connect >= CONNECT_RETRY_MS) {
				last_connect = now;
				wifi_connect();
			}
			continue;
		}

		if (serial[0] == '\0') {
			read_serial();
		}

		if ((!synced && now - last_ntp_try >= 5000) || now - last_sync >= RESYNC_MS) {
			last_ntp_try = now;
			if (ntp_sync()) {
				synced = true;
				last_sync = now;
			}
		}
		if (now >= tariff_try_after && (tariff_requested || tariff_try_after == 0)) {
			tariff_fetch();
		} else if (now >= tariff_try_after) {
			tariff_requested = true; /* periodic refresh */
		}

		if (reserve_scan_req && now - last_scan >= 5000) {
			last_scan = now;
			wifi_scan();
		}

		/* 2. upload, oldest first, only when idle; back off when the server fails */
		if (postq_count() > 0 && now >= send_after && key_rest_ms() >= IDLE_BEFORE_SEND_MS) {
			if (queue_send_head()) {
				send_fails = 0;
				send_after = 0; /* next record right away */
			} else {
				int64_t wait = SEND_BACKOFF_MIN_MS << MIN(send_fails, 5);

				send_fails++;
				send_after = now + MIN(wait, SEND_BACKOFF_MAX_MS);
			}
		}
	}
}

K_THREAD_DEFINE(net_tid, 6144, net_thread, NULL, NULL, NULL, 9, 0, 1500);

void net_init(void)
{
	net_mgmt_init_event_callback(&wifi_cb, wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT |
					     NET_EVENT_WIFI_SCAN_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ip_cb, ip_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ip_cb);
}

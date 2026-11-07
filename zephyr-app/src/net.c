/*
 * WiFi, NTP clock, tariff download and the trip/service upload queue.
 * One thread (`net`) does all blocking network work. The UI never waits for it.
 */
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
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

LOG_MODULE_REGISTER(net, LOG_LEVEL_INF);

#define SERVER_HOST "node.raxitaxi.ir"
#define SERVER_PORT "443"
#define TARIFF_PATH "/api/taxi/TaxiFare?driver_type=1&serial="
#define NTP_SERVER "ntp.day.ir"
#define TZ_OFFSET_S 12600 /* Iran, +03:30, no DST */

#define QUEUE_LEN 10
#define URL_MAX 384
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

/* ---------------- post queue ---------------- */
static char queue[QUEUE_LEN][URL_MAX];
static K_MUTEX_DEFINE(q_lock);

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
	bool ok = false;

	k_mutex_lock(&q_lock, K_FOREVER);
	for (int i = 0; i < QUEUE_LEN; i++) {
		if (queue[i][0] == '\0') {
			strncpy(queue[i], path_query, URL_MAX - 1);
			queue[i][URL_MAX - 1] = '\0';
			ok = true;
			LOG_INF("queued in slot %d", i);
			break;
		}
	}
	k_mutex_unlock(&q_lock);
	if (!ok) {
		LOG_ERR("post queue full, dropped");
	}
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

	clock_get_hm(&h, &m);
	/* trip_price, traffic_price, night_price, speeds were placeholders (0) in the old code */
	snprintf(url, sizeof(url),
		 "/api/taxi/SetTaxiEnd?driver_id=%s&serial=%s&bt_number=%d&start_time=%s"
		 "&stop_time=%02d:%02d&trip_distance=%d&trip_duration=%d&traffic_distance=0"
		 "&traffic_duration=%d&avg_speed=0&max_speed=0&trip_price=0&traffic_price=0"
		 "&night_price=0&total_price=%lld",
		 serial, serial, bt, start_hhmm, h, m, s->dist_m, s->duration_s, s->wait_s,
		 (long long)s->total_rial);
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

static void queue_send_one(void)
{
	static int idx;
	char url[URL_MAX];

	k_mutex_lock(&q_lock, K_FOREVER);
	strncpy(url, queue[idx], sizeof(url));
	k_mutex_unlock(&q_lock);

	if (url[0] != '\0') {
		int st = https_request(HTTP_POST, url, NULL, 0);

		if (st == 200 || st == 301) {
			k_mutex_lock(&q_lock, K_FOREVER);
			queue[idx][0] = '\0';
			k_mutex_unlock(&q_lock);
			LOG_INF("sent slot %d (HTTP %d)", idx, st);
		} else {
			LOG_WRN("send failed slot %d: %d", idx, st);
		}
	}
	idx = (idx + 1) % QUEUE_LEN;
}

/* ---------------- thread ---------------- */
static void net_thread(void *a, void *b, void *c)
{
	int64_t last_connect = -CONNECT_RETRY_MS;
	int64_t last_sync = 0;
	int64_t last_scan = 0;
	int64_t last_ntp_try = -5000;
	bool synced = false;

	while (1) {
		int64_t now = k_uptime_get();

		if (!connected) {
			synced = false;
			if (!connect_pending && now - last_connect >= CONNECT_RETRY_MS) {
				last_connect = now;
				wifi_connect();
			}
			k_sleep(K_MSEC(500));
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

		if (key_rest_ms() >= IDLE_BEFORE_SEND_MS) {
			queue_send_one();
		}
		k_sleep(K_MSEC(300));
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

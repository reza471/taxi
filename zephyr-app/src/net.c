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
#include "trip_store.h"
#include "report.h"
#include "urlq.h"
#include "ota.h"
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
static volatile int server_state;

/* ---------------- post queue: see postq.c ---------------- */
static K_SEM_DEFINE(q_wake, 0, 1);

/* ---------------- reserve scan ---------------- */
static volatile bool reserve_scan_req;
static volatile bool reserve_found;

int net_server_state(void)
{
	return server_state;
}

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
	urlq_t q;

	/* "?&serial=" is how the old firmware wrote it: keep it, the server may depend on it */
	urlq_init(&q, url, sizeof(url), "/api/taxi/StartService?&");
	urlq_str(&q, "serial", serial);
	urlq_int(&q, "serviceNum", service_number);
	urlq_int(&q, "start", start);
	if (urlq_ok(&q)) {
		net_post(url);
	} else {
		LOG_ERR("service record too long, NOT queued");
	}
}

void net_trip_start(int bt)
{
	char url[URL_MAX];
	urlq_t q;

	urlq_init(&q, url, sizeof(url), "/api/taxi/SetTaxiStart?");
	urlq_str(&q, "serial", serial);
	urlq_int(&q, "bt_number", bt);
	if (urlq_ok(&q)) {
		net_post(url);
	} else {
		LOG_ERR("trip start record too long, NOT queued");
	}
}

void trip_submit(int bt, const meter_snap_t *s, const char *start_hhmm)
{
	char url[URL_MAX];
	urlq_t q;
	int h, m;
	char stop[8];

	clock_get_hm(&h, &m);
	snprintf(stop, sizeof(stop), "%02d:%02d", h % 100, m % 100);

	/* trip_price = route price, night_price = night part of it, traffic_price = waiting.
	 * traffic_distance is not measured yet (0). */
	urlq_init(&q, url, sizeof(url), "/api/taxi/SetTaxiEnd?");
	urlq_str(&q, "driver_id", serial);
	urlq_str(&q, "serial", serial);
	urlq_int(&q, "bt_number", bt);
	urlq_str(&q, "start_time", start_hhmm);
	urlq_str(&q, "stop_time", stop);
	urlq_int(&q, "trip_distance", s->dist_m);
	urlq_int(&q, "trip_duration", s->duration_s);
	urlq_int(&q, "traffic_distance", 0);
	urlq_int(&q, "traffic_duration", s->wait_s);
	urlq_int(&q, "avg_speed", s->avg_speed_kmh);
	urlq_int(&q, "max_speed", s->max_speed_kmh);
	urlq_int(&q, "trip_price", s->trip_price);
	urlq_int(&q, "traffic_price", s->traffic_price);
	urlq_int(&q, "night_price", s->night_price);
	urlq_int(&q, "total_price", s->total_rial);
	if (!urlq_ok(&q)) {
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
static int net_wd = -1; /* watchdog channel of the net thread */

struct resp_ctx {
	char *buf;
	size_t size;
	size_t len;
	int status;
	net_sink_t sink; /* body goes to the sink (download) instead of the buffer */
	void *sink_user;
	int sock;
	bool aborted;
};

static int resp_handler(struct http_response *rsp, enum http_final_call final_data, void *user)
{
	struct resp_ctx *c = user;

	c->status = rsp->http_status_code;
	wdog_feed(net_wd); /* a long download keeps the watchdog happy while data arrives */

	if (!rsp->body_found || !rsp->body_frag_start || c->aborted) {
		return 0;
	}
	if (c->sink) {
		if (rsp->http_status_code == 200 && rsp->body_frag_len > 0 &&
		    c->sink((const uint8_t *)rsp->body_frag_start, rsp->body_frag_len,
			    c->sink_user) < 0) {
			/* the sink wants to stop: close the socket, the request ends with an error */
			c->aborted = true;
			if (c->sock >= 0) {
				zsock_close(c->sock);
				c->sock = -1;
			}
		}
	} else if (c->buf) {
		size_t n = MIN(rsp->body_frag_len, c->size - 1 - c->len);

		memcpy(c->buf + c->len, rsp->body_frag_start, n);
		c->len += n;
		c->buf[c->len] = '\0';
	}
	return 0;
}

/* Returns the HTTP status code, or a negative error. `api` = this is the tariff server
 * (its answer decides the HOST UP/BAD text). */
static int https_do(enum http_method method, const char *host, uint16_t port, const char *path,
		    char *body, size_t body_size, net_sink_t sink, void *sink_user,
		    int timeout_ms, bool api)
{
	static uint8_t rx_buf[1024];
	struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *res = NULL;
	char port_str[8];
	int ret;

	snprintf(port_str, sizeof(port_str), "%u", port);
	if (zsock_getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
		LOG_WRN("DNS failed for %s", host);
		if (api) {
			server_state = -1;
		}
		return -EHOSTUNREACH;
	}

	int sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);

	if (sock < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}

	/* NOTE: no CA certificate is installed, so the server is NOT verified.
	 * This is the same as the old Arduino firmware. Add a CA with tls_credential_add()
	 * and use TLS_PEER_VERIFY_REQUIRED + TLS_SEC_TAG_LIST for real security.
	 * (A firmware download is safe anyway: MCUboot checks the signature of the image.) */
	int verify = TLS_PEER_VERIFY_NONE;

	struct zsock_timeval tmo = {.tv_sec = 10, .tv_usec = 0};

	zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
	zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
	zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, host, strlen(host) + 1);

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		ret = -errno;
		zsock_close(sock);
		LOG_WRN("connect to %s failed: %d", host, ret);
		if (api) {
			server_state = -1;
		}
		return ret;
	}

	static const char *headers[] = {"Content-Length: 0\r\n", NULL};
	struct resp_ctx ctx = {.buf = body, .size = body_size, .len = 0, .status = 0,
			       .sink = sink, .sink_user = sink_user, .sock = sock, .aborted = false};
	struct http_request req = {
		.method = method,
		.url = path,
		.host = host,
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

	ret = http_client_req(sock, &req, timeout_ms, &ctx);
	if (ctx.sock >= 0) {
		zsock_close(ctx.sock); /* not closed by the sink */
	}
	if (ctx.aborted) {
		return -ECANCELED;
	}
	if (ret < 0) {
		LOG_WRN("http request failed: %d", ret);
		if (api) {
			server_state = -1;
		}
		return ret;
	}
	if (api) {
		server_state = 1;
	}
	return ctx.status;
}

static int https_request(enum http_method method, const char *path, char *body, size_t body_size)
{
	return https_do(method, SERVER_HOST, 443, path, body, body_size, NULL, NULL,
			10 * MSEC_PER_SEC, true);
}

int net_download(const char *host, uint16_t port, const char *path, net_sink_t sink, void *user)
{
	return https_do(HTTP_GET, host, port, path, NULL, 0, sink, user, 5 * 60 * MSEC_PER_SEC, false);
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
#define TRIP_CHECKPOINT_MS 10000 /* save the running trip this often (flash wear vs. lost fare) */

static void net_thread(void *a, void *b, void *c)
{
	int64_t last_connect = -CONNECT_RETRY_MS;
	int64_t last_sync = 0;
	int64_t last_scan = 0;
	int64_t last_ntp_try = -5000;
	int64_t send_after = 0;
	int64_t last_checkpoint = 0;
	bool checkpoint_active = false; /* the flash holds a running trip */
	int send_fails = 0;
	bool synced = false;
	int wd = wdog_add(150000); /* blocking TLS/DNS calls can take a while */

	net_wd = wd;
	ota_boot_check();

	while (1) {
		wdog_feed(wd);
		/* wake up for a new record, or every 300 ms */
		k_sem_take(&q_wake, K_MSEC(300));
		int64_t now = k_uptime_get();

		/* 1. save queue changes (flash write can stall other threads for a few ms,
		 *    so do it while the driver is not pressing keys) */
		if ((postq_dirty() || report_dirty()) && key_rest_ms() >= FLASH_WRITE_IDLE_MS) {
			postq_flush();
			report_flush();
		}

		/* 1b. running trip -> flash, so a power loss does not lose the fare */
		bool running = meter_any_active();

		if (running && now - last_checkpoint >= TRIP_CHECKPOINT_MS &&
		    key_rest_ms() >= FLASH_WRITE_IDLE_MS) {
			trip_state_t st;

			meter_export(&st);
			if (trip_store_save(&st)) {
				checkpoint_active = true;
				last_checkpoint = now;
			}
		} else if (!running && checkpoint_active) {
			/* trip ended: save the trip record first, then clear the running trip.
			 * Done at once, even while keys are used, so an ended trip never comes back. */
			postq_flush();
			report_flush();
			trip_state_t st;

			meter_export(&st);
			if (trip_store_save(&st)) {
				checkpoint_active = false;
			}
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

		/* firmware: confirm a test image, download an offered one, reboot into it */
		ota_poll();

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

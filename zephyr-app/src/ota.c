/*
 * Firmware update over HTTPS (needs MCUboot in swap mode, see Kconfig TAXI_OTA).
 *
 * Flow:
 *   1. The tariff JSON carries "fw_version" and "fw_url". A newer version that did not fail
 *      before is downloaded into slot 1 (flash_img, progressive erase).
 *   2. The image header is checked while the first bytes arrive (MCUboot magic, version equals
 *      the offer, size fits the slot). A wrong file stops the download at once.
 *   3. boot_request_upgrade(TEST): MCUboot checks the SIGNATURE at the next boot and swaps.
 *      An unsigned or damaged image is refused by MCUboot, the old one keeps running.
 *   4. The new image must reach the server (HOST UP) and run 2 minutes, then it confirms itself.
 *      Any reset before that (crash, watchdog, power) makes MCUboot swap the old image back,
 *      and the failed version is remembered, so it is not tried again.
 *
 * Safety rules for a taxi meter:
 *   - never while a trip is running, never while a key was used in the last minute
 *   - the download stops at once when the driver touches a key or starts a trip
 *   - the reboot waits until the meter is idle (running trips are saved first, see net.c)
 */
#include "ota.h"
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "hw.h"
#include "meter.h"
#include "net.h"
#include "version.h"

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

#ifndef CONFIG_TAXI_OTA

/* no MCUboot in this build: everything is a no-op */
void ota_offer(const char *version, const char *url) { ARG_UNUSED(version); ARG_UNUSED(url); }
void ota_boot_check(void) {}
void ota_poll(void) {}
ota_state_t ota_status(int *percent)
{
	if (percent) {
		*percent = 0;
	}
	return OTA_DISABLED;
}

#else

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include "ota_logic.h"
#include "postq.h"
#include "report.h"

#define SLOT1_SIZE FIXED_PARTITION_SIZE(slot1_partition)
#define IDLE_BEFORE_DOWNLOAD_MS (60 * 1000)
#define IDLE_BEFORE_REBOOT_MS (10 * 1000)
#define STOP_IF_KEY_MS 2000 /* a key in the last 2 s stops the download */
#define FIRST_TRY_AFTER_BOOT_MS (10 * 60 * 1000)
#define CONFIRM_AFTER_MS (2 * 60 * 1000)
#define MAX_ATTEMPTS 3

/* ---- what is kept in flash ---- */
typedef struct {
	char pending[OTA_VER_TEXT]; /* version we asked MCUboot to install */
	char failed[OTA_VER_TEXT];  /* version that failed here, never tried again */
	uint8_t attempts;           /* downloads started for `pending` / the offered version */
	char attempt_ver[OTA_VER_TEXT];
} ota_rec_t;

static ota_rec_t rec;

static int ota_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "r") == 0 && len == sizeof(rec)) {
		ota_rec_t tmp;

		if (read_cb(cb_arg, &tmp, sizeof(tmp)) == sizeof(tmp)) {
			tmp.pending[OTA_VER_TEXT - 1] = '\0';
			tmp.failed[OTA_VER_TEXT - 1] = '\0';
			tmp.attempt_ver[OTA_VER_TEXT - 1] = '\0';
			rec = tmp;
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ota, "ota", NULL, ota_set, NULL, NULL);

static void rec_save(void)
{
	if (settings_save_one("ota/r", &rec, sizeof(rec)) != 0) {
		LOG_ERR("ota record save failed");
	}
}

/* ---- state ---- */
static struct {
	bool have;
	char ver[OTA_VER_TEXT];
	char url[192];
} offer;

static ota_state_t state = OTA_IDLE;
static volatile int percent;
static int64_t next_try;
static int fails;
static ota_ver_t running;

void ota_offer(const char *version, const char *url)
{
	ota_ver_t v;

	if (!version || !url || !ota_parse_version(version, &v) || strlen(url) >= sizeof(offer.url)) {
		offer.have = false;
		return;
	}
	char host[OTA_HOST_MAX], path[OTA_PATH_MAX];
	uint16_t port;

	if (!ota_url_parse(url, host, sizeof(host), &port, path, sizeof(path))) {
		LOG_WRN("firmware offer ignored: bad URL");
		offer.have = false;
		return;
	}
	ota_ver_str(&v, offer.ver, sizeof(offer.ver));
	snprintf(offer.url, sizeof(offer.url), "%s", url);
	offer.have = true;
}

ota_state_t ota_status(int *pct)
{
	if (pct) {
		*pct = percent;
	}
	return state;
}

void ota_boot_check(void)
{
	if (!ota_parse_version(FW_VERSION, &running)) {
		LOG_ERR("own version '%s' is not valid", FW_VERSION);
		return;
	}
	if (rec.pending[0]) {
		ota_ver_t p;

		if (ota_parse_version(rec.pending, &p) && ota_ver_cmp(&p, &running) != 0) {
			/* we asked for `pending` but another version runs: MCUboot went back (or refused) */
			LOG_ERR("update to %s did not stick, it is blocked from now on", rec.pending);
			memcpy(rec.failed, rec.pending, sizeof(rec.failed));
			rec.pending[0] = '\0';
			rec_save();
			state = OTA_FAILED;
		}
	}
	if (!boot_is_img_confirmed()) {
		state = OTA_TESTING;
		LOG_WRN("running a NEW firmware (%s) on test: it must reach the server within a few minutes",
			FW_VERSION);
	}
}

/* ---- download ---- */
struct dl {
	struct flash_img_context fic;
	uint8_t head[OTA_HEADER_MIN];
	size_t head_len;
	bool head_ok;
	bool started;
	size_t total;
	uint32_t expect; /* header + image size, the TLV (signature) comes after */
	ota_ver_t want;
	const char *stop_reason;
};

static struct dl dl;

static bool driver_is_busy(int idle_ms)
{
	return meter_any_active() || key_rest_ms() < idle_ms;
}

static int sink(const uint8_t *data, size_t len, void *user)
{
	struct dl *d = user;

	if (driver_is_busy(STOP_IF_KEY_MS)) {
		d->stop_reason = "driver is using the meter";
		return -1;
	}

	/* collect the first 32 bytes, check them, then write everything */
	if (!d->head_ok) {
		size_t take = MIN(len, sizeof(d->head) - d->head_len);

		memcpy(d->head + d->head_len, data, take);
		d->head_len += take;
		if (d->head_len < sizeof(d->head)) {
			return 0;
		}
		ota_ver_t got;
		uint32_t img_size;

		if (!ota_header_parse(d->head, sizeof(d->head), &got, &img_size)) {
			d->stop_reason = "not an MCUboot image";
			return -2;
		}
		if (ota_ver_cmp(&got, &d->want) != 0) {
			d->stop_reason = "image version is not the offered one";
			return -2;
		}
		if (img_size > SLOT1_SIZE - 4096) {
			d->stop_reason = "image does not fit the slot";
			return -2;
		}
		d->expect = (uint32_t)(d->head[8] | (d->head[9] << 8)) + img_size;
		d->head_ok = true;
		if (flash_img_init(&d->fic) != 0) {
			d->stop_reason = "flash init failed";
			return -3;
		}
		d->started = true;
		if (flash_img_buffered_write(&d->fic, d->head, sizeof(d->head), false) != 0) {
			d->stop_reason = "flash write failed";
			return -3;
		}
		d->total = sizeof(d->head);
		data += take;
		len -= take;
		if (len == 0) {
			return 0;
		}
	}

	if (flash_img_buffered_write(&d->fic, data, len, false) != 0) {
		d->stop_reason = "flash write failed";
		return -3;
	}
	d->total += len;
	if (d->expect) {
		percent = (int)MIN(99, (uint64_t)d->total * 100 / d->expect);
	}
	return 0;
}

static void fail(const char *why, bool block_version)
{
	LOG_ERR("update stopped: %s", why);
	state = OTA_FAILED;
	fails++;
	next_try = k_uptime_get() + MIN((int64_t)3600 * 1000 << MIN(fails, 5), (int64_t)24 * 3600 * 1000);
	if (block_version) {
		memcpy(rec.failed, offer.ver, sizeof(rec.failed));
		rec_save();
	}
}

static void do_download(void)
{
	char host[OTA_HOST_MAX], path[OTA_PATH_MAX];
	uint16_t port;
	ota_ver_t want;

	if (!ota_parse_version(offer.ver, &want) ||
	    !ota_url_parse(offer.url, host, sizeof(host), &port, path, sizeof(path))) {
		return;
	}

	/* count the attempt first: a crash during the download must not loop for ever */
	if (strcmp(rec.attempt_ver, offer.ver) != 0) {
		memset(rec.attempt_ver, 0, sizeof(rec.attempt_ver));
		memcpy(rec.attempt_ver, offer.ver, sizeof(rec.attempt_ver) - 1);
		rec.attempts = 0;
	}
	if (rec.attempts >= MAX_ATTEMPTS) {
		LOG_ERR("update %s was tried %d times, giving up", offer.ver, rec.attempts);
		memcpy(rec.failed, offer.ver, sizeof(rec.failed));
		rec_save();
		state = OTA_FAILED;
		return;
	}
	rec.attempts++;
	rec_save();

	memset(&dl, 0, sizeof(dl));
	dl.want = want;
	percent = 0;
	state = OTA_DOWNLOADING;
	LOG_INF("downloading firmware %s from %s", offer.ver, host);

	int st = net_download(host, port, path, sink, &dl);

	if (st != 200) {
		/* stopped by the sink, by the network or by the server */
		bool bad_file = dl.stop_reason && (strstr(dl.stop_reason, "image") || strstr(dl.stop_reason, "MCUboot"));

		if (dl.stop_reason && strstr(dl.stop_reason, "driver")) {
			state = OTA_IDLE; /* not a failure: try again later when idle */
			rec.attempts--;   /* does not count */
			rec_save();
			next_try = k_uptime_get() + 10 * 60 * 1000;
			LOG_INF("update paused: driver is using the meter");
			return;
		}
		char why[64];

		snprintf(why, sizeof(why), "%s (HTTP/net %d)", dl.stop_reason ? dl.stop_reason : "download failed", st);
		fail(why, bad_file);
		return;
	}
	if (!dl.started || flash_img_buffered_write(&dl.fic, NULL, 0, true) != 0) {
		fail("could not finish the flash write", false);
		return;
	}
	if (dl.expect == 0 || dl.total < dl.expect) {
		fail("file is shorter than its header says", true);
		return;
	}
	if (boot_request_upgrade(BOOT_UPGRADE_TEST) != 0) {
		fail("MCUboot did not accept the request", false);
		return;
	}
	memset(rec.pending, 0, sizeof(rec.pending));
	memcpy(rec.pending, offer.ver, sizeof(rec.pending) - 1);
	rec_save();
	percent = 100;
	state = OTA_READY;
	LOG_WRN("firmware %s downloaded (%u bytes), installing when the meter is idle", offer.ver,
		(unsigned)dl.total);
}

static void maybe_reboot(void)
{
	if (driver_is_busy(IDLE_BEFORE_REBOOT_MS)) {
		return;
	}
	/* save what must not be lost, then restart. MCUboot checks the signature and swaps. */
	postq_flush();
	report_flush();
	LOG_WRN("rebooting into the new firmware");
	k_sleep(K_MSEC(300));
	sys_reboot(SYS_REBOOT_COLD);
}

static void maybe_confirm(int64_t now)
{
	/* the new image proves itself: server answered + ran for a while without a reset */
	if (now < CONFIRM_AFTER_MS || net_server_state() != 1) {
		return;
	}
	if (boot_write_img_confirmed() == 0) {
		LOG_WRN("new firmware %s confirmed", FW_VERSION);
		rec.pending[0] = '\0';
		rec.attempts = 0;
		memset(rec.attempt_ver, 0, sizeof(rec.attempt_ver));
		rec_save();
		state = OTA_IDLE;
	}
}

void ota_poll(void)
{
	int64_t now = k_uptime_get();

	switch (state) {
	case OTA_TESTING:
		maybe_confirm(now);
		return;
	case OTA_READY:
		maybe_reboot();
		return;
	case OTA_DOWNLOADING:
		return;
	default:
		break;
	}

	if (!offer.have || now < FIRST_TRY_AFTER_BOOT_MS || now < next_try) {
		return;
	}
	ota_ver_t want;

	if (!ota_parse_version(offer.ver, &want) || !ota_should_download(&running, &want, rec.failed)) {
		return;
	}
	if (driver_is_busy(IDLE_BEFORE_DOWNLOAD_MS)) {
		return;
	}
	do_download();
}

#endif /* CONFIG_TAXI_OTA */

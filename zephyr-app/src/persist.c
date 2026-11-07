#include "persist.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(persist, LOG_LEVEL_INF);

#define PERSIST_MAGIC 0x54415831U /* "TAX1" */
#define DEFAULT_SSID "Rad"
#define DEFAULT_PASS "123456789"

persist_t persist;

static int persist_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "cfg") == 0 && len == sizeof(persist_t)) {
		persist_t tmp;

		if (read_cb(cb_arg, &tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == PERSIST_MAGIC) {
			persist = tmp;
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(taxi, "taxi", NULL, persist_set, NULL, NULL);

static void apply(void)
{
	meter_set_config(&persist.t1, &persist.t4, &persist.cfg);
}

void persist_init(void)
{
	/* defaults = what the meter module has at boot */
	memset(&persist, 0, sizeof(persist));
	persist.magic = PERSIST_MAGIC;
	persist.t1 = meter_tariff1;
	persist.t4 = meter_tariff4;
	persist.cfg = meter_cfg;
	strncpy(persist.ssid, DEFAULT_SSID, sizeof(persist.ssid) - 1);
	strncpy(persist.pass, DEFAULT_PASS, sizeof(persist.pass) - 1);

	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings init failed: %d", err);
		return;
	}
	settings_load();

	if (persist.ssid[0] == '\0') {
		strncpy(persist.ssid, DEFAULT_SSID, sizeof(persist.ssid) - 1);
		strncpy(persist.pass, DEFAULT_PASS, sizeof(persist.pass) - 1);
	}
	persist.cfg.authorized = true; /* only the server can expire it, as before */
	apply();
	LOG_INF("settings loaded (ssid=%s, PM=%d)", persist.ssid, (int)persist.t1.pm);
}

void persist_save(void)
{
	int err = settings_save_one("taxi/cfg", &persist, sizeof(persist));

	if (err) {
		LOG_ERR("settings save failed: %d", err);
	}
}

void persist_reset_wifi(void)
{
	memset(persist.ssid, 0, sizeof(persist.ssid));
	memset(persist.pass, 0, sizeof(persist.pass));
	strncpy(persist.ssid, DEFAULT_SSID, sizeof(persist.ssid) - 1);
	strncpy(persist.pass, DEFAULT_PASS, sizeof(persist.pass) - 1);
	persist_save();
}

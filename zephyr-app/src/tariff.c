/*
 * Server tariff JSON -> tariffs.
 *
 * Safety rule: a price must never come from half-understood data.
 *   - broken JSON, missing PM or any number outside its allowed range: the whole update is refused
 *   - numbers may be written as 46, 46.0, "46" or true/false (flags); null counts as "missing"
 *   - a missing price key means 0 (same as the old firmware)
 *   - a bad WiFi name/password is ignored, the prices are still applied
 */
#include "tariff.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "json_lite.h"
#include "persist.h"

LOG_MODULE_REGISTER(tariff, LOG_LEVEL_INF);

enum {
	F_PM, F_PRI, F_START_NIGHT, F_STOP_NIGHT, F_C200, F_CSTOP, F_MAXSTOP, F_FREE_TRIP, F_FREE_TIME,
	F_PCT_NIGHT, F_FRACTION, F_PRI2, F_C2_200, F_C2_STOP, F_SAMAR, F_DARBAST, F_KORSI, F_NOBAT,
	F_TRIP3, F_C3_200, F_C3_STOP, F_COUNT
};

static const struct {
	const char *key;
	int64_t min, max;
} fields[F_COUNT] = {
	[F_PM] = {"PM", 1, 20000},
	[F_PRI] = {"pri_cost", 0, 99999999},
	[F_START_NIGHT] = {"Start_night", 0, 23},
	[F_STOP_NIGHT] = {"Stop_night", 0, 23},
	[F_C200] = {"Cost_200", 0, 99999999},
	[F_CSTOP] = {"Cost_stop", 0, 99999999},
	[F_MAXSTOP] = {"Max_stop_km", 0, 250},
	[F_FREE_TRIP] = {"free_trip", 0, 1000000},
	[F_FREE_TIME] = {"free_time", 0, 86400},
	[F_PCT_NIGHT] = {"Percent_night", 0, 100},
	[F_FRACTION] = {"fraction", 0, 1000000},
	[F_PRI2] = {"pri2_cost", 0, 99999999},
	[F_C2_200] = {"Cost2_200", 0, 99999999},
	[F_C2_STOP] = {"Cost2_stop", 0, 99999999},
	[F_SAMAR] = {"samar", 0, 1000000},
	[F_DARBAST] = {"darbast", 0, 1000000},
	[F_KORSI] = {"korsi", 0, 1000000},
	[F_NOBAT] = {"nobat", 0, 1000000},
	[F_TRIP3] = {"trip3", 0, 100000000},
	[F_C3_200] = {"Cost3_200", 0, 99999999},
	[F_C3_STOP] = {"Cost3_stop", 0, 99999999},
};

struct collect {
	int64_t v[F_COUNT];
	bool have[F_COUNT];
	bool range_error;
	int32_t service[SERVICE_MAX];
	int service_n;
	char ssid[WIFI_SSID_MAX];
	char pass[WIFI_PASS_MAX];
	bool have_ssid, have_pass, ssid_bad, pass_bad;
};

static void on_value(const char *path, int idx, const jl_val_t *val, void *user)
{
	struct collect *c = user;

	/* accept the numbers also inside a wrapper object */
	if (strncmp(path, "taxiFare.", 9) == 0) {
		path += 9;
	} else if (strncmp(path, "data.", 5) == 0) {
		path += 5;
	}

	if (strcmp(path, "service") == 0 && idx >= 0) {
		int64_t n;

		if (jl_as_int(val, &n) && n >= 0 && n <= 999999 && c->service_n < SERVICE_MAX) {
			c->service[c->service_n++] = (int32_t)n; /* more than 10 entries: keep the first 10 */
		}
		return;
	}
	if (idx >= 0) {
		return;
	}
	if (strcmp(path, "wifi") == 0 && val->type == JL_STR) {
		size_t n = strlen(val->str);

		if (n >= 1 && n < WIFI_SSID_MAX && !val->str_cut) {
			memcpy(c->ssid, val->str, n + 1);
			c->have_ssid = true;
		} else {
			c->ssid_bad = true;
		}
		return;
	}
	if (strcmp(path, "wifiPass") == 0 && val->type == JL_STR) {
		size_t n = strlen(val->str);

		if (n < WIFI_PASS_MAX && !val->str_cut) {
			memcpy(c->pass, val->str, n + 1);
			c->have_pass = true;
		} else {
			c->pass_bad = true;
		}
		return;
	}
	for (int f = 0; f < F_COUNT; f++) {
		if (strcmp(path, fields[f].key) != 0) {
			continue;
		}
		if (val->type == JL_NULL) {
			return; /* null = missing */
		}
		int64_t n;

		if (!jl_as_int(val, &n)) {
			if (val->type == JL_NUM) {
				c->range_error = true; /* number too big */
			}
			return; /* text that is not a number: treated as missing */
		}
		if (n < fields[f].min || n > fields[f].max) {
			LOG_WRN("tariff: %s=%lld is out of range", fields[f].key, (long long)n);
			c->range_error = true;
			return;
		}
		c->v[f] = n;
		c->have[f] = true;
		return;
	}
}

static void fill_common(fare_tariff_t *t, const struct collect *c)
{
	memset(t, 0, sizeof(*t));
	t->pm = (int32_t)c->v[F_PM];
	t->start_night = (int32_t)c->v[F_START_NIGHT];
	t->stop_night = (int32_t)c->v[F_STOP_NIGHT];
	t->percent_night = (int32_t)c->v[F_PCT_NIGHT];
	t->max_stop_kmh = (int32_t)c->v[F_MAXSTOP];
	t->free_trip_m = (int32_t)c->v[F_FREE_TRIP];
	t->free_time_s = (int32_t)c->v[F_FREE_TIME];
	t->fraction = (int32_t)c->v[F_FRACTION];
}

bool tariff_apply_json(const char *json, size_t len)
{
	static struct collect c; /* too big for the net thread stack */

	memset(&c, 0, sizeof(c));
	int r = jl_parse(json, len, on_value, &c);

	if (r < 0) {
		LOG_ERR("tariff JSON error %d, update refused", r);
		return false;
	}
	if (c.range_error) {
		LOG_ERR("tariff has a value out of range, update refused");
		return false;
	}
	if (!c.have[F_PM]) {
		LOG_ERR("tariff has no PM, update refused");
		return false;
	}

	persist_t before = persist;

	fill_common(&persist.t1, &c);
	persist.t1.pri_cost = (int32_t)c.v[F_PRI];
	persist.t1.trip3_pulses = (int32_t)c.v[F_TRIP3];
	persist.t1.rate.cost_200 = (int32_t)c.v[F_C200];
	persist.t1.rate.cost_stop = (int32_t)c.v[F_CSTOP];
	persist.t1.rate3.cost_200 = (int32_t)c.v[F_C3_200];
	persist.t1.rate3.cost_stop = (int32_t)c.v[F_C3_STOP];

	fill_common(&persist.t4, &c);
	persist.t4.pri_cost = (int32_t)c.v[F_PRI2];
	persist.t4.rate.cost_200 = (int32_t)c.v[F_C2_200];
	persist.t4.rate.cost_stop = (int32_t)c.v[F_C2_STOP];
	persist.t4.rate3 = persist.t4.rate; /* tariff 3 is not used in 4-channel mode */

	persist.cfg.authorized = true;
	persist.cfg.samar = c.v[F_SAMAR] != 0;
	persist.cfg.darbast = c.v[F_DARBAST] != 0;
	persist.cfg.korsi = c.v[F_KORSI] != 0;
	persist.cfg.nobat = c.v[F_NOBAT] != 0;

	persist.service_count = c.service_n;
	memset(persist.service, 0, sizeof(persist.service));
	for (int i = 0; i < c.service_n; i++) {
		persist.service[i] = c.service[i];
	}

	if (c.ssid_bad || c.pass_bad) {
		LOG_WRN("tariff: WiFi name/password has a bad length, WiFi settings ignored");
	} else if (c.have_ssid) {
		memcpy(persist.ssid, c.ssid, sizeof(persist.ssid));
		if (c.have_pass) {
			memcpy(persist.pass, c.pass, sizeof(persist.pass));
		} else {
			memset(persist.pass, 0, sizeof(persist.pass)); /* open network */
		}
	}

	meter_set_config(&persist.t1, &persist.t4, &persist.cfg);
	if (memcmp(&before, &persist, sizeof(persist)) != 0) {
		persist_save(); /* only write flash when something really changed */
	}
	LOG_INF("tariff accepted: PM=%d pri=%d pri2=%d services=%d", (int)c.v[F_PM],
		(int)c.v[F_PRI], (int)c.v[F_PRI2], c.service_n);
	return true;
}

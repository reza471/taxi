#include "tariff.h"
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include "persist.h"

LOG_MODULE_REGISTER(tariff, LOG_LEVEL_INF);

/* Field names are the same as the keys in the server JSON. */
struct tariff_json {
	int32_t PM;
	int32_t pri_cost;
	int32_t Start_night;
	int32_t Stop_night;
	int32_t Cost_200;
	int32_t Cost_stop;
	int32_t Max_stop_km;
	int32_t free_trip;
	int32_t free_time;
	int32_t Percent_night;
	int32_t fraction;
	int32_t taxi;
	int32_t pri2_cost;
	int32_t Cost2_200;
	int32_t Cost2_stop;
	int32_t samar;
	int32_t darbast;
	int32_t korsi;
	int32_t nobat;
	int32_t trip3;
	int32_t Cost3_200;
	int32_t Cost3_stop;
	const char *wifi;
	const char *wifiPass;
	int32_t service[SERVICE_MAX];
	size_t service_len;
};

static const struct json_obj_descr tariff_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct tariff_json, PM, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, pri_cost, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Start_night, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Stop_night, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Cost_200, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Cost_stop, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Max_stop_km, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, free_trip, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, free_time, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Percent_night, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, fraction, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, taxi, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, pri2_cost, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Cost2_200, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Cost2_stop, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, samar, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, darbast, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, korsi, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, nobat, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, trip3, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Cost3_200, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, Cost3_stop, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, wifi, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct tariff_json, wifiPass, JSON_TOK_STRING),
	JSON_OBJ_DESCR_ARRAY(struct tariff_json, service, SERVICE_MAX, service_len, JSON_TOK_NUMBER),
};

static void fill_common(fare_tariff_t *t, const struct tariff_json *j)
{
	memset(t, 0, sizeof(*t));
	t->pm = j->PM;
	t->start_night = j->Start_night;
	t->stop_night = j->Stop_night;
	t->percent_night = j->Percent_night;
	t->max_stop_kmh = j->Max_stop_km;
	t->free_trip_m = j->free_trip;
	t->free_time_s = j->free_time;
	t->fraction = j->fraction;
}

bool tariff_apply_json(char *json, size_t len)
{
	static struct tariff_json j; /* static: too big for the net thread stack */

	memset(&j, 0, sizeof(j));
	int64_t ret = json_obj_parse(json, len, tariff_descr, ARRAY_SIZE(tariff_descr), &j);

	if (ret < 0) {
		LOG_ERR("tariff JSON parse error: %d", (int)ret);
		return false;
	}
	if (j.PM <= 0) {
		LOG_ERR("tariff rejected: PM=%d", (int)j.PM);
		return false;
	}

	fill_common(&persist.t1, &j);
	persist.t1.pri_cost = j.pri_cost;
	persist.t1.trip3_pulses = j.trip3;
	persist.t1.rate.cost_200 = j.Cost_200;
	persist.t1.rate.cost_stop = j.Cost_stop;
	persist.t1.rate3.cost_200 = j.Cost3_200;
	persist.t1.rate3.cost_stop = j.Cost3_stop;

	fill_common(&persist.t4, &j);
	persist.t4.pri_cost = j.pri2_cost;
	persist.t4.rate.cost_200 = j.Cost2_200;
	persist.t4.rate.cost_stop = j.Cost2_stop;
	persist.t4.rate3 = persist.t4.rate; /* tariff 3 is not used in 4-channel mode */

	persist.cfg.authorized = true;
	persist.cfg.samar = j.samar != 0;
	persist.cfg.darbast = j.darbast != 0;
	persist.cfg.korsi = j.korsi != 0;
	persist.cfg.nobat = j.nobat != 0;

	persist.service_count = (int32_t)MIN(j.service_len, SERVICE_MAX);
	memset(persist.service, 0, sizeof(persist.service));
	for (int i = 0; i < persist.service_count; i++) {
		persist.service[i] = j.service[i];
	}

	if (j.wifi && j.wifi[0]) {
		strncpy(persist.ssid, j.wifi, sizeof(persist.ssid) - 1);
		persist.ssid[sizeof(persist.ssid) - 1] = '\0';
		strncpy(persist.pass, j.wifiPass ? j.wifiPass : "", sizeof(persist.pass) - 1);
		persist.pass[sizeof(persist.pass) - 1] = '\0';
	}

	meter_set_config(&persist.t1, &persist.t4, &persist.cfg);
	persist_save();
	LOG_INF("tariff updated: PM=%d pri=%d pri2=%d services=%d", (int)j.PM, (int)j.pri_cost,
		(int)j.pri2_cost, (int)persist.service_count);
	return true;
}

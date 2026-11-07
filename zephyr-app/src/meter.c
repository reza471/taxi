#include "meter.h"
#include "hw.h"
#include "wdog.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(meter, LOG_LEVEL_INF);

meter_cfg_t meter_cfg = {.authorized = true, .korsi = true, .darbast = true,
			 .samar = true, .nobat = true};

/* Placeholder defaults. The real values come from the server tariff. */
fare_tariff_t meter_tariff1 = {
	.pm = 46, .start_night = 22, .stop_night = 6, .percent_night = 10,
	.max_stop_kmh = 20, .free_trip_m = 0, .free_time_s = 0, .fraction = 50,
	.trip3_pulses = 0, .pri_cost = 26200,
	.rate = {.cost_200 = 1318, .cost_stop = 8000},
	.rate3 = {.cost_200 = 1318, .cost_stop = 8000},
};
fare_tariff_t meter_tariff4 = {
	.pm = 46, .start_night = 22, .stop_night = 6, .percent_night = 10,
	.max_stop_kmh = 20, .free_trip_m = 0, .free_time_s = 0, .fraction = 50,
	.trip3_pulses = 0, .pri_cost = 5000,
	.rate = {.cost_200 = 384, .cost_stop = 334},
	.rate3 = {.cost_200 = 384, .cost_stop = 334},
};

static K_MUTEX_DEFINE(lock);
static fare_ch_t ch[4];
static const fare_tariff_t *tar[4] = {&meter_tariff1, &meter_tariff4, &meter_tariff4, &meter_tariff4};
static uint32_t total_pulses;
static char start_hhmm[4][8];
static volatile int live_speed;

static void meter_thread(void *a, void *b, void *c)
{
	int64_t next = k_uptime_get();
	int wd = wdog_add(4000);

	while (1) {
		next += 1000;
		k_sleep(K_TIMEOUT_ABS_MS(next));
		wdog_feed(wd);

		uint32_t pulses = pulse_take();
		int hour = clock_hour_or_unset();

		if (meter_tariff1.pm > 0) {
			live_speed = (int)(((int64_t)pulses * 36) / meter_tariff1.pm);
		}

		k_mutex_lock(&lock, K_FOREVER);
		total_pulses += pulses;
		for (int i = 0; i < 4; i++) {
			if (ch[i].active) {
				fare_tick(&ch[i], tar[i], pulses, hour);
			}
		}
		k_mutex_unlock(&lock);
	}
}

K_THREAD_DEFINE(meter_tid, 2048, meter_thread, NULL, NULL, NULL, 5, 0, 500);

void meter_init(void)
{
}

void meter_set_config(const fare_tariff_t *t1, const fare_tariff_t *t4, const meter_cfg_t *cfg)
{
	k_mutex_lock(&lock, K_FOREVER);
	meter_tariff1 = *t1;
	meter_tariff4 = *t4;
	meter_cfg = *cfg;
	k_mutex_unlock(&lock);
}

void meter_start(int i, bool is4ch)
{
	int h = clock_hour_or_unset();
	int hh, mm;

	clock_get_hm(&hh, &mm);
	k_mutex_lock(&lock, K_FOREVER);
	snprintf(start_hhmm[i], sizeof(start_hhmm[i]), "%02d:%02d", hh % 100, mm % 100);
	tar[i] = (is4ch || i > 0) ? &meter_tariff4 : &meter_tariff1;
	fare_start(&ch[i], tar[i], h);
	k_mutex_unlock(&lock);
}

void meter_stop(int i)
{
	k_mutex_lock(&lock, K_FOREVER);
	ch[i].active = false;
	ch[i].paused = false;
	k_mutex_unlock(&lock);
}

bool meter_toggle_pause(int i)
{
	k_mutex_lock(&lock, K_FOREVER);
	ch[i].paused = !ch[i].paused;
	bool p = ch[i].paused;

	k_mutex_unlock(&lock);
	return p;
}

void meter_snap(int i, meter_snap_t *o)
{
	k_mutex_lock(&lock, K_FOREVER);
	o->active = ch[i].active;
	o->paused = ch[i].paused;
	o->night = ch[i].night;
	o->total_rial = ch[i].active ? fare_total(&ch[i], tar[i]) : 0;
	o->speed_kmh = ch[i].speed_kmh;
	o->hkm = fare_distance_hkm(&ch[i], tar[i]);
	o->dist_m = (int32_t)fare_distance_m(&ch[i], tar[i]);
	o->wait_s = ch[i].seconds_slow;
	o->duration_s = ch[i].seconds_total;
	o->trip_price = ch[i].active ? fare_trip_price(&ch[i], tar[i]) : 0;
	o->traffic_price = ch[i].active ? fare_traffic_price(&ch[i], tar[i]) : 0;
	o->night_price = ch[i].active ? fare_night_price(&ch[i], tar[i]) : 0;
	o->avg_speed_kmh = fare_avg_speed_kmh(&ch[i], tar[i]);
	o->max_speed_kmh = ch[i].max_speed_kmh;
	memcpy(o->start_hhmm, start_hhmm[i], sizeof(o->start_hhmm));
	k_mutex_unlock(&lock);
}

bool meter_any_active(void)
{
	bool a = false;

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < 4; i++) {
		a |= ch[i].active;
	}
	k_mutex_unlock(&lock);
	return a;
}

uint32_t meter_total_pulses(void)
{
	return total_pulses;
}

void meter_clear_total(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	total_pulses = 0;
	k_mutex_unlock(&lock);
}

void meter_export(trip_state_t *out)
{
	memset(out, 0, sizeof(*out));
	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < 4; i++) {
		out->ch[i] = ch[i];
		out->is4[i] = (tar[i] == &meter_tariff4);
		memcpy(out->start[i], start_hhmm[i], sizeof(out->start[i]));
	}
	k_mutex_unlock(&lock);
	trip_state_seal(out);
}

void meter_import(const trip_state_t *in)
{
	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < 4; i++) {
		ch[i] = in->ch[i];
		tar[i] = in->is4[i] ? &meter_tariff4 : &meter_tariff1;
		memcpy(start_hhmm[i], in->start[i], sizeof(start_hhmm[i]));
		start_hhmm[i][sizeof(start_hhmm[i]) - 1] = '\0';
	}
	k_mutex_unlock(&lock);
}

int meter_speed_kmh(void)
{
	return live_speed;
}

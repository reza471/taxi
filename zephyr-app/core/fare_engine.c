#include "fare_engine.h"
#include <string.h>

#define MICRO 1000000LL

bool fare_is_night(const fare_tariff_t *t, int hour)
{
	if (t->stop_night == t->start_night) {
		return false; /* no night configured */
	}
	if (t->stop_night < t->start_night) {
		/* normal: day = [stop, start) */
		return !(hour >= t->stop_night && hour < t->start_night);
	}
	/* wrapped: day = [stop, 24) and [0, start) */
	return !(hour >= t->stop_night || hour < t->start_night);
}

static int64_t with_night(int64_t v, bool night, int32_t percent)
{
	return night ? v + (v * percent) / 100 : v;
}

void fare_start(fare_ch_t *c, const fare_tariff_t *t, int hour)
{
	memset(c, 0, sizeof(*c));
	c->active = true;
	c->night = fare_is_night(t, hour);
	c->night_start = c->night;
	c->start_fare = (int32_t)with_night(t->pri_cost, c->night, t->percent_night);
}

/* micro-Rial per pulse */
static int64_t per_pulse(const fare_rate_t *r, const fare_tariff_t *t, bool night)
{
	/* cost_200 covers 200 m = 20 * pm pulses */
	int64_t v = ((int64_t)r->cost_200 * MICRO) / (20LL * t->pm);
	return with_night(v, night, t->percent_night);
}

/* micro-Rial per second of waiting */
static int64_t per_second(const fare_rate_t *r, const fare_tariff_t *t, bool night)
{
	int64_t v = ((int64_t)r->cost_stop * MICRO) / 60LL;
	return with_night(v, night, t->percent_night);
}

void fare_tick(fare_ch_t *c, const fare_tariff_t *t, uint32_t pulses, int hour)
{
	if (!c->active || c->paused || t->pm <= 0) {
		return;
	}

	/* night rates are checked every second */
	c->night = fare_is_night(t, hour);

	/* km/h = pulses/s * (10 m / pm) * 3.6 = pulses * 36 / pm */
	c->speed_kmh = (int32_t)(((int64_t)pulses * 36) / t->pm);
	if (c->speed_kmh > c->max_speed_kmh) {
		c->max_speed_kmh = c->speed_kmh;
	}

	/* tariff 3 applies after trip3_pulses (checked before adding) */
	const fare_rate_t *r = (t->trip3_pulses > 0 && c->pulses_total >= t->trip3_pulses)
				       ? &t->rate3
				       : &t->rate;

	/* distance: free part first */
	int64_t free_pulses = ((int64_t)t->free_trip_m * t->pm) / 10;
	int64_t before = c->pulses_total;

	c->pulses_total += pulses;

	int64_t free_left = free_pulses > before ? free_pulses - before : 0;
	int64_t charged = (int64_t)pulses > free_left ? (int64_t)pulses - free_left : 0;

	c->pulses_charged += charged;
	int64_t dist_cost = charged * per_pulse(r, t, c->night);

	c->sum_micro += dist_cost;
	if (c->night) {
		c->night_micro += dist_cost;
	}

	/* time: only when slow, free part first */
	c->seconds_total++;
	if (c->speed_kmh < t->max_stop_kmh) {
		c->seconds_slow++;
		if (c->seconds_slow > t->free_time_s) {
			c->seconds_charged++;
			int64_t sec = per_second(r, t, c->night);

			c->sum_micro += sec;
			c->time_micro += sec;
		}
	}
}

int64_t fare_total(const fare_ch_t *c, const fare_tariff_t *t)
{
	int64_t rial = c->sum_micro / MICRO;
	int64_t total = c->start_fare + rial;

	if (t->fraction > 1) {
		int64_t extra = rial - (rial % t->fraction);
		total = c->start_fare + extra;
	}
	return total;
}

int64_t fare_distance_m(const fare_ch_t *c, const fare_tariff_t *t)
{
	return t->pm > 0 ? (c->pulses_total * 10) / t->pm : 0;
}

int32_t fare_distance_hkm(const fare_ch_t *c, const fare_tariff_t *t)
{
	return (int32_t)(fare_distance_m(c, t) / 100);
}

int64_t fare_traffic_price(const fare_ch_t *c, const fare_tariff_t *t)
{
	(void)t;
	return c->time_micro / MICRO;
}

int64_t fare_trip_price(const fare_ch_t *c, const fare_tariff_t *t)
{
	int64_t v = fare_total(c, t) - fare_traffic_price(c, t);

	return v > 0 ? v : 0;
}

int64_t fare_night_price(const fare_ch_t *c, const fare_tariff_t *t)
{
	int64_t v = (c->night_start ? c->start_fare : 0) + c->night_micro / MICRO;
	int64_t trip = fare_trip_price(c, t);

	return v < trip ? v : trip;
}

int32_t fare_avg_speed_kmh(const fare_ch_t *c, const fare_tariff_t *t)
{
	if (c->seconds_total <= 0 || t->pm <= 0) {
		return 0;
	}
	/* km/h = meters / seconds * 3.6 */
	return (int32_t)((fare_distance_m(c, t) * 36) / ((int64_t)c->seconds_total * 10));
}

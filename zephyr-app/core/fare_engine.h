/* Portable fare engine. Pure C, no Zephyr or Arduino calls.
 * Money is in Rial. Internal accumulators use micro-Rial (1e-6 Rial)
 * so no floating point is needed.
 */
#ifndef FARE_ENGINE_H
#define FARE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

/* One tariff (rates only). */
typedef struct {
	int32_t cost_200;  /* Rial per 200 m */
	int32_t cost_stop; /* Rial per minute of slow traffic / waiting */
} fare_rate_t;

/* Settings that the server sends (subset used by the engine). */
typedef struct {
	int32_t pm;            /* pulses per 10 m (must be > 0) */
	int32_t start_night;   /* hour, night starts (day = [stop_night, start_night)) */
	int32_t stop_night;    /* hour, night ends */
	int32_t percent_night; /* night surcharge, percent */
	int32_t max_stop_kmh;  /* below this speed, time cost is charged */
	int32_t free_trip_m;   /* free distance, meters */
	int32_t free_time_s;   /* free time, seconds */
	int32_t fraction;      /* rounding step, Rial (0 or 1 = no rounding) */
	int32_t trip3_pulses;  /* switch to rate3 after this many pulses (0 = off) */
	int32_t pri_cost;      /* start fare, Rial */
	fare_rate_t rate;      /* normal tariff */
	fare_rate_t rate3;     /* tariff after trip3_pulses */
} fare_tariff_t;

/* State of one meter channel. */
typedef struct {
	bool active;
	bool paused;
	bool night;              /* night rates in use right now (updated every tick) */
	bool night_start;        /* the trip started at night */
	int32_t start_fare;      /* start fare (night rate if started at night), Rial */
	int64_t sum_micro;       /* accumulated cost in micro-Rial (without start fare) */
	int64_t time_micro;      /* part of sum_micro that is waiting-time cost */
	int64_t night_micro;     /* route cost charged at night rates (distance only), micro-Rial */
	int32_t max_speed_kmh;
	int64_t pulses_total;    /* all pulses since start (even free ones) */
	int64_t pulses_charged;  /* pulses already past the free distance */
	int32_t seconds_total;   /* seconds since start, not paused */
	int32_t seconds_slow;    /* seconds counted as waiting (speed < max_stop) */
	int32_t seconds_charged; /* waiting seconds already past the free time */
	int32_t speed_kmh;       /* last speed */
} fare_ch_t;

/* True if the given local hour is night. hour < 0 means "clock not set" and is always day. */
bool fare_is_night(const fare_tariff_t *t, int hour);

/* Start a trip on a channel. */
void fare_start(fare_ch_t *c, const fare_tariff_t *t, int hour);

/* Call exactly once per second with the pulses counted in that second and the
 * current local hour. Night is checked every tick, so the rates change live when
 * the night starts or ends during a trip. */
void fare_tick(fare_ch_t *c, const fare_tariff_t *t, uint32_t pulses, int hour);

/* Current fare in Rial: start fare + cost, rounded DOWN to `fraction`. */
int64_t fare_total(const fare_ch_t *c, const fare_tariff_t *t);

/* Price split for the server (all in Rial):
 *   traffic_price = waiting-time cost (stopped / slow traffic)
 *   trip_price    = route price = total - traffic_price (start fare + distance)
 *   night_price   = the part of trip_price that was charged at night rates
 *                   (full night amount, not only the extra; 0 for a day-only trip)
 */
int64_t fare_traffic_price(const fare_ch_t *c, const fare_tariff_t *t);
int64_t fare_trip_price(const fare_ch_t *c, const fare_tariff_t *t);
int64_t fare_night_price(const fare_ch_t *c, const fare_tariff_t *t);

/* Average speed over the whole trip, km/h. */
int32_t fare_avg_speed_kmh(const fare_ch_t *c, const fare_tariff_t *t);

/* Distance in meters since start. */
int64_t fare_distance_m(const fare_ch_t *c, const fare_tariff_t *t);

/* Distance in units of 0.1 km (100 m), for the "1.2" display. */
int32_t fare_distance_hkm(const fare_ch_t *c, const fare_tariff_t *t);

#endif

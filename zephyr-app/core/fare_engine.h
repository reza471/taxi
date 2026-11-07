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
	bool night;
	int32_t start_fare;      /* start fare incl. night surcharge, Rial */
	int64_t sum_micro;       /* accumulated cost in micro-Rial (without start fare) */
	int64_t pulses_total;    /* all pulses since start (even free ones) */
	int64_t pulses_charged;  /* pulses already past the free distance */
	int32_t seconds_total;   /* seconds since start, not paused */
	int32_t seconds_slow;    /* seconds counted as waiting (speed < max_stop) */
	int32_t seconds_charged; /* waiting seconds already past the free time */
	int32_t speed_kmh;       /* last speed */
} fare_ch_t;

/* True if the given local time is night. */
bool fare_is_night(const fare_tariff_t *t, int hour);

/* Start a trip on a channel. */
void fare_start(fare_ch_t *c, const fare_tariff_t *t, int hour);

/* Call exactly once per second with the pulses counted in that second. */
void fare_tick(fare_ch_t *c, const fare_tariff_t *t, uint32_t pulses);

/* Current fare in Rial: start fare + cost, rounded DOWN to `fraction`. */
int64_t fare_total(const fare_ch_t *c, const fare_tariff_t *t);

/* Distance in meters since start. */
int64_t fare_distance_m(const fare_ch_t *c, const fare_tariff_t *t);

/* Distance in units of 0.1 km (100 m), for the "1.2" display. */
int32_t fare_distance_hkm(const fare_ch_t *c, const fare_tariff_t *t);

#endif

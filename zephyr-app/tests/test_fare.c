#include "../core/fare_engine.h"
#include <assert.h>
#include <stdio.h>

static fare_tariff_t base(void)
{
	fare_tariff_t t = {
		.pm = 46, .start_night = 22, .stop_night = 6, .percent_night = 10,
		.max_stop_kmh = 10, .free_trip_m = 0, .free_time_s = 0, .fraction = 1,
		.trip3_pulses = 0, .pri_cost = 19500,
		.rate = {.cost_200 = 1318, .cost_stop = 500000 / 10},
		.rate3 = {.cost_200 = 2000, .cost_stop = 100},
	};
	return t;
}

static void test_night(void)
{
	fare_tariff_t t = base();
	assert(!fare_is_night(&t, 6));
	assert(!fare_is_night(&t, 12));
	assert(!fare_is_night(&t, 21));
	assert(fare_is_night(&t, 22));
	assert(fare_is_night(&t, 3));
	t.stop_night = t.start_night = 0;
	assert(!fare_is_night(&t, 3));
}

static void test_start_fare(void)
{
	fare_tariff_t t = base();
	fare_ch_t c;
	fare_start(&c, &t, 12);
	assert(fare_total(&c, &t) == 19500);
	fare_start(&c, &t, 23);
	assert(fare_total(&c, &t) == 21450); /* +10 % */
}

static void test_distance_cost(void)
{
	fare_tariff_t t = base();
	fare_ch_t c;
	fare_start(&c, &t, 12);
	/* 200 m = 20*46 = 920 pulses, fast: 920 pulses in 1 s is 720 km/h, test only */
	fare_tick(&c, &t, 920, 12);
	/* integer truncation per pulse may lose < 1 Rial per 200 m */
	int64_t cost = fare_total(&c, &t) - 19500;
	assert(cost >= 1317 && cost <= 1318);
	assert(fare_distance_m(&c, &t) == 200);
	assert(fare_distance_hkm(&c, &t) == 2);
}

static void test_speed_and_wait(void)
{
	fare_tariff_t t = base();
	fare_ch_t c;
	fare_start(&c, &t, 12);
	/* 46 pulses/s = 10 m/s = 36 km/h */
	fare_tick(&c, &t, 46, 12);
	assert(c.speed_kmh == 36);
	assert(c.seconds_slow == 0); /* fast: no time cost */
	/* standing still: time cost, 50000/60 Rial per sec */
	int64_t before = fare_total(&c, &t);
	for (int i = 0; i < 60; i++) {
		fare_tick(&c, &t, 0, 12);
	}
	int64_t diff = fare_total(&c, &t) - before;
	assert(diff >= 49990 && diff <= 50000);
	assert(c.seconds_slow == 60);
}

static void test_free_distance_and_time(void)
{
	fare_tariff_t t = base();
	t.free_trip_m = 100; /* 460 pulses */
	t.free_time_s = 30;
	fare_ch_t c;
	fare_start(&c, &t, 12);
	fare_tick(&c, &t, 460, 12);
	assert(fare_total(&c, &t) == 19500); /* all free */
	for (int i = 0; i < 30; i++) {
		fare_tick(&c, &t, 0, 12);
	}
	assert(fare_total(&c, &t) == 19500); /* free time */
	fare_tick(&c, &t, 0, 12);
	assert(fare_total(&c, &t) > 19500);
}

static void test_pause_and_rounding(void)
{
	fare_tariff_t t = base();
	t.fraction = 50;
	fare_ch_t c;
	fare_start(&c, &t, 12);
	c.paused = true;
	fare_tick(&c, &t, 1000, 12);
	assert(fare_total(&c, &t) == 19500);
	c.paused = false;
	fare_tick(&c, &t, 460, 12);
	int64_t cost = fare_total(&c, &t) - 19500;
	assert(cost % 50 == 0);
}

static void test_trip3(void)
{
	fare_tariff_t t = base();
	t.trip3_pulses = 920;
	fare_ch_t c;
	fare_start(&c, &t, 12);
	fare_tick(&c, &t, 920, 12); /* normal rate */
	int64_t a = fare_total(&c, &t) - 19500;
	fare_tick(&c, &t, 920, 12); /* tariff 3 */
	int64_t b = fare_total(&c, &t) - 19500 - a;
	assert(b > a);
}

static void test_price_split(void)
{
	fare_tariff_t t = base();
	fare_ch_t c;

	/* day trip: night price is 0, trip + traffic = total */
	fare_start(&c, &t, 12);
	fare_tick(&c, &t, 920, 12); /* 200 m in one second */
	for (int i = 0; i < 60; i++) {
		fare_tick(&c, &t, 0, 12);
	}
	assert(fare_night_price(&c, &t) == 0);
	assert(fare_traffic_price(&c, &t) >= 49990);
	assert(fare_trip_price(&c, &t) + fare_traffic_price(&c, &t) == fare_total(&c, &t));
	assert(fare_trip_price(&c, &t) >= 19500 + 1317);

	/* night-only trip: the whole route is charged at night rates */
	fare_ch_t n;

	fare_start(&n, &t, 23);
	fare_tick(&n, &t, 920, 23);
	assert(fare_night_price(&n, &t) == fare_trip_price(&n, &t)); /* all of it is night */
	assert(fare_night_price(&n, &t) >= 21450 + 1449 - 2);        /* 19500+10 % and 1318+10 % */
	assert(fare_night_price(&n, &t) <= 21450 + 1450);

	/* the night price is higher than the same day trip */
	fare_ch_t d;

	fare_start(&d, &t, 12);
	fare_tick(&d, &t, 920, 12);
	assert(fare_trip_price(&n, &t) > fare_trip_price(&d, &t));
}

static void test_night_starts_mid_trip(void)
{
	fare_tariff_t t = base();
	fare_ch_t c;

	/* start at 21:xx (day), drive 200 m by day, then 200 m after 22:00 */
	fare_start(&c, &t, 21);
	fare_tick(&c, &t, 920, 21);
	int64_t day_part = fare_trip_price(&c, &t);

	assert(fare_night_price(&c, &t) == 0);
	assert(!c.night);

	fare_tick(&c, &t, 920, 22);
	assert(c.night);
	int64_t night_part = fare_night_price(&c, &t);

	/* only the second 200 m is night, charged at night rate (about 1449 Rial) */
	assert(night_part >= 1447 && night_part <= 1450);
	/* the second 200 m cost more than the first */
	assert(fare_trip_price(&c, &t) - day_part > 1318);

	/* night ends again at 06:00: rates go back to day */
	int64_t before = fare_trip_price(&c, &t);

	fare_tick(&c, &t, 920, 6);
	assert(!c.night);
	assert(fare_trip_price(&c, &t) - before <= 1318);
	assert(fare_night_price(&c, &t) == night_part);
}

static void test_speeds(void)
{
	fare_tariff_t t = base();
	fare_ch_t c;

	fare_start(&c, &t, 12);
	fare_tick(&c, &t, 46, 12);  /* 36 km/h */
	fare_tick(&c, &t, 92, 12);  /* 72 km/h */
	fare_tick(&c, &t, 0, 12);   /* stopped */
	assert(c.max_speed_kmh == 72);
	/* 138 pulses = 30 m in 3 s = 36 km/h */
	assert(fare_distance_m(&c, &t) == 30);
	assert(fare_avg_speed_kmh(&c, &t) == 36);
}

int main(void)
{
	test_night();
	test_start_fare();
	test_distance_cost();
	test_speed_and_wait();
	test_free_distance_and_time();
	test_pause_and_rounding();
	test_trip3();
	test_price_split();
	test_night_starts_mid_trip();
	test_speeds();
	puts("all fare tests passed");
	return 0;
}

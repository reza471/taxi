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
	fare_tick(&c, &t, 920);
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
	fare_tick(&c, &t, 46);
	assert(c.speed_kmh == 36);
	assert(c.seconds_slow == 0); /* fast: no time cost */
	/* standing still: time cost, 50000/60 Rial per sec */
	int64_t before = fare_total(&c, &t);
	for (int i = 0; i < 60; i++) {
		fare_tick(&c, &t, 0);
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
	fare_tick(&c, &t, 460);
	assert(fare_total(&c, &t) == 19500); /* all free */
	for (int i = 0; i < 30; i++) {
		fare_tick(&c, &t, 0);
	}
	assert(fare_total(&c, &t) == 19500); /* free time */
	fare_tick(&c, &t, 0);
	assert(fare_total(&c, &t) > 19500);
}

static void test_pause_and_rounding(void)
{
	fare_tariff_t t = base();
	t.fraction = 50;
	fare_ch_t c;
	fare_start(&c, &t, 12);
	c.paused = true;
	fare_tick(&c, &t, 1000);
	assert(fare_total(&c, &t) == 19500);
	c.paused = false;
	fare_tick(&c, &t, 460);
	int64_t cost = fare_total(&c, &t) - 19500;
	assert(cost % 50 == 0);
}

static void test_trip3(void)
{
	fare_tariff_t t = base();
	t.trip3_pulses = 920;
	fare_ch_t c;
	fare_start(&c, &t, 12);
	fare_tick(&c, &t, 920); /* normal rate */
	int64_t a = fare_total(&c, &t) - 19500;
	fare_tick(&c, &t, 920); /* tariff 3 */
	int64_t b = fare_total(&c, &t) - 19500 - a;
	assert(b > a);
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
	puts("all fare tests passed");
	return 0;
}

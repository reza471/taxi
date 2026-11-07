/*
 * Host tests (native_sim, simulated flash) for the code that must survive power loss:
 *   - postq   : FIFO order, full queue rules, persistence across "reboots"
 *   - tariff  : server JSON -> tariffs, validation, flash save
 *
 * The persistence test needs several runs of the same binary with the same flash file.
 * Each run reads the phase number stored in flash, runs that phase, and stores the next one.
 * Use tests/native/run.sh.
 */
#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/settings/settings.h>
#include "persist.h"
#include "postq.h"
#include "tariff.h"
#include "trip_store.h"

extern int stub_config_calls;

static int phase = -1;

static int test_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "phase") == 0 && len == sizeof(phase)) {
		read_cb(cb_arg, &phase, sizeof(phase));
	}
	return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(ttest, "ttest", NULL, test_set, NULL, NULL);

static void drain(void)
{
	char buf[URL_MAX];

	while (postq_peek(buf, sizeof(buf))) {
		postq_pop();
	}
	postq_flush();
}

/* ---------- RAM behavior ---------- */
ZTEST(taxi, test_a_fifo_order)
{
	if (phase != -1) {
		ztest_test_skip(); /* these tests empty the queue: only on the first run */
	}
	char b[URL_MAX];

	drain();
	zassert_true(postq_push("/x/SetTaxiStart?n=1"));
	zassert_true(postq_push("/x/SetTaxiEnd?n=1"));
	zassert_true(postq_push("/x/StartService?n=2"));
	zassert_equal(postq_count(), 3);
	zassert_true(postq_peek(b, sizeof(b)));
	zassert_str_equal(b, "/x/SetTaxiStart?n=1");
	postq_pop();
	zassert_true(postq_peek(b, sizeof(b)));
	zassert_str_equal(b, "/x/SetTaxiEnd?n=1");
	postq_pop();
	postq_pop();
	zassert_equal(postq_count(), 0);
	zassert_false(postq_peek(b, sizeof(b)));
	postq_pop(); /* pop on empty must be harmless */
	zassert_equal(postq_count(), 0);
	drain();
}

ZTEST(taxi, test_b_full_queue_keeps_trip_ends)
{
	if (phase != -1) {
		ztest_test_skip(); /* these tests empty the queue: only on the first run */
	}
	char b[URL_MAX];
	char u[64];

	drain();
	/* fill: 8 trip ends and 8 starts, interleaved */
	for (int i = 0; i < QUEUE_LEN; i++) {
		snprintf(u, sizeof(u), "/x/%s?i=%d", (i % 2) ? "SetTaxiStart" : "SetTaxiEnd", i);
		zassert_true(postq_push(u));
	}
	zassert_equal(postq_count(), QUEUE_LEN);

	/* one more trip end: the oldest START must be dropped, no trip end lost */
	zassert_true(postq_push("/x/SetTaxiEnd?i=100"));
	zassert_equal(postq_count(), QUEUE_LEN);

	int ends = 0;

	while (postq_peek(b, sizeof(b))) {
		if (strstr(b, "SetTaxiEnd")) {
			ends++;
		}
		postq_pop();
	}
	zassert_equal(ends, QUEUE_LEN / 2 + 1, "all trip ends kept, got %d", ends);

	/* queue of only trip ends: a new record is dropped, old ones stay */
	for (int i = 0; i < QUEUE_LEN; i++) {
		snprintf(u, sizeof(u), "/x/SetTaxiEnd?i=%d", i);
		zassert_true(postq_push(u));
	}
	zassert_false(postq_push("/x/SetTaxiEnd?i=999"));
	zassert_true(postq_peek(b, sizeof(b)));
	zassert_str_equal(b, "/x/SetTaxiEnd?i=0");
	drain();
}

ZTEST(taxi, test_c_too_long_record_is_rejected)
{
	if (phase != -1) {
		ztest_test_skip(); /* these tests empty the queue: only on the first run */
	}
	static char big[URL_MAX + 10];

	drain();
	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	zassert_false(postq_push(big));
	zassert_equal(postq_count(), 0);
}

/* ---------- tariff JSON ---------- */
static const char SAMPLE[] =
	"{\"PM\":46,\"pri_cost\":26200,\"Start_night\":22,\"Stop_night\":6,\"Cost_200\":1318,"
	"\"Cost_stop\":8000,\"Max_stop_km\":20,\"free_trip\":0,\"free_time\":0,\"Percent_night\":10,"
	"\"fraction\":50,\"taxi\":1,\"pri2_cost\":5000,\"Cost2_200\":384,\"Cost2_stop\":334,"
	"\"samar\":1,\"darbast\":1,\"korsi\":0,\"nobat\":1,\"trip3\":0,\"Cost3_200\":0,"
	"\"Cost3_stop\":0,\"wifi\":\"MyNet\",\"wifiPass\":\"secret123\","
	"\"service\":[211,222,233],\"unknown_extra\":5}";

ZTEST(taxi, test_d_tariff_parse)
{
	char json[sizeof(SAMPLE)];

	memcpy(json, SAMPLE, sizeof(SAMPLE));
	zassert_true(tariff_apply_json(json, strlen(json)));

	zassert_equal(persist.t1.pm, 46);
	zassert_equal(persist.t1.pri_cost, 26200);
	zassert_equal(persist.t1.rate.cost_200, 1318);
	zassert_equal(persist.t1.rate.cost_stop, 8000);
	zassert_equal(persist.t1.percent_night, 10);
	zassert_equal(persist.t1.max_stop_kmh, 20);
	zassert_equal(persist.t1.fraction, 50);
	zassert_equal(persist.t4.pri_cost, 5000);
	zassert_equal(persist.t4.rate.cost_200, 384);
	zassert_equal(persist.t4.rate.cost_stop, 334);
	zassert_equal(persist.t4.pm, 46);
	zassert_false(persist.cfg.korsi);
	zassert_true(persist.cfg.darbast);
	zassert_true(persist.cfg.authorized);
	zassert_equal(persist.service_count, 3);
	zassert_equal(persist.service[2], 233);
	zassert_str_equal(persist.ssid, "MyNet");
	zassert_str_equal(persist.pass, "secret123");
	zassert_equal(meter_tariff1.pm, 46, "applied to the meter");
}

ZTEST(taxi, test_e_tariff_rejects_bad_data)
{
	char bad_pm[] = "{\"PM\":0,\"pri_cost\":1}";
	char broken[] = "{\"PM\":46,\"pri_cost\":";
	char empty[] = "";
	int before_pm = persist.t1.pm;

	zassert_false(tariff_apply_json(bad_pm, strlen(bad_pm)));
	zassert_false(tariff_apply_json(broken, strlen(broken)));
	zassert_false(tariff_apply_json(empty, 0));
	zassert_equal(persist.t1.pm, before_pm, "bad data must not change the tariff");
}

ZTEST(taxi, test_f_tariff_without_wifi_keeps_wifi)
{
	char json[] = "{\"PM\":50,\"pri_cost\":100}";

	zassert_true(tariff_apply_json(json, strlen(json)));
	zassert_str_equal(persist.ssid, "MyNet", "missing wifi key must not erase WiFi");
	zassert_equal(persist.t1.pm, 50);
}

/* ---------- running trip saved in flash ---------- */
static void make_running_trip(trip_state_t *st)
{
	static const fare_tariff_t t = {
		.pm = 46, .start_night = 22, .stop_night = 6, .percent_night = 10,
		.max_stop_kmh = 20, .fraction = 50, .pri_cost = 26200,
		.rate = {.cost_200 = 1318, .cost_stop = 8000},
	};

	memset(st, 0, sizeof(*st));
	fare_start(&st->ch[0], &t, 12);
	for (int i = 0; i < 100; i++) {
		fare_tick(&st->ch[0], &t, 40, 12);
	}
	st->ch[0].paused = true;
	fare_start(&st->ch[2], &t, 23);
	st->is4[2] = 1;
	strcpy(st->start[0], "13:05");
	strcpy(st->start[2], "23:50");
	trip_state_seal(st);
}

ZTEST(taxi, test_g_trip_state_seal_and_damage)
{
	trip_state_t st;

	make_running_trip(&st);
	zassert_true(trip_state_valid(&st));
	zassert_true(trip_state_any_active(&st));

	st.ch[0].sum_micro += 1; /* any damaged bit must be detected */
	zassert_false(trip_state_valid(&st));

	make_running_trip(&st);
	st.size += 1; /* data from another firmware version */
	zassert_false(trip_state_valid(&st));

	make_running_trip(&st);
	st.magic = 0;
	zassert_false(trip_state_valid(&st));

	memset(&st, 0, sizeof(st));
	trip_state_seal(&st);
	zassert_true(trip_state_valid(&st));
	zassert_false(trip_state_any_active(&st), "empty state has no active trip");
}

/* ---------- power loss: needs several runs of the binary (see run.sh) ---------- */
ZTEST(taxi, test_z_persistence_across_reboots)
{
	char b[URL_MAX];
	int next = phase + 1;

	TC_PRINT("persistence phase %d\n", phase);
	switch (phase) {
	case -1: /* first run after flash erase: fill the queue, save, "lose power" */
		drain();
		zassert_true(postq_push("/x/SetTaxiStart?n=1"));
		zassert_true(postq_push("/x/SetTaxiEnd?n=1&total=12345"));
		zassert_true(postq_push("/x/SetTaxiEnd?n=2&total=999"));
		zassert_true(postq_dirty());
		postq_flush();
		zassert_false(postq_dirty());
		{
			trip_state_t st;

			make_running_trip(&st);
			zassert_true(trip_store_save(&st));
		}
		break;
	case 0: /* after reboot: everything must be back, in order */
		zassert_equal(postq_count(), 3, "queue lost after reboot: %d", postq_count());
		zassert_true(postq_peek(b, sizeof(b)));
		zassert_str_equal(b, "/x/SetTaxiStart?n=1");
		postq_pop();
		postq_flush(); /* the first one was "sent" */
		{
			/* the running trip comes back exactly as it was saved */
			trip_state_t st, want;

			make_running_trip(&want);
			zassert_true(trip_store_load(&st), "running trip lost after reboot");
			zassert_mem_equal(&st, &want, sizeof(st));
			zassert_true(st.ch[0].paused);
			zassert_str_equal(st.start[2], "23:50");
			zassert_equal(st.is4[2], 1);
			zassert_true(st.ch[0].sum_micro > 0);

			/* the trip ended: save an empty state */
			memset(&st, 0, sizeof(st));
			trip_state_seal(&st);
			zassert_true(trip_store_save(&st));
		}
		break;
	case 1: /* after a second reboot: only the two left, still in order */
		zassert_equal(postq_count(), 2, "got %d", postq_count());
		zassert_true(postq_peek(b, sizeof(b)));
		zassert_str_equal(b, "/x/SetTaxiEnd?n=1&total=12345");
		postq_pop();
		/* push while some are queued, but "lose power" before flush: unsaved is lost */
		zassert_true(postq_push("/x/SetTaxiEnd?n=3&total=777"));
		/* no flush here on purpose */
		break;
	case 2: /* the un-flushed pop and push are gone; the flushed state is intact */
		{
			trip_state_t st;

			zassert_false(trip_store_load(&st), "an ended trip must not come back");
		}
		zassert_equal(postq_count(), 2, "got %d", postq_count());
		zassert_true(postq_peek(b, sizeof(b)));
		zassert_str_equal(b, "/x/SetTaxiEnd?n=1&total=12345");
		/* wrap-around: head moves, fill past the end of the ring, save */
		for (int i = 0; i < QUEUE_LEN - 2; i++) {
			char u[64];

			snprintf(u, sizeof(u), "/x/SetTaxiEnd?w=%d", i);
			zassert_true(postq_push(u));
		}
		zassert_equal(postq_count(), QUEUE_LEN);
		postq_pop();
		postq_pop();
		zassert_true(postq_push("/x/SetTaxiEnd?w=wrapA"));
		zassert_true(postq_push("/x/SetTaxiEnd?w=wrapB"));
		postq_flush();
		break;
	case 3: /* ring wrapped around the end of the slots and still reads in order */
		zassert_equal(postq_count(), QUEUE_LEN, "got %d", postq_count());
		zassert_true(postq_peek(b, sizeof(b)));
		zassert_str_equal(b, "/x/SetTaxiEnd?w=0");
		for (int i = 0; i < QUEUE_LEN - 2; i++) {
			zassert_true(postq_peek(b, sizeof(b)));
			postq_pop();
		}
		zassert_true(postq_peek(b, sizeof(b)));
		zassert_str_equal(b, "/x/SetTaxiEnd?w=wrapA");
		postq_pop();
		zassert_true(postq_peek(b, sizeof(b)));
		zassert_str_equal(b, "/x/SetTaxiEnd?w=wrapB");
		postq_pop();
		postq_flush();
		zassert_equal(postq_count(), 0);
		next = 99; /* done */
		break;
	default:
		next = phase;
		break;
	}

	int err = settings_save_one("ttest/phase", &next, sizeof(next));

	zassert_equal(err, 0);
}

static void *suite_setup(void)
{
	persist_init(); /* settings_subsys_init + settings_load (also loads the queue) */
	return NULL;
}

ZTEST_SUITE(taxi, NULL, suite_setup, NULL, NULL, NULL);

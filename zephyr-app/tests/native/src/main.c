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
#include "report.h"

extern int stub_config_calls;
extern int32_t stub_day;
extern char stub_fw_ver[32];
extern char stub_fw_url[256];
extern int stub_fw_calls;

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

/* ---------- tariff JSON: what could the real server send? ---------- */
static bool try_tariff(const char *text)
{
	static char buf[4096];

	snprintf(buf, sizeof(buf), "%s", text);
	return tariff_apply_json(buf, strlen(buf));
}

#define BASE "\"PM\":46,\"pri_cost\":26200,\"Cost_200\":1318,\"Cost_stop\":8000"

ZTEST(taxi, test_h_json_whitespace_and_order)
{
	zassert_true(try_tariff("\n{\r\n  \"pri_cost\" : 26200 ,\n\t\"PM\":  46\n}\n"));
	zassert_equal(persist.t1.pm, 46);
	zassert_equal(persist.t1.pri_cost, 26200);
}

ZTEST(taxi, test_i_json_too_many_services)
{
	/* the old firmware ignored extra services (kept 10). One long list must not kill the tariff. */
	zassert_true(try_tariff("{" BASE ",\"service\":[1,2,3,4,5,6,7,8,9,10,11,12]}"),
		     "a long service list must not reject the tariff");
	zassert_equal(persist.service_count, 10);
	zassert_equal(persist.service[9], 10);
}

ZTEST(taxi, test_j_json_booleans_and_floats)
{
	/* server written in JS/PHP may send true/false or 1318.0 */
	zassert_true(try_tariff("{" BASE ",\"samar\":true,\"korsi\":false,\"darbast\":1}"),
		     "true/false rejects the whole tariff");
	zassert_true(persist.cfg.samar);
	zassert_false(persist.cfg.korsi);
	zassert_true(try_tariff("{\"PM\":46,\"pri_cost\":26200,\"Cost_200\":1318.5}"),
		     "a decimal number rejects the whole tariff");
	zassert_true(persist.t1.rate.cost_200 == 1318 || persist.t1.rate.cost_200 == 1319);
}

ZTEST(taxi, test_k_json_numbers_as_strings_and_null)
{
	/* PHP/MySQL often send numbers as strings and missing values as null */
	zassert_true(try_tariff("{\"PM\":\"46\",\"pri_cost\":\"26200\",\"wifi\":null,\"service\":null}"),
		     "string numbers / null reject the whole tariff");
	zassert_equal(persist.t1.pm, 46);
	zassert_equal(persist.t1.pri_cost, 26200);
}

ZTEST(taxi, test_l_json_nested_and_extra_keys)
{
	/* extra keys with objects/arrays must be skipped, not break parsing */
	zassert_true(try_tariff("{\"meta\":{\"a\":[1,2,{\"b\":3}]},\"list\":[[1],[2]]," BASE "}"),
		     "unknown nested keys break parsing");
	/* old format seen in the commented code: everything inside "taxiFare" */
	zassert_true(try_tariff("{\"taxiFare\":{" BASE "}}"), "wrapper object {\"taxiFare\":{...}} not accepted");
}

ZTEST(taxi, test_m_json_big_and_negative_numbers)
{
	zassert_false(try_tariff("{\"PM\":46,\"pri_cost\":99999999999999}"),
		      "a number that does not fit 32 bit must be rejected, not wrapped");
	zassert_false(try_tariff("{\"PM\":46,\"pri_cost\":-5}"), "a negative fare must be rejected");
	zassert_false(try_tariff("{\"PM\":46,\"Percent_night\":900}"), "silly night percent must be rejected");
	zassert_false(try_tariff("{\"PM\":46,\"fraction\":-1}"), "negative rounding must be rejected");
	zassert_false(try_tariff("{\"PM\":46,\"Start_night\":99}"), "hour 99 must be rejected");
}

ZTEST(taxi, test_n_json_strings)
{
	zassert_true(try_tariff("{" BASE ",\"wifi\":\"Caf\\u00e9 Net\",\"wifiPass\":\"p\\\"ss\\\\w\"}"));
	zassert_str_equal(persist.pass, "p\"ss\\w", "escaped characters in the password");
	/* a name longer than the field must not overflow or be cut silently */
	/* a bad WiFi name must not stop the prices, and must not change the saved WiFi */
	zassert_true(try_tariff("{" BASE ",\"wifi\":\"0123456789012345678901234567890123456789\"}"));
	zassert_str_equal(persist.ssid, "Caf\xc3\xa9 Net", "WiFi kept");
	zassert_false(try_tariff("{\"PM\":46,\"Cost_200\":99999999999}"), "too big for a price");
}

ZTEST(taxi, test_o_json_garbage)
{
	zassert_false(try_tariff("<html>502 Bad Gateway</html>"));
	zassert_false(try_tariff("[1,2,3]"));
	zassert_false(try_tariff("{\"PM\":46,"));
	zassert_false(try_tariff("null"));
	zassert_false(try_tariff("{}"), "empty object (PM missing)");
}

ZTEST(taxi, test_p_firmware_offer)
{
	try_tariff("{" BASE ",\"fw_version\":\"0.2.0\",\"fw_url\":\"https://fw.example.ir/taxi-0.2.0.signed.bin\"}");
	zassert_str_equal(stub_fw_ver, "0.2.0");
	zassert_str_equal(stub_fw_url, "https://fw.example.ir/taxi-0.2.0.signed.bin");

	/* no offer in the next tariff: the offer is withdrawn */
	try_tariff("{" BASE "}");
	zassert_str_equal(stub_fw_ver, "(none)");

	/* only one of the two fields: no offer */
	try_tariff("{" BASE ",\"fw_version\":\"0.2.0\"}");
	zassert_str_equal(stub_fw_ver, "(none)");

	/* a URL longer than the buffer is ignored, prices still apply */
	char longj[600];
	char url[300];

	memset(url, 'a', sizeof(url) - 1);
	url[sizeof(url) - 1] = '\0';
	snprintf(longj, sizeof(longj), "{" BASE ",\"fw_version\":\"0.3.0\",\"fw_url\":\"https://x.ir/%s\"}", url);
	zassert_true(try_tariff(longj));
	zassert_str_equal(stub_fw_ver, "(none)");

	/* a broken tariff must not touch the offer */
	int calls = stub_fw_calls;

	zassert_false(try_tariff("{\"PM\":0,\"fw_version\":\"9.9.9\",\"fw_url\":\"https://x.ir/a\"}"));
	zassert_equal(stub_fw_calls, calls, "refused tariff must not start an update");
}

/* ---------- report helpers ---------- */
static void expect_hist(int newest_total, int n)
{
	rep_entry_t e;

	zassert_equal(report_hist_count(), n);
	for (int i = 0; i < n; i++) {
		zassert_true(report_hist_get(i, &e));
		zassert_equal(e.total_rial, newest_total - i * 1000, "entry %d", i);
	}
	zassert_false(report_hist_get(n, &e));
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
		/* report: 12 trips on day 100, only the last 10 stay in the history */
		stub_day = 100;
		for (int i = 1; i <= 12; i++) {
			char hm[8];

			snprintf(hm, sizeof(hm), "%02d:%02d", 8 + i / 2, (i % 2) * 30);
			report_add(5, i * 1000, i * 100, hm);
		}
		{
			int64_t total;
			int count;

			report_today(&total, &count);
			zassert_equal(total, 78000);
			zassert_equal(count, 12);
		}
		expect_hist(12000, REPORT_HIST);
		{
			rep_entry_t e;

			zassert_true(report_hist_get(0, &e));
			zassert_equal(e.hhmm, 1400, "start time of the newest trip");
			zassert_equal(e.dist_m, 1200);
			zassert_equal(e.bt, 5);
		}
		zassert_true(report_dirty());
		report_flush();
		zassert_false(report_dirty());
		break;
	case 0: /* after reboot: everything must be back, in order */
		zassert_equal(postq_count(), 3, "queue lost after reboot: %d", postq_count());
		zassert_true(postq_peek(b, sizeof(b)));
		zassert_str_equal(b, "/x/SetTaxiStart?n=1");
		postq_pop();
		postq_flush(); /* the first one was "sent" */
		{
			/* report survived the power loss: same day, same totals, same history */
			int64_t total;
			int count;

			stub_day = 100;
			report_today(&total, &count);
			zassert_equal(total, 78000, "daily total lost: %lld", (long long)total);
			zassert_equal(count, 12);
			expect_hist(12000, REPORT_HIST);

			/* next day: today starts from zero, history stays */
			stub_day = 101;
			report_today(&total, &count);
			zassert_equal(total, 0);
			zassert_equal(count, 0);
			expect_hist(12000, REPORT_HIST);
			report_add(1, 5000, 50, "07:15");
			report_flush();
		}
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
		{
			int64_t total;
			int count;
			rep_entry_t e;

			stub_day = 101;
			report_today(&total, &count);
			zassert_equal(total, 5000, "new day total lost");
			zassert_equal(count, 1);
			zassert_equal(report_hist_count(), REPORT_HIST);
			zassert_true(report_hist_get(0, &e));
			zassert_equal(e.total_rial, 5000);
			zassert_equal(e.bt, 1);
			zassert_equal(e.hhmm, 715);
			zassert_true(report_hist_get(1, &e));
			zassert_equal(e.total_rial, 12000);
		}
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
		{
			/* clock not set: totals keep adding up and are never reset */
			int64_t total, before;
			int count, cbefore;

			stub_day = -1;
			report_today(&before, &cbefore);
			report_add(5, 1234, 10, "xx");
			report_today(&total, &count);
			zassert_equal(total, before + 1234);
			zassert_equal(count, cbefore + 1);
			rep_entry_t e;

			zassert_true(report_hist_get(0, &e));
			zassert_equal(e.hhmm, -1, "unknown start time");
		}
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

/*
 * Pages and menus. One thread, runs every 50 ms. Never blocks.
 * Behavior is ported from the Arduino version (Menu.cpp), see CLAUDE.md.
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "hw.h"
#include "meter.h"
#include "ui.h"
#include "net.h"
#include "wdog.h"
#include "report.h"
#include "postq.h"
#include "version.h"
#include "ota.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define UI_PERIOD_MS 20
#define BEEP_MENU() beep(4000, 100)
#define BASE_FARE_SHOW_MS 2000
#define BLINK_MS 4000
#define MENU_IDLE_MS 30000

enum page { P_MAIN, P_MENU, P_SAMAR, P_RESERVE, P_PULSE, P_FARE1, P_FARE4, P_OFF,
	    P_INFO, P_REPORT, P_CAL };

static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), backlight_gpios);

static key_snapshot_t k;
static enum page page = P_MAIN;
static int64_t now;

static char big[32];
static char small_[16];
static bool colon;

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

#define PRESSED(n) (k.pressed[(n) - 1])
#define RELEASED(n) (k.released[(n) - 1])
#define STILL(n) (k.still[(n) - 1])

static void show_clock(void)
{
	int h, m;

	if (!clock_is_set()) {
		snprintf(small_, sizeof(small_), "----");
		colon = false;
		return;
	}
	clock_get_hm(&h, &m);
	snprintf(small_, sizeof(small_), "%02d%02d", h, m);
	colon = true;
}

static bool wait_release; /* after a page change: ignore keys until all are up */

static void goto_page(enum page p)
{
	page = p;
	wait_release = true; /* the key that changed the page must not act on the new page */
}

static bool blink_phase(void);
static void menu_reset(void);

/* ---------------- OFF ---------------- */
static void page_off(void)
{
	snprintf(big, sizeof(big), "        ");
	snprintf(small_, sizeof(small_), "    ");
	colon = false;
	if (PRESSED(5)) {
		BEEP_MENU();
		goto_page(P_MAIN);
	}
}

/* ---------------- MAIN ---------------- */
static uint8_t menu_flag;
static bool show_speed;      /* K3 toggles the small field: clock <-> speed */
static int64_t msg_until;    /* timed error text on the big field */
static char msg_text[16];

/* error codes on the 7-segment display (E-xxx) */
#define ERR_EXPIRED "E-403   " /* license expired (server answered 403) */
#define ERR_MODE_OFF "E-001   " /* this meter mode is switched off in the tariff */

static void show_error(const char *code)
{
	snprintf(msg_text, sizeof(msg_text), "%s", code);
	msg_until = now + 2000;
	beep(600, 300); /* one long low beep = "no" */
}

static void page_main(void)
{
	if (now < msg_until) {
		snprintf(big, sizeof(big), "%s", msg_text);
	} else if (!meter_cfg.authorized) {
		snprintf(big, sizeof(big), ERR_EXPIRED); /* stays until the server accepts again */
	} else {
		snprintf(big, sizeof(big), "--------");
	}

	if (show_speed) {
		snprintf(small_, sizeof(small_), "%4d", clampi(meter_speed_kmh(), 0, 999));
		colon = false;
	} else {
		show_clock();
	}

	if (PRESSED(3)) {
		show_speed = !show_speed;
	}
	/* hold K3 for 10 s: reset WiFi settings and restart */
	if (STILL(3) && k.hold_ms > 10000) {
		net_reset_wifi();
	}

	if (PRESSED(1)) {
		if (!meter_cfg.authorized) {
			show_error(ERR_EXPIRED);
		} else if (!meter_cfg.korsi) {
			show_error(ERR_MODE_OFF);
		} else {
			BEEP_MENU();
			goto_page(P_FARE4); /* shows channel 1; a second press starts it */
			return;
		}
	}
	if (PRESSED(2)) {
		meter_clear_total();
		goto_page(P_PULSE);
		return;
	}
	if (PRESSED(4)) {
		if (!meter_cfg.authorized) {
			show_error(ERR_EXPIRED);
		} else if (!meter_cfg.darbast) {
			show_error(ERR_MODE_OFF);
		} else {
			BEEP_MENU();
			meter_start(0, false);
			net_trip_start(5);
			goto_page(P_FARE1);
			return;
		}
	}

	/* menu and power off work even when the license is expired */
	if (STILL(5)) {
		if (k.hold_ms > 3000 && menu_flag == 0) {
			BEEP_MENU();
			menu_flag = 1;
		}
		if (k.hold_ms > 5000 && menu_flag == 1) {
			BEEP_MENU();
			menu_flag = 0;
			goto_page(P_OFF);
			return;
		}
	}
	if (RELEASED(5) && menu_flag == 1) {
		menu_flag = 0;
		menu_reset();
		snprintf(small_, sizeof(small_), "P-1 ");
		goto_page(P_MENU);
	}
}

/* ---------------- MENU (2 pages of items) ----------------
 * Page 1:  K1 MORE   K2 SAMR   K3 NOBT   K4 TARF   K5 back
 * Page 2:  K1 MORE   K2 INFO   K3 REPT   K4 CAL    K5 back
 * K1 switches the page at once. Other keys: press once = show the name, press again = enter.
 */
static uint8_t menu_sel;
static uint8_t menu_pg;

static bool menu_pick(int key, const char *label, int id)
{
	/* returns true when this item was confirmed (second press) */
	snprintf(small_, sizeof(small_), "%s", label);
	if (menu_sel == id) {
		menu_sel = 0;
		return true;
	}
	menu_sel = id;
	(void)key;
	return false;
}

static void menu_reset(void)
{
	menu_sel = 0;
	menu_pg = 0;
}

/* item switched off in the tariff: say so instead of doing nothing */
static void menu_off(void)
{
	snprintf(small_, sizeof(small_), "OFF ");
	menu_sel = 0;
	beep(600, 300);
}

static void page_menu(void)
{
	colon = false;
	snprintf(big, sizeof(big), "MENU    ");

	if (RELEASED(1)) {
		menu_pg = !menu_pg;
		menu_sel = 0;
		snprintf(small_, sizeof(small_), menu_pg ? "P-2 " : "P-1 ");
	}
	if (menu_sel == 0 && !(RELEASED(2) || RELEASED(3) || RELEASED(4)) && k.rest_ms > 1500) {
		snprintf(small_, sizeof(small_), menu_pg ? "P-2 " : "P-1 ");
	}

	if (menu_pg == 0) {
		if (RELEASED(2) && !meter_cfg.samar) {
			menu_off();
		} else if (RELEASED(2) && menu_pick(2, "SAMR", 2)) {
			goto_page(P_SAMAR);
			return;
		}
		if (RELEASED(3) && !meter_cfg.nobat) {
			menu_off();
		} else if (RELEASED(3) && menu_pick(3, "NOBT", 3)) {
			goto_page(P_RESERVE);
			return;
		}
		if (RELEASED(4) && menu_pick(4, "TARF", 4)) {
			net_reload_tariff();
			goto_page(P_MAIN);
			return;
		}
	} else {
		if (RELEASED(2) && menu_pick(2, "INFO", 12)) {
			goto_page(P_INFO);
			return;
		}
		if (RELEASED(3) && menu_pick(3, "rEPT", 13)) {
			goto_page(P_REPORT);
			return;
		}
		if (RELEASED(4) && menu_pick(4, "CAL ", 14)) {
			goto_page(P_CAL);
			return;
		}
	}
	if (RELEASED(5)) {
		BEEP_MENU();
		menu_sel = 0;
		menu_pg = 0;
		goto_page(P_MAIN);
	}
}

/* ---------------- SAMAR (service numbers, e.g. a school bus) ----------------
 * Every service number is one passenger (child). K1 next, K2 previous, K3 toggles the shown number:
 * first press = "start" (child called / picked up), second press = "done" (child arrived).
 * Many passengers can be on the way at the same time. The state is kept when you leave the page.
 * Big field: "SR- 1234" (idle) or "1234 STA" (on the way). Small field: "A  n" = n passengers on the way.
 * Hold K5 for 3 s: back to the menu.
 */
static bool samar_on[SERVICE_MAX];

static int samar_active(int count)
{
	int n = 0;

	for (int i = 0; i < count && i < SERVICE_MAX; i++) {
		n += samar_on[i];
	}
	return n;
}

static void page_samar(void)
{
	static int idx;
	int count = cfg_service_count();

	colon = false;
	if (count > SERVICE_MAX) {
		count = SERVICE_MAX;
	}
	if (idx >= count) {
		idx = 0;
	}

	int act = samar_active(count);

	if (act > 0) {
		snprintf(small_, sizeof(small_), "A %2d", clampi(act, 0, 99));
	} else {
		snprintf(small_, sizeof(small_), "    ");
	}

	if (count <= 0) {
		snprintf(big, sizeof(big), "SR- ----");
		idx = 0;
	} else if (!samar_on[idx]) {
		snprintf(big, sizeof(big), "SR- %4d", clampi(cfg_service(idx), 0, 9999));
	} else {
		snprintf(big, sizeof(big), "%4d STA", clampi(cfg_service(idx), 0, 9999));
	}

	if (count > 0) {
		if (PRESSED(1)) {
			idx = (idx + 1) % count;
		}
		if (PRESSED(2)) {
			idx = (idx + count - 1) % count;
		}
		if (PRESSED(3)) {
			BEEP_MENU();
			net_service_send(cfg_service(idx), samar_on[idx] ? 2 : 1);
			samar_on[idx] = !samar_on[idx];
		}
	}

	if (STILL(5) && k.hold_ms > 3000) {
		idx = 0;
		goto_page(P_MENU);
	}
}

/* ---------------- NOBAT (queue at a taxi station) ----------------
 * Big field shows what is going on, small field the clock:
 *   "SEArCH  "  looking for the station WiFi (Usefi)
 *   "SEnd    "  station found, telling the server "ready"
 *   "nobt   3"  in the queue, 3 = your place (1 = you are next)
 *   "tUrn    "  blinking + beeps: your turn. K3 = passenger is in: the single meter starts
 * Hold K5 3 s: leave the queue, back to the menu.
 */
static void page_reserve(void)
{
	static bool entered;
	static int64_t next_beep;

	if (!entered) {
		entered = true;
		next_beep = 0;
		net_nobat_begin();
	}
	show_clock();

	nb_phase_t ph = net_nobat_phase();

	if (!net_wifi_connected()) {
		snprintf(big, sizeof(big), "nEt  OFF");
	} else if (ph == NB_SEARCH) {
		snprintf(big, sizeof(big), "SEArCH  ");
	} else if (ph == NB_JOIN) {
		snprintf(big, sizeof(big), "SEnd    ");
	} else if (ph == NB_WAIT) {
		snprintf(big, sizeof(big), "nobt %3d", clampi(net_nobat_position(), 0, 999));
		next_beep = 0;
	} else if (ph == NB_TURN) {
		snprintf(big, sizeof(big), "%s", blink_phase() ? "tUrn    " : "        ");
		if (now >= next_beep) { /* call the driver again every 10 s until he answers */
			beep(3000, 150);
			next_beep = now + 10000;
		}
	}
	if (now < msg_until) {
		snprintf(big, sizeof(big), "%s", msg_text);
	}

	if (ph == NB_TURN && PRESSED(3)) {
		if (!meter_cfg.authorized) {
			show_error(ERR_EXPIRED);
			return;
		}
		BEEP_MENU();
		net_nobat_board();
		entered = false;
		meter_start(0, false);
		net_trip_start(5);
		goto_page(P_FARE1);
		return;
	}

	if (STILL(5) && k.hold_ms > 3000) {
		BEEP_MENU();
		net_nobat_leave();
		entered = false;
		goto_page(P_MENU);
	}
}

/* ---------------- PULSE ---------------- */
static void page_pulse(void)
{
	colon = false;
	snprintf(big, sizeof(big), "%8u", (unsigned)meter_total_pulses());
	snprintf(small_, sizeof(small_), "PL  ");
	if (PRESSED(2)) {
		meter_clear_total();
	}
	if (PRESSED(5)) {
		goto_page(P_MAIN);
	}
}

/* ---------------- helpers for fare pages ---------------- */
static void fmt_fare(char *dst, size_t n, int ch_no, int64_t rial)
{
	/* display is in Toman = Rial / 10 */
	if (rial < 0) {
		rial = 0;
	}
	if (ch_no > 0) {
		snprintf(dst, n, "%d-%6lu", ch_no % 10, (unsigned long)(rial / 10 % 1000000));
	} else {
		snprintf(dst, n, "%8lu", (unsigned long)(rial / 10 % 100000000));
	}
}

static bool blink_phase(void)
{
	return (now / 240) & 1; /* ~480 ms period */
}

/* ---------------- SINGLE METER ---------------- */
static void page_fare1(void)
{
	static int64_t show_until;
	static int64_t blink_until;
	static int idx;
	static bool entered;
	meter_snap_t s;

	if (!entered) {
		entered = true;
		show_until = now + BASE_FARE_SHOW_MS;
		blink_until = 0;
		idx = 0;
	}

	meter_snap(0, &s);

	if (now < show_until) {
		fmt_fare(big, sizeof(big), 0, s.total_rial);
		show_clock();
		return;
	}

	bool blinking = now < blink_until;

	switch (idx) {
	case 0:
		show_clock();
		break;
	case 1:
		colon = false;
		snprintf(small_, sizeof(small_), "%4d", clampi(s.speed_kmh, 0, 999));
		break;
	case 2:
		colon = false;
		int hkm = clampi(s.hkm, 0, 9999);

		snprintf(small_, sizeof(small_), "%3d.%d", hkm / 10, hkm % 10);
		break;
	default:
		colon = false;
		snprintf(small_, sizeof(small_), "%4d", clampi(s.wait_s / 60, 0, 9999));
		break;
	}

	if (blinking) {
		if (blink_phase()) {
			fmt_fare(big, sizeof(big), 0, s.total_rial);
		} else {
			snprintf(big, sizeof(big), "        ");
		}
	} else if (s.paused) {
		snprintf(big, sizeof(big), "  STOP  ");
	} else {
		fmt_fare(big, sizeof(big), 0, s.total_rial);
	}

	if (PRESSED(3)) {
		BEEP_MENU();
		meter_toggle_pause(0);
	}

	if (RELEASED(4)) {
		if (blinking) {
			BEEP_MENU();
			trip_submit(5, &s, s.start_hhmm);
			report_add(5, s.total_rial, s.dist_m, s.start_hhmm);
			meter_stop(0);
			entered = false;
			goto_page(P_MAIN);
			return;
		}
		if (!s.paused) {
			BEEP_MENU();
			blink_until = now + BLINK_MS;
		}
	}

	if (PRESSED(2)) {
		idx = (idx + 1) % 4;
	}
}

/* ---------------- 4-CHANNEL METER ----------------
 * One page shows one channel (cur). K1..K4 pick the channel:
 *   - another channel than the shown one: only switch to it (no blink, no start)
 *   - the shown channel, idle:   starts it. Base fare for 2 s, then the running fare
 *   - the shown channel, active: blinks for 4 s; press again while blinking = end the trip
 * An idle channel shows its number: "1-  ----". With no running channel the page closes after 10 s
 * without a key, or with K5.
 */
#define FARE4_IDLE_EXIT_MS 10000

static void page_fare4(void)
{
	static int64_t show_until;
	static int64_t blink_until;
	static int64_t idle_since;
	static int64_t base_rial;
	static int cur;
	static bool entered;
	meter_snap_t s[4];

	for (int i = 0; i < 4; i++) {
		meter_snap(i, &s[i]);
	}

	if (!entered) {
		entered = true;
		show_until = 0;
		blink_until = 0;
		idle_since = now;
		cur = 0;
		for (int i = 3; i >= 0; i--) { /* after a power loss: show a running channel */
			if (s[i].active) {
				cur = i;
			}
		}
	}

	show_clock();

	bool blinking = now < blink_until;

	if (s[cur].active && now < show_until) {
		fmt_fare(big, sizeof(big), cur + 1, base_rial);
	} else if (s[cur].active) {
		fmt_fare(big, sizeof(big), cur + 1, s[cur].total_rial);
	} else {
		snprintf(big, sizeof(big), "%d-  ----", (cur + 1) % 10);
	}
	if (blinking && blink_phase()) {
		snprintf(big, sizeof(big), "%d-      ", (cur + 1) % 10);
	}
	if (now < msg_until) {
		snprintf(big, sizeof(big), "%s", msg_text);
	}

	for (int n = 0; n < 4; n++) {
		if (!RELEASED(n + 1)) {
			continue;
		}
		BEEP_MENU();
		idle_since = now;
		if (n != cur) {
			cur = n; /* only switch */
			blink_until = 0;
			show_until = 0;
		} else if (!s[n].active) {
			if (!meter_cfg.authorized) {
				show_error(ERR_EXPIRED);
				continue;
			}
			meter_start(n, true);
			net_trip_start(n + 1);
			meter_snap_t first;

			meter_snap(n, &first);
			base_rial = first.total_rial;
			show_until = now + BASE_FARE_SHOW_MS;
			blink_until = 0;
		} else if (blinking) {
			trip_submit(n + 1, &s[n], s[n].start_hhmm);
			report_add(n + 1, s[n].total_rial, s[n].dist_m, s[n].start_hhmm);
			meter_stop(n);
			blink_until = 0;
		} else {
			blink_until = now + BLINK_MS;
		}
	}

	if (meter_any_active()) {
		idle_since = now;
	} else if (PRESSED(5) || now - idle_since >= FARE4_IDLE_EXIT_MS) {
		entered = false;
		goto_page(P_MAIN);
	}
}

/* ---------------- INFO: status of the device ----------------
 * K2 next, K3 previous, K5 exit. Pages (big field / small field):
 *   1 NET  ON / NET OFF           2 HOST UP / HOST BAD / HOST ---
 *   3 PEND  n (records waiting)   4 serial: first 8 chars big, last 4 small
 *   5 SOFT 0.1.0                  6 TIME SET / TIME ---
 *   7 UPD ---/UPD n (download %)/UPD DONE (reboot waits)/UPD TEST (new firmware on test)/UPD FAIL/UPD OFF
 * (letters K Q V W X are not readable on 7 segments, so they are not used)
 */
#define INFO_PAGES 7

static void page_info(void)
{
	static int idx;
	const char *ser = net_serial();

	colon = false;
	snprintf(small_, sizeof(small_), "%d-%d ", idx + 1, INFO_PAGES);

	switch (idx) {
	case 0:
		snprintf(big, sizeof(big), net_wifi_connected() ? "NET  ON " : "NET OFF ");
		break;
	case 1: {
		int st = net_server_state();

		snprintf(big, sizeof(big), st > 0 ? "HOST UP " : (st < 0 ? "HOST BAD" : "HOST ---"));
		break;
	}
	case 2:
		snprintf(big, sizeof(big), "PEND %3d", clampi(postq_count(), 0, 999));
		break;
	case 3:
		if (strlen(ser) == 12) {
			snprintf(big, sizeof(big), "%.8s", ser);
			snprintf(small_, sizeof(small_), "%.4s", ser + 8);
		} else {
			snprintf(big, sizeof(big), "--------");
			snprintf(small_, sizeof(small_), "----");
		}
		break;
	case 4:
		snprintf(big, sizeof(big), "SOFT %s", FW_VERSION);
		break;
	case 5:
		snprintf(big, sizeof(big), clock_is_set() ? "TIME SET" : "TIME ---");
		break;
	default: {
		int pct;

		switch (ota_status(&pct)) {
		case OTA_DOWNLOADING:
			snprintf(big, sizeof(big), "UPD %3d", clampi(pct, 0, 100));
			break;
		case OTA_READY:
			snprintf(big, sizeof(big), "UPD DONE");
			break;
		case OTA_TESTING:
			snprintf(big, sizeof(big), "UPD TEST");
			break;
		case OTA_FAILED:
			snprintf(big, sizeof(big), "UPD FAIL");
			break;
		case OTA_DISABLED:
			snprintf(big, sizeof(big), "UPD  OFF");
			break;
		default:
			snprintf(big, sizeof(big), "UPD  ---");
			break;
		}
		break;
	}
	}

	if (PRESSED(2)) {
		idx = (idx + 1) % INFO_PAGES;
	}
	if (PRESSED(3)) {
		idx = (idx + INFO_PAGES - 1) % INFO_PAGES;
	}
	if (PRESSED(5)) {
		idx = 0;
		goto_page(P_MENU);
	}
}

/* ---------------- REPORT: today and the last trips ----------------
 * K2 next, K3 previous, K5 exit.
 *   page 1: total of today in Toman, small field "SUM "
 *   page 2: number of trips today,   small field "CNT "
 *   page 3..: last trips, newest first: fare in Toman, small field = start time (HHMM)
 */
static void page_report(void)
{
	static int idx;
	int64_t total;
	int count;
	int pages;

	report_today(&total, &count);
	pages = 2 + report_hist_count();
	idx = clampi(idx, 0, pages - 1);
	colon = false;

	if (idx == 0) {
		snprintf(big, sizeof(big), "%8lu", (unsigned long)(total / 10 % 100000000));
		snprintf(small_, sizeof(small_), "SUM ");
	} else if (idx == 1) {
		snprintf(big, sizeof(big), "%8d", clampi(count, 0, 99999999));
		snprintf(small_, sizeof(small_), "CNT ");
	} else {
		rep_entry_t e;

		if (report_hist_get(idx - 2, &e)) {
			snprintf(big, sizeof(big), "%8lu",
				 (unsigned long)(((unsigned)e.total_rial) / 10 % 100000000));
			if (e.hhmm >= 0) {
				snprintf(small_, sizeof(small_), "%04d", e.hhmm);
				colon = true;
			} else {
				snprintf(small_, sizeof(small_), "----");
			}
		}
	}

	if (PRESSED(2)) {
		idx = (idx + 1) % pages;
	}
	if (PRESSED(3)) {
		idx = (idx + pages - 1) % pages;
	}
	if (PRESSED(5)) {
		idx = 0;
		goto_page(P_MENU);
	}
}

/* ---------------- CAL: measure PM (pulses per 10 m) ----------------
 * Drive exactly 100 m.  K2 = start, K2 again = stop.  The screen shows PM = pulses / 10.
 * K3 switches between the measured PM and the PM of the server tariff. K4 = measure again.
 * This only MEASURES. The tariff PM comes from the server, so tell the office the number.
 */
static void page_cal(void)
{
	static int state; /* 0 ready, 1 counting, 2 result */
	static uint32_t base, result;
	static bool show_server;

	colon = false;
	if (state == 0) {
		snprintf(big, sizeof(big), "CAL 100 ");
		snprintf(small_, sizeof(small_), "GO  ");
		if (PRESSED(2)) {
			base = meter_total_pulses();
			state = 1;
		}
	} else if (state == 1) {
		uint32_t n = meter_total_pulses() - base; /* updates once per second */

		snprintf(big, sizeof(big), "%8u", (unsigned)n);
		snprintf(small_, sizeof(small_), "rUN ");
		if (PRESSED(2)) {
			result = meter_total_pulses() - base;
			state = 2;
			show_server = false;
			BEEP_MENU();
		}
	} else {
		if (PRESSED(3)) {
			show_server = !show_server;
		}
		if (show_server) {
			snprintf(big, sizeof(big), "PS  %4d", clampi(meter_tariff1.pm, 0, 9999));
			snprintf(small_, sizeof(small_), "HOST");
		} else {
			snprintf(big, sizeof(big), "PM  %4u", (unsigned)MIN((result + 5) / 10, 9999));
			snprintf(small_, sizeof(small_), "CAL ");
		}
		if (PRESSED(4)) {
			state = 0;
		}
	}

	if (PRESSED(5)) {
		state = 0;
		goto_page(P_MENU);
	}
}

/* ---------------- thread ---------------- */
static void update_backlight(void)
{
	/* backlight on at night; also on while the clock is unknown (easier to read) */
	bool on = page != P_OFF &&
		  (!clock_is_set() || fare_is_night(&meter_tariff1, clock_hour_or_unset()));

	gpio_pin_set_dt(&backlight, on);
}

static void ui_thread(void *a, void *b, void *c)
{
	int64_t next = k_uptime_get();
	int wd = wdog_add(2000);

	if (gpio_is_ready_dt(&backlight)) {
		gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_INACTIVE);
	}

	while (1) {
		wdog_feed(wd);
		next += UI_PERIOD_MS;
		now = k_uptime_get();
		key_poll(&k);
		if (wait_release) {
			bool down = false;

			for (int i = 0; i < KEY_COUNT; i++) {
				down |= k.still[i];
			}
			wait_release = down;
			memset(k.pressed, 0, sizeof(k.pressed));
			memset(k.released, 0, sizeof(k.released));
			memset(k.still, 0, sizeof(k.still));
		}
		/* menu pages go back to the main page by themselves after 30 s without a key */
		if ((page == P_MENU || page == P_INFO || page == P_REPORT) && k.rest_ms > MENU_IDLE_MS) {
			menu_reset();
			goto_page(P_MAIN);
		}

		switch (page) {
		case P_MAIN:    page_main(); break;
		case P_MENU:    page_menu(); break;
		case P_SAMAR:   page_samar(); break;
		case P_RESERVE: page_reserve(); break;
		case P_PULSE:   page_pulse(); break;
		case P_FARE1:   page_fare1(); break;
		case P_FARE4:   page_fare4(); break;
		case P_OFF:     page_off(); break;
		case P_INFO:    page_info(); break;
		case P_REPORT:  page_report(); break;
		case P_CAL:     page_cal(); break;
		}

		display_set(big, small_, colon);
		update_backlight();
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_STACK_DEFINE(ui_stack, 2560);
static struct k_thread ui_tcb;

void ui_start(void)
{
	/* after a power loss the meter was restored from flash: go back to its page */
	if (meter_any_active()) {
		trip_state_t st;

		meter_export(&st);
		bool single = st.ch[0].active && !st.is4[0];

		page = single ? P_FARE1 : P_FARE4;
		LOG_WRN("resumed a trip after restart (%s)", single ? "single meter" : "4 channels");
	}
	k_thread_create(&ui_tcb, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack), ui_thread,
			NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&ui_tcb, "ui");
}

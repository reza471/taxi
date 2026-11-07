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

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define UI_PERIOD_MS 20
#define BEEP_MENU() beep(4000, 100)
#define BASE_FARE_SHOW_MS 2000
#define BLINK_MS 4000

enum page { P_MAIN, P_MENU, P_SAMAR, P_RESERVE, P_PULSE, P_FARE1, P_FARE4, P_OFF };

static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), backlight_gpios);

static key_snapshot_t k;
static enum page page = P_MAIN;
static int64_t now;

static char big[32];
static char small_[16];
static bool colon;
static char start_hhmm[8];

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

static void stamp_start_time(void)
{
	int h, m;

	clock_get_hm(&h, &m);
	snprintf(start_hhmm, sizeof(start_hhmm), "%02d:%02d", h, m);
}

static void goto_page(enum page p)
{
	page = p;
}

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

static void page_main(void)
{
	static int64_t show_until;

	ARG_UNUSED(show_until);
	snprintf(big, sizeof(big), "--------");
	show_clock();

	/* hold K3 for 10 s: reset WiFi settings and restart */
	if (STILL(3) && k.hold_ms > 10000) {
		net_reset_wifi();
	}

	if (!meter_cfg.authorized) {
		return;
	}

	if (PRESSED(1) && meter_cfg.korsi) {
		BEEP_MENU();
		stamp_start_time();
		meter_start(0, true);
		net_trip_start(1);
		goto_page(P_FARE4);
		return;
	}
	if (PRESSED(2)) {
		meter_clear_total();
		goto_page(P_PULSE);
		return;
	}
	if (PRESSED(4) && meter_cfg.darbast) {
		BEEP_MENU();
		stamp_start_time();
		meter_start(0, false);
		net_trip_start(5);
		goto_page(P_FARE1);
		return;
	}

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
		snprintf(small_, sizeof(small_), "    ");
		goto_page(P_MENU);
	}
}

/* ---------------- MENU ---------------- */
static uint8_t menu_sel;

static void page_menu(void)
{
	colon = false;
	snprintf(big, sizeof(big), "MENU    ");

	if (RELEASED(1)) {
		snprintf(small_, sizeof(small_), "----");
		if (menu_sel == 1) {
			menu_sel = 0;
			goto_page(P_MAIN);
			return;
		}
		menu_sel = 1;
	}
	if (RELEASED(2) && meter_cfg.samar) {
		snprintf(small_, sizeof(small_), "SAMR");
		if (menu_sel == 2) {
			menu_sel = 0;
			goto_page(P_SAMAR);
			return;
		}
		menu_sel = 2;
	}
	if (RELEASED(3) && meter_cfg.nobat) {
		snprintf(small_, sizeof(small_), "NOBT");
		if (menu_sel == 3) {
			menu_sel = 0;
			goto_page(P_RESERVE);
			return;
		}
		menu_sel = 3;
	}
	if (RELEASED(4)) {
		snprintf(small_, sizeof(small_), "TARF");
		if (menu_sel == 4) {
			menu_sel = 0;
			net_reload_tariff();
			goto_page(P_MAIN);
			return;
		}
		menu_sel = 4;
	}
	if (RELEASED(5)) {
		BEEP_MENU();
		menu_sel = 0;
		goto_page(P_MAIN);
	}
}

/* ---------------- SAMAR (service numbers) ---------------- */
static void page_samar(void)
{
	static int idx;
	static bool selected;
	int count = cfg_service_count();

	colon = false;
	snprintf(small_, sizeof(small_), "    ");

	if (count <= 0) {
		snprintf(big, sizeof(big), "SR- ----");
		idx = 0;
	} else if (!selected) {
		snprintf(big, sizeof(big), "SR- %4d", clampi(cfg_service(idx), 0, 9999));
	} else {
		snprintf(big, sizeof(big), "%4d STA", clampi(cfg_service(idx), 0, 9999));
	}

	if (count > 0 && !selected) {
		if (PRESSED(1)) {
			idx = (idx + 1) % count;
		}
		if (PRESSED(2)) {
			idx = (idx + count - 1) % count;
		}
		if (PRESSED(3)) {
			net_service_send(cfg_service(idx), 1);
			selected = true;
		}
	} else if (count > 0 && selected && PRESSED(5)) {
		net_service_send(cfg_service(idx), 2);
		idx = 0;
		selected = false;
	}

	if (STILL(5) && k.hold_ms > 3000 && !selected) {
		idx = 0;
		goto_page(P_MENU);
	}
}

/* ---------------- RESERVE (nobat): wait for the dispatch AP ---------------- */
static void page_reserve(void)
{
	static bool entered;

	colon = false;
	snprintf(small_, sizeof(small_), "nobt");

	if (!entered) {
		entered = true;
		net_reserve_reset();
	}
	net_reserve_scan_request();

	if (net_reserve_found()) {
		snprintf(big, sizeof(big), "211  ---");
	} else {
		snprintf(big, sizeof(big), "%s", (now / 500) & 1 ? "--------" : "        ");
	}

	if (STILL(5) && k.hold_ms > 3000) {
		snprintf(small_, sizeof(small_), "    ");
		entered = false;
		net_reserve_reset();
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
			trip_submit(5, &s, start_hhmm);
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

/* ---------------- 4-CHANNEL METER ---------------- */
static void page_fare4(void)
{
	static int64_t show_until;
	static int64_t blink_until;
	static int cur;
	static bool entered;
	meter_snap_t s[4];

	if (!entered) {
		entered = true;
		show_until = now + BASE_FARE_SHOW_MS;
		blink_until = 0;
		cur = 0;
	}

	for (int i = 0; i < 4; i++) {
		meter_snap(i, &s[i]);
	}

	if (now < show_until) {
		fmt_fare(big, sizeof(big), cur + 1, s[cur].total_rial);
		show_clock();
		return;
	}

	show_clock();

	bool blinking = now < blink_until;

	if (s[cur].active) {
		fmt_fare(big, sizeof(big), cur + 1, s[cur].total_rial);
	} else {
		snprintf(big, sizeof(big), "%d ----- ", (cur + 1) % 10);
	}
	if (blinking && blink_phase()) {
		snprintf(big, sizeof(big), "%d-      ", (cur + 1) % 10);
	}

	for (int n = 0; n < 4; n++) {
		if (!RELEASED(n + 1)) {
			continue;
		}
		BEEP_MENU();
		if (!s[n].active) {
			stamp_start_time();
			meter_start(n, true);
			net_trip_start(n + 1);
			cur = n;
			blink_until = 0;
			show_until = now + 1000;
			break;
		}
		if (cur == n && blinking) {
			trip_submit(n + 1, &s[n], start_hhmm);
			meter_stop(n);
			blink_until = 0;
		} else {
			cur = n;
			blink_until = now + BLINK_MS;
		}
	}

	if (!meter_any_active()) {
		entered = false;
		goto_page(P_MAIN);
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

		switch (page) {
		case P_MAIN:    page_main(); break;
		case P_MENU:    page_menu(); break;
		case P_SAMAR:   page_samar(); break;
		case P_RESERVE: page_reserve(); break;
		case P_PULSE:   page_pulse(); break;
		case P_FARE1:   page_fare1(); break;
		case P_FARE4:   page_fare4(); break;
		case P_OFF:     page_off(); break;
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
	k_thread_create(&ui_tcb, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack), ui_thread,
			NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&ui_tcb, "ui");
}

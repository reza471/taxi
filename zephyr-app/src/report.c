/*
 * Daily total and the last trips, kept in flash.
 * "Today" follows the local calendar day (needs the clock). If the clock is not set the
 * totals just keep adding up until it is.
 */
#include "report.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include "hw.h"

LOG_MODULE_REGISTER(report, LOG_LEVEL_INF);

#define REP_MAGIC 0x52455054U /* "REPT" */

typedef struct {
	uint32_t magic;
	uint32_t size;
	int32_t day;          /* local day number of today_*, -1 unknown */
	int32_t today_count;
	int64_t today_total;
	int32_t head;         /* index of the newest entry */
	int32_t count;        /* entries used */
	rep_entry_t hist[REPORT_HIST];
	uint32_t crc;
} rep_state_t;

static rep_state_t st = {.day = -1, .head = REPORT_HIST - 1};
static bool dirty;
static K_MUTEX_DEFINE(lock);

static uint32_t calc_crc(const rep_state_t *s)
{
	return crc32_ieee((const uint8_t *)s, offsetof(rep_state_t, crc));
}

static int rep_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "d") == 0 && len == sizeof(rep_state_t)) {
		rep_state_t tmp;

		if (read_cb(cb_arg, &tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == REP_MAGIC &&
		    tmp.size == sizeof(tmp) && tmp.crc == calc_crc(&tmp) && tmp.head >= 0 &&
		    tmp.head < REPORT_HIST && tmp.count >= 0 && tmp.count <= REPORT_HIST) {
			st = tmp;
		} else {
			LOG_WRN("saved report is damaged, ignored");
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rep, "rep", NULL, rep_set, NULL, NULL);

/* new local day -> today starts from zero (history stays) */
static void roll_locked(void)
{
	int32_t day = clock_day();

	if (day < 0) {
		return; /* clock not set: keep adding */
	}
	if (st.day != day) {
		if (st.day >= 0 || st.today_count > 0) {
			dirty = true;
		}
		/* a first valid clock after boot keeps the totals counted without a clock */
		if (st.day >= 0) {
			st.today_total = 0;
			st.today_count = 0;
		}
		st.day = day;
	}
}

static int16_t parse_hhmm(const char *s)
{
	int h, m;

	if (s && sscanf(s, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
		return (int16_t)(h * 100 + m);
	}
	return -1;
}

void report_add(int bt, int64_t total_rial, int32_t dist_m, const char *start_hhmm)
{
	k_mutex_lock(&lock, K_FOREVER);
	roll_locked();

	st.today_total += total_rial;
	st.today_count++;

	st.head = (st.head + 1) % REPORT_HIST;
	st.hist[st.head].total_rial = (int32_t)MIN(total_rial, (int64_t)INT32_MAX);
	st.hist[st.head].dist_m = dist_m;
	st.hist[st.head].hhmm = parse_hhmm(start_hhmm);
	st.hist[st.head].bt = (uint8_t)bt;
	if (st.count < REPORT_HIST) {
		st.count++;
	}
	dirty = true;
	k_mutex_unlock(&lock);
}

void report_today(int64_t *total_rial, int *count)
{
	k_mutex_lock(&lock, K_FOREVER);
	roll_locked();
	*total_rial = st.today_total;
	*count = st.today_count;
	k_mutex_unlock(&lock);
}

int report_hist_count(void)
{
	return st.count;
}

bool report_hist_get(int i, rep_entry_t *out)
{
	bool ok = false;

	k_mutex_lock(&lock, K_FOREVER);
	if (i >= 0 && i < st.count) {
		*out = st.hist[(st.head - i + REPORT_HIST * 2) % REPORT_HIST];
		ok = true;
	}
	k_mutex_unlock(&lock);
	return ok;
}

bool report_dirty(void)
{
	return dirty;
}

void report_flush(void)
{
	rep_state_t copy;

	k_mutex_lock(&lock, K_FOREVER);
	if (!dirty) {
		k_mutex_unlock(&lock);
		return;
	}
	copy = st;
	dirty = false;
	k_mutex_unlock(&lock);

	copy.magic = REP_MAGIC;
	copy.size = sizeof(copy);
	copy.crc = calc_crc(&copy);
	if (settings_save_one("rep/d", &copy, sizeof(copy)) != 0) {
		LOG_ERR("report save failed, will retry");
		k_mutex_lock(&lock, K_FOREVER);
		dirty = true;
		k_mutex_unlock(&lock);
	}
}

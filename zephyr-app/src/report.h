#ifndef REPORT_H
#define REPORT_H
#include <stdbool.h>
#include <stdint.h>

#define REPORT_HIST 10

typedef struct {
	int32_t total_rial;
	int32_t dist_m;
	int16_t hhmm;  /* start time as HHMM (1305 = 13:05), -1 if unknown */
	uint8_t bt;    /* 1..4 channel, 5 single meter */
	uint8_t pad;
} rep_entry_t;

/* Add an ended trip. Only RAM changes, the net thread saves it later (report_flush). */
void report_add(int bt, int64_t total_rial, int32_t dist_m, const char *start_hhmm);

/* Total and number of trips of today (local day). Starts at 0 on a new day. */
void report_today(int64_t *total_rial, int *count);

/* Last trips, i = 0 is the newest. */
int report_hist_count(void);
bool report_hist_get(int i, rep_entry_t *out);

bool report_dirty(void);
void report_flush(void); /* write to flash, net thread only */
#endif

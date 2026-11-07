#include "hw.h"
#include <zephyr/kernel.h>

static int64_t base_epoch_s;   /* epoch at uptime_base_s */
static int64_t uptime_base_s;
static int32_t tz_offset;
static bool is_set;

void clock_set_epoch(int64_t epoch_s, int32_t tz_offset_s)
{
	base_epoch_s = epoch_s;
	uptime_base_s = k_uptime_get() / 1000;
	tz_offset = tz_offset_s;
	is_set = true;
}

bool clock_is_set(void)
{
	return is_set;
}

void clock_get_hm(int *h, int *m)
{
	if (!is_set) {
		*h = 0;
		*m = 0;
		return;
	}
	int64_t now = base_epoch_s + (k_uptime_get() / 1000 - uptime_base_s) + tz_offset;
	int64_t day_s = ((now % 86400) + 86400) % 86400;

	*h = (int)(day_s / 3600);
	*m = (int)((day_s % 3600) / 60);
}

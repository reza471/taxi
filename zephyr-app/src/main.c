#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "hw.h"
#include "meter.h"
#include "ui.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ---- temporary stubs until the network layer is written ---- */
__weak void trip_submit(int bt, const meter_snap_t *s, const char *start)
{
	LOG_INF("trip end bt=%d start=%s total=%lld rial dist=%d m", bt, start,
		(long long)s->total_rial, s->dist_m);
}
__weak void net_reload_tariff(void) { LOG_INF("reload tariff (no network yet)"); }
__weak void net_trip_start(int bt) { LOG_INF("trip start bt=%d", bt); }
__weak void net_reset_wifi(void) { LOG_INF("reset wifi (no network yet)"); }

int main(void)
{
	LOG_INF("taxi meter start");
	beep_init();
	pulse_init();
	keys_init();
	display_init();
	meter_init();
	ui_start();
	return 0;
}

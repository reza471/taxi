#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include "hw.h"
#include "meter.h"
#include "ui.h"
#include "net.h"
#include "persist.h"
#include "wdog.h"
#include "trip_store.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* A crash (stack overflow, bad pointer, assert) restarts the meter at once
 * instead of freezing. The queued trips are in flash, so nothing is lost. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	LOG_PANIC();
	LOG_ERR("fatal error %u, rebooting", reason);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

int main(void)
{
	log_reset_cause();
	wdog_init();
	LOG_INF("taxi meter start");
	beep_init();
	pulse_init();
	keys_init();
	display_init();
	meter_init();
	persist_init();
	net_init();

	/* power was lost during a trip: bring it back (time without power is not counted) */
	trip_state_t saved;

	if (trip_store_load(&saved)) {
		meter_import(&saved);
		LOG_WRN("restored a running trip from flash");
	}
	ui_start();
	return 0;
}

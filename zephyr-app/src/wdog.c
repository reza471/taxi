#include "wdog.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wdog, LOG_LEVEL_INF);

static const struct device *const hw_wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
static bool ready;

void wdog_init(void)
{
	if (!hw_wdt || !device_is_ready(hw_wdt)) {
		LOG_ERR("hardware watchdog not available");
		return;
	}
	int err = task_wdt_init(hw_wdt);

	if (err) {
		LOG_ERR("task_wdt_init failed: %d", err);
		return;
	}
	ready = true;
	LOG_INF("watchdog enabled");
}

int wdog_add(uint32_t timeout_ms)
{
	if (!ready) {
		return -1;
	}
	/* callback NULL = reset the chip when this channel expires */
	return task_wdt_add(timeout_ms, NULL, NULL);
}

void wdog_feed(int id)
{
	if (ready && id >= 0) {
		task_wdt_feed(id);
	}
}

void log_reset_cause(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) == 0) {
		LOG_INF("reset cause: 0x%x%s%s%s%s", cause,
			(cause & RESET_POR) ? " POWER-ON" : "",
			(cause & RESET_WATCHDOG) ? " WATCHDOG" : "",
			(cause & RESET_SOFTWARE) ? " SOFTWARE" : "",
			(cause & RESET_BROWNOUT) ? " BROWNOUT" : "");
		hwinfo_clear_reset_cause();
	}
}

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "hw.h"
#include "meter.h"
#include "ui.h"
#include "net.h"
#include "persist.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("taxi meter start");
	beep_init();
	pulse_init();
	keys_init();
	display_init();
	meter_init();
	persist_init();
	net_init();
	ui_start();
	return 0;
}

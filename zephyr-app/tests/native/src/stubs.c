/* Stand-ins for the parts of meter.c that need real hardware. */
#include "meter.h"
#include "hw.h"

meter_cfg_t meter_cfg = {.authorized = true, .korsi = true, .darbast = true,
			 .samar = true, .nobat = true};
fare_tariff_t meter_tariff1 = {.pm = 46, .pri_cost = 26200};
fare_tariff_t meter_tariff4 = {.pm = 46, .pri_cost = 5000};
int stub_config_calls;

void meter_set_config(const fare_tariff_t *t1, const fare_tariff_t *t4, const meter_cfg_t *cfg)
{
	meter_tariff1 = *t1;
	meter_tariff4 = *t4;
	meter_cfg = *cfg;
	stub_config_calls++;
}

/* the local day the tests pretend it is (-1 = clock not set) */
int32_t stub_day = 100;

int32_t clock_day(void)
{
	return stub_day;
}

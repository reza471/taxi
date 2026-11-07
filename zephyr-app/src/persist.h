#ifndef PERSIST_H
#define PERSIST_H
#include <stdint.h>
#include "meter.h"
#include "net.h"

#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 65

typedef struct {
	uint32_t magic;
	fare_tariff_t t1;
	fare_tariff_t t4;
	meter_cfg_t cfg;
	int32_t service[SERVICE_MAX];
	int32_t service_count;
	char ssid[WIFI_SSID_MAX];
	char pass[WIFI_PASS_MAX];
} persist_t;

extern persist_t persist;

/* Load from flash and apply to the meter. Falls back to defaults. */
void persist_init(void);
/* Write `persist` to flash. */
void persist_save(void);
/* Reset WiFi to the factory default (Rad) and save. */
void persist_reset_wifi(void);
#endif

#ifndef METER_H
#define METER_H
#include <stdbool.h>
#include <stdint.h>
#include "fare_engine.h"
#include "trip_store.h"

/* Feature flags (from the server tariff). */
typedef struct {
	bool authorized; /* false when the server says "expired" (HTTP 403) */
	bool korsi;      /* 4-channel meter allowed */
	bool darbast;    /* single meter allowed */
	bool samar;
	bool nobat;
} meter_cfg_t;

extern meter_cfg_t meter_cfg;
extern fare_tariff_t meter_tariff1; /* single meter */
extern fare_tariff_t meter_tariff4; /* 4-channel meter */

typedef struct {
	bool active;
	bool paused;
	bool night;
	int64_t total_rial;
	int32_t speed_kmh;
	int32_t hkm;    /* distance in 0.1 km */
	int32_t dist_m;
	int32_t wait_s; /* seconds counted as waiting */
	int32_t duration_s;
	int64_t trip_price;    /* route price (start fare + distance) */
	int64_t traffic_price; /* waiting-time price */
	int64_t night_price;   /* night surcharge inside trip_price */
	int32_t avg_speed_kmh;
	int32_t max_speed_kmh;
	char start_hhmm[8];   /* "HH:MM" when this trip started */
} meter_snap_t;

void meter_init(void);
/* Replace tariffs and flags (thread safe). */
void meter_set_config(const fare_tariff_t *t1, const fare_tariff_t *t4, const meter_cfg_t *cfg);
void meter_start(int ch, bool is4ch);
void meter_stop(int ch);
bool meter_toggle_pause(int ch);
void meter_snap(int ch, meter_snap_t *out);
/* Power-loss recovery */
void meter_export(trip_state_t *out);  /* sealed snapshot of all channels */
void meter_import(const trip_state_t *in);
bool meter_any_active(void);
uint32_t meter_total_pulses(void);
int meter_speed_kmh(void); /* live speed, also when no trip is running */
void meter_clear_total(void);

/* Implemented outside (network layer). Weak stubs live in main.c for now. */
void trip_submit(int bt_number, const meter_snap_t *s, const char *start_hhmm);
void net_reload_tariff(void);
void net_trip_start(int bt_number);
void net_reset_wifi(void);

#endif

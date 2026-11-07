#ifndef TRIP_STORE_H
#define TRIP_STORE_H
#include <stdbool.h>
#include <stdint.h>
#include "fare_engine.h"

/* Snapshot of every running meter channel. Saved in flash so a trip survives a power loss. */
typedef struct {
	uint32_t magic;
	uint32_t size;      /* sizeof(trip_state_t), rejects data from another version */
	fare_ch_t ch[4];
	uint8_t is4[4];     /* channel uses the 4-channel tariff */
	char start[4][8];   /* "HH:MM" of the trip start */
	uint32_t crc;       /* CRC32 of everything above */
} trip_state_t;

/* Fill magic/size/crc. Call before trip_store_save(). */
void trip_state_seal(trip_state_t *s);
/* True if `s` is intact (magic, size, crc). */
bool trip_state_valid(const trip_state_t *s);
/* True if at least one channel is active. */
bool trip_state_any_active(const trip_state_t *s);

/* Write to flash (net thread only, may stall other threads for a few ms). */
bool trip_store_save(const trip_state_t *s);
/* The state found in flash at boot. Returns true only for a valid state with an active channel. */
bool trip_store_load(trip_state_t *out);

#endif

/*
 * Nobat (taxi queue at a station): read the server answer. Pure C, host tested.
 *
 * Answer of NobatReady / NobatStatus (JSON, any key order, numbers may be strings):
 *   {"state": "wait" | "turn" | "none", "position": 3, "nobat_id": 1234}
 *   wait = in the queue, `position` cars before you + 1 (1 = you are next)
 *   turn = it is your turn now (go to the passenger)
 *   none = the server has no queue entry for this car (join again)
 */
#ifndef NOBAT_H
#define NOBAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { NOBAT_NONE = 0, NOBAT_WAIT, NOBAT_TURN } nobat_state_t;

typedef struct {
	nobat_state_t state;
	int32_t position; /* 0..999 (clamped) */
	int64_t id;       /* 0 = none */
} nobat_info_t;

/* Returns false when the text is not valid JSON or has no known "state". `out` is set only on true. */
bool nobat_parse(const char *text, size_t len, nobat_info_t *out);

#endif

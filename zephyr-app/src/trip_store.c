#include "trip_store.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(trip_store, LOG_LEVEL_INF);

#define TRIP_MAGIC 0x54524950U /* "TRIP" */

static trip_state_t loaded;
static bool have_loaded;

static uint32_t calc_crc(const trip_state_t *s)
{
	return crc32_ieee((const uint8_t *)s, offsetof(trip_state_t, crc));
}

void trip_state_seal(trip_state_t *s)
{
	s->magic = TRIP_MAGIC;
	s->size = sizeof(*s);
	s->crc = calc_crc(s);
}

bool trip_state_valid(const trip_state_t *s)
{
	return s->magic == TRIP_MAGIC && s->size == sizeof(*s) && s->crc == calc_crc(s);
}

bool trip_state_any_active(const trip_state_t *s)
{
	for (int i = 0; i < 4; i++) {
		if (s->ch[i].active) {
			return true;
		}
	}
	return false;
}

static int trip_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "s") == 0 && len == sizeof(loaded)) {
		if (read_cb(cb_arg, &loaded, sizeof(loaded)) == sizeof(loaded)) {
			have_loaded = true;
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(trip, "trip", NULL, trip_set, NULL, NULL);

bool trip_store_save(const trip_state_t *s)
{
	int err = settings_save_one("trip/s", s, sizeof(*s));

	if (err) {
		LOG_ERR("trip state save failed: %d", err);
		return false;
	}
	return true;
}

bool trip_store_load(trip_state_t *out)
{
	if (!have_loaded || !trip_state_valid(&loaded)) {
		if (have_loaded) {
			LOG_WRN("saved trip state is damaged, ignored");
		}
		return false;
	}
	if (!trip_state_any_active(&loaded)) {
		return false;
	}
	*out = loaded;
	return true;
}

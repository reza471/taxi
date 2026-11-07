#include "hw.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(keys, LOG_LEVEL_INF);

#define SCAN_MS 10
#define DEBOUNCE_SAMPLES 3 /* 30 ms stable */

#define KEY_SPEC(i) GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), keys_gpios, i)

static const struct gpio_dt_spec keys[KEY_COUNT] = {
	KEY_SPEC(0), KEY_SPEC(1), KEY_SPEC(2), KEY_SPEC(3), KEY_SPEC(4),
};

static K_MUTEX_DEFINE(lock);
static key_snapshot_t state; /* edges accumulate here until key_poll() */

static void scan_thread(void *a, void *b, void *c)
{
	bool stable[KEY_COUNT] = {0};
	uint8_t cnt[KEY_COUNT] = {0};
	int64_t next = k_uptime_get();

	while (1) {
		next += SCAN_MS;

		bool any = false;

		k_mutex_lock(&lock, K_FOREVER);
		for (int i = 0; i < KEY_COUNT; i++) {
			bool raw = gpio_pin_get_dt(&keys[i]) > 0;

			if (raw == stable[i]) {
				cnt[i] = 0;
			} else if (++cnt[i] >= DEBOUNCE_SAMPLES) {
				stable[i] = raw;
				cnt[i] = 0;
				if (raw) {
					state.pressed[i] = true;
				} else {
					state.released[i] = true;
				}
			}
			state.still[i] = stable[i];
			any |= stable[i];
		}
		if (any) {
			state.hold_ms += SCAN_MS;
			state.rest_ms = 0;
		} else {
			state.hold_ms = 0;
			if (state.rest_ms < INT32_MAX - SCAN_MS) {
				state.rest_ms += SCAN_MS;
			}
		}
		k_mutex_unlock(&lock);

		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(keys_tid, 1024, scan_thread, NULL, NULL, NULL, 6, 0, 200);

void keys_init(void)
{
	for (int i = 0; i < KEY_COUNT; i++) {
		if (!gpio_is_ready_dt(&keys[i])) {
			LOG_ERR("key %d gpio not ready", i + 1);
			continue;
		}
		/* flags (active low, pull-up) come from the devicetree */
		gpio_pin_configure_dt(&keys[i], GPIO_INPUT);
	}
}

void key_poll(key_snapshot_t *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = state;
	for (int i = 0; i < KEY_COUNT; i++) {
		state.pressed[i] = false;
		state.released[i] = false;
	}
	k_mutex_unlock(&lock);
}

int32_t key_rest_ms(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	int32_t v = state.rest_ms;

	k_mutex_unlock(&lock);
	return v;
}

#include "hw.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec buzzer = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), buzzer_gpios);

struct beep_msg {
	uint32_t freq;
	uint32_t ms;
};

K_MSGQ_DEFINE(beep_q, sizeof(struct beep_msg), 4, 4);

static void beep_thread(void *a, void *b, void *c)
{
	struct beep_msg m;

	while (1) {
		k_msgq_get(&beep_q, &m, K_FOREVER);
		uint32_t half_us = 500000U / m.freq;
		uint32_t n = (m.ms * 1000U) / (2 * half_us);

		for (uint32_t i = 0; i < n; i++) {
			gpio_pin_set_dt(&buzzer, 1);
			k_busy_wait(half_us);
			gpio_pin_set_dt(&buzzer, 0);
			k_busy_wait(half_us);
		}
	}
}

K_THREAD_DEFINE(beep_tid, 1024, beep_thread, NULL, NULL, NULL, 12, 0, 0);

void beep_init(void)
{
	if (gpio_is_ready_dt(&buzzer)) {
		gpio_pin_configure_dt(&buzzer, GPIO_OUTPUT_INACTIVE);
	}
}

void beep(uint32_t freq_hz, uint32_t ms)
{
	struct beep_msg m = {.freq = freq_hz ? freq_hz : 1000, .ms = ms};

	k_msgq_put(&beep_q, &m, K_NO_WAIT);
}

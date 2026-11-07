#include "hw.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pulse, LOG_LEVEL_INF);

#define MIN_EDGE_US 100 /* ignore edges closer than this (glitch filter) */

static const struct gpio_dt_spec pulse_gpio = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pulse_gpios);
static struct gpio_callback pulse_cb;
static atomic_t pulse_count;
static uint32_t last_cycles;

static void pulse_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	uint32_t now = k_cycle_get_32();
	uint32_t dt_us = k_cyc_to_us_floor32(now - last_cycles);

	if (dt_us < MIN_EDGE_US) {
		return;
	}
	last_cycles = now;
	atomic_inc(&pulse_count);
}

void pulse_init(void)
{
	if (!gpio_is_ready_dt(&pulse_gpio)) {
		LOG_ERR("pulse gpio not ready");
		return;
	}
	gpio_pin_configure_dt(&pulse_gpio, GPIO_INPUT);
	gpio_init_callback(&pulse_cb, pulse_isr, BIT(pulse_gpio.pin));
	gpio_add_callback(pulse_gpio.port, &pulse_cb);
	gpio_pin_interrupt_configure_dt(&pulse_gpio, GPIO_INT_EDGE_RISING);
}

uint32_t pulse_take(void)
{
	return (uint32_t)atomic_set(&pulse_count, 0);
}

#include "Odometer.h"
#include <stdbool.h>
#include <driver/pcnt.h>

// Configuration
const int pulse_gpio = 32;                 // Pulse input GPIO
const int ctrl_gpio = 5;                   // Control GPIO (not used in this example)
const pcnt_unit_t pcnt_unit = PCNT_UNIT_0; // Pulse Counter unit

void Odometer_Init()
{
    // Configure PCNT unit
    pcnt_config_t pcnt_config = {
        .pulse_gpio_num = pulse_gpio,
        .ctrl_gpio_num = ctrl_gpio,
        .lctrl_mode = PCNT_MODE_KEEP, // Rising edge count mode
        .hctrl_mode = PCNT_MODE_KEEP, // Falling edge count mode
        .pos_mode = PCNT_COUNT_INC,   // Count rising edges
        .neg_mode = PCNT_COUNT_DIS,   // Don't count falling edges
        .counter_h_lim = 32767,
        .counter_l_lim = -32768,
        .unit = pcnt_unit,
        .channel = PCNT_CHANNEL_0,
    };

    // Initialize PCNT unit
    pcnt_unit_config(&pcnt_config);

    // Enable glitch filter (100ms)
    pcnt_set_filter_value(pcnt_unit, 100);
    pcnt_filter_enable(pcnt_unit);

    // Initialize PCNT's counter
    pcnt_counter_pause(pcnt_unit);
    pcnt_counter_clear(pcnt_unit);

    // Start counting
    pcnt_counter_resume(pcnt_unit);
}

uint16_t pcnt_get_counter()
{ // Read Pulse Counter
    static int16_t result;
    pcnt_counter_pause(PCNT_UNIT_0);
    pcnt_get_counter_value(PCNT_UNIT_0, (int16_t *)&result);
    pcnt_counter_clear(PCNT_UNIT_0);
    pcnt_counter_resume(pcnt_unit);

    return (uint16_t)result;
}
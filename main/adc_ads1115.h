#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ADS1115 16-bit I2C ADC, ADDR→GND = 0x48 */

bool ads1115_init(void);
bool ads1115_is_ready(void);
bool ads1115_read_channel(int channel, int16_t *raw_out);

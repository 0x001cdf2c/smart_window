#pragma once

#include <stdbool.h>
#include "esp_adc/adc_oneshot.h"

bool smoke_sensor_init(void);
bool smoke_sensor_read(int *air_quality_pct);
adc_oneshot_unit_handle_t smoke_sensor_get_adc_handle(void);

#pragma once

#include <stdbool.h>

bool rain_sensor_init(void);
bool rain_sensor_read(int *rain_pct);

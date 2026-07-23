#pragma once

#include <stdbool.h>

bool smoke_sensor_init(void);
bool smoke_sensor_read(int *air_quality_pct);
